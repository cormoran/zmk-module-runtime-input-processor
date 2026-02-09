/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <stdbool.h>

/**
 * @brief Keybind processor configuration
 */
struct zmk_input_processor_keybind_config {
    int32_t tick;           // Movement units needed per activation
    int32_t degree_offset;  // Degree offset to rotate split points (0-359)
    bool track_remainders;  // Whether to track remainders
    uint32_t wait_ms;       // Delay before next activation
    uint32_t tap_ms;        // Press-to-release timing
    uint32_t active_layers; // Active layers bitmask (0 = all layers)
};

/**
 * @brief Find a keybind processor by name
 *
 * @param name Name of the processor to find
 * @return Pointer to the device structure, or NULL if not found
 */
const struct device *zmk_input_processor_keybind_find_by_name(const char *name);

/**
 * @brief Get the current configuration of a keybind processor
 *
 * @param dev Pointer to the device structure
 * @param name Pointer to store the processor name (can be NULL)
 * @param config Pointer to store the configuration (can be NULL)
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_keybind_get_config(const struct device *dev, const char **name,
                                           struct zmk_input_processor_keybind_config *config);

/**
 * @brief Set tick value for a keybind processor
 *
 * @param dev Pointer to the device structure
 * @param tick Movement units needed per activation
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_keybind_set_tick(const struct device *dev, int32_t tick);

/**
 * @brief Set degree offset for a keybind processor
 *
 * @param dev Pointer to the device structure
 * @param degree_offset Degree offset to rotate split points (0-359)
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_keybind_set_degree_offset(const struct device *dev, int32_t degree_offset);

/**
 * @brief Set wait delay for a keybind processor
 *
 * @param dev Pointer to the device structure
 * @param wait_ms Delay before next activation (milliseconds)
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_keybind_set_wait_ms(const struct device *dev, uint32_t wait_ms);

/**
 * @brief Set tap timing for a keybind processor
 *
 * @param dev Pointer to the device structure
 * @param tap_ms Press-to-release timing (milliseconds)
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_keybind_set_tap_ms(const struct device *dev, uint32_t tap_ms);

/**
 * @brief Set active layers bitmask for a keybind processor
 *
 * @param dev Pointer to the device structure
 * @param layers Bitmask of layers where processor should be active (0 = all layers)
 * @return 0 on success, negative error code on failure
 */
int zmk_input_processor_keybind_set_active_layers(const struct device *dev, uint32_t layers);

/**
 * @brief Iterate over all keybind processors
 *
 * @param callback Callback function to call for each processor
 * @param user_data User data to pass to the callback
 * @return 0 on success, or the first non-zero value returned by callback
 */
int zmk_input_processor_keybind_foreach(int (*callback)(const struct device *dev, void *user_data),
                                        void *user_data);
