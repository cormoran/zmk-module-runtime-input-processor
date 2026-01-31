/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_runtime

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/dlist.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <math.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zmk/event_manager.h>
#include <zmk/events/input_processor_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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
};

struct runtime_processor_data {
    // Current active values (may be temporary from behavior)
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
    int32_t rotation_degrees;
    
    // Persistent values (saved to settings, not affected by behavior)
    uint32_t persistent_scale_multiplier;
    uint32_t persistent_scale_divisor;
    int32_t persistent_rotation_degrees;
    
    // Precomputed rotation values
    int32_t cos_val;  // cos * 1000
    int32_t sin_val;  // sin * 1000
    
    // Last seen X/Y values for rotation
    int16_t last_x;
    int16_t last_y;
    bool has_x;
    bool has_y;
};

// Global list head for all runtime processors
static sys_slist_t runtime_processors = SYS_SLIST_STATIC_INIT(&runtime_processors);

struct runtime_processor_node {
    sys_snode_t node;
    const struct device *dev;
#if IS_ENABLED(CONFIG_SETTINGS)
    struct k_work_delayable save_work;
#endif
};

static void update_rotation_values(struct runtime_processor_data *data) {
    if (data->rotation_degrees == 0) {
        data->cos_val = 1000;
        data->sin_val = 0;
        return;
    }

    // Convert degrees to radians and compute sin/cos
    double angle_rad = (double)data->rotation_degrees * 3.14159265359 / 180.0;
    data->cos_val = (int32_t)(cos(angle_rad) * 1000.0);
    data->sin_val = (int32_t)(sin(angle_rad) * 1000.0);

    LOG_DBG("Rotation %d degrees: cos=%d, sin=%d", data->rotation_degrees, 
            data->cos_val, data->sin_val);
}

static int code_idx(uint16_t code, const uint16_t *list, size_t len) {
    for (int i = 0; i < len; i++) {
        if (list[i] == code) {
            return i;
        }
    }
    return -ENODEV;
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

static int runtime_processor_handle_event(const struct device *dev, 
                                          struct input_event *event,
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

    bool is_x = (x_idx >= 0);
    int16_t value = event->value;

    // Apply scaling first
    if (data->scale_multiplier > 0 && data->scale_divisor > 0) {
        scale_val(event, data->scale_multiplier, data->scale_divisor, state);
        value = event->value;
    }

    // Apply rotation if configured
    if (data->rotation_degrees != 0) {
        if (is_x) {
            data->last_x = value;
            data->has_x = true;
            
            // If we have both X and Y, apply rotation
            if (data->has_y) {
                // X' = X * cos - Y * sin
                // Using 1000 as scaling factor for fixed-point arithmetic (precision: 0.001)
                int32_t rotated_x = (data->last_x * data->cos_val - data->last_y * data->sin_val) / 1000;
                event->value = (int16_t)rotated_x;
                data->has_x = false;
                data->has_y = false;
            }
            // If no Y yet, event value already contains scaled X (rotation applied on next pair)
        } else {
            data->last_y = value;
            data->has_y = true;
            
            // If we have both X and Y, apply rotation
            if (data->has_x) {
                // Y' = X * sin + Y * cos
                int32_t rotated_y = (data->last_x * data->sin_val + data->last_y * data->cos_val) / 1000;
                event->value = (int16_t)rotated_y;
                data->has_x = false;
                data->has_y = false;
            }
            // If no X yet, event value already contains scaled Y (rotation applied on next pair)
        }
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
};

static void save_processor_settings_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct runtime_processor_node *node = CONTAINER_OF(dwork, struct runtime_processor_node, save_work);
    const struct device *dev = node->dev;
    const struct runtime_processor_config *cfg = dev->config;
    struct runtime_processor_data *data = dev->data;
    
    struct processor_settings settings = {
        .scale_multiplier = data->persistent_scale_multiplier,
        .scale_divisor = data->persistent_scale_divisor,
        .rotation_degrees = data->persistent_rotation_degrees,
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
    struct runtime_processor_node *node;
    
    SYS_SLIST_FOR_EACH_CONTAINER(&runtime_processors, node, node) {
        if (node->dev == dev) {
            return k_work_reschedule(&node->save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
        }
    }
    
    return -ENODEV;
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
            
            // Apply to current values
            data->scale_multiplier = settings.scale_multiplier;
            data->scale_divisor = settings.scale_divisor;
            data->rotation_degrees = settings.rotation_degrees;
            update_rotation_values(data);
            
            LOG_INF("Loaded settings for %s: scale=%d/%d, rotation=%d",
                    cfg->name, settings.scale_multiplier, settings.scale_divisor,
                    settings.rotation_degrees);
            return 0;
        }
    }
    return -EINVAL;
}

static int runtime_processor_settings_load_cb(const char *name, size_t len, 
                                              settings_read_cb read_cb, void *cb_arg) {
    struct runtime_processor_node *node;
    
    SYS_SLIST_FOR_EACH_CONTAINER(&runtime_processors, node, node) {
        const struct runtime_processor_config *cfg = node->dev->config;
        if (strcmp(name, cfg->name) == 0) {
            return load_processor_settings_cb(name, len, read_cb, cb_arg, (void *)node->dev);
        }
    }
    
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(input_proc, "input_proc", NULL, 
                               runtime_processor_settings_load_cb, NULL, NULL);
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

    update_rotation_values(data);

    // Add to global list
    struct runtime_processor_node *node = k_malloc(sizeof(struct runtime_processor_node));
    if (!node) {
        LOG_ERR("Failed to allocate memory for processor node");
        return -ENOMEM;
    }
    
    node->dev = dev;
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&node->save_work, save_processor_settings_work_handler);
#endif
    sys_slist_append(&runtime_processors, &node->node);
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
    
    raise_zmk_input_processor_state_changed((struct zmk_input_processor_state_changed){
        .name = name,
        .config = config
    });
}

// Public API for runtime configuration
int zmk_input_processor_runtime_set_scaling(const struct device *dev, 
                                            uint32_t multiplier, 
                                            uint32_t divisor,
                                            bool persistent) {
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
    
    LOG_DBG("Restored persistent values");
}

int zmk_input_processor_runtime_get_config(const struct device *dev,
                                           const char **name,
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
        config->scale_multiplier = data->scale_multiplier;
        config->scale_divisor = data->scale_divisor;
        config->rotation_degrees = data->rotation_degrees;
    }

    return 0;
}

const struct device *zmk_input_processor_runtime_find_by_name(const char *name) {
    struct runtime_processor_node *node;
    
    SYS_SLIST_FOR_EACH_CONTAINER(&runtime_processors, node, node) {
        const struct runtime_processor_config *cfg = node->dev->config;
        if (strcmp(cfg->name, name) == 0) {
            return node->dev;
        }
    }
    
    return NULL;
}

int zmk_input_processor_runtime_foreach(int (*callback)(const struct device *dev, void *user_data),
                                       void *user_data) {
    struct runtime_processor_node *node;
    
    SYS_SLIST_FOR_EACH_CONTAINER(&runtime_processors, node, node) {
        int ret = callback(node->dev, user_data);
        if (ret != 0) {
            return ret;
        }
    }
    
    return 0;
}

#define RUNTIME_PROCESSOR_INST(n)                                                                  \
    static const uint16_t runtime_x_codes_##n[] = DT_INST_PROP(n, x_codes);                       \
    static const uint16_t runtime_y_codes_##n[] = DT_INST_PROP(n, y_codes);                       \
    BUILD_ASSERT(ARRAY_SIZE(runtime_x_codes_##n) == ARRAY_SIZE(runtime_y_codes_##n),              \
                 "X and Y codes need to be the same size");                                        \
    static const struct runtime_processor_config runtime_config_##n = {                            \
        .name = DT_INST_PROP(n, name),                                                             \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                            \
        .x_codes_len = DT_INST_PROP_LEN(n, x_codes),                                               \
        .y_codes_len = DT_INST_PROP_LEN(n, y_codes),                                               \
        .x_codes = runtime_x_codes_##n,                                                            \
        .y_codes = runtime_y_codes_##n,                                                            \
        .initial_scale_multiplier = DT_INST_PROP_OR(n, scale_multiplier, 1),                       \
        .initial_scale_divisor = DT_INST_PROP_OR(n, scale_divisor, 1),                             \
        .initial_rotation_degrees = DT_INST_PROP_OR(n, rotation_degrees, 0),                       \
    };                                                                                             \
    static struct runtime_processor_data runtime_data_##n;                                         \
    DEVICE_DT_INST_DEFINE(n, &runtime_processor_init, NULL, &runtime_data_##n,                     \
                          &runtime_config_##n, POST_KERNEL,                                        \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &runtime_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RUNTIME_PROCESSOR_INST)
