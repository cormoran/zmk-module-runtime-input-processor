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
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <cormoran/zmk/custom_settings.h>
#include <drivers/input_processor.h>
#include <zmk/keymap.h>
#include <zmk/events/input_processor_inertia_state_changed.h>
#include <zmk/pointing/input_processor_runtime.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* processor-label of tests/studio/native_sim.keymap's runtime_input_processor. */
#define TEST_PROCESSOR_NAME "default"

static enum zmk_input_processor_inertia_stop_reason last_inertia_stop_reason;
static bool last_inertia_fast_input;
static uint32_t inertia_fast_input_transitions;

static int test_inertia_state_listener(const zmk_event_t *eh) {
    const struct zmk_input_processor_inertia_state_changed *ev =
        as_zmk_input_processor_inertia_state_changed(eh);
    if (ev) {
        if (ev->state == ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STARTED ||
            ev->state == ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STOPPED) {
            last_inertia_fast_input =
                ev->state == ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STARTED;
            inertia_fast_input_transitions++;
        } else if (ev->state == ZMK_INPUT_PROCESSOR_INERTIA_STOPPED) {
            last_inertia_stop_reason = ev->stop_reason;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(test_inertia_state, test_inertia_state_listener);
ZMK_SUBSCRIPTION(test_inertia_state, zmk_input_processor_inertia_state_changed);

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

/* Exercise the processor through its public driver API so inertia
 * entry and reverse-direction exit are tested at the runtime boundary. */
static int process_test_relative_x_with_code(const struct device *dev, int32_t input,
                                             int32_t *output, uint16_t *code) {
    struct input_event event = {
        .type = INPUT_EV_REL,
        .code = INPUT_REL_X,
        .value = input,
    };

    int ret = zmk_input_processor_handle_event(dev, &event, 0, 0, NULL);
    if (ret != ZMK_INPUT_PROC_CONTINUE) {
        LOG_ERR("Input processor returned %d", ret);
        return -EINVAL;
    }

    *output = event.value;
    if (code) {
        *code = event.code;
    }
    return 0;
}

static int process_test_relative_x(const struct device *dev, int32_t input, int32_t *output) {
    return process_test_relative_x_with_code(dev, input, output, NULL);
}

static int process_test_relative_x_with_remainder(const struct device *dev, int16_t input,
                                                  int16_t *remainder, int32_t *output) {
    struct input_event event = {.type = INPUT_EV_REL, .code = INPUT_REL_X, .value = input};
    struct zmk_input_processor_state state = {.remainder = remainder};
    int ret = zmk_input_processor_handle_event(dev, &event, 0, 0, &state);
    *output = event.value;
    return ret == ZMK_INPUT_PROC_CONTINUE ? 0 : -EINVAL;
}

static int test_inertia(void) {
    int ret = zmk_input_processor_runtime_test_inertia_sliding_window();
    if (ret < 0) {
        LOG_ERR("Inertia sliding-window split/expiry check failed");
        return ret;
    }
    const struct device *dev = zmk_input_processor_runtime_find_by_name(TEST_PROCESSOR_NAME);
    if (!dev) {
        LOG_ERR("Test processor '%s' not found", TEST_PROCESSOR_NAME);
        return -ENODEV;
    }
    ret = zmk_input_processor_runtime_set_inertia_notifications(dev, true);
    if (ret < 0) {
        return ret;
    }

    /* Keep preceding persistence tests from affecting these output checks. */
    ret = zmk_input_processor_runtime_set_rotation(
        dev, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_scaling(dev, 1, 1,
                                                  ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_xy_to_scroll_enabled(
        dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_window(
        dev, 100, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_interval(
        dev, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 50, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    if (zmk_input_processor_runtime_set_inertia_decay(
            dev, 101, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY) != -EINVAL) {
        LOG_ERR("Inertia decay accepted an out-of-range percentage");
        return -EINVAL;
    }

    /* Disabling inertia keeps the threshold available for later use. */
    ret = zmk_input_processor_runtime_set_inertia_enabled(
        dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    int32_t output;
    uint16_t output_code;
    if (ret < 0 ||
        (ret = process_test_relative_x_with_code(dev, 30, &output, &output_code)) < 0) {
        return ret;
    }
    if (output_code != INPUT_REL_HWHEEL || zmk_input_processor_runtime_test_inertia_active(dev)) {
        return -EINVAL;
    }

    ret = zmk_input_processor_runtime_set_inertia_enabled(
        dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || zmk_input_processor_runtime_set_inertia_threshold(
            dev, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY) != -EINVAL) {
        return ret < 0 ? ret : -EINVAL;
    }
    ret = zmk_input_processor_runtime_set_inertia_threshold(
        dev, 10, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }

    /* The triggering input seeds the speed, so output starts without waiting
     * for another physical event. */
    if ((ret = process_test_relative_x(dev, 6, &output)) < 0 ||
        (ret = process_test_relative_x(dev, 5, &output)) < 0) {
        return ret;
    }
    if (!zmk_input_processor_runtime_test_inertia_active(dev)) {
        return -EINVAL;
    }
    ret = zmk_input_processor_runtime_test_inertia_output_code(dev, &output_code);
    if (ret < 0 || output_code != INPUT_REL_HWHEEL) {
        LOG_ERR("Automatic scroll output code: got %u expected %u", output_code,
                INPUT_REL_HWHEEL);
        return ret < 0 ? ret : -EINVAL;
    }
    int16_t inertia_output;
    ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output);
    if (ret < 0 || inertia_output != 2) {
        LOG_ERR("Inertia entry output: got %d expected 2", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }

    /* In inertia mode, post-entry input is accumulated in a 100 ms window
     * while output is emitted every 20 ms. Fractional output is carried. */
    if ((ret = process_test_relative_x(dev, 17, &output)) < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output);
    if (ret < 0 || inertia_output != 5) {
        LOG_ERR("Inertia first output: got %d expected 5", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }
    /* The same measurement window now totals 34, increasing the speed. */
    if ((ret = process_test_relative_x(dev, 6, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 7) {
        LOG_ERR("Inertia max output: got %d expected 7", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }

    /* With no new input, the retained speed decays across output intervals. */
    if ((ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 7) {
        LOG_ERR("Inertia initial decay output: got %d expected 7", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }
    if ((ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 3) {
        LOG_ERR("Inertia decayed output: got %d expected 3", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }

    /* An isolated reverse count is sensor jitter; deliberate reversal exits. */
    if ((ret = process_test_relative_x(dev, -1, &output)) < 0) {
        return ret;
    }
    if (!zmk_input_processor_runtime_test_inertia_active(dev) ||
        (ret = process_test_relative_x(dev, -4, &output)) < 0) {
        return ret < 0 ? ret : -EINVAL;
    }
    if (zmk_input_processor_runtime_test_inertia_active(dev)) {
        return -EINVAL;
    }
    if (last_inertia_stop_reason != ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_REVERSE_INPUT) {
        LOG_ERR("Inertia reverse stop reason missing");
        return -EINVAL;
    }

    /* A small decay rate follows a fractional exponential curve. */
    ret = zmk_input_processor_runtime_set_inertia_window(
        dev, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_interval(
        dev, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 5, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = process_test_relative_x(dev, 6, &output)) < 0 ||
        (ret = process_test_relative_x(dev, 5, &output)) < 0 ||
        (ret = process_test_relative_x(dev, 50, &output)) < 0) {
        return ret;
    }
    if ((ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 61 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 61 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 57 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 56) {
        LOG_ERR("Inertia fractional decay: got %d expected 56", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }

    /* Zero decay keeps output alive. Carry fractional counts so a slow
     * setting still produces movement across several output intervals. */
    ret = zmk_input_processor_runtime_set_inertia_window(
        dev, 100, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_interval(
        dev, 10, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_threshold(
        dev, 3, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = process_test_relative_x(dev, 3, &output)) < 0) {
        return ret;
    }
    int32_t total = 0;
    for (int i = 0; i < 20; i++) {
        ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output);
        if (ret < 0) {
            return ret;
        }
        total += inertia_output;
    }
    if (total != 6 || !zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("Zero-decay low-speed output: total=%d active=%d expected 6/1", total,
                zmk_input_processor_runtime_test_inertia_active(dev));
        return -EINVAL;
    }

    /* At low speed, decay must multiply the fractional retained speed each
     * interval. Integer-count decay emits on tick 7 instead of tick 8. */
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 5, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = process_test_relative_x(dev, 3, &output)) < 0) {
        return ret;
    }
    static const int16_t exponential_tail[] = {0, 0, 0, 1, 0, 0, 0, 1};
    for (size_t i = 0; i < ARRAY_SIZE(exponential_tail); i++) {
        ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output);
        if (ret < 0 || inertia_output != exponential_tail[i]) {
            LOG_ERR("Exponential tail tick %u: got %d expected %d", (unsigned int)i,
                    inertia_output, exponential_tail[i]);
            return ret < 0 ? ret : -EINVAL;
        }
    }

    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 100, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = process_test_relative_x(dev, 3, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        !zmk_input_processor_runtime_test_inertia_active(dev) ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("100%% decay did not stop after one further interval");
        return ret < 0 ? ret : -EINVAL;
    }
    if (last_inertia_stop_reason != ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_SETTLED) {
        LOG_ERR("Inertia settled stop reason missing");
        return -EINVAL;
    }

    /* A sub-count tail must end as soon as its entire future contribution,
     * including the carried fraction, cannot make another scroll count. */
    ret = zmk_input_processor_runtime_set_inertia_window(
        dev, 200, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = zmk_input_processor_runtime_set_inertia_interval(
                          dev, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_decay(
             dev, 50, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_threshold(
             dev, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = process_test_relative_x(dev, 1, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 0 || !zmk_input_processor_runtime_test_inertia_active(dev) ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 0 || zmk_input_processor_runtime_test_inertia_active(dev) ||
        last_inertia_stop_reason != ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_SETTLED) {
        LOG_ERR("Sub-count inertia tail remained active");
        return ret < 0 ? ret : -EINVAL;
    }

    /* Entry uses actual scaled reports; the synthetic tail uses raw speed
     * and carries the 1/60 scale remainder across output intervals. */
    ret = zmk_input_processor_runtime_set_scaling(
        dev, 1, 60, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = zmk_input_processor_runtime_set_inertia_decay(
                          dev, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_threshold(
             dev, 2, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0) {
        return ret;
    }
    int16_t physical_remainder = 0;
    if ((ret = process_test_relative_x_with_remainder(dev, 60, &physical_remainder, &output)) < 0 ||
        output != 1 || zmk_input_processor_runtime_test_inertia_active(dev) ||
        (ret = process_test_relative_x_with_remainder(dev, 59, &physical_remainder, &output)) < 0 ||
        output != 0 || zmk_input_processor_runtime_test_inertia_active(dev) ||
        (ret = process_test_relative_x_with_remainder(dev, 1, &physical_remainder, &output)) < 0 ||
        output != 1 || !zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("Scaled inertia threshold did not follow physical scroll output");
        return ret < 0 ? ret : -EINVAL;
    }
    total = 0;
    for (int i = 0; i < 10; i++) {
        ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output);
        if (ret < 0) {
            return ret;
        }
        total += inertia_output;
    }
    if (total != 2 || inertia_output != 1 ||
        !zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("Raw inertia output lost its 1/60 scaling: total=%d last=%d", total,
                inertia_output);
        return -EINVAL;
    }
    /* The next raw report scales to zero, but must still raise the speed
     * used by the active inertia tail. */
    if ((ret = process_test_relative_x_with_remainder(dev, 59, &physical_remainder, &output)) < 0 ||
        output != 0) {
        return ret < 0 ? ret : -EINVAL;
    }
    total = 0;
    for (int i = 0; i < 20; i++) {
        ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output);
        if (ret < 0) {
            return ret;
        }
        total += inertia_output;
    }
    if (total != 5) {
        LOG_ERR("Zero-scale physical input was lost to inertia: total=%d", total);
        return -EINVAL;
    }
    ret = zmk_input_processor_runtime_set_scaling(
        dev, 1, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || zmk_input_processor_runtime_test_inertia_active(dev) ||
        last_inertia_stop_reason != ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_SETTINGS_CHANGED) {
        return ret < 0 ? ret : -EINVAL;
    }

    /* Turning off the only targeted layer must stop its active inertia before
     * another synthetic interval can emit scroll. */
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0 || (ret = zmk_keymap_layer_activate(1, false)) < 0 ||
        (ret = zmk_input_processor_runtime_set_active_layers(
             dev, BIT(1), ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = process_test_relative_x(dev, 3, &output)) < 0 ||
        !zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("Failed to start inertia on targeted layer");
        return ret < 0 ? ret : -EINVAL;
    }
    ret = zmk_keymap_layer_deactivate(1, false);
    if (ret < 0 || zmk_input_processor_runtime_test_inertia_active(dev) ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 0 || zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("Inertia continued after targeted layer deactivation");
        return ret < 0 ? ret : -EINVAL;
    }
    if (last_inertia_stop_reason != ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_LAYER_INACTIVE) {
        LOG_ERR("Inertia layer stop reason missing");
        return -EINVAL;
    }
    if ((ret = process_test_relative_x(dev, 10, &output)) < 0 ||
        zmk_input_processor_runtime_test_inertia_active(dev)) {
        LOG_ERR("Inertia restarted while targeted layer was inactive");
        return ret < 0 ? ret : -EINVAL;
    }

    /* Fast output starts only after the second scaled input threshold. */
    uint32_t fast_transitions_before = inertia_fast_input_transitions;
    if ((ret = zmk_input_processor_runtime_set_active_layers(
             dev, 0, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_window(
             dev, 100, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_interval(
             dev, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_threshold(
             dev, 10, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_fast_threshold(
             dev, 20, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_fast_output_percent(
             dev, 200, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        zmk_input_processor_runtime_set_inertia_fast_output_percent(
            dev, 99, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY) != -EINVAL ||
        (ret = process_test_relative_x(dev, 11, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 2 || last_inertia_fast_input ||
        inertia_fast_input_transitions != fast_transitions_before + 1 ||
        (ret = process_test_relative_x(dev, 10, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 8 || !last_inertia_fast_input ||
        inertia_fast_input_transitions != fast_transitions_before + 2) {
        LOG_ERR("Fast inertia stage failed: output=%d", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }
    if ((ret = zmk_input_processor_runtime_set_scaling(
             dev, 2, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_fast_threshold(
             dev, 30, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY)) < 0 ||
        (ret = process_test_relative_x(dev, 6, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 2 ||
        (ret = process_test_relative_x(dev, 9, &output)) < 0 ||
        (ret = zmk_input_processor_runtime_test_inertia_tick(dev, &inertia_output)) < 0 ||
        inertia_output != 12) {
        LOG_ERR("Scaled fast threshold failed: output=%d", inertia_output);
        return ret < 0 ? ret : -EINVAL;
    }

    /* The first fast notification also covers a start that already exceeds the fast threshold. */
    ret = zmk_input_processor_runtime_set_inertia_fast_threshold(
        dev, 10, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    fast_transitions_before = inertia_fast_input_transitions;
    if ((ret = process_test_relative_x(dev, 11, &output)) < 0 ||
        !last_inertia_fast_input ||
        inertia_fast_input_transitions != fast_transitions_before + 1) {
        LOG_ERR("Fast input notification missing at inertia start");
        return ret < 0 ? ret : -EINVAL;
    }

    zmk_input_processor_runtime_restore_persistent(dev);
    zmk_input_processor_runtime_set_inertia_notifications(dev, false);
    LOG_INF("PASS: rip_inertia_decay_reverse_scroll");
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

    // The tuned inertia defaults must survive unrelated writes.
    if (cfg.inertia_window_ms != 200 || cfg.inertia_interval_ms != 20 ||
        cfg.inertia_threshold != 12 || cfg.inertia_decay_percent != 8 ||
        !cfg.inertia_enabled || cfg.inertia_fast_threshold != 0 ||
        cfg.inertia_fast_output_percent != 200 ||
        cfg.inertia_notifications_enabled) {
        LOG_ERR("Unexpected inertia defaults: window=%u interval=%u threshold=%u decay=%u",
                cfg.inertia_window_ms, cfg.inertia_interval_ms, cfg.inertia_threshold,
                cfg.inertia_decay_percent);
        return -EINVAL;
    }
    ret = zmk_input_processor_runtime_set_inertia_window(
        dev, 250, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_interval(
        dev, 25, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_threshold(
        dev, 23, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 7, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    if ((ret = zmk_input_processor_runtime_set_inertia_fast_threshold(
             dev, 48, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_fast_output_percent(
             dev, 250, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST)) < 0) {
        return ret;
    }
    zmk_input_processor_runtime_test_flush_save(dev);

    ret = zmk_input_processor_runtime_set_inertia_window(
        dev, 50, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_interval(
        dev, 5, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_threshold(
        dev, 7, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_decay(
        dev, 1, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    if ((ret = zmk_input_processor_runtime_set_inertia_fast_threshold(
             dev, 60, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)) < 0 ||
        (ret = zmk_input_processor_runtime_set_inertia_fast_output_percent(
             dev, 300, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY)) < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_set_inertia_notifications(dev, true);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_input_processor_runtime_discard_all();
    if (ret < 0) {
        return ret;
    }
    zmk_input_processor_runtime_get_config(dev, NULL, &cfg);
    if (cfg.inertia_window_ms != 250 || cfg.inertia_interval_ms != 25 ||
        cfg.inertia_threshold != 23 || cfg.inertia_decay_percent != 7 ||
        cfg.inertia_fast_threshold != 48 || cfg.inertia_fast_output_percent != 250 ||
        !cfg.inertia_notifications_enabled) {
        LOG_ERR("Inertia discard did not restore flash value: window=%u interval=%u "
                "threshold=%u decay=%u",
                cfg.inertia_window_ms, cfg.inertia_interval_ms, cfg.inertia_threshold,
                cfg.inertia_decay_percent);
        return -EINVAL;
    }

    ret = zmk_input_processor_runtime_test_apply_legacy_v1(dev);
    if (ret < 0) {
        return ret;
    }
    zmk_input_processor_runtime_get_config(dev, NULL, &cfg);
    if (cfg.rotation_degrees != 42 || cfg.inertia_window_ms != 250 ||
        cfg.inertia_interval_ms != 25 || cfg.inertia_threshold != 23 ||
        cfg.inertia_decay_percent != 7 || cfg.inertia_fast_threshold != 48 ||
        cfg.inertia_fast_output_percent != 250) {
        LOG_ERR("Legacy v1 settings were not migrated");
        return -EINVAL;
    }
    ret = zmk_input_processor_runtime_set_inertia_enabled(
        dev, false, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }
    zmk_input_processor_runtime_test_flush_save(dev);
    ret = zmk_input_processor_runtime_set_inertia_enabled(
        dev, true, ZMK_INPUT_PROCESSOR_RUNTIME_WRITE_MODE_MEMORY);
    if (ret < 0 || (ret = zmk_input_processor_runtime_discard_all()) < 0) {
        return ret;
    }
    zmk_input_processor_runtime_get_config(dev, NULL, &cfg);
    if (cfg.inertia_enabled || cfg.inertia_threshold != 23) {
        LOG_ERR("Inertia enabled flag did not persist independently of threshold");
        return -EINVAL;
    }
    LOG_INF("PASS: rip_settings_write_modes rot=%d legacy_v1=ok", cfg.rotation_degrees);
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

    ret = test_inertia();
    if (ret < 0) {
        LOG_ERR("FAIL: rip_inertia_decay_reverse_scroll ret=%d", ret);
    }

    ret = test_write_modes_and_save_discard();
    if (ret < 0) {
        LOG_ERR("FAIL: rip_settings_write_modes ret=%d", ret);
    }
    return 0;
}
SYS_INIT(rip_settings_test_init, APPLICATION, 99);
