/**
 * Runtime Input Processor - Event Listener for Studio Notifications
 *
 * Listens to input processor state changed events and sends notifications to
 * Studio
 */

#include <cormoran/rip/custom.pb.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zmk/event_manager.h>
#include <zmk/events/input_processor_state_changed.h>
#include <zmk/events/input_processor_inertia_state_changed.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/studio/custom.h>
#include <zmk/workqueue.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC)

// Encoder for the notification
static bool encode_notification(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    cormoran_rip_Notification *notification = (cormoran_rip_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    size_t size;
    if (!pb_get_encoded_size(&size, cormoran_rip_Notification_fields, notification)) {
        LOG_WRN("Failed to get encoded size for notification");
        return false;
    }

    if (!pb_encode_varint(stream, size)) {
        return false;
    }
    return pb_encode(stream, cormoran_rip_Notification_fields, notification);
}

// Find subsystem index by iterating through registered subsystems
static uint8_t find_subsystem_index(const char *identifier) {
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_start[];
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_end[];

    uint8_t index = 0;
    for (struct zmk_rpc_custom_subsystem *subsys = _zmk_rpc_custom_subsystem_list_start;
         subsys < _zmk_rpc_custom_subsystem_list_end; subsys++) {
        if (strcmp(subsys->identifier, identifier) == 0) {
            return index;
        }
        index++;
    }
    return 0; // Default to first subsystem if not found
}

static int input_processor_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_input_processor_state_changed *ev = as_zmk_input_processor_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Input processor state changed: %s (id=%d)", ev->name, ev->id);

    cormoran_rip_Notification notification = cormoran_rip_Notification_init_zero;
    notification.which_notification_type = cormoran_rip_Notification_input_processor_changed_tag;
    notification.notification_type.input_processor_changed.has_processor = true;
    cormoran_rip_InputProcessorInfo *info =
        &notification.notification_type.input_processor_changed.processor;

    info->id = ev->id;
    strncpy(info->name, ev->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->scale_multiplier = ev->config.scale_multiplier;
    info->scale_divisor = ev->config.scale_divisor;
    info->rotation_degrees = ev->config.rotation_degrees;
    info->temp_layer_enabled = ev->config.temp_layer_enabled;
    info->temp_layer_layer = ev->config.temp_layer_layer;
    info->temp_layer_activation_delay_ms = ev->config.temp_layer_activation_delay_ms;
    info->temp_layer_deactivation_delay_ms = ev->config.temp_layer_deactivation_delay_ms;
    info->active_layers = ev->config.active_layers;
    info->axis_snap_mode = ev->config.axis_snap_mode;
    info->axis_snap_threshold = ev->config.axis_snap_threshold;
    info->axis_snap_timeout_ms = ev->config.axis_snap_timeout_ms;
    info->xy_to_scroll_enabled = ev->config.xy_to_scroll_enabled;
    info->xy_swap_enabled = ev->config.xy_swap_enabled;
    info->x_invert = ev->config.x_invert;
    info->y_invert = ev->config.y_invert;
    info->inertia_interval_ms = ev->config.inertia_interval_ms;
    info->inertia_threshold = ev->config.inertia_threshold;
    info->inertia_window_ms = ev->config.inertia_window_ms;
    info->inertia_decay_percent = ev->config.inertia_decay_percent;
    info->inertia_fast_threshold = ev->config.inertia_fast_threshold;
    info->inertia_fast_output_percent = ev->config.inertia_fast_output_percent;
    info->inertia_enabled = ev->config.inertia_enabled;
    info->inertia_notifications_enabled = ev->config.inertia_notifications_enabled;
    info->inertia_active = ev->config.inertia_active;

    // Send notification via custom studio subsystem
    pb_callback_t encode_cb = {.funcs.encode = encode_notification, .arg = &notification};

    // Raise notification event
    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = find_subsystem_index("cormoran_rip"), .encode_payload = encode_cb});

    LOG_INF("Sent notification for processor %s", ev->name);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(input_processor_state_listener, input_processor_state_changed_listener);
ZMK_SUBSCRIPTION(input_processor_state_listener, zmk_input_processor_state_changed);

/* Input processors run on Zephyr's small input thread stack. Encoding and
 * sending a Studio notification there can overflow that stack. Queue the
 * transition and do the RPC work on ZMK's low priority work queue instead.
 * A FIFO preserves start/stop transitions even when they happen quickly. */
struct inertia_state_notification {
    const struct device *dev;
    enum zmk_input_processor_inertia_state state;
    enum zmk_input_processor_inertia_stop_reason stop_reason;
};

K_MSGQ_DEFINE(inertia_state_notifications, sizeof(struct inertia_state_notification), 32, 4);
static atomic_t dropped_inertia_state_notifications;

static void send_inertia_state_notifications(struct k_work *work) {
    ARG_UNUSED(work);
    struct inertia_state_notification state;
    while (k_msgq_get(&inertia_state_notifications, &state, K_NO_WAIT) == 0) {
        struct zmk_input_processor_runtime_config config;
        if (zmk_input_processor_runtime_get_config(state.dev, NULL, &config) < 0 ||
            !config.inertia_notifications_enabled) {
            continue;
        }
        int id = zmk_input_processor_runtime_get_id(state.dev);
        if (id < 0) {
            continue;
        }
        cormoran_rip_Notification notification = cormoran_rip_Notification_init_zero;
        if (state.state == ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STARTED ||
            state.state == ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STOPPED) {
            notification.which_notification_type =
                cormoran_rip_Notification_inertia_fast_input_changed_tag;
            notification.notification_type.inertia_fast_input_changed.id = id;
            notification.notification_type.inertia_fast_input_changed.fast_input =
                state.state == ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STARTED;
        } else {
            notification.which_notification_type = cormoran_rip_Notification_inertia_state_changed_tag;
            notification.notification_type.inertia_state_changed.id = id;
            notification.notification_type.inertia_state_changed.active =
                state.state == ZMK_INPUT_PROCESSOR_INERTIA_STARTED;
            notification.notification_type.inertia_state_changed.stop_reason =
                (cormoran_rip_InertiaStopReason)state.stop_reason;
        }
        pb_callback_t encode_cb = {.funcs.encode = encode_notification, .arg = &notification};
        raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
            .subsystem_index = find_subsystem_index("cormoran_rip"), .encode_payload = encode_cb});
    }
    atomic_val_t dropped = atomic_set(&dropped_inertia_state_notifications, 0);
    if (dropped > 0) {
        LOG_WRN("Dropped %d inertia state notifications", dropped);
    }
}

K_WORK_DEFINE(inertia_state_notification_work, send_inertia_state_notifications);

static int input_processor_inertia_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_input_processor_inertia_state_changed *ev =
        as_zmk_input_processor_inertia_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    struct inertia_state_notification state = {
        .dev = ev->dev, .state = ev->state, .stop_reason = ev->stop_reason};
    if (k_msgq_put(&inertia_state_notifications, &state, K_NO_WAIT) < 0) {
        atomic_inc(&dropped_inertia_state_notifications);
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &inertia_state_notification_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(input_processor_inertia_state_listener,
             input_processor_inertia_state_changed_listener);
ZMK_SUBSCRIPTION(input_processor_inertia_state_listener,
                 zmk_input_processor_inertia_state_changed);

// NOTE: relay from peripheral is not required because all input-processors can
// be defined in central side
//       input-processor should be set to zmk,input-split in central side

#endif // CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC
