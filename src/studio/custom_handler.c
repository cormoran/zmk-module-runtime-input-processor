/**
 * Runtime Input Processor - Custom Studio RPC Handler
 *
 * This file implements custom RPC subsystem for runtime input processor
 * configuration.
 */

#include <cormoran/rip/custom.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/input_processor_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/studio/custom.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR)

/**
 * Metadata for the custom subsystem.
 */
static struct zmk_rpc_custom_subsystem_meta rip_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(
        "https://cormoran.github.io/zmk-module-runtime-input-processor/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

/**
 * Register the custom RPC subsystem.
 * Using "cormoran_rip" as the identifier (input processor runtime)
 */
ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran_rip, &rip_feature_meta,
                         rip_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran_rip, cormoran_rip_Response);

static int handle_list_input_processors(
    const cormoran_rip_ListInputProcessorsRequest *req,
    cormoran_rip_Response *resp);
static int handle_get_input_processor(
    const cormoran_rip_GetInputProcessorRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_scale_multiplier(
    const cormoran_rip_SetScaleMultiplierRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_scale_divisor(
    const cormoran_rip_SetScaleDivisorRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_rotation(const cormoran_rip_SetRotationRequest *req,
                               cormoran_rip_Response *resp);
static int handle_reset_input_processor(
    const cormoran_rip_ResetInputProcessorRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_temp_layer_enabled(
    const cormoran_rip_SetTempLayerEnabledRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_temp_layer_layer(
    const cormoran_rip_SetTempLayerLayerRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_temp_layer_activation_delay(
    const cormoran_rip_SetTempLayerActivationDelayRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_temp_layer_deactivation_delay(
    const cormoran_rip_SetTempLayerDeactivationDelayRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_active_layers(
    const cormoran_rip_SetActiveLayersRequest *req,
    cormoran_rip_Response *resp);
static int handle_get_layer_info(
    const cormoran_rip_GetLayerInfoRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_axis_snap_mode(
    const cormoran_rip_SetAxisSnapModeRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_axis_snap_threshold(
    const cormoran_rip_SetAxisSnapThresholdRequest *req,
    cormoran_rip_Response *resp);
static int handle_set_axis_snap_timeout(
    const cormoran_rip_SetAxisSnapTimeoutRequest *req,
    cormoran_rip_Response *resp);

/**
 * Main request handler for the custom RPC subsystem.
 */
static bool rip_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                   pb_callback_t *encode_response) {
    cormoran_rip_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran_rip,
                                                          encode_response);

    cormoran_rip_Request req = cormoran_rip_Request_init_zero;

    // Decode the incoming request from the raw payload
    pb_istream_t req_stream = pb_istream_from_buffer(raw_request->payload.bytes,
                                                     raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_rip_Request_fields, &req)) {
        LOG_WRN("Failed to decode rip request: %s", PB_GET_ERROR(&req_stream));
        cormoran_rip_ErrorResponse err = cormoran_rip_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = cormoran_rip_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
        case cormoran_rip_Request_list_input_processors_tag:
            rc = handle_list_input_processors(
                &req.request_type.list_input_processors, resp);
            break;
        case cormoran_rip_Request_get_input_processor_tag:
            rc = handle_get_input_processor(
                &req.request_type.get_input_processor, resp);
            break;
        case cormoran_rip_Request_set_scale_multiplier_tag:
            rc = handle_set_scale_multiplier(
                &req.request_type.set_scale_multiplier, resp);
            break;
        case cormoran_rip_Request_set_scale_divisor_tag:
            rc = handle_set_scale_divisor(&req.request_type.set_scale_divisor,
                                          resp);
            break;
        case cormoran_rip_Request_set_rotation_tag:
            rc = handle_set_rotation(&req.request_type.set_rotation, resp);
            break;
        case cormoran_rip_Request_reset_input_processor_tag:
            rc = handle_reset_input_processor(
                &req.request_type.reset_input_processor, resp);
            break;
        case cormoran_rip_Request_set_temp_layer_enabled_tag:
            rc = handle_set_temp_layer_enabled(
                &req.request_type.set_temp_layer_enabled, resp);
            break;
        case cormoran_rip_Request_set_temp_layer_layer_tag:
            rc = handle_set_temp_layer_layer(
                &req.request_type.set_temp_layer_layer, resp);
            break;
        case cormoran_rip_Request_set_temp_layer_activation_delay_tag:
            rc = handle_set_temp_layer_activation_delay(
                &req.request_type.set_temp_layer_activation_delay, resp);
            break;
        case cormoran_rip_Request_set_temp_layer_deactivation_delay_tag:
            rc = handle_set_temp_layer_deactivation_delay(
                &req.request_type.set_temp_layer_deactivation_delay, resp);
            break;
        case cormoran_rip_Request_set_active_layers_tag:
            rc = handle_set_active_layers(
                &req.request_type.set_active_layers, resp);
            break;
        case cormoran_rip_Request_get_layer_info_tag:
            rc = handle_get_layer_info(
                &req.request_type.get_layer_info, resp);
            break;
        case cormoran_rip_Request_set_axis_snap_mode_tag:
            rc = handle_set_axis_snap_mode(
                &req.request_type.set_axis_snap_mode, resp);
            break;
        case cormoran_rip_Request_set_axis_snap_threshold_tag:
            rc = handle_set_axis_snap_threshold(
                &req.request_type.set_axis_snap_threshold, resp);
            break;
        case cormoran_rip_Request_set_axis_snap_timeout_tag:
            rc = handle_set_axis_snap_timeout(
                &req.request_type.set_axis_snap_timeout, resp);
            break;
        default:
            LOG_WRN("Unsupported rip request type: %d", req.which_request_type);
            rc = -1;
    }

    if (rc != 0) {
        cormoran_rip_ErrorResponse err = cormoran_rip_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request");
        resp->which_response_type = cormoran_rip_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}

// Helper callback to send notification for each processor during list operation
struct list_processors_context {
    int count;
};

static int list_processors_callback(const struct device *dev, void *user_data) {
    struct list_processors_context *ctx =
        (struct list_processors_context *)user_data;

    const char *name;
    struct zmk_input_processor_runtime_config config;
    int ret = zmk_input_processor_runtime_get_config(dev, &name, &config);
    if (ret < 0) {
        return 0;
    }

    // Get processor ID
    int id = zmk_input_processor_runtime_get_id(dev);
    if (id < 0) {
        return 0;
    }

    // Raise event which will be caught by listener and sent as notification
    raise_zmk_input_processor_state_changed(
        (struct zmk_input_processor_state_changed){.id     = (uint8_t)id,
                                                   .name   = name,
                                                   .config = config});

    ctx->count++;
    return 0;
}

static void list_input_processors_work_handler(struct k_work *work) {
    struct list_processors_context ctx = {.count = 0};
    zmk_input_processor_runtime_foreach(list_processors_callback, &ctx);
    LOG_INF("Raised events for %d input processors", ctx.count);
}

K_WORK_DEFINE(list_input_processors_work, list_input_processors_work_handler);

/**
 * Handle listing all input processors - raises events for each
 */
static int handle_list_input_processors(
    const cormoran_rip_ListInputProcessorsRequest *req,
    cormoran_rip_Response *resp) {
    k_work_submit(&list_input_processors_work);

    // Return empty response (notifications sent via events contain the data)
    resp->which_response_type = cormoran_rip_Response_list_input_processors_tag;
    resp->response_type.list_input_processors =
        (cormoran_rip_ListInputProcessorsResponse)
            cormoran_rip_ListInputProcessorsResponse_init_zero;
    return 0;
}

/**
 * Handle getting a specific input processor configuration
 */
static int handle_get_input_processor(
    const cormoran_rip_GetInputProcessorRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Getting input processor: id=%d", req->id);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    cormoran_rip_GetInputProcessorResponse result =
        cormoran_rip_GetInputProcessorResponse_init_zero;

    const char *name;
    struct zmk_input_processor_runtime_config config;
    int ret = zmk_input_processor_runtime_get_config(dev, &name, &config);
    if (ret < 0) {
        return ret;
    }

    result.processor.id = req->id;
    strncpy(result.processor.name, name, sizeof(result.processor.name) - 1);
    result.processor.name[sizeof(result.processor.name) - 1] = '\0';
    result.processor.scale_multiplier   = config.scale_multiplier;
    result.processor.scale_divisor      = config.scale_divisor;
    result.processor.rotation_degrees   = config.rotation_degrees;
    result.processor.temp_layer_enabled = config.temp_layer_enabled;
    result.processor.temp_layer_layer   = config.temp_layer_layer;
    result.processor.temp_layer_activation_delay_ms =
        config.temp_layer_activation_delay_ms;
    result.processor.temp_layer_deactivation_delay_ms =
        config.temp_layer_deactivation_delay_ms;
    result.processor.active_layers = config.active_layers;

    resp->which_response_type = cormoran_rip_Response_get_input_processor_tag;
    resp->response_type.get_input_processor = result;

    return 0;
}

/**
 * Handle setting scale multiplier
 */
static int handle_set_scale_multiplier(
    const cormoran_rip_SetScaleMultiplierRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting scale multiplier for id=%d to %d", req->id, req->value);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Get current divisor
    struct zmk_input_processor_runtime_config config;
    int ret = zmk_input_processor_runtime_get_config(dev, NULL, &config);
    if (ret < 0) {
        return ret;
    }

    // Set new multiplier (persistent)
    ret = zmk_input_processor_runtime_set_scaling(dev, req->value,
                                                  config.scale_divisor, true);
    if (ret < 0) {
        LOG_ERR("Failed to set scale multiplier: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_scale_multiplier_tag;
    resp->response_type.set_scale_multiplier =
        (cormoran_rip_SetScaleMultiplierResponse)
            cormoran_rip_SetScaleMultiplierResponse_init_zero;

    return 0;
}

/**
 * Handle setting scale divisor
 */
static int handle_set_scale_divisor(
    const cormoran_rip_SetScaleDivisorRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting scale divisor for id=%d to %d", req->id, req->value);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Get current multiplier
    struct zmk_input_processor_runtime_config config;
    int ret = zmk_input_processor_runtime_get_config(dev, NULL, &config);
    if (ret < 0) {
        return ret;
    }

    // Set new divisor (persistent)
    ret = zmk_input_processor_runtime_set_scaling(dev, config.scale_multiplier,
                                                  req->value, true);
    if (ret < 0) {
        LOG_ERR("Failed to set scale divisor: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_scale_divisor_tag;
    resp->response_type.set_scale_divisor =
        (cormoran_rip_SetScaleDivisorResponse)
            cormoran_rip_SetScaleDivisorResponse_init_zero;

    return 0;
}

/**
 * Handle setting rotation
 */
static int handle_set_rotation(const cormoran_rip_SetRotationRequest *req,
                               cormoran_rip_Response *resp) {
    LOG_DBG("Setting rotation for id=%d to %d degrees", req->id, req->value);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set rotation (persistent)
    int ret = zmk_input_processor_runtime_set_rotation(dev, req->value, true);
    if (ret < 0) {
        LOG_ERR("Failed to set rotation: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type        = cormoran_rip_Response_set_rotation_tag;
    resp->response_type.set_rotation = (cormoran_rip_SetRotationResponse)
        cormoran_rip_SetRotationResponse_init_zero;

    return 0;
}

/**
 * Handle resetting input processor to defaults
 */
static int handle_reset_input_processor(
    const cormoran_rip_ResetInputProcessorRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Resetting input processor: id=%d", req->id);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Reset to defaults
    int ret = zmk_input_processor_runtime_reset(dev);
    if (ret < 0) {
        LOG_ERR("Failed to reset processor: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_reset_input_processor_tag;
    resp->response_type.reset_input_processor =
        (cormoran_rip_ResetInputProcessorResponse)
            cormoran_rip_ResetInputProcessorResponse_init_zero;

    return 0;
}

/**
 * Handle setting temp-layer enabled state
 */
static int handle_set_temp_layer_enabled(
    const cormoran_rip_SetTempLayerEnabledRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting temp-layer enabled for id=%d to %d", req->id, req->enabled);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set temp-layer enabled (persistent)
    int ret = zmk_input_processor_runtime_set_temp_layer_enabled(
        dev, req->enabled, true);
    if (ret < 0) {
        LOG_ERR("Failed to set temp-layer enabled: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_temp_layer_enabled_tag;
    resp->response_type.set_temp_layer_enabled =
        (cormoran_rip_SetTempLayerEnabledResponse)
            cormoran_rip_SetTempLayerEnabledResponse_init_zero;

    return 0;
}

/**
 * Handle setting temp-layer target layer
 */
static int handle_set_temp_layer_layer(
    const cormoran_rip_SetTempLayerLayerRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting temp-layer layer for id=%d to %d", req->id, req->layer);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set temp-layer layer (persistent)
    int ret = zmk_input_processor_runtime_set_temp_layer_layer(
        dev, req->layer, true);
    if (ret < 0) {
        LOG_ERR("Failed to set temp-layer layer: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_temp_layer_layer_tag;
    resp->response_type.set_temp_layer_layer =
        (cormoran_rip_SetTempLayerLayerResponse)
            cormoran_rip_SetTempLayerLayerResponse_init_zero;

    return 0;
}

/**
 * Handle setting temp-layer activation delay
 */
static int handle_set_temp_layer_activation_delay(
    const cormoran_rip_SetTempLayerActivationDelayRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting temp-layer activation delay for id=%d to %dms", req->id,
            req->activation_delay_ms);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set temp-layer activation delay (persistent)
    int ret = zmk_input_processor_runtime_set_temp_layer_activation_delay(
        dev, req->activation_delay_ms, true);
    if (ret < 0) {
        LOG_ERR("Failed to set temp-layer activation delay: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type =
        cormoran_rip_Response_set_temp_layer_activation_delay_tag;
    resp->response_type.set_temp_layer_activation_delay =
        (cormoran_rip_SetTempLayerActivationDelayResponse)
            cormoran_rip_SetTempLayerActivationDelayResponse_init_zero;

    return 0;
}

/**
 * Handle setting temp-layer deactivation delay
 */
static int handle_set_temp_layer_deactivation_delay(
    const cormoran_rip_SetTempLayerDeactivationDelayRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting temp-layer deactivation delay for id=%d to %dms", req->id,
            req->deactivation_delay_ms);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set temp-layer deactivation delay (persistent)
    int ret = zmk_input_processor_runtime_set_temp_layer_deactivation_delay(
        dev, req->deactivation_delay_ms, true);
    if (ret < 0) {
        LOG_ERR("Failed to set temp-layer deactivation delay: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type =
        cormoran_rip_Response_set_temp_layer_deactivation_delay_tag;
    resp->response_type.set_temp_layer_deactivation_delay =
        (cormoran_rip_SetTempLayerDeactivationDelayResponse)
            cormoran_rip_SetTempLayerDeactivationDelayResponse_init_zero;

    return 0;
}

/**
 * Handle setting active layers bitmask
 */
static int handle_set_active_layers(
    const cormoran_rip_SetActiveLayersRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting active layers for id=%d to 0x%08x", req->id, req->layers);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set active layers (persistent)
    int ret = zmk_input_processor_runtime_set_active_layers(
        dev, req->layers, true);
    if (ret < 0) {
        LOG_ERR("Failed to set active layers: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_active_layers_tag;
    resp->response_type.set_active_layers =
        (cormoran_rip_SetActiveLayersResponse)
            cormoran_rip_SetActiveLayersResponse_init_zero;

    return 0;
}

/**
 * Handle setting axis snap mode
 */
static int handle_set_axis_snap_mode(
    const cormoran_rip_SetAxisSnapModeRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting axis snap mode for id=%d to %d", req->id, req->mode);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set axis snap mode (persistent)
    int ret = zmk_input_processor_runtime_set_axis_snap_mode(
        dev, req->mode, true);
    if (ret < 0) {
        LOG_ERR("Failed to set axis snap mode: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_axis_snap_mode_tag;
    resp->response_type.set_axis_snap_mode =
        (cormoran_rip_SetAxisSnapModeResponse)
            cormoran_rip_SetAxisSnapModeResponse_init_zero;

    return 0;
}

/**
 * Handle setting axis snap threshold
 */
static int handle_set_axis_snap_threshold(
    const cormoran_rip_SetAxisSnapThresholdRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting axis snap threshold for id=%d to %d", req->id, req->threshold);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set axis snap threshold (persistent)
    int ret = zmk_input_processor_runtime_set_axis_snap_threshold(
        dev, req->threshold, true);
    if (ret < 0) {
        LOG_ERR("Failed to set axis snap threshold: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_axis_snap_threshold_tag;
    resp->response_type.set_axis_snap_threshold =
        (cormoran_rip_SetAxisSnapThresholdResponse)
            cormoran_rip_SetAxisSnapThresholdResponse_init_zero;

    return 0;
}

/**
 * Handle setting axis snap timeout
 */
static int handle_set_axis_snap_timeout(
    const cormoran_rip_SetAxisSnapTimeoutRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Setting axis snap timeout for id=%d to %d ms", req->id, req->timeout_ms);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Input processor not found: id=%d", req->id);
        return -ENODEV;
    }

    // Set axis snap timeout (persistent)
    int ret = zmk_input_processor_runtime_set_axis_snap_timeout(
        dev, req->timeout_ms, true);
    if (ret < 0) {
        LOG_ERR("Failed to set axis snap timeout: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_axis_snap_timeout_tag;
    resp->response_type.set_axis_snap_timeout =
        (cormoran_rip_SetAxisSnapTimeoutResponse)
            cormoran_rip_SetAxisSnapTimeoutResponse_init_zero;

    return 0;
}

/**
 * Handle getting layer information
 */


static bool encode_layer_name(pb_ostream_t *stream, const pb_field_t *field,
                              void *const *arg) {
    const char *name = (const char *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, (const pb_byte_t *)name, strlen(name));
}

static bool encode_layer_info(pb_ostream_t *stream, const pb_field_t *field,
                              void *const *arg) {
    for (int layer_idx = 0; layer_idx < ZMK_KEYMAP_LAYERS_LEN; layer_idx++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);

        if (layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            continue;
        }

        const char *layer_name = zmk_keymap_layer_name(layer_id);
        if (layer_name) {
            cormoran_rip_LayerInfo info = cormoran_rip_LayerInfo_init_zero;
            info.index = layer_idx;
            
            // Set up callback for encoding the name string
            info.name.funcs.encode = encode_layer_name;
            info.name.arg = (void *)layer_name;

            if (!pb_encode_tag_for_field(stream, field)) {
                return false;
            }

            if (!pb_encode_submessage(stream, cormoran_rip_LayerInfo_fields,
                                     &info)) {
                return false;
            }
        }
    }

    return true;
}

static int handle_get_layer_info(
    const cormoran_rip_GetLayerInfoRequest *req,
    cormoran_rip_Response *resp) {
    LOG_DBG("Getting layer information");

    cormoran_rip_GetLayerInfoResponse result =
        cormoran_rip_GetLayerInfoResponse_init_zero;

    // Set up callback for encoding layers
    result.layers.funcs.encode = encode_layer_info;

    resp->which_response_type = cormoran_rip_Response_get_layer_info_tag;
    resp->response_type.get_layer_info = result;
    return 0;
}

#endif  // CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR
