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
static int handle_set_temp_layer(const cormoran_rip_SetTempLayerRequest *req,
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
        case cormoran_rip_Request_set_temp_layer_tag:
            rc = handle_set_temp_layer(&req.request_type.set_temp_layer, resp);
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

    // Raise event which will be caught by listener and sent as notification
    raise_zmk_input_processor_state_changed(
        (struct zmk_input_processor_state_changed){.name   = name,
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
    LOG_DBG("Getting input processor: %s", req->name);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
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
    LOG_DBG("Setting scale multiplier for %s to %d", req->name, req->value);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
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
    LOG_DBG("Setting scale divisor for %s to %d", req->name, req->value);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
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
    LOG_DBG("Setting rotation for %s to %d degrees", req->name, req->value);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
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
    LOG_DBG("Resetting input processor: %s", req->name);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
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
 * Handle setting temp-layer layer configuration
 */
static int handle_set_temp_layer(const cormoran_rip_SetTempLayerRequest *req,
                                 cormoran_rip_Response *resp) {
    LOG_DBG(
        "Setting temp-layer for %s: enabled=%d, layer=%d, act_delay=%d, "
        "deact_delay=%d",
        req->name, req->enabled, req->layer, req->activation_delay_ms,
        req->deactivation_delay_ms);

    const struct device *dev =
        zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
        return -ENODEV;
    }

    // Set temp-layer configuration (persistent)
    int ret = zmk_input_processor_runtime_set_temp_layer(
        dev, req->enabled, req->layer, req->activation_delay_ms,
        req->deactivation_delay_ms, true);
    if (ret < 0) {
        LOG_ERR("Failed to set temp-layer: %d", ret);
        return ret;
    }

    // Return empty response
    resp->which_response_type = cormoran_rip_Response_set_temp_layer_tag;
    resp->response_type.set_temp_layer = (cormoran_rip_SetTempLayerResponse)
        cormoran_rip_SetTempLayerResponse_init_zero;

    return 0;
}

#endif  // CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR
