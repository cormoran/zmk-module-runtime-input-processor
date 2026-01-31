/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/pointing/input_processor_runtime.h>

struct zmk_input_processor_state_changed {
    const char *name;
    struct zmk_input_processor_runtime_config config;
};

ZMK_EVENT_DECLARE(zmk_input_processor_state_changed);
