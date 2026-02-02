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

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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