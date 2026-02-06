/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <stdbool.h>

/**
 * @brief Axis snap mode
 */
enum zmk_input_processor_axis_snap_mode {
    ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_NONE = 0,
    ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_X = 1,
    ZMK_INPUT_PROCESSOR_AXIS_SNAP_MODE_Y = 2,
};

/**
 * @brief Runtime input processor configuration
 */
struct zmk_input_processor_runtime_config {
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
    int32_t rotation_degrees;
    // Temp-layer layer settings
    bool temp_layer_enabled;
    uint8_t temp_layer_layer;
    uint16_t temp_layer_activation_delay_ms;
    uint16_t temp_layer_deactivation_delay_ms;
    // Active layers bitmask (0 = all layers, otherwise each bit represents a layer)
    uint32_t active_layers;
    // Axis snap settings
    uint8_t axis_snap_mode;  // zmk_input_processor_axis_snap_mode
    uint16_t axis_snap_threshold;  // Threshold for unsnapping
    uint16_t axis_snap_timeout_ms;  // Time window for checking threshold
};

/**
 * @brief Set the scaling parameters for a runtime input processor
 *
 * @param dev Pointer to the device structure
 * @param multiplier Scaling multiplier (must be > 0)
 * @param divisor Scaling divisor (must be > 0)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_scaling(const struct device *dev,
                                            uint32_t multiplier,
                                            uint32_t divisor,
                                            bool persistent);

/**
 * @brief Set the rotation angle for a runtime input processor
 *
 * @param dev Pointer to the device structure
 * @param degrees Rotation angle in degrees
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_rotation(const struct device *dev, int32_t degrees,
                                             bool persistent);

/**
 * @brief Reset processor to default values and save to persistent storage
 *
 * @param dev Pointer to the device structure
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_reset(const struct device *dev);

/**
 * @brief Restore persistent values (used after temporary changes from behavior)
 *
 * @param dev Pointer to the device structure
 */
void zmk_input_processor_runtime_restore_persistent(const struct device *dev);

/**
 * @brief Get the current configuration of a runtime input processor
 *
 * @param dev Pointer to the device structure
 * @param name Pointer to store the processor name (can be NULL)
 * @param config Pointer to store the configuration (can be NULL)
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_get_config(const struct device *dev,
                                           const char **name,
                                           struct zmk_input_processor_runtime_config *config);

/**
 * @brief Find a runtime input processor by name
 *
 * @param name Name of the processor to find
 * @return Pointer to the device structure, or NULL if not found
 */
const struct device *zmk_input_processor_runtime_find_by_name(const char *name);

/**
 * @brief Find a runtime input processor by ID
 *
 * @param id ID of the processor to find
 * @return Pointer to the device structure, or NULL if not found
 */
const struct device *zmk_input_processor_runtime_find_by_id(uint8_t id);

/**
 * @brief Get the ID of a runtime input processor
 *
 * @param dev Pointer to the device structure
 * @return ID of the processor, or -1 if not found
 */
int zmk_input_processor_runtime_get_id(const struct device *dev);

/**
 * @brief Iterate over all runtime input processors
 *
 * @param callback Callback function to call for each processor
 * @param user_data User data to pass to the callback
 * @return 0 on success, or the first non-zero value returned by callback
 */
int zmk_input_processor_runtime_foreach(int (*callback)(const struct device *dev, void *user_data),
                                       void *user_data);

/**
 * @brief Set temp-layer layer configuration
 *
 * @param dev Pointer to the device structure
 * @param enabled Whether temp-layer is enabled
 * @param layer Target layer ID for temp-layer
 * @param activation_delay_ms Delay before activating layer after input starts (ms)
 * @param deactivation_delay_ms Delay before deactivating layer after input stops (ms)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_temp_layer(const struct device *dev,
                                               bool enabled,
                                               uint8_t layer,
                                               uint32_t activation_delay_ms,
                                               uint32_t deactivation_delay_ms,
                                               bool persistent);

/**
 * @brief Set temp-layer enabled state
 *
 * @param dev Pointer to the device structure
 * @param enabled Whether temp-layer is enabled
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_temp_layer_enabled(const struct device *dev,
                                                       bool enabled,
                                                       bool persistent);

/**
 * @brief Set temp-layer target layer
 *
 * @param dev Pointer to the device structure
 * @param layer Target layer ID for temp-layer
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_temp_layer_layer(const struct device *dev,
                                                     uint8_t layer,
                                                     bool persistent);

/**
 * @brief Set temp-layer activation delay
 *
 * @param dev Pointer to the device structure
 * @param activation_delay_ms Delay before activating layer after input starts (ms)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_temp_layer_activation_delay(
    const struct device *dev, uint32_t activation_delay_ms, bool persistent);

/**
 * @brief Set temp-layer deactivation delay
 *
 * @param dev Pointer to the device structure
 * @param deactivation_delay_ms Delay before deactivating layer after input stops (ms)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_temp_layer_deactivation_delay(
    const struct device *dev, uint32_t deactivation_delay_ms, bool persistent);

/**
 * @brief Set active layers bitmask
 *
 * @param dev Pointer to the device structure
 * @param layers Bitmask of layers where processor should be active (0 = all layers)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_active_layers(const struct device *dev,
                                                  uint32_t layers,
                                                  bool persistent);

/**
 * @brief Notify temp-layer to keep the layer active (called by behavior)
 *
 * @param dev Pointer to the device structure
 * @param keep_active If true, prevent temp-layer from deactivating; if false, allow deactivation
 */
void zmk_input_processor_runtime_temp_layer_keep_active(const struct device *dev, bool keep_active);

/**
 * @brief Set axis snap mode
 *
 * @param dev Pointer to the device structure
 * @param mode Snap mode (NONE, X, or Y)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_axis_snap_mode(const struct device *dev,
                                                    uint8_t mode,
                                                    bool persistent);

/**
 * @brief Set axis snap threshold
 *
 * @param dev Pointer to the device structure
 * @param threshold Threshold value for unsnapping
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_axis_snap_threshold(const struct device *dev,
                                                         uint16_t threshold,
                                                         bool persistent);

/**
 * @brief Set axis snap timeout
 *
 * @param dev Pointer to the device structure
 * @param timeout_ms Time window for checking threshold (ms)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_axis_snap_timeout(const struct device *dev,
                                                       uint16_t timeout_ms,
                                                       bool persistent);

/**
 * @brief Set all axis snap configuration
 *
 * @param dev Pointer to the device structure
 * @param mode Snap mode (NONE, X, or Y)
 * @param threshold Threshold value for unsnapping
 * @param timeout_ms Time window for checking threshold (ms)
 * @param persistent If true, save to persistent storage; if false, temporary
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_runtime_set_axis_snap(const struct device *dev,
                                               uint8_t mode,
                                               uint16_t threshold,
                                               uint16_t timeout_ms,
                                               bool persistent);
