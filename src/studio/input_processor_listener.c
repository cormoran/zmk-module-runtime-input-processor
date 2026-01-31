/**
 * Runtime Input Processor - Event Listener for Studio Notifications
 *
 * Listens to input processor state changed events and sends notifications to Studio
 */

#include <zmk/event_manager.h>
#include <zmk/events/input_processor_state_changed.h>
#include <zmk/studio/custom.h>
#include <zmk/template/custom.pb.h>
#include <pb_encode.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TEMPLATE_FEATURE_STUDIO_RPC)

// Global buffer for notification
static zmk_template_Notification notification_buffer;

// Encoder for the notification
static bool encode_notification(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    return pb_encode_submessage(stream, zmk_template_Notification_fields, &notification_buffer);
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
    const struct zmk_input_processor_state_changed *ev =
        as_zmk_input_processor_state_changed(eh);
    
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Input processor state changed: %s", ev->name);

    // Prepare notification
    notification_buffer = (zmk_template_Notification)zmk_template_Notification_init_zero;
    notification_buffer.which_notification_type = zmk_template_Notification_input_processor_changed_tag;
    
    zmk_template_InputProcessorInfo *info =
        &notification_buffer.notification_type.input_processor_changed.processor;
    
    strncpy(info->name, ev->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->scale_multiplier = ev->config.scale_multiplier;
    info->scale_divisor = ev->config.scale_divisor;
    info->rotation_degrees = ev->config.rotation_degrees;

    // Send notification via custom studio subsystem
    pb_callback_t encode_cb = {
        .funcs.encode = encode_notification,
        .arg = NULL
    };
    
    // Raise notification event
    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = find_subsystem_index("zmk__template"),
        .encode_payload = encode_cb
    });
    
    LOG_INF("Sent notification for processor %s", ev->name);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(input_processor_state_listener, input_processor_state_changed_listener);
ZMK_SUBSCRIPTION(input_processor_state_listener, zmk_input_processor_state_changed);

// TODO: Relay event to peripheral when macros are available in ZMK
// ZMK_SPLIT_RELAY_EVENT_FROM_CENTRAL(zmk_input_processor_state_changed);
// ZMK_SPLIT_RELAY_EVENT_FROM_PERIPHERAL(zmk_input_processor_state_changed);

#endif // CONFIG_ZMK_TEMPLATE_FEATURE_STUDIO_RPC
