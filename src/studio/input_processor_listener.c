/**
 * Runtime Input Processor - Event Listener for Studio Notifications
 *
 * Listens to input processor state changed events and sends notifications to
 * Studio
 */

#include <cormoran/rip/custom.pb.h>
#include <pb_encode.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/input_processor_state_changed.h>
#include <zmk/studio/custom.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC)

// Encoder for the notification
static bool encode_notification(pb_ostream_t *stream, const pb_field_t *field,
                                void *const *arg) {
    cormoran_rip_Notification *notification = (cormoran_rip_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    size_t size;
    if (!pb_get_encoded_size(&size, cormoran_rip_Notification_fields,
                             notification)) {
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
    extern struct zmk_rpc_custom_subsystem
        _zmk_rpc_custom_subsystem_list_start[];
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_end[];

    uint8_t index = 0;
    for (struct zmk_rpc_custom_subsystem *subsys =
             _zmk_rpc_custom_subsystem_list_start;
         subsys < _zmk_rpc_custom_subsystem_list_end; subsys++) {
        if (strcmp(subsys->identifier, identifier) == 0) {
            return index;
        }
        index++;
    }
    return 0;  // Default to first subsystem if not found
}

static int input_processor_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_input_processor_state_changed *ev =
        as_zmk_input_processor_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Input processor state changed: %s (id=%d)", ev->name, ev->id);

    cormoran_rip_Notification notification =
        cormoran_rip_Notification_init_zero;
    notification.which_notification_type =
        cormoran_rip_Notification_input_processor_changed_tag;
    notification.notification_type.input_processor_changed.has_processor = true;
    cormoran_rip_InputProcessorInfo *info =
        &notification.notification_type.input_processor_changed.processor;

    info->id = ev->id;
    strncpy(info->name, ev->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->scale_multiplier             = ev->config.scale_multiplier;
    info->scale_divisor                = ev->config.scale_divisor;
    info->rotation_degrees             = ev->config.rotation_degrees;
    info->temp_layer_enabled           = ev->config.temp_layer_enabled;
    info->temp_layer_layer             = ev->config.temp_layer_layer;
    info->temp_layer_activation_delay_ms =
        ev->config.temp_layer_activation_delay_ms;
    info->temp_layer_deactivation_delay_ms =
        ev->config.temp_layer_deactivation_delay_ms;
    info->active_layers                = ev->config.active_layers;

    // Send notification via custom studio subsystem
    pb_callback_t encode_cb = {.funcs.encode = encode_notification,
                               .arg          = &notification};

    // Raise notification event
    raise_zmk_studio_custom_notification(
        (struct zmk_studio_custom_notification){
            .subsystem_index = find_subsystem_index("cormoran_rip"),
            .encode_payload  = encode_cb});

    LOG_INF("Sent notification for processor %s", ev->name);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(input_processor_state_listener,
             input_processor_state_changed_listener);
ZMK_SUBSCRIPTION(input_processor_state_listener,
                 zmk_input_processor_state_changed);

// NOTE: relay from peripheral is not required because all input-processors can
// be defined in central side
//       input-processor should be set to zmk,input-split in central side

#endif  // CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC
