/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZMK_DT_BINDINGS_INPUT_PROCESSOR_H_
#define ZMK_DT_BINDINGS_INPUT_PROCESSOR_H_

/**
 * @brief Axis snap modes for runtime input processor
 *
 * These constants can be used in device tree files to configure
 * axis snapping behavior.
 */

/** No axis snapping */
#define AXIS_SNAP_MODE_NONE 0

/** Snap to X axis (horizontal only) */
#define AXIS_SNAP_MODE_X 1

/** Snap to Y axis (vertical only) */
#define AXIS_SNAP_MODE_Y 2

#endif /* ZMK_DT_BINDINGS_INPUT_PROCESSOR_H_ */
