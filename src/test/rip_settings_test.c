/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file rip_settings_test.c
 *
 * Exercises the zmk-feature-custom-settings-backed persistence added for the
 * runtime input processor (see docs/design/custom-settings-storage.md): a
 * Set*-equivalent write through the module's public firmware API must
 * survive a settings backend reload, not just an in-RAM cache.
 *
 * This runs from a SYS_INIT hook at APPLICATION level, i.e. before ZMK
 * main() calls settings_subsys_init()/settings_load() - so it registers its
 * own minimal fake in-RAM struct settings_store first (mirroring
 * zmk-feature-custom-settings' own src/test/custom_settings_test.c
 * test_settings_backend_init/test_settings_save/test_settings_load), then:
 *
 *  1. Writes a new persistent scaling value through the ordinary firmware
 *     API (what the Studio RPC handlers call) and force-flushes the
 *     debounced save.
 *  2. Clobbers the *device's own* runtime state (a temporary, non-persistent
 *     set to different values) so step 4 cannot pass by accident just
 *     because the device's live struct still happened to hold the right
 *     value.
 *  3. Forces zmk-feature-custom-settings to reload every entry from the fake
 *     settings backend (settings_load_subtree) - exactly what ZMK main()'s
 *     real settings_load() does at boot.
 *  4. Re-runs this module's boot-apply logic and confirms the reloaded value
 *     made it back into the processor's persistent/current fields.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <zmk/pointing/input_processor_runtime.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* processor-label of tests/studio/native_sim.keymap's runtime_input_processor. */
#define TEST_PROCESSOR_NAME "default"

/* --- Minimal fake in-RAM settings backend -------------------------------
 * ZMK main() normally calls settings_subsys_init() + settings_load() after
 * every SYS_INIT level has run, so this SYS_INIT-driven test cannot rely on
 * a real (flash-backed) settings store existing yet. Register a fake one
 * ourselves instead, exactly like zmk-feature-custom-settings' own
 * src/test/custom_settings_test.c does. */
#define TEST_SETTINGS_STORAGE_CAPACITY 4

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t len;
};

static struct test_settings_record test_settings_storage[TEST_SETTINGS_STORAGE_CAPACITY];

static struct test_settings_record *test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        if (test_settings_storage[i].present &&
            strncmp(test_settings_storage[i].name, name, sizeof(test_settings_storage[i].name)) ==
                0) {
            return &test_settings_storage[i];
        }
    }
    return NULL;
}

static ssize_t test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);
    memcpy(data, record->data, read_len);
    return read_len;
}

static int test_settings_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        struct test_settings_record *record = &test_settings_storage[i];
        if (!record->present) {
            continue;
        }
        int ret = settings_call_set_handler(record->name, record->len, test_settings_read_cb,
                                            record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static int test_settings_save(struct settings_store *cs, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(cs);

    struct test_settings_record *record = test_settings_find_record(name);
    if (value == NULL) {
        if (record) {
            record->present = false;
        }
        return 0;
    }

    if (val_len > sizeof(record->data)) {
        return -EMSGSIZE;
    }
    if (strlen(name) >= SETTINGS_MAX_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    if (!record) {
        for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
            if (!test_settings_storage[i].present) {
                record = &test_settings_storage[i];
                break;
            }
        }
    }
    if (!record) {
        return -ENOMEM;
    }

    record->present = true;
    strcpy(record->name, name);
    memcpy(record->data, value, val_len);
    record->len = val_len;
    return 0;
}

static const struct settings_store_itf test_settings_itf = {
    .csi_load = test_settings_load,
    .csi_save = test_settings_save,
};

static struct settings_store test_settings_store = {.cs_itf = &test_settings_itf};

static int test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&test_settings_store);
    settings_dst_register(&test_settings_store);
    return 0;
}

/* --- The actual persistence round-trip test ----------------------------- */

static int test_scaling_persists_across_reload(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_name(TEST_PROCESSOR_NAME);
    if (!dev) {
        LOG_ERR("Test processor '%s' not found", TEST_PROCESSOR_NAME);
        return -ENODEV;
    }

    struct zmk_input_processor_runtime_config before;
    int ret = zmk_input_processor_runtime_get_config(dev, NULL, &before);
    if (ret < 0) {
        return ret;
    }

    // Pick values guaranteed to differ from the devicetree default.
    uint32_t new_multiplier = before.scale_multiplier + 5;
    uint32_t new_divisor = before.scale_divisor + 3;

    ret = zmk_input_processor_runtime_set_scaling(dev, new_multiplier, new_divisor,
                                                  ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        LOG_ERR("set_scaling failed: %d", ret);
        return ret;
    }

    // 1. Force the debounced save to run (and actually persist) now.
    zmk_input_processor_runtime_test_flush_save(dev);

    // 2. Clobber the *device's own* persistent_* fields back to devicetree
    //    defaults (zmk_input_processor_runtime_reset also reschedules a
    //    save of the defaults, but that debounced work is never flushed
    //    before this test's assertions run below, so it cannot race with
    //    step 3/4). This proves step 4 cannot pass by accident just because
    //    the device's struct simply never changed.
    zmk_input_processor_runtime_reset(dev);

    // 3. Force a reload from the settings backend, exactly like ZMK main()'s
    //    real settings_load() at boot.
    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        LOG_ERR("settings_load_subtree failed: %d", ret);
        return ret;
    }

    // 4. Re-run the boot-apply logic and verify the persisted value won.
    zmk_input_processor_runtime_test_apply_persisted_settings();

    struct zmk_input_processor_runtime_config after;
    ret = zmk_input_processor_runtime_get_config(dev, NULL, &after);
    if (ret < 0) {
        return ret;
    }

    if (after.scale_multiplier != new_multiplier || after.scale_divisor != new_divisor) {
        LOG_ERR("Persisted scaling not restored: got %u/%u expected %u/%u", after.scale_multiplier,
                after.scale_divisor, new_multiplier, new_divisor);
        return -EINVAL;
    }

    LOG_INF("PASS: rip_settings_persist_reload scale=%u/%u", after.scale_multiplier,
            after.scale_divisor);
    return 0;
}

/* Exercises the write modes and the save/discard-all operations:
 *  - a WRITE_MODE_MEMORY write updates the baseline in RAM but must NOT reach
 *    flash, so discard_all (reload from flash) reverts it;
 *  - a subsequent save_all flushes the in-RAM baseline to flash so it then
 *    survives a settings reload.
 * Uses rotation (independent of the scaling the first test leaves persisted).
 */
static int test_write_modes_and_save_discard(void) {
    const struct device *dev = zmk_input_processor_runtime_find_by_name(TEST_PROCESSOR_NAME);
    if (!dev) {
        LOG_ERR("Test processor '%s' not found", TEST_PROCESSOR_NAME);
        return -ENODEV;
    }

    // Establish a known persisted baseline (persist + flush to the backend).
    const int32_t persisted_rot = 11;
    int ret = zmk_input_processor_runtime_set_rotation(
        dev, persisted_rot, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    zmk_input_processor_runtime_test_flush_save(dev);

    // MEMORY write: updates the baseline in RAM only (visible via get_config)
    // but must not touch flash.
    const int32_t memory_rot = persisted_rot + 7;
    ret = zmk_input_processor_runtime_set_rotation(dev, memory_rot,
                                                   ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    struct zmk_input_processor_runtime_config cfg;
    zmk_input_processor_runtime_get_config(dev, NULL, &cfg);
    if (cfg.rotation_degrees != memory_rot) {
        LOG_ERR("Memory write not staged: got %d expected %d", cfg.rotation_degrees, memory_rot);
        return -EINVAL;
    }

    // discard_all must drop the unsaved memory change and revert to flash.
    ret = zmk_input_processor_runtime_discard_all();
    if (ret < 0) {
        return ret;
    }
    zmk_input_processor_runtime_get_config(dev, NULL, &cfg);
    if (cfg.rotation_degrees != persisted_rot) {
        LOG_ERR("Discard did not revert memory write: got %d expected %d", cfg.rotation_degrees,
                persisted_rot);
        return -EINVAL;
    }

    // A memory write followed by save_all must survive a settings reload.
    const int32_t saved_rot = persisted_rot + 3;
    ret = zmk_input_processor_runtime_set_rotation(dev, saved_rot,
                                                   ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_save_all();
    if (ret < 0) {
        return ret;
    }

    // Clobber the live struct, reload from the backend, re-apply.
    zmk_input_processor_runtime_reset(dev);
    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }
    zmk_input_processor_runtime_test_apply_persisted_settings();

    zmk_input_processor_runtime_get_config(dev, NULL, &cfg);
    if (cfg.rotation_degrees != saved_rot) {
        LOG_ERR("save_all did not persist memory value: got %d expected %d", cfg.rotation_degrees,
                saved_rot);
        return -EINVAL;
    }

    LOG_INF("PASS: rip_settings_write_modes rot=%d", cfg.rotation_degrees);
    return 0;
}

static int rip_settings_test_init(void) {
    int ret = test_settings_backend_init();
    if (ret < 0) {
        LOG_ERR("FAIL: rip_settings_persist_reload backend_init ret=%d", ret);
        return 0; // Never fail boot because of a test failure.
    }

    ret = test_scaling_persists_across_reload();
    if (ret < 0) {
        LOG_ERR("FAIL: rip_settings_persist_reload ret=%d", ret);
    }

    ret = test_write_modes_and_save_discard();
    if (ret < 0) {
        LOG_ERR("FAIL: rip_settings_write_modes ret=%d", ret);
    }
    return 0;
}
SYS_INIT(rip_settings_test_init, APPLICATION, 99);
