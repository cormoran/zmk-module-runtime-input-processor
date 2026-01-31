/**
 * Template Feature - Custom Studio RPC Handler
 *
 * This file implements a custom RPC subsystem for ZMK Studio.
 * It demonstrates the minimum code needed to handle custom RPC requests.
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/template/custom.pb.h>
#include <zmk/pointing/input_processor_runtime.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TEMPLATE_FEATURE_RUNTIME_INPUT_PROCESSOR)

/**
 * Metadata for the custom subsystem.
 * - ui_urls: URLs where the custom UI can be loaded from
 * - security: Security level for the RPC handler
 */
static struct zmk_rpc_custom_subsystem_meta template_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("http://localhost:5173"),
    // Unsecured is suggested by default to avoid unlocking in un-reliable
    // environments.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

/**
 * Register the custom RPC subsystem.
 * The first argument is the subsystem name used to route requests from the
 * frontend. Format: <namespace>__<feature> (double underscore)
 */
ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__template, &template_feature_meta,
                         template_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__template, zmk_template_Response);

static int handle_sample_request(const zmk_template_SampleRequest *req,
                                 zmk_template_Response *resp);
static int handle_list_input_processors(const zmk_template_ListInputProcessorsRequest *req,
                                        zmk_template_Response *resp);
static int handle_get_input_processor(const zmk_template_GetInputProcessorRequest *req,
                                      zmk_template_Response *resp);
static int handle_set_input_processor(const zmk_template_SetInputProcessorRequest *req,
                                      zmk_template_Response *resp);
#endif

/**
 * Main request handler for the custom RPC subsystem.
 * Sets up the encoding callback for the response.
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
#if IS_ENABLED(CONFIG_ZMK_TEMPLATE_FEATURE_RUNTIME_INPUT_PROCESSOR)
  case zmk_template_Request_list_input_processors_tag:
    rc = handle_list_input_processors(&req.request_type.list_input_processors, resp);
    break;
  case zmk_template_Request_get_input_processor_tag:
    rc = handle_get_input_processor(&req.request_type.get_input_processor, resp);
    break;
  case zmk_template_Request_set_input_processor_tag:
    rc = handle_set_input_processor(&req.request_type.set_input_processor, resp);
    break;
#endif
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
 * Handle the SampleRequest and populate the response.
 */
static int handle_sample_request(const zmk_template_SampleRequest *req,
                                 zmk_template_Response *resp) {
  LOG_DBG("Received sample request with value: %d", req->value);

  zmk_template_SampleResponse result = zmk_template_SampleResponse_init_zero;

  // Create a simple response string based on the request value
  snprintf(result.value, sizeof(result.value),
           "Hello from firmware! Received: %d", req->value);

  resp->which_response_type = zmk_template_Response_sample_tag;
  resp->response_type.sample = result;
  return 0;
}

#if IS_ENABLED(CONFIG_ZMK_TEMPLATE_FEATURE_RUNTIME_INPUT_PROCESSOR)

// Callback helper for listing processors
struct list_processors_context {
  zmk_template_ListInputProcessorsResponse *response;
  size_t count;
};

static int list_processors_callback(const struct device *dev, void *user_data) {
  struct list_processors_context *ctx = (struct list_processors_context *)user_data;
  
  if (ctx->count >= ARRAY_SIZE(ctx->response->processors)) {
    LOG_WRN("Too many processors, truncating list");
    return 1; // Stop iteration
  }

  const char *name;
  uint32_t scale_mul, scale_div;
  int32_t rotation;

  int ret = zmk_input_processor_runtime_get_config(dev, &name, &scale_mul, &scale_div, &rotation);
  if (ret < 0) {
    return 0; // Continue iteration
  }

  zmk_template_InputProcessorInfo *info = &ctx->response->processors[ctx->count];
  strncpy(info->name, name, sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  info->scale_multiplier = scale_mul;
  info->scale_divisor = scale_div;
  info->rotation_degrees = rotation;

  ctx->count++;
  return 0;
}

/**
 * Handle listing all runtime input processors
 */
static int handle_list_input_processors(const zmk_template_ListInputProcessorsRequest *req,
                                       zmk_template_Response *resp) {
  LOG_DBG("Listing input processors");

  zmk_template_ListInputProcessorsResponse result = zmk_template_ListInputProcessorsResponse_init_zero;
  
  struct list_processors_context ctx = {
    .response = &result,
    .count = 0,
  };

  zmk_input_processor_runtime_foreach(list_processors_callback, &ctx);

  result.processors_count = ctx.count;
  
  resp->which_response_type = zmk_template_Response_list_input_processors_tag;
  resp->response_type.list_input_processors = result;
  
  LOG_INF("Listed %d input processors", ctx.count);
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
 * Handle setting input processor parameters
 */
static int handle_set_input_processor(const zmk_template_SetInputProcessorRequest *req,
                                     zmk_template_Response *resp) {
  LOG_DBG("Setting input processor: %s", req->name);

  const struct device *dev = zmk_input_processor_runtime_find_by_name(req->name);
  if (!dev) {
    LOG_WRN("Input processor not found: %s", req->name);
    return -ENODEV;
  }

  // Update scaling if provided
  if (req->scale_multiplier > 0 && req->scale_divisor > 0) {
    int ret = zmk_input_processor_runtime_set_scaling(dev, req->scale_multiplier, req->scale_divisor);
    if (ret < 0) {
      LOG_ERR("Failed to set scaling: %d", ret);
      return ret;
    }
  }

  // Update rotation if provided (>= 0)
  if (req->rotation_degrees >= -360 && req->rotation_degrees <= 360) {
    int ret = zmk_input_processor_runtime_set_rotation(dev, req->rotation_degrees);
    if (ret < 0) {
      LOG_ERR("Failed to set rotation: %d", ret);
      return ret;
    }
  }

  // Return updated configuration
  zmk_template_SetInputProcessorResponse result = zmk_template_SetInputProcessorResponse_init_zero;
  
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

  resp->which_response_type = zmk_template_Response_set_input_processor_tag;
  resp->response_type.set_input_processor = result;

  LOG_INF("Updated processor %s: scale=%d/%d, rotation=%d", 
          name, scale_mul, scale_div, rotation);
  
  return 0;
}

#endif // CONFIG_ZMK_TEMPLATE_FEATURE_RUNTIME_INPUT_PROCESSOR