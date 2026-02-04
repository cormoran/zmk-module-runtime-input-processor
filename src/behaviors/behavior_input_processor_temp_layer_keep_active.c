/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_input_processor_temp_layer_keep_active

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_input_processor_temp_layer_keep_active_config {
    const char *processor_name;
};

struct behavior_input_processor_temp_layer_keep_active_data {
    const struct device *processor;
    bool is_active;
};

static int behavior_input_processor_temp_layer_keep_active_init(const struct device *dev) {
    struct behavior_input_processor_temp_layer_keep_active_data *data = dev->data;
    const struct behavior_input_processor_temp_layer_keep_active_config *cfg = dev->config;

    // Find the processor by name
    data->processor = zmk_input_processor_runtime_find_by_name(cfg->processor_name);
    if (!data->processor) {
        LOG_ERR("Input processor '%s' not found", cfg->processor_name);
        return -ENODEV;
    }

    data->is_active = false;
    LOG_DBG("Temp-layer keep-active behavior initialized for processor: %s", cfg->processor_name);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_temp_layer_keep_active_data *data = dev->data;
    const struct behavior_input_processor_temp_layer_keep_active_config *cfg = dev->config;

    if (!data->processor) {
        return -ENODEV;
    }

    // Set keep_active flag to prevent temp-layer layer from deactivating
    zmk_input_processor_runtime_temp_layer_keep_active(data->processor, true);
    data->is_active = true;
    
    LOG_INF("Temp-layer keep-active enabled for %s", cfg->processor_name);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_temp_layer_keep_active_data *data = dev->data;
    const struct behavior_input_processor_temp_layer_keep_active_config *cfg = dev->config;

    if (!data->processor || !data->is_active) {
        return 0;
    }

    // Release keep_active flag to allow temp-layer layer to deactivate
    zmk_input_processor_runtime_temp_layer_keep_active(data->processor, false);
    data->is_active = false;
    
    LOG_INF("Temp-layer keep-active disabled for %s", cfg->processor_name);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_input_processor_temp_layer_keep_active_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define AUTO_MOUSE_KEEP_ACTIVE_INST(n)                                                             \
    static struct behavior_input_processor_temp_layer_keep_active_data                             \
        behavior_input_processor_temp_layer_keep_active_data_##n;                                  \
    static const struct behavior_input_processor_temp_layer_keep_active_config                     \
        behavior_input_processor_temp_layer_keep_active_config_##n = {                             \
            .processor_name = DT_INST_PROP(n, processor_name),                                     \
        };                                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_input_processor_temp_layer_keep_active_init, NULL,         \
                            &behavior_input_processor_temp_layer_keep_active_data_##n,             \
                            &behavior_input_processor_temp_layer_keep_active_config_##n,           \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_input_processor_temp_layer_keep_active_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AUTO_MOUSE_KEEP_ACTIVE_INST)
