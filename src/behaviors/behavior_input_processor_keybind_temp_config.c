/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_input_processor_keybind_temp_config

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_processor_keybind.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_keybind_temp_config_config {
    const char *processor_name;
    int32_t tick;
    int32_t degree_offset;
    uint32_t wait_ms;
    uint32_t tap_ms;
};

struct behavior_keybind_temp_config_data {
    const struct device *processor;
    bool is_active;
    // Saved values for restoration
    int32_t saved_tick;
    int32_t saved_degree_offset;
    uint32_t saved_wait_ms;
    uint32_t saved_tap_ms;
};

static int behavior_keybind_temp_config_init(const struct device *dev) {
    struct behavior_keybind_temp_config_data *data = dev->data;
    const struct behavior_keybind_temp_config_config *cfg = dev->config;

    // Find the processor by name
    data->processor = zmk_input_processor_keybind_find_by_name(cfg->processor_name);
    if (!data->processor) {
        LOG_ERR("Keybind processor '%s' not found", cfg->processor_name);
        return -ENODEV;
    }

    data->is_active = false;
    LOG_DBG("Temporary keybind config behavior initialized for processor: %s", cfg->processor_name);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_keybind_temp_config_data *data = dev->data;
    const struct behavior_keybind_temp_config_config *cfg = dev->config;

    if (!data->processor) {
        return -ENODEV;
    }

    // Save current configuration
    struct zmk_input_processor_keybind_config current_config;
    int ret = zmk_input_processor_keybind_get_config(data->processor, NULL, &current_config);
    if (ret < 0) {
        LOG_ERR("Failed to get current keybind config: %d", ret);
        return ret;
    }

    data->saved_tick = current_config.tick;
    data->saved_degree_offset = current_config.degree_offset;
    data->saved_wait_ms = current_config.wait_ms;
    data->saved_tap_ms = current_config.tap_ms;

    // Apply temporary configuration
    if (cfg->tick > 0) {
        ret = zmk_input_processor_keybind_set_tick(data->processor, cfg->tick);
        if (ret < 0) {
            LOG_ERR("Failed to set temporary tick: %d", ret);
            return ret;
        }
    }

    if (cfg->degree_offset >= 0) {
        ret = zmk_input_processor_keybind_set_degree_offset(data->processor, cfg->degree_offset);
        if (ret < 0) {
            LOG_ERR("Failed to set temporary degree offset: %d", ret);
            return ret;
        }
    }

    if (cfg->wait_ms > 0) {
        ret = zmk_input_processor_keybind_set_wait_ms(data->processor, cfg->wait_ms);
        if (ret < 0) {
            LOG_ERR("Failed to set temporary wait_ms: %d", ret);
            return ret;
        }
    }

    if (cfg->tap_ms > 0) {
        ret = zmk_input_processor_keybind_set_tap_ms(data->processor, cfg->tap_ms);
        if (ret < 0) {
            LOG_ERR("Failed to set temporary tap_ms: %d", ret);
            return ret;
        }
    }

    data->is_active = true;
    LOG_INF("Applied temporary keybind config to %s: tick=%d, offset=%d°, wait=%dms, tap=%dms",
            cfg->processor_name, cfg->tick, cfg->degree_offset, cfg->wait_ms, cfg->tap_ms);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_keybind_temp_config_data *data = dev->data;
    const struct behavior_keybind_temp_config_config *cfg = dev->config;

    if (!data->processor || !data->is_active) {
        return 0;
    }

    // Restore saved configuration
    zmk_input_processor_keybind_set_tick(data->processor, data->saved_tick);
    zmk_input_processor_keybind_set_degree_offset(data->processor, data->saved_degree_offset);
    zmk_input_processor_keybind_set_wait_ms(data->processor, data->saved_wait_ms);
    zmk_input_processor_keybind_set_tap_ms(data->processor, data->saved_tap_ms);

    data->is_active = false;
    LOG_INF("Restored keybind config for %s", cfg->processor_name);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_keybind_temp_config_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define KEYBIND_TEMP_CONFIG_INST(n)                                                                \
    static struct behavior_keybind_temp_config_data behavior_keybind_temp_config_data_##n;         \
    static const struct behavior_keybind_temp_config_config                                        \
        behavior_keybind_temp_config_config_##n = {                                                \
            .processor_name = DT_INST_PROP(n, processor_name),                                     \
            .tick = DT_INST_PROP_OR(n, tick, -1),                                                  \
            .degree_offset = DT_INST_PROP_OR(n, degree_offset, -1),                                \
            .wait_ms = DT_INST_PROP_OR(n, wait_ms, 0),                                             \
            .tap_ms = DT_INST_PROP_OR(n, tap_ms, 0),                                               \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(                                                                       \
        n, behavior_keybind_temp_config_init, NULL, &behavior_keybind_temp_config_data_##n,        \
        &behavior_keybind_temp_config_config_##n, POST_KERNEL,                                     \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_keybind_temp_config_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KEYBIND_TEMP_CONFIG_INST)
