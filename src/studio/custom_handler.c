/**
 * Runtime Input Processor - Custom Studio RPC Handler
 *
 * This file implements custom RPC subsystem for runtime input processor configuration.
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/template/custom.pb.h>
#include <zmk/pointing/input_processor_runtime.h>
#include <zmk/event_manager.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TEMPLATE_FEATURE_RUNTIME_INPUT_PROCESSOR)

/**
 * Metadata for the custom subsystem.
 */
static struct zmk_rpc_custom_subsystem_meta template_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("http://localhost:5173"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

/**
 * Register the custom RPC subsystem.
 */
ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__template, &template_feature_meta,
                         template_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__template, zmk_template_Response);

// Global buffer for notifications
static zmk_template_Notification notification_buffer;

static int handle_sample_request(const zmk_template_SampleRequest *req,
                                 zmk_template_Response *resp);
static int handle_list_input_processors(const zmk_template_ListInputProcessorsRequest *req,
                                        zmk_template_Response *resp);
static int handle_get_input_processor(const zmk_template_GetInputProcessorRequest *req,
                                      zmk_template_Response *resp);
static int handle_set_scale_multiplier(const zmk_template_SetScaleMultiplierRequest *req,
                                       zmk_template_Response *resp);
static int handle_set_scale_divisor(const zmk_template_SetScaleDivisorRequest *req,
                                    zmk_template_Response *resp);
static int handle_set_rotation(const zmk_template_SetRotationRequest *req,
                               zmk_template_Response *resp);
static int handle_reset_input_processor(const zmk_template_ResetInputProcessorRequest *req,
                                        zmk_template_Response *resp);

/**
 * Main request handler for the custom RPC subsystem.
 */
static bool
template_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                            pb_callback_t *encode_response) {
  zmk_template_Response *resp =
      ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zmk__template,
                                                        encode_response);

  zmk_template_Request req = zmk_template_Request_init_zero;

  // Decode the incoming request from the raw payload
  pb_istream_t req_stream = pb_istream_from_buffer(raw_request->payload.bytes,
                                                   raw_request->payload.size);
  if (!pb_decode(&req_stream, zmk_template_Request_fields, &req)) {
    LOG_WRN("Failed to decode template request: %s", PB_GET_ERROR(&req_stream));
    zmk_template_ErrorResponse err = zmk_template_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "Failed to decode request");
    resp->which_response_type = zmk_template_Response_error_tag;
    resp->response_type.error = err;
    return true;
  }

  int rc = 0;
  switch (req.which_request_type) {
  case zmk_template_Request_sample_tag:
    rc = handle_sample_request(&req.request_type.sample, resp);
    break;
  case zmk_template_Request_list_input_processors_tag:
    rc = handle_list_input_processors(&req.request_type.list_input_processors, resp);
    break;
  case zmk_template_Request_get_input_processor_tag:
    rc = handle_get_input_processor(&req.request_type.get_input_processor, resp);
    break;
  case zmk_template_Request_set_scale_multiplier_tag:
    rc = handle_set_scale_multiplier(&req.request_type.set_scale_multiplier, resp);
    break;
  case zmk_template_Request_set_scale_divisor_tag:
    rc = handle_set_scale_divisor(&req.request_type.set_scale_divisor, resp);
    break;
  case zmk_template_Request_set_rotation_tag:
    rc = handle_set_rotation(&req.request_type.set_rotation, resp);
    break;
  case zmk_template_Request_reset_input_processor_tag:
    rc = handle_reset_input_processor(&req.request_type.reset_input_processor, resp);
    break;
  default:
    LOG_WRN("Unsupported template request type: %d", req.which_request_type);
    rc = -1;
  }

  if (rc != 0) {
    zmk_template_ErrorResponse err = zmk_template_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "Failed to process request");
    resp->which_response_type = zmk_template_Response_error_tag;
    resp->response_type.error = err;
  }
  return true;
}

/**
 * Handle the SampleRequest
 */
static int handle_sample_request(const zmk_template_SampleRequest *req,
                                 zmk_template_Response *resp) {
  LOG_DBG("Received sample request with value: %d", req->value);

  zmk_template_SampleResponse result = zmk_template_SampleResponse_init_zero;

  snprintf(result.value, sizeof(result.value),
           "Hello from firmware! Received: %d", req->value);

  resp->which_response_type = zmk_template_Response_sample_tag;
  resp->response_type.sample = result;
  return 0;
}

// Helper to send processor changed notification
static void send_processor_notification(const struct device *dev) {
    const char *name;
    uint32_t scale_mul, scale_div;
    int32_t rotation;

    int ret = zmk_input_processor_runtime_get_config(dev, &name, &scale_mul, &scale_div, &rotation);
    if (ret < 0) {
        return;
    }

    // Prepare notification
    notification_buffer = (zmk_template_Notification)zmk_template_Notification_init_zero;
    notification_buffer.which_notification_type = zmk_template_Notification_input_processor_changed_tag;
    
    zmk_template_InputProcessorInfo *info = &notification_buffer.notification_type.input_processor_changed.processor;
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->scale_multiplier = scale_mul;
    info->scale_divisor = scale_div;
    info->rotation_degrees = rotation;

    // TODO: Implement notification sending via ZMK event system for peripheral relay
    // This requires implementing event relay infrastructure
    LOG_WRN("Notification prepared but not sent (peripheral relay not implemented): %s", name);
}

// Helper callback to send notification for each processor during list operation
struct list_processors_context {
    int count;
};

static int list_processors_callback(const struct device *dev, void *user_data) {
    struct list_processors_context *ctx = (struct list_processors_context *)user_data;
    send_processor_notification(dev);
    ctx->count++;
    return 0;
}

/**
 * Handle listing all input processors - sends notifications
 */
static int handle_list_input_processors(const zmk_template_ListInputProcessorsRequest *req,
                                       zmk_template_Response *resp) {
    LOG_DBG("Listing input processors via notifications");

    struct list_processors_context ctx = { .count = 0 };

    zmk_input_processor_runtime_foreach(list_processors_callback, &ctx);

    // Return empty/no error response (notifications contain the data)
    resp->which_response_type = zmk_template_Response_set_input_processor_tag;
    resp->response_type.set_input_processor = (zmk_template_SetInputProcessorResponse)
        zmk_template_SetInputProcessorResponse_init_zero;

    LOG_INF("Sent notifications for %d input processors", ctx.count);
    return 0;
}

/**
 * Handle getting a specific input processor configuration
 */
static int handle_get_input_processor(const zmk_template_GetInputProcessorRequest *req,
                                     zmk_template_Response *resp) {
    LOG_DBG("Getting input processor: %s", req->name);

    const struct device *dev = zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
        return -ENODEV;
    }

    zmk_template_GetInputProcessorResponse result = zmk_template_GetInputProcessorResponse_init_zero;

    const char *name;
    uint32_t scale_mul, scale_div;
    int32_t rotation;

    int ret = zmk_input_processor_runtime_get_config(dev, &name, &scale_mul, &scale_div, &rotation);
    if (ret < 0) {
        return ret;
    }

    strncpy(result.processor.name, name, sizeof(result.processor.name) - 1);
    result.processor.name[sizeof(result.processor.name) - 1] = '\0';
    result.processor.scale_multiplier = scale_mul;
    result.processor.scale_divisor = scale_div;
    result.processor.rotation_degrees = rotation;

    resp->which_response_type = zmk_template_Response_get_input_processor_tag;
    resp->response_type.get_input_processor = result;

    return 0;
}

/**
 * Handle setting scale multiplier
 */
static int handle_set_scale_multiplier(const zmk_template_SetScaleMultiplierRequest *req,
                                       zmk_template_Response *resp) {
    LOG_DBG("Setting scale multiplier for %s to %d", req->name, req->value);

    const struct device *dev = zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
        return -ENODEV;
    }

    // Get current divisor
    uint32_t scale_div;
    int ret = zmk_input_processor_runtime_get_config(dev, NULL, NULL, &scale_div, NULL);
    if (ret < 0) {
        return ret;
    }

    // Set new multiplier (persistent)
    ret = zmk_input_processor_runtime_set_scaling(dev, req->value, scale_div, true);
    if (ret < 0) {
        LOG_ERR("Failed to set scale multiplier: %d", ret);
        return ret;
    }

    // Send notification
    send_processor_notification(dev);

    // Return empty response
    resp->which_response_type = zmk_template_Response_set_input_processor_tag;
    resp->response_type.set_input_processor = (zmk_template_SetInputProcessorResponse)
        zmk_template_SetInputProcessorResponse_init_zero;

    return 0;
}

/**
 * Handle setting scale divisor
 */
static int handle_set_scale_divisor(const zmk_template_SetScaleDivisorRequest *req,
                                    zmk_template_Response *resp) {
    LOG_DBG("Setting scale divisor for %s to %d", req->name, req->value);

    const struct device *dev = zmk_input_processor_runtime_find_by_name(req->name);
    if (!dev) {
        LOG_WRN("Input processor not found: %s", req->name);
        return -ENODEV;
    }

    // Get current multiplier
    uint32_t scale_mul;
    int ret = zmk_input_processor_runtime_get_config(dev, NULL, &scale_mul, NULL, NULL);
    if (ret < 0) {
        return ret;
    }

    // Set new divisor (persistent)
    ret = zmk_input_processor_runtime_set_scaling(dev, scale_mul, req->value, true);
    if (ret < 0) {
        LOG_ERR("Failed to set scale divisor: %d", ret);
        return ret;
    }

    // Send notification
    send_processor_notification(dev);

    // Return empty response
    resp->which_response_type = zmk_template_Response_set_input_processor_tag;
    resp->response_type.set_input_processor = (zmk_template_SetInputProcessorResponse)
        zmk_template_SetInputProcessorResponse_init_zero;

    return 0;
}

/**
 * Handle setting rotation
 */
static int handle_set_rotation(const zmk_template_SetRotationRequest *req,
                               zmk_template_Response *resp) {
    LOG_DBG("Setting rotation for %s to %d degrees", req->name, req->value);

    const struct device *dev = zmk_input_processor_runtime_find_by_name(req->name);
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

    // Send notification
    send_processor_notification(dev);

    // Return empty response
    resp->which_response_type = zmk_template_Response_set_input_processor_tag;
    resp->response_type.set_input_processor = (zmk_template_SetInputProcessorResponse)
        zmk_template_SetInputProcessorResponse_init_zero;

    return 0;
}

/**
 * Handle resetting input processor to defaults
 */
static int handle_reset_input_processor(const zmk_template_ResetInputProcessorRequest *req,
                                        zmk_template_Response *resp) {
    LOG_DBG("Resetting input processor: %s", req->name);

    const struct device *dev = zmk_input_processor_runtime_find_by_name(req->name);
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

    // Send notification
    send_processor_notification(dev);

    // Return empty response
    resp->which_response_type = zmk_template_Response_set_input_processor_tag;
    resp->response_type.set_input_processor = (zmk_template_SetInputProcessorResponse)
        zmk_template_SetInputProcessorResponse_init_zero;

    return 0;
}

#endif // CONFIG_ZMK_TEMPLATE_FEATURE_RUNTIME_INPUT_PROCESSOR
