/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_input_processor_temp_config

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_input_processor_temp_config_config {
    const char *processor_name;
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
    int32_t rotation_degrees;
};

struct behavior_input_processor_temp_config_data {
    const struct device *processor;
    bool is_active;
};

static int behavior_input_processor_temp_config_init(const struct device *dev) {
    struct behavior_input_processor_temp_config_data *data = dev->data;
    const struct behavior_input_processor_temp_config_config *cfg = dev->config;

    // Find the processor by name
    data->processor = zmk_input_processor_runtime_find_by_name(cfg->processor_name);
    if (!data->processor) {
        LOG_ERR("Input processor '%s' not found", cfg->processor_name);
        return -ENODEV;
    }

    data->is_active = false;
    LOG_DBG("Temporary config behavior initialized for processor: %s", cfg->processor_name);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_temp_config_data *data = dev->data;
    const struct behavior_input_processor_temp_config_config *cfg = dev->config;

    if (!data->processor) {
        return -ENODEV;
    }

    // Apply temporary configuration (non-persistent)
    int ret = 0;
    if (cfg->scale_multiplier > 0 && cfg->scale_divisor > 0) {
        ret = zmk_input_processor_runtime_set_scaling(data->processor, 
                                                       cfg->scale_multiplier,
                                                       cfg->scale_divisor,
                                                       false);  // temporary
        if (ret < 0) {
            LOG_ERR("Failed to set temporary scaling: %d", ret);
            return ret;
        }
    }

    // Update rotation if provided and valid (non-persistent)
    if (cfg->rotation_degrees >= -360 && cfg->rotation_degrees <= 360) {
        ret = zmk_input_processor_runtime_set_rotation(data->processor, 
                                                        cfg->rotation_degrees,
                                                        false);  // temporary
        if (ret < 0) {
            LOG_ERR("Failed to set temporary rotation: %d", ret);
            return ret;
        }
    }

    data->is_active = true;
    LOG_INF("Applied temporary config to %s: scale=%d/%d, rotation=%d",
            cfg->processor_name, cfg->scale_multiplier, cfg->scale_divisor, 
            cfg->rotation_degrees);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_temp_config_data *data = dev->data;
    const struct behavior_input_processor_temp_config_config *cfg = dev->config;

    if (!data->processor || !data->is_active) {
        return 0;
    }

    // Restore persistent configuration
    zmk_input_processor_runtime_restore_persistent(data->processor);

    data->is_active = false;
    LOG_INF("Restored persistent config for %s", cfg->processor_name);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_input_processor_temp_config_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define TEMP_CONFIG_INST(n)                                                                        \
    static struct behavior_input_processor_temp_config_data                                        \
        behavior_input_processor_temp_config_data_##n;                                             \
    static const struct behavior_input_processor_temp_config_config                                \
        behavior_input_processor_temp_config_config_##n = {                                        \
            .processor_name = DT_INST_PROP(n, processor_name),                                     \
            .scale_multiplier = DT_INST_PROP_OR(n, scale_multiplier, 0),                           \
            .scale_divisor = DT_INST_PROP_OR(n, scale_divisor, 0),                                 \
            .rotation_degrees = DT_INST_PROP_OR(n, rotation_degrees, 0),                           \
        };                                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_input_processor_temp_config_init, NULL,                    \
                            &behavior_input_processor_temp_config_data_##n,                        \
                            &behavior_input_processor_temp_config_config_##n, POST_KERNEL,         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_input_processor_temp_config_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TEMP_CONFIG_INST)
