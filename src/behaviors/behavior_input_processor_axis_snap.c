/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_input_processor_axis_snap

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_input_processor_axis_snap_config {
    const char *processor_name;
    uint16_t timeout_ms;
};

struct behavior_input_processor_axis_snap_data {
    const struct device *processor;
    bool is_active;
};

static int behavior_input_processor_axis_snap_init(const struct device *dev) {
    struct behavior_input_processor_axis_snap_data *data = dev->data;
    const struct behavior_input_processor_axis_snap_config *cfg = dev->config;

    // Find the processor by name
    data->processor = zmk_input_processor_runtime_find_by_name(cfg->processor_name);
    if (!data->processor) {
        LOG_ERR("Input processor '%s' not found", cfg->processor_name);
        return -ENODEV;
    }

    data->is_active = false;
    LOG_DBG("Axis snap behavior initialized for processor: %s", cfg->processor_name);
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_axis_snap_data *data = dev->data;
    const struct behavior_input_processor_axis_snap_config *cfg = dev->config;

    if (!data->processor) {
        return -ENODEV;
    }

    // Get snap parameters from binding (param1 = mode, param2 = threshold)
    uint8_t snap_mode = binding->param1;
    uint16_t threshold = binding->param2;
    // Get timeout from DTS config
    uint16_t timeout_ms = cfg->timeout_ms;

    // Apply temporary axis snap configuration (non-persistent)
    int ret = zmk_input_processor_runtime_set_axis_snap(data->processor,
                                                         snap_mode,
                                                         threshold,
                                                         timeout_ms,
                                                         false);  // temporary
    if (ret < 0) {
        LOG_ERR("Failed to set temporary axis snap: %d", ret);
        return ret;
    }

    data->is_active = true;
    LOG_INF("Applied temporary axis snap to %s: mode=%d, threshold=%d, timeout=%d",
            cfg->processor_name, snap_mode, threshold, timeout_ms);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_input_processor_axis_snap_data *data = dev->data;
    const struct behavior_input_processor_axis_snap_config *cfg = dev->config;

    if (!data->processor || !data->is_active) {
        return 0;
    }

    // Restore persistent configuration
    zmk_input_processor_runtime_restore_persistent(data->processor);

    data->is_active = false;
    LOG_INF("Restored persistent config for %s", cfg->processor_name);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_input_processor_axis_snap_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define AXIS_SNAP_INST(n)                                                                        \
    static struct behavior_input_processor_axis_snap_data                                        \
        behavior_input_processor_axis_snap_data_##n;                                             \
    static const struct behavior_input_processor_axis_snap_config                                \
        behavior_input_processor_axis_snap_config_##n = {                                        \
            .processor_name = DT_INST_PROP(n, processor_name),                                   \
            .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 1000),                                  \
        };                                                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_input_processor_axis_snap_init, NULL,                    \
                            &behavior_input_processor_axis_snap_data_##n,                        \
                            &behavior_input_processor_axis_snap_config_##n, POST_KERNEL,         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                 \
                            &behavior_input_processor_axis_snap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AXIS_SNAP_INST)
