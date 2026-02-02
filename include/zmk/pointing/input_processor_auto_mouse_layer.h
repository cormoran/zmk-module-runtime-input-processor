/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

/**
 * @brief Set whether the auto mouse layer should be kept active
 *
 * @param keep_active If true, prevents automatic deactivation of the layer
 */
void zmk_input_processor_auto_mouse_layer_set_keep_active(bool keep_active);
