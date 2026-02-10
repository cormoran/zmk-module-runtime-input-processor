/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_runtime

#include <drivers/input_processor.h>
#include <math.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/input_processor_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_KEYBIND_BEHAVIORS 8

struct runtime_processor_config {
    const char *name;
    uint8_t type;
    size_t x_codes_len;
    size_t y_codes_len;
    const uint16_t *x_codes;
    const uint16_t *y_codes;
    uint32_t initial_scale_multiplier;
    uint32_t initial_scale_divisor;
    int32_t initial_rotation_degrees;
    // Temp-layer behavior references for efficient comparison
    const struct device *temp_layer_transparent_behavior;
    const struct device *temp_layer_kp_behavior;
    size_t temp_layer_keep_keycodes_len;
    const uint16_t *temp_layer_keep_keycodes;
    // Temp-layer default settings from DT
    bool initial_temp_layer_enabled;
    uint8_t initial_temp_layer_layer;
    uint16_t initial_temp_layer_activation_delay_ms;
    uint16_t initial_temp_layer_deactivation_delay_ms;
    // Active layers bitmask from DT
    uint32_t initial_active_layers;
    // Axis snap default settings from DT
    uint8_t initial_axis_snap_mode;
    uint16_t initial_axis_snap_threshold;
    uint16_t initial_axis_snap_timeout_ms;
    // Code mapping default settings from DT
    bool initial_xy_to_scroll_enabled;
    bool initial_xy_swap_enabled;
    // Axis reverse default settings from DT
    bool initial_x_invert;
    bool initial_y_invert;
    // Keybind default settings from DT
    const char **keybind_behaviors;
    size_t keybind_behaviors_len;
    bool initial_keybind_enabled;
    uint8_t initial_keybind_behavior_count;
    uint16_t initial_keybind_degree_offset;
    uint16_t initial_keybind_tick;
};

struct runtime_processor_data {
    const struct device *dev;
#if IS_ENABLED(CONFIG_SETTINGS)
    struct k_work_delayable save_work;
#endif
    // Current active values (may be temporary from behavior)
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
    int32_t rotation_degrees;

    // Persistent values (saved to settings, not affected by behavior)
    uint32_t persistent_scale_multiplier;
    uint32_t persistent_scale_divisor;
    int32_t persistent_rotation_degrees;

    // Precomputed rotation values
    int32_t cos_val; // cos * 1000
    int32_t sin_val; // sin * 1000

    // Last seen X/Y values for rotation
    int16_t last_x;
    int16_t last_y;
    bool has_x;
    bool has_y;

    // Temp-layer layer settings
    bool temp_layer_enabled;
    uint8_t temp_layer_layer;
    uint16_t temp_layer_activation_delay_ms;
    uint16_t temp_layer_deactivation_delay_ms;

    // Persistent temp-layer settings
    bool persistent_temp_layer_enabled;
    uint8_t persistent_temp_layer_layer;
    uint16_t persistent_temp_layer_activation_delay_ms;
    uint16_t persistent_temp_layer_deactivation_delay_ms;

    // Active layers bitmask (0 = all layers)
    uint32_t active_layers;
    uint32_t persistent_active_layers;

    // Axis snap settings
    uint8_t axis_snap_mode;
    uint16_t axis_snap_threshold;
    uint16_t axis_snap_timeout_ms;

    // Persistent axis snap settings
    uint8_t persistent_axis_snap_mode;
    uint16_t persistent_axis_snap_threshold;
    uint16_t persistent_axis_snap_timeout_ms;

    // Axis snap runtime state
    int16_t axis_snap_cross_axis_accum;     // Accumulated movement on cross axis
    int64_t axis_snap_last_decay_timestamp; // Last time accumulator was decayed

    // Code mapping settings
    bool xy_to_scroll_enabled;
    bool xy_swap_enabled;

    // Persistent code mapping settings
    bool persistent_xy_to_scroll_enabled;
    bool persistent_xy_swap_enabled;

    // Axis reverse settings
    bool x_invert;
    bool y_invert;

    // Persistent axis reverse settings
    bool persistent_x_invert;
    bool persistent_y_invert;

    // Keybind settings
    bool keybind_enabled;
    uint8_t keybind_behavior_count;
    uint16_t keybind_degree_offset;
    uint16_t keybind_tick;

    // Persistent keybind settings
    bool persistent_keybind_enabled;
    uint8_t persistent_keybind_behavior_count;
    uint16_t persistent_keybind_degree_offset;
    uint16_t persistent_keybind_tick;

    // Keybind runtime state
    int32_t keybind_x_accum;
    int32_t keybind_y_accum;

    // Temp-layer runtime state
    struct k_work_delayable temp_layer_activation_work;
    struct k_work_delayable temp_layer_deactivation_work;
    bool temp_layer_layer_active;
    bool temp_layer_keep_active; // Set by behavior to prevent deactivation
    int64_t last_input_timestamp;
    int64_t last_keypress_timestamp;
};

static void update_rotation_values(struct runtime_processor_data *data) {
    if (data->rotation_degrees == 0) {
        data->cos_val = 1000;
        data->sin_val = 0;
        return;
    }

    // Convert degrees to radians and compute sin/cos
    double angle_rad = (double)data->rotation_degrees * M_PI / 180.0;
    data->cos_val = (int32_t)(cos(angle_rad) * 1000.0);
    data->sin_val = (int32_t)(sin(angle_rad) * 1000.0);

    LOG_DBG("Rotation %d degrees: cos=%d, sin=%d", data->rotation_degrees, data->cos_val,
            data->sin_val);
}

// Temp-layer layer work handlers
static void temp_layer_activation_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct runtime_processor_data *data =
        CONTAINER_OF(dwork, struct runtime_processor_data, temp_layer_activation_work);

    if (!data->temp_layer_enabled || data->temp_layer_layer_active) {
        return;
    }

    // Activate the temp-layer layer
    int ret = zmk_keymap_layer_activate(data->temp_layer_layer);
    if (ret == 0) {
        data->temp_layer_layer_active = true;
        LOG_INF("Temp-layer layer %d activated", data->temp_layer_layer);
    } else {
        LOG_ERR("Failed to activate temp-layer layer %d: %d", data->temp_layer_layer, ret);
    }
}

static void temp_layer_deactivation_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct runtime_processor_data *data =
        CONTAINER_OF(dwork, struct runtime_processor_data, temp_layer_deactivation_work);

    if (!data->temp_layer_layer_active || data->temp_layer_keep_active) {
        return;
    }

    // Deactivate the temp-layer layer
    int ret = zmk_keymap_layer_deactivate(data->temp_layer_layer);
    if (ret == 0) {
        data->temp_layer_layer_active = false;
        LOG_INF("Temp-layer layer %d deactivated", data->temp_layer_layer);
    } else {
        LOG_ERR("Failed to deactivate temp-layer layer %d: %d", data->temp_layer_layer, ret);
    }
}

static int code_idx(uint16_t code, const uint16_t *list, size_t len) {
    for (int i = 0; i < len; i++) {
        if (list[i] == code) {
            return i;
        }
    }
    return -ENODEV;
}

static bool is_processor_active_for_current_layers(uint32_t active_layers_mask) {
    // If mask is 0, processor is active for all layers
    if (active_layers_mask == 0) {
        return true;
    }

    // Check only the layers that are set in the bitmask
    // This is more efficient than checking all layers
    uint32_t remaining_mask = active_layers_mask;
    int layer_idx = 0;

    while (remaining_mask != 0 && layer_idx < ZMK_KEYMAP_LAYERS_LEN) {
        // Check if this bit is set
        if (remaining_mask & 1) {
            zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);

            if (layer_id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(layer_id)) {
                return true;
            }
        }

        remaining_mask >>= 1;
        layer_idx++;
    }

    return false;
}

// Helper function to trigger keybind behavior
static int trigger_keybind_behavior(const struct device *dev, uint8_t behavior_idx) {
    const struct runtime_processor_config *cfg = dev->config;

    if (behavior_idx >= cfg->keybind_behaviors_len) {
        LOG_ERR("Keybind behavior index %d out of range (max %d)", behavior_idx,
                cfg->keybind_behaviors_len - 1);
        return -EINVAL;
    }

    const char *behavior_name = cfg->keybind_behaviors[behavior_idx];
    if (!behavior_name) {
        LOG_ERR("Keybind behavior at index %d is NULL", behavior_idx);
        return -EINVAL;
    }

    const struct device *behavior_dev = zmk_behavior_get_binding(behavior_name);
    if (!behavior_dev) {
        LOG_ERR("Failed to get behavior device for '%s'", behavior_name);
        return -ENODEV;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_name,
        .param1 = 0,
        .param2 = 0,
    };

    struct zmk_behavior_binding_event binding_event = {
        .layer = zmk_keymap_highest_layer_active(),
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    // Press the behavior
    int ret = zmk_behavior_invoke_binding(&binding, binding_event, true);
    if (ret < 0) {
        LOG_ERR("Failed to invoke behavior press for '%s': %d", behavior_name, ret);
        return ret;
    }

    LOG_INF("Triggered keybind behavior %d (%s)", behavior_idx, behavior_name);

    // Release the behavior immediately
    ret = zmk_behavior_invoke_binding(&binding, binding_event, false);
    if (ret < 0) {
        LOG_ERR("Failed to invoke behavior release for '%s': %d", behavior_name, ret);
    }

    return 0;
}

// Helper function to process keybind logic
// Returns true if event was consumed by keybind, false if should continue processing
static bool process_keybind(const struct device *dev, bool is_x, int16_t value) {
    const struct runtime_processor_config *cfg = dev->config;
    struct runtime_processor_data *data = dev->data;

    if (!data->keybind_enabled || data->keybind_behavior_count == 0 ||
        cfg->keybind_behaviors_len == 0) {
        return false;
    }

    // Clamp behavior count to available behaviors
    uint8_t behavior_count = data->keybind_behavior_count;
    if (behavior_count > cfg->keybind_behaviors_len) {
        behavior_count = cfg->keybind_behaviors_len;
    }
    if (behavior_count > MAX_KEYBIND_BEHAVIORS) {
        behavior_count = MAX_KEYBIND_BEHAVIORS;
    }

    // Accumulate X and Y movement
    if (is_x) {
        data->keybind_x_accum += value;
    } else {
        data->keybind_y_accum += value;
    }

    // Check if accumulated movement exceeds tick threshold
    int32_t total_movement_sq = data->keybind_x_accum * data->keybind_x_accum +
                                data->keybind_y_accum * data->keybind_y_accum;
    int32_t tick_threshold_sq = data->keybind_tick * data->keybind_tick;

    if (total_movement_sq < tick_threshold_sq) {
        // Not enough movement yet, consume the event
        return true;
    }

    // Calculate angle from accumulated X/Y
    // atan2(y, x) returns angle in radians from -PI to PI
    // We convert to degrees 0-360
    double angle_rad = atan2((double)data->keybind_y_accum, (double)data->keybind_x_accum);
    double angle_deg = angle_rad * 180.0 / M_PI;
    if (angle_deg < 0) {
        angle_deg += 360.0;
    }

    // Apply degree offset
    angle_deg += data->keybind_degree_offset;
    if (angle_deg >= 360.0) {
        angle_deg -= 360.0;
    }

    // Calculate which behavior to trigger based on behavior count
    // Divide 360 degrees by behavior count
    uint8_t behavior_idx = 0;

    if (behavior_count == 1) {
        // Only one behavior, always trigger it
        behavior_idx = 0;
    } else {
        // Divide circle into equal segments
        double segment_size = 360.0 / behavior_count;
        // Offset by half segment to center the first segment at 0 degrees
        double adjusted_angle = angle_deg + (segment_size / 2.0);
        if (adjusted_angle >= 360.0) {
            adjusted_angle -= 360.0;
        }
        behavior_idx = (uint8_t)(adjusted_angle / segment_size) % behavior_count;
    }

    LOG_DBG("Keybind: accum=(%d,%d) angle=%.1f deg, offset=%d, behavior_idx=%d/%d",
            data->keybind_x_accum, data->keybind_y_accum, angle_deg - data->keybind_degree_offset,
            data->keybind_degree_offset, behavior_idx, behavior_count);

    // Trigger the behavior
    trigger_keybind_behavior(dev, behavior_idx);

    // Reset accumulation
    data->keybind_x_accum = 0;
    data->keybind_y_accum = 0;

    // Consume the event
    return true;
}

static int scale_val(struct input_event *event, uint32_t mul, uint32_t div,
                     struct zmk_input_processor_state *state) {
    if (mul == 0 || div == 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int16_t value_mul = event->value * (int16_t)mul;

    if (state && state->remainder) {
        value_mul += *state->remainder;
    }

    int16_t scaled = value_mul / (int16_t)div;

    if (state && state->remainder) {
        *state->remainder = value_mul - (scaled * (int16_t)div);
    }

    LOG_DBG("scaled %d with %d/%d to %d", event->value, mul, div, scaled);

    event->value = scaled;
    return 0;
}

static int runtime_processor_handle_event(const struct device *dev, struct input_event *event,
                                          uint32_t param1, uint32_t param2,
                                          struct zmk_input_processor_state *state) {
    const struct runtime_processor_config *cfg = dev->config;
    struct runtime_processor_data *data = dev->data;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int x_idx = code_idx(event->code, cfg->x_codes, cfg->x_codes_len);
    int y_idx = code_idx(event->code, cfg->y_codes, cfg->y_codes_len);

    if (x_idx < 0 && y_idx < 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // Check if processor should be active for current layers
    if (!is_processor_active_for_current_layers(data->active_layers)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool is_x = (x_idx >= 0);
    int16_t value = event->value;

    // Process keybind mode (if enabled, this consumes the event)
    if (process_keybind(dev, is_x, value)) {
        // Event was consumed by keybind, stop processing
        return ZMK_INPUT_PROC_STOP;
    }

    // Apply code mapping (XY swap and XY-to-scroll)
    // These mappings are mutually exclusive - XY-to-scroll takes precedence
    if (data->xy_to_scroll_enabled) {
        // Map X/Y to horizontal/vertical scroll
        // X (0x00) -> HWHEEL (0x06), Y (0x01) -> WHEEL (0x08)
        if (is_x) {
            event->code = INPUT_REL_HWHEEL;
            LOG_DBG("XY-to-scroll: mapped X to HWHEEL");
        } else {
            event->code = INPUT_REL_WHEEL;
            LOG_DBG("XY-to-scroll: mapped Y to WHEEL");
        }
    } else if (data->xy_swap_enabled) {
        // Swap X and Y axes
        // X (0x00) -> Y (0x01), Y (0x01) -> X (0x00)
        if (is_x) {
            event->code = INPUT_REL_Y;
            LOG_DBG("XY-swap: swapped X to Y");
        } else {
            event->code = INPUT_REL_X;
            LOG_DBG("XY-swap: swapped Y to X");
        }
    }

    // Handle temp-layer layer activation
    if (data->temp_layer_enabled && event->value != 0) {
        int64_t now = k_uptime_get();
        data->last_input_timestamp = now;

        // Check if we should activate the layer
        if (!data->temp_layer_layer_active) {
            // Only activate if no key press within activation delay window
            if (data->last_keypress_timestamp == 0 ||
                (now - data->last_keypress_timestamp) >= data->temp_layer_activation_delay_ms) {
                // Schedule activation
                k_work_reschedule(&data->temp_layer_activation_work, K_NO_WAIT);
            }
        }
    }

    // Apply rotation
    if (data->rotation_degrees != 0) {
        if (is_x) {
            data->last_x = value;
            data->has_x = true;

            // If we have both X and Y, apply rotation
            if (data->has_y) {
                // X' = X * cos - Y * sin
                // Using 1000 as scaling factor for fixed-point arithmetic
                // (precision: 0.001)
                int32_t rotated_x =
                    (data->last_x * data->cos_val - data->last_y * data->sin_val) / 1000;
                event->value = (int16_t)rotated_x;
                data->has_y = false;
            } else {
                event->value = 0;
            }
        } else {
            data->last_y = value;
            data->has_y = true;

            // If we have both X and Y, apply rotation
            if (data->has_x) {
                // Y' = X * sin + Y * cos
                int32_t rotated_y =
                    (data->last_x * data->sin_val + data->last_y * data->cos_val) / 1000;
                event->value = (int16_t)rotated_y;
                data->has_x = false;
            } else {
                event->value = 0;
            }
        }
    }

    // Apply axis inversion after rotation
    if ((is_x && data->x_invert) || (!is_x && data->y_invert)) {
        event->value = -event->value;
    }
    value = event->value;

    // Apply axis snapping if configured
    if (data->axis_snap_mode != ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_NONE && event->value != 0) {
        int64_t now = k_uptime_get();
        bool is_snapped_axis =
            (data->axis_snap_mode == ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_X && is_x) ||
            (data->axis_snap_mode == ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y && !is_x);
        bool is_cross_axis = !is_snapped_axis;

        // Decay accumulator over time
        if (data->axis_snap_timeout_ms > 0 && data->axis_snap_last_decay_timestamp > 0) {
            int64_t elapsed = now - data->axis_snap_last_decay_timestamp;
            if (elapsed > 0) {
                // Decay rate: threshold per timeout period
                // Decay every 50ms
                int64_t decay_periods = elapsed / 50;
                if (decay_periods > 0) {
                    int16_t decay_per_50ms =
                        data->axis_snap_threshold / (data->axis_snap_timeout_ms / 50);
                    if (decay_per_50ms < 1) {
                        decay_per_50ms = 1; // Minimum decay of 1
                    }

                    int16_t total_decay = decay_per_50ms * decay_periods;

                    // Decay towards zero
                    if (data->axis_snap_cross_axis_accum > 0) {
                        data->axis_snap_cross_axis_accum -= total_decay;
                        if (data->axis_snap_cross_axis_accum < 0) {
                            data->axis_snap_cross_axis_accum = 0;
                        }
                    } else if (data->axis_snap_cross_axis_accum < 0) {
                        data->axis_snap_cross_axis_accum += total_decay;
                        if (data->axis_snap_cross_axis_accum > 0) {
                            data->axis_snap_cross_axis_accum = 0;
                        }
                    }

                    data->axis_snap_last_decay_timestamp = now;
                    LOG_DBG("Axis snap: decayed accum to %d (decay=%d)",
                            data->axis_snap_cross_axis_accum, total_decay);
                }
            }
        }

        if (is_cross_axis) {
            int16_t current_abs_accum = data->axis_snap_cross_axis_accum < 0
                                            ? -data->axis_snap_cross_axis_accum
                                            : data->axis_snap_cross_axis_accum;
            bool is_unsnapped = current_abs_accum >= data->axis_snap_threshold;

            if (is_unsnapped) {
                // Just increase accumulator when already unsnapped
                data->axis_snap_cross_axis_accum = current_abs_accum + (value > 0 ? value : -value);
            } else {
                // Accumulate normally when snapped (no abs)
                data->axis_snap_cross_axis_accum += value;
            }
            // Reset decay timer on movement
            data->axis_snap_last_decay_timestamp = now;

            // Check if threshold exceeded (check absolute value)
            int16_t abs_accum = data->axis_snap_cross_axis_accum < 0
                                    ? -data->axis_snap_cross_axis_accum
                                    : data->axis_snap_cross_axis_accum;

            if (abs_accum >= data->axis_snap_threshold) {
                LOG_DBG("Axis snap: unlocked (threshold=%d exceeded with accum=%d)",
                        data->axis_snap_threshold, data->axis_snap_cross_axis_accum);
                // cap the accumulator to twice the threshold so that it decays
                // under threshold within timeout
                if (abs_accum > data->axis_snap_threshold * 2) {
                    data->axis_snap_cross_axis_accum =
                        (data->axis_snap_cross_axis_accum > 0 ? data->axis_snap_threshold
                                                              : -data->axis_snap_threshold) *
                        2;
                }
            } else {
                // Suppress cross-axis movement while locked
                event->value = 0;
                LOG_DBG("Axis snap: suppressing cross-axis movement (accum=%d, "
                        "threshold=%d)",
                        data->axis_snap_cross_axis_accum, data->axis_snap_threshold);
            }
        }

        // Update value after snap processing
        value = event->value;
    }

    // Apply scaling
    if (data->scale_multiplier > 0 && data->scale_divisor > 0) {
        scale_val(event, data->scale_multiplier, data->scale_divisor, state);
        value = event->value;
    }

    // Schedule deactivation after input stops
    if (data->temp_layer_enabled && data->temp_layer_layer_active &&
        !data->temp_layer_keep_active) {
        k_work_reschedule(&data->temp_layer_deactivation_work,
                          K_MSEC(data->temp_layer_deactivation_delay_ms));
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api runtime_processor_driver_api = {
    .handle_event = runtime_processor_handle_event,
};

#if IS_ENABLED(CONFIG_SETTINGS)
struct processor_settings {
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
    int32_t rotation_degrees;
    bool temp_layer_enabled;
    uint8_t temp_layer_layer;
    uint16_t temp_layer_activation_delay_ms;
    uint16_t temp_layer_deactivation_delay_ms;
    uint32_t active_layers;
    uint8_t axis_snap_mode;
    uint16_t axis_snap_threshold;
    uint16_t axis_snap_timeout_ms;
    bool xy_to_scroll_enabled;
    bool xy_swap_enabled;
    bool x_invert;
    bool y_invert;
    bool keybind_enabled;
    uint8_t keybind_behavior_count;
    uint16_t keybind_degree_offset;
    uint16_t keybind_tick;
};

static void save_processor_settings_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct runtime_processor_data *data =
        CONTAINER_OF(dwork, struct runtime_processor_data, save_work);
    const struct device *dev = data->dev;
    const struct runtime_processor_config *cfg = dev->config;

    struct processor_settings settings = {
        .scale_multiplier = data->persistent_scale_multiplier,
        .scale_divisor = data->persistent_scale_divisor,
        .rotation_degrees = data->persistent_rotation_degrees,
        .temp_layer_enabled = data->persistent_temp_layer_enabled,
        .temp_layer_layer = data->persistent_temp_layer_layer,
        .temp_layer_activation_delay_ms = data->persistent_temp_layer_activation_delay_ms,
        .temp_layer_deactivation_delay_ms = data->persistent_temp_layer_deactivation_delay_ms,
        .active_layers = data->persistent_active_layers,
        .axis_snap_mode = data->persistent_axis_snap_mode,
        .axis_snap_threshold = data->persistent_axis_snap_threshold,
        .axis_snap_timeout_ms = data->persistent_axis_snap_timeout_ms,
        .xy_to_scroll_enabled = data->persistent_xy_to_scroll_enabled,
        .xy_swap_enabled = data->persistent_xy_swap_enabled,
        .x_invert = data->persistent_x_invert,
        .y_invert = data->persistent_y_invert,
        .keybind_enabled = data->persistent_keybind_enabled,
        .keybind_behavior_count = data->persistent_keybind_behavior_count,
        .keybind_degree_offset = data->persistent_keybind_degree_offset,
        .keybind_tick = data->persistent_keybind_tick,
    };

    char path[64];
    snprintf(path, sizeof(path), "input_proc/%s", cfg->name);

    int ret = settings_save_one(path, &settings, sizeof(settings));
    if (ret < 0) {
        LOG_ERR("Failed to save settings for %s: %d", cfg->name, ret);
    } else {
        LOG_INF("Saved settings for %s", cfg->name);
    }
}

static int schedule_save_processor_settings(const struct device *dev) {
    struct runtime_processor_data *data = dev->data;
    return k_work_reschedule(&data->save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
}

static int load_processor_settings_cb(const char *name, size_t len, settings_read_cb read_cb,
                                      void *cb_arg, void *param) {
    const struct device *dev = (const struct device *)param;
    struct runtime_processor_data *data = dev->data;
    const struct runtime_processor_config *cfg = dev->config;

    if (len == sizeof(struct processor_settings)) {
        struct processor_settings settings;
        int rc = read_cb(cb_arg, &settings, sizeof(settings));
        if (rc >= 0) {
            data->persistent_scale_multiplier = settings.scale_multiplier;
            data->persistent_scale_divisor = settings.scale_divisor;
            data->persistent_rotation_degrees = settings.rotation_degrees;
            data->persistent_temp_layer_enabled = settings.temp_layer_enabled;
            data->persistent_temp_layer_layer = settings.temp_layer_layer;
            data->persistent_temp_layer_activation_delay_ms =
                settings.temp_layer_activation_delay_ms;
            data->persistent_temp_layer_deactivation_delay_ms =
                settings.temp_layer_deactivation_delay_ms;
            data->persistent_active_layers = settings.active_layers;
            data->persistent_axis_snap_mode = settings.axis_snap_mode;
            data->persistent_axis_snap_threshold = settings.axis_snap_threshold;
            data->persistent_axis_snap_timeout_ms = settings.axis_snap_timeout_ms;
            data->persistent_xy_to_scroll_enabled = settings.xy_to_scroll_enabled;
            data->persistent_xy_swap_enabled = settings.xy_swap_enabled;
            data->persistent_x_invert = settings.x_invert;
            data->persistent_y_invert = settings.y_invert;
            data->persistent_keybind_enabled = settings.keybind_enabled;
            data->persistent_keybind_behavior_count = settings.keybind_behavior_count;
            data->persistent_keybind_degree_offset = settings.keybind_degree_offset;
            data->persistent_keybind_tick = settings.keybind_tick;

            // Apply to current values
            data->scale_multiplier = settings.scale_multiplier;
            data->scale_divisor = settings.scale_divisor;
            data->rotation_degrees = settings.rotation_degrees;
            data->temp_layer_enabled = settings.temp_layer_enabled;
            data->temp_layer_layer = settings.temp_layer_layer;
            data->temp_layer_activation_delay_ms = settings.temp_layer_activation_delay_ms;
            data->temp_layer_deactivation_delay_ms = settings.temp_layer_deactivation_delay_ms;
            data->active_layers = settings.active_layers;
            data->axis_snap_mode = settings.axis_snap_mode;
            data->axis_snap_threshold = settings.axis_snap_threshold;
            data->axis_snap_timeout_ms = settings.axis_snap_timeout_ms;
            data->xy_to_scroll_enabled = settings.xy_to_scroll_enabled;
            data->xy_swap_enabled = settings.xy_swap_enabled;
            data->x_invert = settings.x_invert;
            data->y_invert = settings.y_invert;
            data->keybind_enabled = settings.keybind_enabled;
            data->keybind_behavior_count = settings.keybind_behavior_count;
            data->keybind_degree_offset = settings.keybind_degree_offset;
            data->keybind_tick = settings.keybind_tick;
            update_rotation_values(data);

            LOG_INF("Loaded settings for %s: scale=%d/%d, rotation=%d, "
                    "temp_layer=%d, active_layers=0x%08x, axis_snap=%d",
                    cfg->name, settings.scale_multiplier, settings.scale_divisor,
                    settings.rotation_degrees, settings.temp_layer_enabled, settings.active_layers,
                    settings.axis_snap_mode);
            return 0;
        }
    }
    return -EINVAL;
}

static int runtime_processor_settings_load_cb(const char *name, size_t len,
                                              settings_read_cb read_cb, void *cb_arg);

SETTINGS_STATIC_HANDLER_DEFINE(input_proc, "input_proc", NULL, runtime_processor_settings_load_cb,
                               NULL, NULL);
#endif

static int runtime_processor_init(const struct device *dev) {
    const struct runtime_processor_config *cfg = dev->config;
    struct runtime_processor_data *data = dev->data;

    // Initialize with default values
    data->scale_multiplier = cfg->initial_scale_multiplier;
    data->scale_divisor = cfg->initial_scale_divisor;
    data->rotation_degrees = cfg->initial_rotation_degrees;

    // Initialize persistent values same as current
    data->persistent_scale_multiplier = cfg->initial_scale_multiplier;
    data->persistent_scale_divisor = cfg->initial_scale_divisor;
    data->persistent_rotation_degrees = cfg->initial_rotation_degrees;

    // Initialize rotation state
    data->has_x = false;
    data->has_y = false;
    data->last_x = 0;
    data->last_y = 0;

    // Initialize temp-layer settings from DT defaults
    data->temp_layer_enabled = cfg->initial_temp_layer_enabled;
    data->temp_layer_layer = cfg->initial_temp_layer_layer;
    data->temp_layer_activation_delay_ms = cfg->initial_temp_layer_activation_delay_ms;
    data->temp_layer_deactivation_delay_ms = cfg->initial_temp_layer_deactivation_delay_ms;
    data->persistent_temp_layer_enabled = cfg->initial_temp_layer_enabled;
    data->persistent_temp_layer_layer = cfg->initial_temp_layer_layer;
    data->persistent_temp_layer_activation_delay_ms = cfg->initial_temp_layer_activation_delay_ms;
    data->persistent_temp_layer_deactivation_delay_ms =
        cfg->initial_temp_layer_deactivation_delay_ms;

    // Initialize temp-layer runtime state
    data->temp_layer_layer_active = false;
    data->temp_layer_keep_active = false;
    data->last_input_timestamp = 0;
    data->last_keypress_timestamp = 0;

    // Initialize active layers from DT defaults
    data->active_layers = cfg->initial_active_layers;
    data->persistent_active_layers = cfg->initial_active_layers;

    // Initialize axis snap settings from DT defaults
    data->axis_snap_mode = cfg->initial_axis_snap_mode;
    data->axis_snap_threshold = cfg->initial_axis_snap_threshold;
    data->axis_snap_timeout_ms = cfg->initial_axis_snap_timeout_ms;
    data->persistent_axis_snap_mode = cfg->initial_axis_snap_mode;
    data->persistent_axis_snap_threshold = cfg->initial_axis_snap_threshold;
    data->persistent_axis_snap_timeout_ms = cfg->initial_axis_snap_timeout_ms;

    // Initialize axis snap runtime state
    data->axis_snap_cross_axis_accum = 0;
    data->axis_snap_last_decay_timestamp = 0;

    // Initialize code mapping settings from DT defaults
    data->xy_to_scroll_enabled = cfg->initial_xy_to_scroll_enabled;
    data->xy_swap_enabled = cfg->initial_xy_swap_enabled;
    data->persistent_xy_to_scroll_enabled = cfg->initial_xy_to_scroll_enabled;
    data->persistent_xy_swap_enabled = cfg->initial_xy_swap_enabled;
    // Initialize axis invert settings from DT defaults
    data->x_invert = cfg->initial_x_invert;
    data->y_invert = cfg->initial_y_invert;
    data->persistent_x_invert = cfg->initial_x_invert;
    data->persistent_y_invert = cfg->initial_y_invert;

    // Initialize keybind settings from DT defaults
    data->keybind_enabled = cfg->initial_keybind_enabled;
    data->keybind_behavior_count = cfg->initial_keybind_behavior_count;
    data->keybind_degree_offset = cfg->initial_keybind_degree_offset;
    data->keybind_tick = cfg->initial_keybind_tick;
    data->persistent_keybind_enabled = cfg->initial_keybind_enabled;
    data->persistent_keybind_behavior_count = cfg->initial_keybind_behavior_count;
    data->persistent_keybind_degree_offset = cfg->initial_keybind_degree_offset;
    data->persistent_keybind_tick = cfg->initial_keybind_tick;

    // Initialize keybind runtime state
    data->keybind_x_accum = 0;
    data->keybind_y_accum = 0;

    update_rotation_values(data);

    data->dev = dev;
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&data->save_work, save_processor_settings_work_handler);
#endif
    // Initialize temp-layer work queues
    k_work_init_delayable(&data->temp_layer_activation_work, temp_layer_activation_work_handler);
    k_work_init_delayable(&data->temp_layer_deactivation_work,
                          temp_layer_deactivation_work_handler);

    LOG_INF("Runtime processor '%s' initialized", cfg->name);

    return 0;
}

// Helper to raise state changed event
static void raise_state_changed_event(const struct device *dev) {
    const char *name;
    struct zmk_input_processor_runtime_config config;

    int ret = zmk_input_processor_runtime_get_config(dev, &name, &config);
    if (ret < 0) {
        return;
    }

    raise_zmk_input_processor_state_changed(
        (struct zmk_input_processor_state_changed){.name = name, .config = config});
}

// Public API for runtime configuration
int zmk_input_processor_runtime_set_scaling(const struct device *dev, uint32_t multiplier,
                                            uint32_t divisor, bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;

    if (multiplier > 0) {
        data->scale_multiplier = multiplier;
        if (persistent) {
            data->persistent_scale_multiplier = multiplier;
        }
    }
    if (divisor > 0) {
        data->scale_divisor = divisor;
        if (persistent) {
            data->persistent_scale_divisor = divisor;
        }
    }

    LOG_INF("Set scaling to %d/%d%s", data->scale_multiplier, data->scale_divisor,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        // Raise event for persistent changes
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_rotation(const struct device *dev, int32_t degrees,
                                             bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->rotation_degrees = degrees;
    if (persistent) {
        data->persistent_rotation_degrees = degrees;
    }
    update_rotation_values(data);

    LOG_INF("Set rotation to %d degrees%s", degrees, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        // Raise event for persistent changes
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_reset(const struct device *dev) {
    if (!dev) {
        return -EINVAL;
    }

    const struct runtime_processor_config *cfg = dev->config;
    struct runtime_processor_data *data = dev->data;

    // Reset to initial values
    data->scale_multiplier = cfg->initial_scale_multiplier;
    data->scale_divisor = cfg->initial_scale_divisor;
    data->rotation_degrees = cfg->initial_rotation_degrees;

    data->persistent_scale_multiplier = cfg->initial_scale_multiplier;
    data->persistent_scale_divisor = cfg->initial_scale_divisor;
    data->persistent_rotation_degrees = cfg->initial_rotation_degrees;

    // Reset temp-layer settings to defaults
    data->temp_layer_enabled = cfg->initial_temp_layer_enabled;
    data->temp_layer_layer = cfg->initial_temp_layer_layer;
    data->temp_layer_activation_delay_ms = cfg->initial_temp_layer_activation_delay_ms;
    data->temp_layer_deactivation_delay_ms = cfg->initial_temp_layer_deactivation_delay_ms;
    data->persistent_temp_layer_enabled = cfg->initial_temp_layer_enabled;
    data->persistent_temp_layer_layer = cfg->initial_temp_layer_layer;
    data->persistent_temp_layer_activation_delay_ms = cfg->initial_temp_layer_activation_delay_ms;
    data->persistent_temp_layer_deactivation_delay_ms =
        cfg->initial_temp_layer_deactivation_delay_ms;

    // Reset active layers to defaults
    data->active_layers = cfg->initial_active_layers;
    data->persistent_active_layers = cfg->initial_active_layers;

    // Deactivate temp-layer layer if active
    if (data->temp_layer_layer_active) {
        zmk_keymap_layer_deactivate(data->temp_layer_layer);
        data->temp_layer_layer_active = false;
    }

    // Reset axis invert settings to defaults
    data->x_invert = cfg->initial_x_invert;
    data->y_invert = cfg->initial_y_invert;
    data->persistent_x_invert = cfg->initial_x_invert;
    data->persistent_y_invert = cfg->initial_y_invert;

    // Reset keybind settings to defaults
    data->keybind_enabled = cfg->initial_keybind_enabled;
    data->keybind_behavior_count = cfg->initial_keybind_behavior_count;
    data->keybind_degree_offset = cfg->initial_keybind_degree_offset;
    data->keybind_tick = cfg->initial_keybind_tick;
    data->persistent_keybind_enabled = cfg->initial_keybind_enabled;
    data->persistent_keybind_behavior_count = cfg->initial_keybind_behavior_count;
    data->persistent_keybind_degree_offset = cfg->initial_keybind_degree_offset;
    data->persistent_keybind_tick = cfg->initial_keybind_tick;
    data->keybind_x_accum = 0;
    data->keybind_y_accum = 0;

    update_rotation_values(data);

    LOG_INF("Reset processor '%s' to defaults", cfg->name);

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    ret = schedule_save_processor_settings(dev);
    // Raise event
    raise_state_changed_event(dev);
#endif

    return ret;
}

void zmk_input_processor_runtime_restore_persistent(const struct device *dev) {
    if (!dev) {
        return;
    }

    struct runtime_processor_data *data = dev->data;

    // Restore persistent values (used after temporary behavior changes)
    data->scale_multiplier = data->persistent_scale_multiplier;
    data->scale_divisor = data->persistent_scale_divisor;
    data->rotation_degrees = data->persistent_rotation_degrees;
    update_rotation_values(data);

    // Restore axis snap settings
    data->axis_snap_mode = data->persistent_axis_snap_mode;
    data->axis_snap_threshold = data->persistent_axis_snap_threshold;
    data->axis_snap_timeout_ms = data->persistent_axis_snap_timeout_ms;
    // Reset snap state when restoring
    data->axis_snap_cross_axis_accum = 0;
    data->axis_snap_last_decay_timestamp = 0;

    // Restore axis invert settings
    data->x_invert = data->persistent_x_invert;
    data->y_invert = data->persistent_y_invert;

    // Restore keybind settings
    data->keybind_enabled = data->persistent_keybind_enabled;
    data->keybind_behavior_count = data->persistent_keybind_behavior_count;
    data->keybind_degree_offset = data->persistent_keybind_degree_offset;
    data->keybind_tick = data->persistent_keybind_tick;
    // Reset keybind state when restoring
    data->keybind_x_accum = 0;
    data->keybind_y_accum = 0;

    LOG_DBG("Restored persistent values");
}

int zmk_input_processor_runtime_get_config(const struct device *dev, const char **name,
                                           struct zmk_input_processor_runtime_config *config) {
    if (!dev) {
        return -EINVAL;
    }

    const struct runtime_processor_config *cfg = dev->config;
    struct runtime_processor_data *data = dev->data;

    if (name) {
        *name = cfg->name;
    }
    if (config) {
        config->scale_multiplier = data->persistent_scale_multiplier;
        config->scale_divisor = data->persistent_scale_divisor;
        config->rotation_degrees = data->persistent_rotation_degrees;
        config->temp_layer_enabled = data->persistent_temp_layer_enabled;
        config->temp_layer_layer = data->persistent_temp_layer_layer;
        config->temp_layer_activation_delay_ms = data->persistent_temp_layer_activation_delay_ms;
        config->temp_layer_deactivation_delay_ms =
            data->persistent_temp_layer_deactivation_delay_ms;
        config->active_layers = data->persistent_active_layers;
        config->axis_snap_mode = data->persistent_axis_snap_mode;
        config->axis_snap_threshold = data->persistent_axis_snap_threshold;
        config->axis_snap_timeout_ms = data->persistent_axis_snap_timeout_ms;
        config->xy_to_scroll_enabled = data->persistent_xy_to_scroll_enabled;
        config->xy_swap_enabled = data->persistent_xy_swap_enabled;
        config->x_invert = data->persistent_x_invert;
        config->y_invert = data->persistent_y_invert;
        config->keybind_enabled = data->persistent_keybind_enabled;
        config->keybind_behavior_count = data->persistent_keybind_behavior_count;
        config->keybind_degree_offset = data->persistent_keybind_degree_offset;
        config->keybind_tick = data->persistent_keybind_tick;
    }

    return 0;
}

// Helper macro to get behavior device name string from phandle array
// DEVICE_DT_NAME returns the device name string (e.g., behavior node names)
// which is used by zmk_behavior_get_binding() to look up the behavior device
#define KEYBIND_BEHAVIOR_NAME(node, idx)                                                           \
    DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node, keybind_behaviors, idx))

// Macro to generate keybind behaviors array based on array length
#define KEYBIND_BEHAVIORS_0(node)
#define KEYBIND_BEHAVIORS_1(node) KEYBIND_BEHAVIOR_NAME(node, 0),
#define KEYBIND_BEHAVIORS_2(node) KEYBIND_BEHAVIORS_1(node) KEYBIND_BEHAVIOR_NAME(node, 1),
#define KEYBIND_BEHAVIORS_3(node) KEYBIND_BEHAVIORS_2(node) KEYBIND_BEHAVIOR_NAME(node, 2),
#define KEYBIND_BEHAVIORS_4(node) KEYBIND_BEHAVIORS_3(node) KEYBIND_BEHAVIOR_NAME(node, 3),
#define KEYBIND_BEHAVIORS_5(node) KEYBIND_BEHAVIORS_4(node) KEYBIND_BEHAVIOR_NAME(node, 4),
#define KEYBIND_BEHAVIORS_6(node) KEYBIND_BEHAVIORS_5(node) KEYBIND_BEHAVIOR_NAME(node, 5),
#define KEYBIND_BEHAVIORS_7(node) KEYBIND_BEHAVIORS_6(node) KEYBIND_BEHAVIOR_NAME(node, 6),
#define KEYBIND_BEHAVIORS_8(node) KEYBIND_BEHAVIORS_7(node) KEYBIND_BEHAVIOR_NAME(node, 7),

#define KEYBIND_BEHAVIORS(node, len) KEYBIND_BEHAVIORS_##len(node)

#define RUNTIME_PROCESSOR_INST(n)                                                                  \
    static const uint16_t runtime_x_codes_##n[] = DT_INST_PROP(n, x_codes);                        \
    static const uint16_t runtime_y_codes_##n[] = DT_INST_PROP(n, y_codes);                        \
    BUILD_ASSERT(ARRAY_SIZE(runtime_x_codes_##n) == ARRAY_SIZE(runtime_y_codes_##n),               \
                 "X and Y codes need to be the same size");                                        \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(n, temp_layer_keep_keycodes),                                \
                (static const uint16_t runtime_temp_layer_keep_keycodes_##n[] =                    \
                     DT_INST_PROP(n, temp_layer_keep_keycodes);),                                  \
                ())                                                                                \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(n, keybind_behaviors),                                       \
                (static const char *runtime_keybind_behaviors_##n[] = {KEYBIND_BEHAVIORS(          \
                     DT_DRV_INST(n), DT_INST_PROP_LEN(n, keybind_behaviors))};),                   \
                ())                                                                                \
    BUILD_ASSERT(sizeof(DT_INST_PROP(n, processor_label)) <=                                       \
                     CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_NAME_MAX_LEN,                              \
                 "processor_label " DT_INST_PROP(                                                  \
                     n, processor_label) " property +1 exceeds maximum "                           \
                                         "length " STRINGIFY(                                      \
                                             CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_NAME_MAX_LEN));    \
    static const struct runtime_processor_config runtime_config_##n = {                            \
        .name = DT_INST_PROP(n, processor_label),                                                  \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                            \
        .x_codes_len = DT_INST_PROP_LEN(n, x_codes),                                               \
        .y_codes_len = DT_INST_PROP_LEN(n, y_codes),                                               \
        .x_codes = runtime_x_codes_##n,                                                            \
        .y_codes = runtime_y_codes_##n,                                                            \
        .initial_scale_multiplier = DT_INST_PROP_OR(n, scale_multiplier, 1),                       \
        .initial_scale_divisor = DT_INST_PROP_OR(n, scale_divisor, 1),                             \
        .initial_rotation_degrees = DT_INST_PROP_OR(n, rotation_degrees, 0),                       \
        .temp_layer_transparent_behavior = COND_CODE_1(                                            \
            DT_INST_NODE_HAS_PROP(n, temp_layer_transparent_behavior),                             \
            (DEVICE_DT_GET(DT_INST_PHANDLE(n, temp_layer_transparent_behavior))), (NULL)),         \
        .temp_layer_kp_behavior =                                                                  \
            COND_CODE_1(DT_INST_NODE_HAS_PROP(n, temp_layer_kp_behavior),                          \
                        (DEVICE_DT_GET(DT_INST_PHANDLE(n, temp_layer_kp_behavior))), (NULL)),      \
        .temp_layer_keep_keycodes_len =                                                            \
            COND_CODE_1(DT_INST_NODE_HAS_PROP(n, temp_layer_keep_keycodes),                        \
                        (DT_INST_PROP_LEN(n, temp_layer_keep_keycodes)), (0)),                     \
        .temp_layer_keep_keycodes =                                                                \
            COND_CODE_1(DT_INST_NODE_HAS_PROP(n, temp_layer_keep_keycodes),                        \
                        (runtime_temp_layer_keep_keycodes_##n), (NULL)),                           \
        .initial_temp_layer_enabled = DT_INST_PROP(n, temp_layer_enabled),                         \
        .initial_temp_layer_layer = DT_INST_PROP_OR(n, temp_layer, 0),                             \
        .initial_temp_layer_activation_delay_ms =                                                  \
            DT_INST_PROP_OR(n, temp_layer_activation_delay_ms, 100),                               \
        .initial_temp_layer_deactivation_delay_ms =                                                \
            DT_INST_PROP_OR(n, temp_layer_deactivation_delay_ms, 500),                             \
        .initial_active_layers = DT_INST_PROP_OR(n, active_layers, 0),                             \
        .initial_axis_snap_mode = DT_INST_PROP_OR(n, axis_snap_mode, 0),                           \
        .initial_axis_snap_threshold = DT_INST_PROP_OR(n, axis_snap_threshold, 100),               \
        .initial_axis_snap_timeout_ms = DT_INST_PROP_OR(n, axis_snap_timeout_ms, 1000),            \
        .initial_xy_to_scroll_enabled = DT_INST_PROP(n, xy_to_scroll_enabled),                     \
        .initial_xy_swap_enabled = DT_INST_PROP(n, xy_swap_enabled),                               \
        .initial_x_invert = DT_INST_PROP(n, x_invert),                                             \
        .initial_y_invert = DT_INST_PROP(n, y_invert),                                             \
        .keybind_behaviors = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, keybind_behaviors),              \
                                         (runtime_keybind_behaviors_##n), (NULL)),                 \
        .keybind_behaviors_len = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, keybind_behaviors),          \
                                             (DT_INST_PROP_LEN(n, keybind_behaviors)), (0)),       \
        .initial_keybind_enabled = DT_INST_PROP(n, keybind_enabled),                               \
        .initial_keybind_behavior_count = DT_INST_PROP_OR(n, keybind_behavior_count, 4),           \
        .initial_keybind_degree_offset = DT_INST_PROP_OR(n, keybind_degree_offset, 0),             \
        .initial_keybind_tick = DT_INST_PROP_OR(n, keybind_tick, 10),                              \
    };                                                                                             \
    static struct runtime_processor_data runtime_data_##n;                                         \
    DEVICE_DT_INST_DEFINE(n, &runtime_processor_init, NULL, &runtime_data_##n,                     \
                          &runtime_config_##n, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,   \
                          &runtime_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RUNTIME_PROCESSOR_INST)

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#define DEVICE_ADDR(idx) DEVICE_DT_GET(DT_DRV_INST(idx)),

static struct device *runtime_processors[] = {DT_INST_FOREACH_STATUS_OKAY(DEVICE_ADDR)};

static const size_t runtime_processors_count = sizeof(runtime_processors) / sizeof(struct device *);

#else

static struct device *runtime_processors[] = {};
static const size_t runtime_processors_count = 0;

#endif

int zmk_input_processor_runtime_foreach(int (*callback)(const struct device *dev, void *user_data),
                                        void *user_data) {
    for (size_t i = 0; i < runtime_processors_count; i++) {
        int ret = callback(runtime_processors[i], user_data);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

const struct device *zmk_input_processor_runtime_find_by_name(const char *name) {
    for (size_t i = 0; i < runtime_processors_count; i++) {
        const struct device *dev = runtime_processors[i];
        const struct runtime_processor_config *cfg = dev->config;
        if (strcmp(cfg->name, name) == 0) {
            return dev;
        }
    }

    return NULL;
}

const struct device *zmk_input_processor_runtime_find_by_id(uint8_t id) {
    if (id < runtime_processors_count) {
        return runtime_processors[id];
    }
    return NULL;
}

int zmk_input_processor_runtime_get_id(const struct device *dev) {
    for (size_t i = 0; i < runtime_processors_count; i++) {
        if (runtime_processors[i] == dev) {
            return i;
        }
    }
    return -1;
}

#if IS_ENABLED(CONFIG_SETTINGS)

static int runtime_processor_settings_load_cb(const char *name, size_t len,
                                              settings_read_cb read_cb, void *cb_arg) {
    for (size_t i = 0; i < runtime_processors_count; i++) {
        const struct device *dev = runtime_processors[i];
        const struct runtime_processor_config *cfg = dev->config;
        if (strcmp(name, cfg->name) == 0) {
            return load_processor_settings_cb(name, len, read_cb, cb_arg, (void *)dev);
        }
    }
    return -ENOENT;
}

#endif

// Event listener for keycode changes (for timestamp tracking)
static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Only handle key presses
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Update last keypress timestamp for all processors
    int64_t now = k_uptime_get();
    for (size_t i = 0; i < runtime_processors_count; i++) {
        const struct device *dev = runtime_processors[i];
        struct runtime_processor_data *data = dev->data;
        data->last_keypress_timestamp = now;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

// Event listener for position changes (for temp-layer deactivation logic)
static int position_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Only handle key presses
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Check temp-layer deactivation for all processors
    for (size_t i = 0; i < runtime_processors_count; i++) {
        const struct device *dev = runtime_processors[i];
        const struct runtime_processor_config *cfg = dev->config;
        struct runtime_processor_data *data = dev->data;

        // Check if temp-layer layer should be deactivated
        if (!data->temp_layer_enabled || !data->temp_layer_layer_active ||
            data->temp_layer_keep_active) {
            continue;
        }

        // Check if the temp-layer layer has a non-transparent binding for this
        // position
        zmk_keymap_layer_id_t temp_layer_layer_id = data->temp_layer_layer;
        const struct zmk_behavior_binding *temp_layer_binding =
            zmk_keymap_get_layer_binding_at_idx(temp_layer_layer_id, ev->position);

        // If temp-layer layer has non-transparent binding, don't deactivate
        // Use device pointer comparison if transparent behavior is configured
        bool is_transparent = false;
        if (temp_layer_binding) {
            if (cfg->temp_layer_transparent_behavior) {
                // Efficient device pointer comparison
                const struct device *binding_dev =
                    zmk_behavior_get_binding(temp_layer_binding->behavior_dev);
                is_transparent = (binding_dev == cfg->temp_layer_transparent_behavior);
            } else {
                // Fallback to string comparison if not configured
                is_transparent = (strcmp(temp_layer_binding->behavior_dev, "trans") == 0 ||
                                  strcmp(temp_layer_binding->behavior_dev, "TRANS") == 0);
            }

            if (!is_transparent) {
                LOG_DBG("Temp-layer layer has non-transparent binding at position "
                        "%d, not deactivating",
                        ev->position);
                continue;
            }
        }

        // Temp-layer binding is transparent, check the resolved binding
        // Find the highest active layer's non-transparent binding
        const struct zmk_behavior_binding *resolved_binding = NULL;

        for (int layer_idx = ZMK_KEYMAP_LAYERS_LEN - 1; layer_idx >= 0; layer_idx--) {
            zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);

            if (layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
                continue;
            }

            if (!zmk_keymap_layer_active(layer_id)) {
                continue;
            }

            const struct zmk_behavior_binding *binding =
                zmk_keymap_get_layer_binding_at_idx(layer_id, ev->position);

            if (binding) {
                bool binding_is_transparent = false;
                if (cfg->temp_layer_transparent_behavior) {
                    const struct device *binding_dev =
                        zmk_behavior_get_binding(binding->behavior_dev);
                    binding_is_transparent = (binding_dev == cfg->temp_layer_transparent_behavior);
                } else {
                    binding_is_transparent = (strcmp(binding->behavior_dev, "trans") == 0 ||
                                              strcmp(binding->behavior_dev, "TRANS") == 0);
                }

                if (!binding_is_transparent) {
                    resolved_binding = binding;
                    break;
                }
            }
        }

        // If resolved binding is &kp with a modifier keycode, don't deactivate
        if (resolved_binding) {
            bool is_kp = false;
            if (cfg->temp_layer_kp_behavior) {
                const struct device *binding_dev =
                    zmk_behavior_get_binding(resolved_binding->behavior_dev);
                is_kp = (binding_dev == cfg->temp_layer_kp_behavior);
            } else {
                is_kp = (strcmp(resolved_binding->behavior_dev, "kp") == 0 ||
                         strcmp(resolved_binding->behavior_dev, "KEY_PRESS") == 0);
            }

            if (is_kp) {
                // The param1 contains the keycode for &kp behavior
                uint32_t keycode_encoded = resolved_binding->param1;
                uint16_t usage_page = ZMK_HID_USAGE_PAGE(keycode_encoded);
                uint16_t usage_id = ZMK_HID_USAGE_ID(keycode_encoded);

                if (!usage_page) {
                    usage_page = HID_USAGE_KEY;
                }

                // Check if it's in the keep-keycodes list if configured
                bool should_keep = false;
                if (cfg->temp_layer_keep_keycodes_len > 0) {
                    for (size_t j = 0; j < cfg->temp_layer_keep_keycodes_len; j++) {
                        if (cfg->temp_layer_keep_keycodes[j] == usage_id) {
                            should_keep = true;
                            break;
                        }
                    }
                } else {
                    // Fallback to is_mod check if keycodes not configured
                    should_keep = is_mod(usage_page, usage_id);
                }

                if (should_keep) {
                    LOG_DBG("Resolved binding is keep keycode, not deactivating "
                            "temp-layer layer");
                    continue;
                }
            }
        }

        // Deactivate the temp-layer layer
        LOG_DBG("Deactivating temp-layer layer %d due to key press at position %d",
                data->temp_layer_layer, ev->position);
        k_work_cancel_delayable(&data->temp_layer_deactivation_work);
        int ret = zmk_keymap_layer_deactivate(data->temp_layer_layer);
        if (ret == 0) {
            data->temp_layer_layer_active = false;
            LOG_INF("Temp-layer layer %d deactivated by key press", data->temp_layer_layer);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(runtime_processor_keycode_listener, keycode_state_changed_listener);
ZMK_SUBSCRIPTION(runtime_processor_keycode_listener, zmk_keycode_state_changed);

ZMK_LISTENER(runtime_processor_position_listener, position_state_changed_listener);
ZMK_SUBSCRIPTION(runtime_processor_position_listener, zmk_position_state_changed);

// Temp-layer layer configuration API
int zmk_input_processor_runtime_set_temp_layer(const struct device *dev, bool enabled,
                                               uint8_t layer, uint32_t activation_delay_ms,
                                               uint32_t deactivation_delay_ms, bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;

    data->temp_layer_enabled = enabled;
    data->temp_layer_layer = layer;
    data->temp_layer_activation_delay_ms = activation_delay_ms;
    data->temp_layer_deactivation_delay_ms = deactivation_delay_ms;

    if (persistent) {
        data->persistent_temp_layer_enabled = enabled;
        data->persistent_temp_layer_layer = layer;
        data->persistent_temp_layer_activation_delay_ms = activation_delay_ms;
        data->persistent_temp_layer_deactivation_delay_ms = deactivation_delay_ms;
    }

    LOG_INF("Temp-layer layer config: enabled=%d, layer=%d, act_delay=%d, "
            "deact_delay=%d%s",
            enabled, layer, activation_delay_ms, deactivation_delay_ms,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        // Raise event for persistent changes
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_temp_layer_enabled(const struct device *dev, bool enabled,
                                                       bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->temp_layer_enabled = enabled;

    if (persistent) {
        data->persistent_temp_layer_enabled = enabled;
    }

    LOG_INF("Temp-layer enabled: %d%s", enabled, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_temp_layer_layer(const struct device *dev, uint8_t layer,
                                                     bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->temp_layer_layer = layer;

    if (persistent) {
        data->persistent_temp_layer_layer = layer;
    }

    LOG_INF("Temp-layer layer: %d%s", layer, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_temp_layer_activation_delay(const struct device *dev,
                                                                uint32_t activation_delay_ms,
                                                                bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->temp_layer_activation_delay_ms = activation_delay_ms;

    if (persistent) {
        data->persistent_temp_layer_activation_delay_ms = activation_delay_ms;
    }

    LOG_INF("Temp-layer activation delay: %dms%s", activation_delay_ms,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_temp_layer_deactivation_delay(const struct device *dev,
                                                                  uint32_t deactivation_delay_ms,
                                                                  bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->temp_layer_deactivation_delay_ms = deactivation_delay_ms;

    if (persistent) {
        data->persistent_temp_layer_deactivation_delay_ms = deactivation_delay_ms;
    }

    LOG_INF("Temp-layer deactivation delay: %dms%s", deactivation_delay_ms,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_active_layers(const struct device *dev, uint32_t layers,
                                                  bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->active_layers = layers;

    if (persistent) {
        data->persistent_active_layers = layers;
    }

    LOG_INF("Active layers: 0x%08x%s", layers, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_axis_snap_mode(const struct device *dev, uint8_t mode,
                                                   bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    if (mode > ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->axis_snap_mode = mode;

    // Reset snap state when mode changes
    data->axis_snap_cross_axis_accum = 0;

    if (persistent) {
        data->persistent_axis_snap_mode = mode;
    }

    LOG_INF("Axis snap mode: %d%s", mode, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_axis_snap_threshold(const struct device *dev,
                                                        uint16_t threshold, bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->axis_snap_threshold = threshold;

    if (persistent) {
        data->persistent_axis_snap_threshold = threshold;
    }

    LOG_INF("Axis snap threshold: %d%s", threshold, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_axis_snap_timeout(const struct device *dev, uint16_t timeout_ms,
                                                      bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->axis_snap_timeout_ms = timeout_ms;

    if (persistent) {
        data->persistent_axis_snap_timeout_ms = timeout_ms;
    }

    LOG_INF("Axis snap timeout: %d ms%s", timeout_ms,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_axis_snap(const struct device *dev, uint8_t mode,
                                              uint16_t threshold, uint16_t timeout_ms,
                                              bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    if (mode > ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->axis_snap_mode = mode;
    data->axis_snap_threshold = threshold;
    data->axis_snap_timeout_ms = timeout_ms;

    // Reset snap state when configuration changes
    data->axis_snap_cross_axis_accum = 0;

    if (persistent) {
        data->persistent_axis_snap_mode = mode;
        data->persistent_axis_snap_threshold = threshold;
        data->persistent_axis_snap_timeout_ms = timeout_ms;
    }

    LOG_INF("Axis snap config: mode=%d, threshold=%d, timeout=%d ms%s", mode, threshold, timeout_ms,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_x_invert(const struct device *dev, bool invert,
                                             bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->x_invert = invert;

    if (persistent) {
        data->persistent_x_invert = invert;
    }

    LOG_INF("X axis invert: %s%s", invert ? "true" : "false",
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_y_invert(const struct device *dev, bool invert,
                                             bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->y_invert = invert;

    if (persistent) {
        data->persistent_y_invert = invert;
    }

    LOG_INF("Y axis invert: %s%s", invert ? "true" : "false",
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_keybind_enabled(const struct device *dev, bool enabled,
                                                    bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->keybind_enabled = enabled;

    if (persistent) {
        data->persistent_keybind_enabled = enabled;
    }

    LOG_INF("Keybind enabled: %s%s", enabled ? "true" : "false",
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_keybind_behavior_count(const struct device *dev, uint8_t count,
                                                           bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    if (count < 1 || count > MAX_KEYBIND_BEHAVIORS) {
        LOG_ERR("Invalid keybind behavior count %d (must be 1-%d)", count, MAX_KEYBIND_BEHAVIORS);
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->keybind_behavior_count = count;

    if (persistent) {
        data->persistent_keybind_behavior_count = count;
    }

    LOG_INF("Keybind behavior count: %d%s", count, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_keybind_degree_offset(const struct device *dev,
                                                          uint16_t degree_offset, bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    if (degree_offset >= 360) {
        LOG_ERR("Invalid keybind degree offset %d (must be 0-359)", degree_offset);
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->keybind_degree_offset = degree_offset;

    if (persistent) {
        data->persistent_keybind_degree_offset = degree_offset;
    }

    LOG_INF("Keybind degree offset: %d%s", degree_offset,
            persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_keybind_tick(const struct device *dev, uint16_t tick,
                                                 bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    if (tick == 0) {
        LOG_ERR("Invalid keybind tick %d (must be > 0)", tick);
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->keybind_tick = tick;

    if (persistent) {
        data->persistent_keybind_tick = tick;
    }

    LOG_INF("Keybind tick: %d%s", tick, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

void zmk_input_processor_runtime_temp_layer_keep_active(const struct device *dev,
                                                        bool keep_active) {
    if (!dev) {
        return;
    }

    struct runtime_processor_data *data = dev->data;
    data->temp_layer_keep_active = keep_active;

    LOG_DBG("Temp-layer keep_active set to %d", keep_active);

    // If releasing keep_active and layer is still active, deactivate
    // immediately
    if (!keep_active && data->temp_layer_enabled && data->temp_layer_layer_active) {
        k_work_reschedule(&data->temp_layer_deactivation_work, K_NO_WAIT);
    }
}

int zmk_input_processor_runtime_set_xy_to_scroll_enabled(const struct device *dev, bool enabled,
                                                         bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->xy_to_scroll_enabled = enabled;

    if (persistent) {
        data->persistent_xy_to_scroll_enabled = enabled;
    }

    LOG_INF("XY-to-scroll enabled: %d%s", enabled, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}

int zmk_input_processor_runtime_set_xy_swap_enabled(const struct device *dev, bool enabled,
                                                    bool persistent) {
    if (!dev) {
        return -EINVAL;
    }

    struct runtime_processor_data *data = dev->data;
    data->xy_swap_enabled = enabled;

    if (persistent) {
        data->persistent_xy_swap_enabled = enabled;
    }

    LOG_INF("XY-swap enabled: %d%s", enabled, persistent ? " (persistent)" : " (temporary)");

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    if (persistent) {
        ret = schedule_save_processor_settings(dev);
        raise_state_changed_event(dev);
    }
#endif

    return ret;
}
