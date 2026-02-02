/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_auto_mouse_layer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

struct auto_mouse_layer_config {
    uint8_t layer;
    int16_t require_prior_idle_ms;
    int16_t time_to_max_ms;
};

struct auto_mouse_layer_data {
    const struct device *dev;
    struct k_mutex lock;
    struct k_work_delayable deactivate_work;
    bool is_active;
    bool keep_active;  // Set by behavior
    int64_t last_keycode_timestamp;
};

/* Layer State Management */
static void update_layer_state(struct auto_mouse_layer_data *data, 
                              const struct auto_mouse_layer_config *cfg,
                              bool activate) {
    if (data->is_active == activate) {
        return;
    }

    data->is_active = activate;
    if (activate) {
        zmk_keymap_layer_activate(cfg->layer);
        LOG_DBG("Auto mouse layer %d activated", cfg->layer);
    } else {
        zmk_keymap_layer_deactivate(cfg->layer);
        LOG_DBG("Auto mouse layer %d deactivated", cfg->layer);
    }
}

/* Work Queue Callback for Layer Deactivation */
static void layer_deactivate_work_cb(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct auto_mouse_layer_data *data = 
        CONTAINER_OF(d_work, struct auto_mouse_layer_data, deactivate_work);
    const struct auto_mouse_layer_config *cfg = data->dev->config;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Error locking for deactivation %d", ret);
        return;
    }

    // Only deactivate if not being kept active by behavior
    if (!data->keep_active && data->is_active) {
        update_layer_state(data, cfg, false);
    }

    k_mutex_unlock(&data->lock);
}

/* Event Handlers */
static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct auto_mouse_layer_data *data = dev->data;
    const struct auto_mouse_layer_config *cfg = dev->config;
    
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    // If our layer was deactivated externally, update state
    if (!zmk_keymap_layer_active(cfg->layer) && data->is_active) {
        LOG_DBG("Layer deactivated externally");
        data->is_active = false;
        k_work_cancel_delayable(&data->deactivate_work);
    }

    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev->state) {  // Only care about key press, not release
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct auto_mouse_layer_data *data = dev->data;
    const struct auto_mouse_layer_config *cfg = dev->config;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    // If layer is active, check if this key should deactivate it
    if (data->is_active) {
        // Get the binding for this position on the active layer
        const struct zmk_behavior_binding *binding = 
            zmk_keymap_get_layer_binding_at_idx(cfg->layer, ev->position);
        
        if (binding) {
            const struct device *behavior_dev = zmk_behavior_get_binding(binding->behavior_dev);
            if (behavior_dev) {
                const char *behavior_name = behavior_dev->name;
                
                // Check if it's trans or none behavior
                if (strcmp(behavior_name, "TRANS") == 0 || 
                    strcmp(behavior_name, "NONE") == 0) {
                    LOG_DBG("Trans/None key pressed, deactivating auto mouse layer");
                    update_layer_state(data, cfg, false);
                    k_work_cancel_delayable(&data->deactivate_work);
                }
            }
        }
    }

    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev->state) {  // Only care about key press
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct auto_mouse_layer_data *data = dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    // Track last keycode timestamp for prior idle check
    data->last_keycode_timestamp = ev->timestamp;

    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_event_dispatcher(const zmk_event_t *eh) {
    const struct device *dev = DEVICE_DT_INST_GET(0);
    
    if (as_zmk_layer_state_changed(eh) != NULL) {
        return handle_layer_state_changed(dev, eh);
    } else if (as_zmk_position_state_changed(eh) != NULL) {
        return handle_position_state_changed(dev, eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return handle_keycode_state_changed(dev, eh);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

/* Driver Implementation */
static int auto_mouse_layer_handle_event(const struct device *dev, 
                                        struct input_event *event,
                                        uint32_t param1, uint32_t param2,
                                        struct zmk_input_processor_state *state) {
    struct auto_mouse_layer_data *data = dev->data;
    const struct auto_mouse_layer_config *cfg = dev->config;

    // Ignore sync events and events with zero value
    if (event->type == INPUT_EV_SYN || event->value == 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    // Check if enough idle time has passed since last keycode
    int64_t now = k_uptime_get();
    bool can_activate = (now - data->last_keycode_timestamp) >= cfg->require_prior_idle_ms;

    // Activate layer if not active and idle check passes
    if (!data->is_active && can_activate) {
        update_layer_state(data, cfg, true);
    }

    // If layer is active, reschedule deactivation timer
    if (data->is_active && cfg->time_to_max_ms > 0) {
        k_work_reschedule(&data->deactivate_work, K_MSEC(cfg->time_to_max_ms));
    }

    k_mutex_unlock(&data->lock);
    return ZMK_INPUT_PROC_CONTINUE;
}

/* Public API for behavior to control keep_active state */
void zmk_input_processor_auto_mouse_layer_set_keep_active(bool keep_active) {
    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct auto_mouse_layer_data *data = dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Failed to lock mutex: %d", ret);
        return;
    }

    data->keep_active = keep_active;
    
    // If we're setting keep_active to false and layer is active, start deactivation timer
    if (!keep_active && data->is_active) {
        const struct auto_mouse_layer_config *cfg = dev->config;
        if (cfg->time_to_max_ms > 0) {
            k_work_reschedule(&data->deactivate_work, K_MSEC(cfg->time_to_max_ms));
        }
    } else if (keep_active) {
        // Cancel any pending deactivation
        k_work_cancel_delayable(&data->deactivate_work);
    }

    k_mutex_unlock(&data->lock);
    LOG_DBG("Auto mouse layer keep_active set to %d", keep_active);
}

static int auto_mouse_layer_init(const struct device *dev) {
    struct auto_mouse_layer_data *data = dev->data;
    
    data->dev = dev;
    data->is_active = false;
    data->keep_active = false;
    data->last_keycode_timestamp = 0;
    
    k_mutex_init(&data->lock);
    k_work_init_delayable(&data->deactivate_work, layer_deactivate_work_cb);

    LOG_INF("Auto mouse layer processor initialized");
    return 0;
}

/* Driver API */
static const struct zmk_input_processor_driver_api auto_mouse_layer_driver_api = {
    .handle_event = auto_mouse_layer_handle_event,
};

/* Event Listeners */
ZMK_LISTENER(processor_auto_mouse_layer, handle_event_dispatcher);
ZMK_SUBSCRIPTION(processor_auto_mouse_layer, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(processor_auto_mouse_layer, zmk_position_state_changed);
ZMK_SUBSCRIPTION(processor_auto_mouse_layer, zmk_keycode_state_changed);

/* Device Instantiation */
#define AUTO_MOUSE_LAYER_INST(n)                                                        \
    static struct auto_mouse_layer_data processor_auto_mouse_layer_data_##n = {};       \
    static const struct auto_mouse_layer_config processor_auto_mouse_layer_config_##n = { \
        .layer = DT_INST_PROP(n, layer),                                                \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),          \
        .time_to_max_ms = DT_INST_PROP_OR(n, time_to_max_ms, 600),                      \
    };                                                                                  \
    DEVICE_DT_INST_DEFINE(n, auto_mouse_layer_init, NULL,                               \
                          &processor_auto_mouse_layer_data_##n,                          \
                          &processor_auto_mouse_layer_config_##n, POST_KERNEL,           \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                           \
                          &auto_mouse_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AUTO_MOUSE_LAYER_INST)
