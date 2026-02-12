# Keybind Feature Implementation Summary

## Overview

Successfully implemented a keybind feature for the ZMK runtime input processor module that converts 2D input movement (from trackpad, trackball, etc.) into behavior triggers (key presses) based on movement direction.

## Implementation Details

### 1. Data Structures

#### `runtime_processor_config` (Device Tree Configuration)
- `keybind_behaviors` - Array of behavior device names extracted from DT phandle array
- `keybind_behaviors_len` - Number of behaviors configured
- `initial_keybind_enabled` - Default enabled state
- `initial_keybind_behavior_count` - Default number of active behaviors (1-8)
- `initial_keybind_degree_offset` - Default rotation offset (0-359°)
- `initial_keybind_tick` - Default movement threshold

#### `runtime_processor_data` (Runtime State)
- Current keybind settings (enabled, behavior_count, degree_offset, tick)
- Persistent keybind settings (for save/restore)
- Runtime accumulators (keybind_x_accum, keybind_y_accum)

#### `processor_settings` (Persistent Storage)
- All keybind settings are persisted to non-volatile storage

### 2. Core Logic

#### Movement Accumulation
- Accumulates X and Y movement until threshold is exceeded
- Uses Euclidean distance: `sqrt(x² + y²) >= tick_threshold`

#### Direction Calculation
- Uses `atan2(y, x)` to calculate angle in radians
- Converts to degrees (0-360°)
- Applies user-configurable degree offset for rotation

#### Behavior Selection
The 2D space is divided based on behavior count:
- **1 behavior**: Any movement triggers it
- **2 behaviors**: 180° segments (e.g., horizontal or vertical split)
- **4 behaviors**: 90° segments (cardinal directions)
- **8 behaviors**: 45° segments (8-way directional)

Formula: `behavior_idx = floor((angle + degree_offset + segment_size/2) / segment_size) % behavior_count`

#### Behavior Triggering
- Looks up behavior by name using `zmk_behavior_get_binding()`
- Creates a `zmk_behavior_binding` with the behavior device name
- Invokes press and release events immediately
- Resets X/Y accumulation after triggering

### 3. API Functions

All functions follow the existing pattern with `persistent` parameter:

```c
// Enable/disable keybind mode
int zmk_input_processor_runtime_set_keybind_enabled(dev, enabled, persistent);

// Set number of active behaviors (1-8)
int zmk_input_processor_runtime_set_keybind_behavior_count(dev, count, persistent);

// Set rotation offset (0-359°)
int zmk_input_processor_runtime_set_keybind_degree_offset(dev, offset, persistent);

// Set movement threshold
int zmk_input_processor_runtime_set_keybind_tick(dev, tick, persistent);
```

### 4. Device Tree Integration

#### Bindings (Already existed in `zmk,input-processor-runtime.yaml`)
```yaml
keybind-enabled: boolean property
keybind-behaviors: phandle-array of behaviors
keybind-behavior-count: int (default 4, range 1-8)
keybind-degree-offset: int (default 0, range 0-359)
keybind-tick: int (default 10)
```

#### Macro Implementation
Created helper macros to extract behavior device names from DT phandle array:
- `KEYBIND_BEHAVIOR_NAME(node, idx)` - Gets device name for behavior at index
- `KEYBIND_BEHAVIORS_N(node)` - Expands to N behavior names
- Supports up to 8 behaviors (defined by `MAX_KEYBIND_BEHAVIORS` constant)

### 5. Event Processing

The keybind logic is integrated early in the event handler:
1. Checks if processor is active for current layers
2. If keybind enabled, processes movement and potentially consumes event
3. If event consumed, returns `ZMK_INPUT_PROC_STOP`
4. Otherwise, continues with normal processing (scaling, rotation, etc.)

This ensures keybind mode takes precedence when enabled.

### 6. Code Quality

#### Constants
- `M_PI` - Defined for portability (3.14159265358979323846)
- `MAX_KEYBIND_BEHAVIORS` - Maximum behaviors supported (8)

#### Error Handling
- Validates behavior index bounds
- Checks for NULL behavior devices
- Logs errors with descriptive messages
- Returns appropriate error codes

#### Persistence
- All settings saved to non-volatile storage
- Settings restored on boot
- Reset function restores DT defaults
- Temporary behavior changes supported

## Testing

- ✅ All unit tests pass
- ✅ No compilation errors or warnings
- ✅ CodeQL security scan passed
- ✅ Code review comments addressed

## Documentation

Created `KEYBIND_EXAMPLE.md` with:
- Detailed feature explanation
- Device tree configuration examples
- Use case examples (4-way navigation, 2-way scroll, 8-way gestures, volume control)
- Runtime configuration guide
- API usage examples
- Best practices and tips

## Usage Example

```dts
trackpad_keybind: trackpad_keybind {
    compatible = "zmk,input-processor-runtime";
    processor-label = "tpkb";
    type = <INPUT_EV_REL>;
    x-codes = <INPUT_REL_X>;
    y-codes = <INPUT_REL_Y>;

    // Keybind: 4-way directional gestures
    keybind-enabled;
    keybind-behaviors = <&kp UP &kp LEFT &kp DOWN &kp RIGHT>;
    keybind-behavior-count = <4>;
    keybind-degree-offset = <45>;  // Diagonal directions
    keybind-tick = <20>;

    #input-processor-cells = <0>;
};
```

## Future Enhancements (Not Implemented)

Potential improvements for future versions:
1. Support for more than 8 behaviors
2. Non-uniform angle divisions (custom angles per behavior)
3. Gesture patterns (e.g., swipe then hold)
4. Velocity-based triggering
5. Hysteresis to prevent rapid re-triggering

## Files Modified

1. `src/pointing/input_processor_runtime.c` - Main implementation
2. `include/zmk/pointing/input_processor_runtime.h` - API declarations and config structure
3. `KEYBIND_EXAMPLE.md` - Documentation and examples (new file)
4. `IMPLEMENTATION_SUMMARY.md` - This file (new)

## Performance Considerations

- Keybind processing is O(1) - constant time
- Uses simple integer arithmetic for accumulation
- Trigonometric functions (atan2) only called when threshold exceeded
- Minimal memory overhead (2 int32_t accumulators + settings)
- Event consumption prevents unnecessary downstream processing

## Security Considerations

- Input validation on all public API functions
- Bounds checking on behavior array access
- No buffer overflows or memory leaks
- No use of unsafe functions
- CodeQL scan found no issues
