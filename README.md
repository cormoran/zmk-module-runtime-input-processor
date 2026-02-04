# ZMK Runtime Input Processor Module

This ZMK module provides runtime configurable input processors for pointing devices. You can adjust scaling and rotation parameters dynamically through a web interface without rebuilding firmware.

## Features

- **Runtime Configuration**: Adjust input processor parameters without rebuilding firmware
- **Web Interface**: Configure settings through a browser-based UI (currently being updated)
- **Scaling Support**: Configure speed multipliers (e.g., x2 faster, x0.5 slower)
- **Rotation Support**: Apply rotation transformations in degrees (fully implemented with paired X/Y handling)
- **Persistent Settings**: Settings saved to non-volatile storage
- **Multiple Processors**: Support for multiple input processors with individual configuration
- **Short Names**: Processor names limited to 8 characters for BLE efficiency
- **Temporary Changes**: Hold a key to temporarily change settings (perfect for DPI toggle)
- **Temp-Layer Layer**: Automatically activate a layer when using pointing device, deactivate on key press or timeout

## Setup

### 1. Add dependency to your `config/west.yml`

```yaml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-module-runtime-input-processor
      remote: cormoran
      revision: main # or latest commit hash
    # Below setting required to use unofficial studio custom RPC feature
    - name: zmk
      remote: cormoran
      revision: v0.3+custom-studio-protocol
      import:
        file: app/west.yml
```

### 2. Enable the feature in your `config/<shield>.conf`

```conf
CONFIG_ZMK_POINTING=y

# Enable runtime input processor
CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR=y

# Enable studio custom RPC features for web UI
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_STUDIO_RPC=y
```

### 3. Add runtime input processor to your keymap

```dts
#include <dt-bindings/zmk/input.h>
#include <input/processors.dtsi>
#include <input/processors/runtime-input-processor.dtsi>
// The .dtsi provides default device definitions
// - mouse_runtime_input_processor
// - scroll_runtime_input_processor

/ {
    // Then use it in your input device configuration
    my_input_listener {
        // ... other config ...
        input-processors = <&mouse_runtime_input_processor>;

        scroller {
			// layers = <9>;
			input-processors = <&zip_xy_to_scroll_mapper &scroll_runtime_input_processor>;
		};
    };

    // For split keyboard, you can configure input processor in central
    split_input: split_input {
        compatible = "zmk,input-split"
    };
    split_listener: split_listener {
        compatible = "zmk,input-listener";
        status = <disabled>;
        device = <&split_input>;
    };

    my_rip: my_rip {
		compatible = "zmk,input-processor-runtime";
		processor-label = "custom";
		type = <INPUT_EV_REL>;
		x-codes = <INPUT_REL_X>;
		y-codes = <INPUT_REL_Y>;
		scale-multiplier = <1>;
		scale-divisor = <1>;
		rotation-degrees = <0>;
		track-remainders;

		// Optional: Temp-layer layer default settings
		temp-layer-enabled;  // Enable temp-layer by default
		temp-layer-layer = <1>;  // Default to layer 1
		temp-layer-activation-delay-ms = <100>;  // 100ms activation delay
		temp-layer-deactivation-delay-ms = <500>;  // 500ms deactivation delay

		// Optional: Performance optimization
		temp-layer-transparent-behavior = <&trans>;
		temp-layer-kp-behavior = <&kp>;
		temp-layer-keep-keycodes = <LEFT_CONTROL LEFT_SHIFT LEFT_ALT LEFT_GUI RIGHT_CONTROL RIGHT_SHIFT RIGHT_ALT RIGHT_GUI>;

		#input-processor-cells = <0>;
	};
};

// <central>.overlay
&split_listener {
    status = "okay";
    input-processors = <&my_rip>;
};
```

## Usage

### Web Interface

1. Build and flash your firmware with the runtime input processor enabled
2. Connect to your keyboard via Web Serial (Chrome/Edge)
3. The web interface will automatically detect available input processors
4. Adjust scaling and rotation parameters
5. Changes are applied immediately without restarting

### Configuration Parameters

- **Scaling Multiplier/Divisor**: Controls pointer speed
  - Example: `2/1` = 2x faster, `1/2` = 0.5x slower
  - Values are applied as: `output = input * multiplier / divisor`
  - Remainders are tracked for precise scaling

### Example Configurations

**2x Speed:**

```
scale-multiplier = 2
scale-divisor = 1
```

**Half Speed:**

```
scale-multiplier = 1
scale-divisor = 2
```

### Temporary Configuration via Behavior

You can temporarily change input processor settings while holding a key, useful for temporary DPI changes:

```dts
#include <behaviors/runtime-input-processor.dtsi>
/ {


    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                // Use &temp_fast in your keymap
                &hdpi  // Hold this key for 1.5x mouse speed
                &ldpi  // Hold this key for 0.5x mouse speed
                &hscr  // High scroll speed
                &lscr  // Low scroll speed
                // ... other keys
            >;
        };
    };
};
// Customization
&hdpi {
    scale-multiplier = <3>;
	scale-divisor = <2>;
}
```

When you press and hold a key with the temporary config behavior:

1. Current settings are saved
2. Temporary settings are applied
3. When you release the key, original settings are restored

### Temp-Layer Layer

The temp-layer layer feature automatically activates a specified layer when you use your pointing device (trackpad, trackball, etc.) and deactivates it after a period of inactivity or when you press a key.

**Configuration via Device Tree (Optional):**

You can configure default temp-layer settings in your device tree:

```dts
my_pointer_processor: my_pointer_processor {
    compatible = "zmk,input-processor-runtime";
    processor-label = "trackpad";
    // ... basic config ...
    
    // Enable temp-layer with default settings
    temp-layer-enabled;  // Boolean property - presence enables it
    temp-layer-layer = <1>;  // Target layer (default: 0)
    temp-layer-activation-delay-ms = <100>;  // Activation delay (default: 100)
    temp-layer-deactivation-delay-ms = <500>;  // Deactivation delay (default: 500)
};
```

**Configuration via Web UI:**

Temp-layer layer settings can also be configured through the web interface:

- **Enable/Disable**: Toggle the temp-layer layer feature
- **Target Layer**: The layer number to activate (e.g., layer 1, 2, etc.)
- **Activation Delay**: Time to wait after input starts before activating the layer (milliseconds)
- **Deactivation Delay**: Time to wait after input stops before deactivating the layer (milliseconds)

**Behavior:**

- When you move the pointing device, the layer activates after the activation delay
- The layer stays active while you continue using the pointing device
- When you press any keyboard key, the layer deactivates immediately (unless it's a modifier or the key is on the temp-layer layer)
- If you stop moving the pointing device, the layer deactivates after the deactivation delay
- If a key press occurs before the activation delay expires, the layer won't activate

**Keep Temp-Layer Layer Active:**

You can create a behavior to prevent the temp-layer layer from deactivating while holding a key:

```dts
#include <behaviors/runtime-input-processor.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                // temp-layer keep active
                &amka  // Hold this key to keep temp-layer layer active
                // ... other keys
            >;
        };
    };
};
```

When holding the keep-active behavior key, the temp-layer layer will not deactivate when you press other keys or after the timeout period.

## Development Guide

### Setup

There are two west workspace layout options.

#### Option1: Download dependencies in parent directory

This option is west's standard way. Choose this option if you want to re-use dependent projects in other zephyr module development.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
# rm -r .west # if exists to reset workspace
west init -l . --mf tests/west-test.yml
west update --narrow
west zephyr-export
```

The directory structure becomes like below:

```
west-workspace
  - .west/config
  - build : build output directory
  - <this repository>
  # other dependencies
  - zmk
  - zephyr
  - ...
  # You can develop other zephyr modules in this workspace
  - your-other-repo
```

You can switch between modules by removing `west-workspace/.west` and re-executing `west init ...`.

#### Option2: Download dependencies in ./dependencies (Enabled in dev-container)

Choose this option if you want to download dependencies under this directory (like node_modules in npm). This option is useful for specifying cache target in CI. The layout is relatively easy to recognize if you want to isolate dependencies.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-standalone.yml
# If you use dev container, start from below commands. Above commands are executed
# automatically.
west update --narrow
west zephyr-export
```

The directory structure becomes like below:

```
<this repository>
  - .west/config
  - build : build output directory
  - dependencies
    - zmk
    - zephyr
    - ...
```

### Dev container

Dev container is configured for setup option2. The container creates below volumes to re-use resources among containers.

- zmk-dependencies: dependencies dir for setup option2
- zmk-build: build output directory
- zmk-root-user: /root, the same to ZMK's official dev container

### Web UI

Please refer [./web/README.md](./web/README.md).

## Test

**ZMK firmware test**

`./tests` directory contains test config for posix to confirm module functionality and config for xiao board to confirm build works.

Tests can be executed by below command:

```bash
# Run all test case and verify results
python -m unittest
```

If you want to execute west command manually, run below. (for zmk-build, the result is not verified.)

```
# Build test firmware for xiao
# `-m tests/zmk-config .` means tests/zmk-config and this repo are added as additional zephyr module
west zmk-build tests/zmk-config/config -m tests/zmk-config .

# Run zmk test cases
# -m . is required to add this module to build
west zmk-test tests -m .
```

**Web UI test**

The `./web` directory includes Jest tests. See [./web/README.md](./web/README.md#testing) for more details.

```bash
cd web
npm test
```

## Publishing Web UI

### GitHub Pages (Production)

Github actions are pre-configured to publish web UI to github pages.

1. Visit Settings>Pages
1. Set source as "Github Actions"
1. Visit Actions>"Test and Build Web UI"
1. Click "Run workflow"

Then, the Web UI will be available in
`https://<your github account>.github.io/<repository name>/` like https://cormoran.github.io/zmk-module-template-with-custom-studio-rpc.

### Cloudflare Workers (Pull Request Preview)

For previewing web UI changes in pull requests:

1. Create a Cloudflare Workers project and configure secrets:
   - `CLOUDFLARE_API_TOKEN`: API token with Cloudflare Pages edit permission
   - `CLOUDFLARE_ACCOUNT_ID`: Your Cloudflare account ID
   - (Optional) `CLOUDFLARE_PROJECT_NAME`: Project name (defaults to `zmk-module-web-ui`)
   - Enable "Preview URLs" feature in cloudflare the project

2. Optionally set up an `approval-required` environment in github repository settings requiring approval from repository owners

3. Create a pull request with web UI changes - the preview deployment will trigger automatically and wait for approval

## Sync changes in template

By running `Actions > Sync Changes in Template > Run workflow`, pull request is created to your repository to reflect changes in template repository.

If the template contains changes in `.github/workflows/*`, registering your github personal access token as `GH_TOKEN` to repository secret is required.
The fine-grained token requires write to contents, pull-requests and workflows.
Please see detail in [actions-template-sync](https://github.com/AndreasAugustin/actions-template-sync).

## More Info

For more info on modules, you can read through through the
[Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html)
and [ZMK's page on using modules](https://zmk.dev/docs/features/modules).
[Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests)
may also be of use.
