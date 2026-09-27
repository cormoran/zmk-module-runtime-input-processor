/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zmk/event_manager.h>

enum zmk_input_processor_inertia_stop_reason {
    ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_UNSPECIFIED = 0,
    ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_SETTLED = 1,
    ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_REVERSE_INPUT = 3,
    ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_LAYER_INACTIVE = 4,
    ZMK_INPUT_PROCESSOR_INERTIA_STOP_REASON_SETTINGS_CHANGED = 5,
};

enum zmk_input_processor_inertia_state {
    ZMK_INPUT_PROCESSOR_INERTIA_STARTED,
    ZMK_INPUT_PROCESSOR_INERTIA_STOPPED,
    ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STARTED,
    ZMK_INPUT_PROCESSOR_INERTIA_FAST_INPUT_STOPPED,
};

struct zmk_input_processor_inertia_state_changed {
    const struct device *dev;
    enum zmk_input_processor_inertia_state state;
    enum zmk_input_processor_inertia_stop_reason stop_reason;
};

ZMK_EVENT_DECLARE(zmk_input_processor_inertia_state_changed);
