/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <stdbool.h>

/**
 * @brief Runtime input processor configuration
 */
struct zmk_input_processor_runtime_config {
    uint32_t scale_multiplier;
    uint32_t scale_divisor;
    int32_t rotation_degrees;
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
 * @brief Iterate over all runtime input processors
 *
 * @param callback Callback function to call for each processor
 * @param user_data User data to pass to the callback
 * @return 0 on success, or the first non-zero value returned by callback
 */
int zmk_input_processor_runtime_foreach(int (*callback)(const struct device *dev, void *user_data),
                                       void *user_data);
