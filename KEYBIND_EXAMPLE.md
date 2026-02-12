# Keybind Feature Example

This document demonstrates how to use the keybind feature in the ZMK Runtime Input Processor module.

## Overview

The keybind feature converts 2D input movement (from trackpad, trackball, etc.) into behavior triggers (key presses) based on movement direction. This is useful for creating gesture-based controls for your keyboard.

## Features

- Support for 1-8 behaviors defined in device tree
- Runtime configurable via web UI:
  - Enable/disable keybind mode
  - Number of behaviors to use (1-8)
  - Degree offset to rotate the 2D division (0-359)
  - Tick threshold for triggering behaviors
- Automatic reset of accumulation after triggering

## How it Works

### Direction Division

The 2D space is divided based on the number of behaviors configured:

- **1 behavior**: Trigger on any movement
- **2 behaviors**: 180° each (e.g., right and left, or up and down with offset)
- **4 behaviors**: 90° each (right, up, left, down)
- **8 behaviors**: 45° each (8 directions)

### Degree Offset

The degree offset rotates the entire division:
- With 2 behaviors and offset=0: y>0 (up) and y<0 (down)
- With 2 behaviors and offset=90: x>0 (right) and x<0 (left)
- With 4 behaviors and offset=0: right (0°), up (90°), left (180°), down (270°)
- With 4 behaviors and offset=45: up-right, up-left, down-left, down-right

## Device Tree Configuration

```dts
#include <dt-bindings/zmk/input.h>
#include <behaviors.dtsi>
#include <input/processors.dtsi>
#include <input/processors/runtime-input-processor.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";

        default_layer {
            bindings = <
                &kp UP
                &kp DOWN
                &kp LEFT
                &kp RIGHT
                // ... other keys
            >;
        };
    };

    // Define the runtime input processor with keybind behaviors
    trackpad_keybind: trackpad_keybind {
        compatible = "zmk,input-processor-runtime";
        processor-label = "tpkb";
        type = <INPUT_EV_REL>;
        x-codes = <INPUT_REL_X>;
        y-codes = <INPUT_REL_Y>;

        // Keybind configuration
        keybind-enabled;  // Enable keybind by default
        keybind-behaviors = <&kp UP &kp LEFT &kp DOWN &kp RIGHT>;
        keybind-behavior-count = <4>;  // Use 4 behaviors (90° each)
        keybind-degree-offset = <45>;  // Rotate 45° so directions are diagonal
        keybind-tick = <20>;  // Movement threshold

        // Other settings
        scale-multiplier = <1>;
        scale-divisor = <1>;

        #input-processor-cells = <0>;
    };

    // Use it in your input listener
    trackpad_listener {
        compatible = "zmk,input-listener";
        device = <&trackpad>;
        input-processors = <&trackpad_keybind>;
    };
};
```

## Example Use Cases

### 1. Directional Navigation (4-way)

```dts
keybind-behaviors = <&kp UP &kp LEFT &kp DOWN &kp RIGHT>;
keybind-behavior-count = <4>;
keybind-degree-offset = <45>;  // Diagonal directions
```

With this configuration:
- Swipe up-right → UP
- Swipe up-left → LEFT
- Swipe down-left → DOWN
- Swipe down-right → RIGHT

### 2. Scroll Navigation (2-way)

```dts
keybind-behaviors = <&kp PG_UP &kp PG_DN>;
keybind-behavior-count = <2>;
keybind-degree-offset = <90>;  // Horizontal division
```

- Swipe right → Page Up
- Swipe left → Page Down

### 3. 8-Way Gesture Control

```dts
keybind-behaviors = <
    &kp N1  // Right
    &kp N2  // Up-right
    &kp N3  // Up
    &kp N4  // Up-left
    &kp N5  // Left
    &kp N6  // Down-left
    &kp N7  // Down
    &kp N8  // Down-right
>;
keybind-behavior-count = <8>;
keybind-degree-offset = <0>;
```

### 4. Volume Control with Layers

```dts
// Use with behaviors that support parameters
keybind-behaviors = <&kp C_VOL_UP &kp C_VOL_DN>;
keybind-behavior-count = <2>;
keybind-degree-offset = <90>;  // Horizontal swipe for volume
```

## Runtime Configuration via Web UI

Once your firmware is built and flashed, you can adjust keybind settings through the web interface:

1. **Enable/Disable Keybind**: Toggle keybind mode on/off
2. **Behavior Count**: Change how many behaviors to use (1-8)
3. **Degree Offset**: Rotate the division (0-359°)
4. **Tick Threshold**: Adjust sensitivity (higher = less sensitive)

All changes are saved to persistent storage.

## API Functions

You can also control keybind settings programmatically:

```c
// Enable/disable keybind
zmk_input_processor_runtime_set_keybind_enabled(dev, true, true);

// Set behavior count
zmk_input_processor_runtime_set_keybind_behavior_count(dev, 4, true);

// Set degree offset
zmk_input_processor_runtime_set_keybind_degree_offset(dev, 45, true);

// Set tick threshold
zmk_input_processor_runtime_set_keybind_tick(dev, 20, true);
```

## Tips

1. **Start with 4 behaviors**: It's the most intuitive for directional gestures
2. **Adjust tick threshold**: If gestures are too sensitive, increase the tick value
3. **Use degree offset creatively**: Rotate divisions to match your natural swipe directions
4. **Combine with layers**: Use different keybind configurations on different layers
5. **Test with web UI**: Use the web interface to experiment with settings before committing to device tree values

## Notes

- Keybind mode consumes input events, so scaled/rotated output won't be sent
- When keybind is disabled, the processor works normally (scaling, rotation, etc.)
- Accumulation resets after each behavior trigger
- The feature works alongside other runtime processor features (temp-layer, active layers, etc.)
