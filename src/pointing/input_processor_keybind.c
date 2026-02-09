/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Input processor that converts input events to key press events.
 * Supports 1-8 behaviors with 2D dimension separation and degree offset rotation.
 *
 * Special thanks to https://github.com/zettaface/zmk-input-processor-keybind
 * for the original concept and inspiration.
 */

#define DT_DRV_COMPAT zmk_input_processor_keybind

#include <drivers/input_processor.h>
#include <math.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/pointing/input_processor_keybind.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_BINDINGS 8

struct keybind_processor_config {
    const char *name;
    uint8_t type;
    size_t x_codes_len;
    size_t y_codes_len;
    const uint16_t *x_codes;
    const uint16_t *y_codes;
    size_t bindings_len;
    struct zmk_behavior_binding bindings[MAX_BINDINGS];
    int32_t initial_tick;
    int32_t initial_degree_offset;
    bool track_remainders;
    uint32_t initial_wait_ms;
    uint32_t initial_tap_ms;
    uint32_t initial_active_layers;
};

struct keybind_processor_data {
    const struct device *dev;
    // Accumulation for X and Y
    int32_t x_accum;
    int32_t y_accum;
    // Remainders for precise tracking
    int32_t x_remainder;
    int32_t y_remainder;
    // Runtime configurable settings
    int32_t tick;
    int32_t degree_offset;
    uint32_t wait_ms;
    uint32_t tap_ms;
    // Active layers bitmask (0 = all layers)
    uint32_t active_layers;
    // Precomputed rotation values for degree offset
    int32_t cos_offset; // cos * 1000
    int32_t sin_offset; // sin * 1000
    // Work for delayed key release
    struct k_work_delayable release_work;
    int last_triggered_binding;
    bool key_pressed;
};

static void update_offset_rotation(struct keybind_processor_data *data, int32_t degree_offset) {
    if (degree_offset == 0) {
        data->cos_offset = 1000;
        data->sin_offset = 0;
        return;
    }

    // Convert degrees to radians and compute sin/cos
    double angle_rad = (double)degree_offset * 3.14159265359 / 180.0;
    data->cos_offset = (int32_t)(cos(angle_rad) * 1000.0);
    data->sin_offset = (int32_t)(sin(angle_rad) * 1000.0);

    LOG_DBG("Offset rotation %d degrees: cos=%d, sin=%d", degree_offset, data->cos_offset,
            data->sin_offset);
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

    // Check if at least one of the specified layers is active
    uint32_t remaining_mask = active_layers_mask;
    int layer_idx = 0;

    while (remaining_mask != 0 && layer_idx < ZMK_KEYMAP_LAYERS_LEN) {
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

static void release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct keybind_processor_data *data =
        CONTAINER_OF(dwork, struct keybind_processor_data, release_work);
    const struct keybind_processor_config *cfg = data->dev->config;

    if (!data->key_pressed || data->last_triggered_binding < 0) {
        return;
    }

    // Release the key
    struct zmk_behavior_binding *binding = &cfg->bindings[data->last_triggered_binding];
    struct zmk_behavior_binding_event event = {
        .position = INT32_MAX,
        .timestamp = k_uptime_get(),
    };

    int ret = zmk_behavior_invoke_binding(binding, event, false);
    if (ret < 0) {
        LOG_ERR("Failed to release binding %d: %d", data->last_triggered_binding, ret);
    } else {
        LOG_DBG("Released binding %d", data->last_triggered_binding);
    }

    data->key_pressed = false;
    data->last_triggered_binding = -1;
}

static int trigger_binding(struct keybind_processor_data *data,
                           const struct keybind_processor_config *cfg, int binding_idx) {
    if (binding_idx < 0 || binding_idx >= cfg->bindings_len) {
        return -EINVAL;
    }

    // If there's a key currently pressed, release it first
    if (data->key_pressed && data->last_triggered_binding >= 0) {
        k_work_cancel_delayable(&data->release_work);
        release_work_handler(&data->release_work.work);
    }

    // Press the new key
    struct zmk_behavior_binding *binding = &cfg->bindings[binding_idx];
    struct zmk_behavior_binding_event event = {
        .position = INT32_MAX,
        .timestamp = k_uptime_get(),
    };

    int ret = zmk_behavior_invoke_binding(binding, event, true);
    if (ret < 0) {
        LOG_ERR("Failed to press binding %d: %d", binding_idx, ret);
        return ret;
    }

    LOG_DBG("Pressed binding %d", binding_idx);
    data->key_pressed = true;
    data->last_triggered_binding = binding_idx;

    // Schedule release after tap_ms
    k_work_reschedule(&data->release_work, K_MSEC(data->tap_ms));

    return 0;
}

static int determine_direction(int32_t x, int32_t y, int32_t cos_offset, int32_t sin_offset,
                               size_t num_bindings) {
    if (num_bindings == 0) {
        return -1;
    }

    // Apply offset rotation to the coordinates
    // x' = x * cos - y * sin
    // y' = x * sin + y * cos
    int32_t x_rotated = (x * cos_offset - y * sin_offset) / 1000;
    int32_t y_rotated = (x * sin_offset + y * cos_offset) / 1000;

    // Calculate angle in degrees (0-359)
    // atan2 returns angle from -π to π, we want 0-360 starting from positive Y axis (up)
    double angle_rad = atan2((double)x_rotated, (double)y_rotated);
    double angle_deg = angle_rad * 180.0 / 3.14159265359;

    // Normalize to 0-360
    if (angle_deg < 0) {
        angle_deg += 360.0;
    }

    // Calculate section size
    double section_size = 360.0 / (double)num_bindings;

    // Determine which section this angle falls into
    int section = (int)(angle_deg / section_size);

    // Ensure section is within bounds
    if (section >= num_bindings) {
        section = num_bindings - 1;
    }

    LOG_DBG("Direction: x=%d, y=%d -> x_rot=%d, y_rot=%d, angle=%.1f°, section=%d/%zu", x, y,
            x_rotated, y_rotated, angle_deg, section, num_bindings);

    return section;
}

static int keybind_processor_handle_event(const struct device *dev, struct input_event *event,
                                          uint32_t param1, uint32_t param2,
                                          struct zmk_input_processor_state *state) {
    const struct keybind_processor_config *cfg = dev->config;
    struct keybind_processor_data *data = dev->data;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (cfg->bindings_len == 0) {
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

    // Accumulate movement
    if (is_x) {
        data->x_accum += value;
        if (cfg->track_remainders && state && state->remainder) {
            data->x_remainder += *state->remainder;
        }
    } else {
        data->y_accum += value;
        if (cfg->track_remainders && state && state->remainder) {
            data->y_remainder += *state->remainder;
        }
    }

    // Check if we've accumulated enough movement to trigger
    int32_t total_movement_sq = data->x_accum * data->x_accum + data->y_accum * data->y_accum;
    int32_t tick_sq = data->tick * data->tick;

    if (total_movement_sq >= tick_sq) {
        // Determine which binding to trigger based on direction
        int binding_idx = determine_direction(data->x_accum, data->y_accum, data->cos_offset,
                                              data->sin_offset, cfg->bindings_len);

        if (binding_idx >= 0) {
            // Trigger the binding
            int ret = trigger_binding(data, cfg, binding_idx);
            if (ret < 0) {
                LOG_ERR("Failed to trigger binding: %d", ret);
            }

            // Reset accumulation after triggering
            data->x_accum = 0;
            data->y_accum = 0;
            if (cfg->track_remainders) {
                data->x_remainder = 0;
                data->y_remainder = 0;
            }

            // Apply wait delay if configured
            // Note: Using k_sleep here intentionally blocks to prevent rapid-fire triggers.
            // This is acceptable as it only blocks when a key is actually triggered,
            // and the delay is typically very short (default is 0ms).
            if (data->wait_ms > 0) {
                k_sleep(K_MSEC(data->wait_ms));
            }
        }
    }

    // Consume the event (don't pass to next processor)
    event->value = 0;
    return ZMK_INPUT_PROC_STOP;
}

static struct zmk_input_processor_driver_api keybind_processor_driver_api = {
    .handle_event = keybind_processor_handle_event,
};

static int keybind_processor_init(const struct device *dev) {
    struct keybind_processor_data *data = dev->data;
    const struct keybind_processor_config *cfg = dev->config;

    data->dev = dev;
    data->x_accum = 0;
    data->y_accum = 0;
    data->x_remainder = 0;
    data->y_remainder = 0;
    data->tick = cfg->initial_tick;
    data->degree_offset = cfg->initial_degree_offset;
    data->wait_ms = cfg->initial_wait_ms;
    data->tap_ms = cfg->initial_tap_ms;
    data->active_layers = cfg->initial_active_layers;
    data->last_triggered_binding = -1;
    data->key_pressed = false;

    // Initialize work for delayed release
    k_work_init_delayable(&data->release_work, release_work_handler);

    // Precompute offset rotation values
    update_offset_rotation(data, data->degree_offset);

    LOG_INF("Keybind processor '%s' initialized: %d bindings, tick=%d, offset=%d°", cfg->name,
            cfg->bindings_len, data->tick, data->degree_offset);

    return 0;
}

#define GET_BINDING(node_id, prop, idx)                                                            \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node_id, prop, idx)),                     \
        .param1 = 0,                                                                               \
        .param2 = 0,                                                                               \
    },

#define KEYBIND_PROCESSOR_INST(n)                                                                  \
    static const uint16_t keybind_processor_x_codes_##n[] = DT_INST_PROP(n, x_codes);              \
    static const uint16_t keybind_processor_y_codes_##n[] = DT_INST_PROP(n, y_codes);              \
    static struct keybind_processor_data keybind_processor_data_##n;                               \
    static const struct keybind_processor_config keybind_processor_config_##n = {                  \
        .name = DT_INST_PROP(n, processor_label),                                                  \
        .type = DT_INST_PROP(n, type),                                                             \
        .x_codes = keybind_processor_x_codes_##n,                                                  \
        .x_codes_len = DT_INST_PROP_LEN(n, x_codes),                                               \
        .y_codes = keybind_processor_y_codes_##n,                                                  \
        .y_codes_len = DT_INST_PROP_LEN(n, y_codes),                                               \
        .bindings_len = DT_INST_PROP_LEN_OR(n, bindings, 0),                                       \
        .bindings = {COND_CODE_1(DT_INST_NODE_HAS_PROP(n, bindings),                               \
                                 (DT_INST_FOREACH_PROP_ELEM_SEP(n, bindings, GET_BINDING, ())),    \
                                 ())},                                                             \
        .initial_tick = DT_INST_PROP(n, tick),                                                     \
        .initial_degree_offset = DT_INST_PROP(n, degree_offset),                                   \
        .track_remainders = DT_INST_PROP(n, track_remainders),                                     \
        .initial_wait_ms = DT_INST_PROP(n, wait_ms),                                               \
        .initial_tap_ms = DT_INST_PROP(n, tap_ms),                                                 \
        .initial_active_layers = DT_INST_PROP(n, active_layers),                                   \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, keybind_processor_init, NULL, &keybind_processor_data_##n,            \
                          &keybind_processor_config_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,  \
                          &keybind_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KEYBIND_PROCESSOR_INST)

// Keep track of all keybind processors for lookup
#define KEYBIND_PROCESSOR_REF(n) DEVICE_DT_INST_GET(n),
static const struct device *keybind_processors[] = {
    DT_INST_FOREACH_STATUS_OKAY(KEYBIND_PROCESSOR_REF)};
static const size_t keybind_processors_count = ARRAY_SIZE(keybind_processors);

// API Functions
const struct device *zmk_input_processor_keybind_find_by_name(const char *name) {
    for (size_t i = 0; i < keybind_processors_count; i++) {
        const struct device *dev = keybind_processors[i];
        const struct keybind_processor_config *cfg = dev->config;
        if (strcmp(cfg->name, name) == 0) {
            return dev;
        }
    }
    return NULL;
}

int zmk_input_processor_keybind_get_config(const struct device *dev, const char **name,
                                           struct zmk_input_processor_keybind_config *config) {
    if (!dev) {
        return -EINVAL;
    }

    const struct keybind_processor_config *cfg = dev->config;
    struct keybind_processor_data *data = dev->data;

    if (name) {
        *name = cfg->name;
    }

    if (config) {
        config->tick = data->tick;
        config->degree_offset = data->degree_offset;
        config->track_remainders = cfg->track_remainders;
        config->wait_ms = data->wait_ms;
        config->tap_ms = data->tap_ms;
        config->active_layers = data->active_layers;
    }

    return 0;
}

int zmk_input_processor_keybind_set_tick(const struct device *dev, int32_t tick) {
    if (!dev || tick <= 0) {
        return -EINVAL;
    }

    struct keybind_processor_data *data = dev->data;
    data->tick = tick;

    LOG_INF("Set tick to %d", tick);
    return 0;
}

int zmk_input_processor_keybind_set_degree_offset(const struct device *dev, int32_t degree_offset) {
    if (!dev) {
        return -EINVAL;
    }

    // Normalize to 0-359 range
    while (degree_offset < 0) {
        degree_offset += 360;
    }
    while (degree_offset >= 360) {
        degree_offset -= 360;
    }

    struct keybind_processor_data *data = dev->data;
    data->degree_offset = degree_offset;
    update_offset_rotation(data, degree_offset);

    LOG_INF("Set degree offset to %d", degree_offset);
    return 0;
}

int zmk_input_processor_keybind_set_wait_ms(const struct device *dev, uint32_t wait_ms) {
    if (!dev) {
        return -EINVAL;
    }

    struct keybind_processor_data *data = dev->data;
    data->wait_ms = wait_ms;

    LOG_INF("Set wait_ms to %d", wait_ms);
    return 0;
}

int zmk_input_processor_keybind_set_tap_ms(const struct device *dev, uint32_t tap_ms) {
    if (!dev) {
        return -EINVAL;
    }

    struct keybind_processor_data *data = dev->data;
    data->tap_ms = tap_ms;

    LOG_INF("Set tap_ms to %d", tap_ms);
    return 0;
}

int zmk_input_processor_keybind_set_active_layers(const struct device *dev, uint32_t layers) {
    if (!dev) {
        return -EINVAL;
    }

    struct keybind_processor_data *data = dev->data;
    data->active_layers = layers;

    LOG_INF("Set active layers to 0x%08x", layers);
    return 0;
}

int zmk_input_processor_keybind_foreach(int (*callback)(const struct device *dev, void *user_data),
                                        void *user_data) {
    if (!callback) {
        return -EINVAL;
    }

    for (size_t i = 0; i < keybind_processors_count; i++) {
        int ret = callback(keybind_processors[i], user_data);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}
