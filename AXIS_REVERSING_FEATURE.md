# Axis Reversing Feature Implementation

## Overview

This feature adds the ability to reverse/invert input axis values at runtime, allowing users to flip the direction of X and Y axes independently. For example, when X-axis inversion is enabled, an input value of `2` becomes `-2` and vice versa.

## Implementation Details

### Processing Order

The axis inversion is applied **after rotation** and **before axis snapping**, following this sequence:

1. **Input received** - Raw input values from device
2. **Rotation applied** - If configured (e.g., 90°, 180°, etc.)
3. **Axis inversion applied** ✨ **[NEW FEATURE]**
4. **Axis snapping applied** - If configured (lock to X or Y axis)
5. **Scaling applied** - Multiply/divide for speed adjustment

This order ensures that:
- Inversion works correctly with rotated inputs
- Axis snapping operates on the final (inverted) values
- Scaling applies to the correct direction

### Code Changes

#### 1. Core Data Structures (`src/pointing/input_processor_runtime.c`)

Added to `runtime_processor_config`:
```c
bool initial_x_invert;  // Default X inversion from DT
bool initial_y_invert;  // Default Y inversion from DT
```

Added to `runtime_processor_data`:
```c
bool x_invert;            // Current X inversion state
bool y_invert;            // Current Y inversion state
bool persistent_x_invert; // Persistent X inversion (saved to storage)
bool persistent_y_invert; // Persistent Y inversion (saved to storage)
```

#### 2. Event Handler Logic

In `runtime_processor_handle_event()` (lines 335-339):
```c
// Apply axis inversion after rotation
if ((is_x && data->x_invert) || (!is_x && data->y_invert)) {
    event->value = -event->value;
}
value = event->value;
```

This simple logic:
- Checks if the current axis should be inverted
- Negates the value if inversion is enabled
- Updates the value variable for subsequent processing

#### 3. API Functions (`include/zmk/pointing/input_processor_runtime.h`)

Added public API:
```c
int zmk_input_processor_runtime_set_x_invert(const struct device *dev,
                                              bool invert,
                                              bool persistent);

int zmk_input_processor_runtime_set_y_invert(const struct device *dev,
                                              bool invert,
                                              bool persistent);
```

#### 4. Device Tree Support

Updated `dts/bindings/input_processors/zmk,input-processor-runtime.yaml`:
```yaml
x-invert:
  type: boolean
  description: If present, invert X axis values

y-invert:
  type: boolean
  description: If present, invert Y axis values
```

Example usage in DTS:
```dts
my_processor: my_processor {
    compatible = "zmk,input-processor-runtime";
    processor-label = "mouse";
    type = <INPUT_EV_REL>;
    x-codes = <INPUT_REL_X>;
    y-codes = <INPUT_REL_Y>;
    x-invert;  // Enable X-axis inversion by default
    // y-invert can be omitted if not needed
};
```

#### 5. Protobuf/RPC Support

Added to `proto/cormoran/rip/custom.proto`:
```protobuf
message InputProcessorInfo {
    // ... existing fields ...
    bool x_invert = 14;  // Whether to invert X axis
    bool y_invert = 15;  // Whether to invert Y axis
}

message SetXInvertRequest {
    uint32 id = 1;
    bool invert = 2;
}

message SetYInvertRequest {
    uint32 id = 1;
    bool invert = 2;
}
```

#### 6. Web UI

Added checkbox controls in `web/src/App.tsx`:
- "Invert X Axis" checkbox
- "Invert Y Axis" checkbox
- Both placed in a dedicated "Axis Inversion" section
- Properly integrated with state management and RPC calls

## Configuration Options

### 1. Device Tree (Default/Initial State)

Set default inversion state at compile time:

```dts
mouse_runtime_input_processor: mouse_runtime_input_processor {
    compatible = "zmk,input-processor-runtime";
    processor-label = "mouse";
    type = <INPUT_EV_REL>;
    x-codes = <INPUT_REL_X>;
    y-codes = <INPUT_REL_Y>;
    
    // Invert X axis by default
    x-invert;
    
    // Y axis not inverted by default (omit property)
};
```

### 2. Web UI (Runtime Configuration)

1. Connect to your ZMK device via USB/Serial
2. Open the web interface
3. Select the input processor to configure
4. Scroll to the "Axis Inversion" section
5. Check/uncheck "Invert X Axis" or "Invert Y Axis"
6. Click "Apply Settings"
7. Settings are automatically saved to persistent storage

### 3. Programmatic API

Use the C API in your firmware:

```c
// Get the processor device
const struct device *processor = zmk_input_processor_runtime_find_by_name("mouse");

// Enable X-axis inversion (persistent)
zmk_input_processor_runtime_set_x_invert(processor, true, true);

// Enable Y-axis inversion (temporary - will be reset on reboot)
zmk_input_processor_runtime_set_y_invert(processor, true, false);
```

## Testing

### Manual Testing Steps

1. **Build the firmware** with the updated module
2. **Flash to device** and connect via USB
3. **Open web UI** and connect to device
4. **Test X-axis inversion:**
   - Move mouse/trackball horizontally
   - Enable "Invert X Axis"
   - Verify horizontal movement is reversed
5. **Test Y-axis inversion:**
   - Move mouse/trackball vertically  
   - Enable "Invert Y Axis"
   - Verify vertical movement is reversed
6. **Test combined with rotation:**
   - Set rotation to 90°
   - Enable X or Y inversion
   - Verify rotation is applied first, then inversion
7. **Test persistence:**
   - Enable inversion
   - Disconnect and reconnect device
   - Verify settings are preserved

### Expected Behavior Examples

**Without inversion:**
- Input: `x=5, y=3` → Output: `x=5, y=3`

**With X-axis inversion:**
- Input: `x=5, y=3` → Output: `x=-5, y=3`

**With Y-axis inversion:**
- Input: `x=5, y=3` → Output: `x=5, y=-3`

**With both axes inverted:**
- Input: `x=5, y=3` → Output: `x=-5, y=-3`

**With 90° rotation + X-axis inversion:**
1. Input: `x=5, y=3`
2. After rotation: `x=-3, y=5` (rotated)
3. After X-inversion: `x=3, y=5` (X axis inverted)
4. Output: `x=3, y=5`

## Implementation Notes

### Thread Safety

- All API functions properly update both current and persistent values
- Settings save is scheduled via work queue to debounce writes
- State changes trigger event notifications for web UI updates

### Memory Impact

- Added 4 booleans to config struct (4 bytes, read-only in flash)
- Added 4 booleans to data struct (4 bytes per processor instance in RAM)
- Minimal overhead per processor

### Performance Impact

- Single branch check per axis per event
- Simple negation operation (1 CPU instruction)
- Negligible performance impact

### Backward Compatibility

- New fields default to `false` (no inversion)
- Existing configurations work without modification
- Optional DT properties - only specify if needed

## Related Features

This feature works seamlessly with:
- **Rotation**: Applied before inversion
- **Axis Snapping**: Applied after inversion  
- **Scaling**: Applied after inversion
- **Temp-Layer**: Independent feature, no interaction
- **Active Layers**: Inversion respects layer activation

## Troubleshooting

**Inversion doesn't work:**
- Verify processor is active for current layer
- Check web UI shows the correct state
- Try toggling inversion off and on again

**Settings not saved:**
- Ensure `CONFIG_SETTINGS=y` in your config
- Check flash has enough space for settings
- Verify no errors in logs during save

**Unexpected behavior with rotation:**
- Remember: rotation is applied BEFORE inversion
- Test with rotation disabled to isolate the issue
- Verify rotation angle is correct

## Future Enhancements

Potential improvements:
- Add behavior binding for temporary inversion toggle
- Support per-layer inversion overrides
- Add velocity-based inversion threshold
