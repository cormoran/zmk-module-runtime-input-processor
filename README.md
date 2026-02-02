# ZMK Module Template with Custom Web UI

This repository contains a template for a ZMK module with Web UI by using
**unofficial** custom studio rpc protocol.

Basic usage is the same to official template. Read through the
[ZMK Module Creation](https://zmk.dev/docs/development/module-creation) page for
details on how to configure this template.

### Supporting custom studio RPC protocol

This template contains sample implementation. Please edit and rename below files
to implement your protocol.

- proto `proto/zmk/template/custom.proto` and `custom.options`
- handler `src/studio/custom_handler.c`
- flags in `Kconfig`
- test `./tests/studio`

### Implementing Web UI for the custom protocol

`./web` contains boilerplate based on
[vite template `react-ts`](https://github.com/vitejs/vite/tree/main/packages/create-vite/template-react-ts)
(`npm create vite@latest web -- --template react-ts`) and react hook library
[@cormoran/react-zmk-studio](https://github.com/cormoran/react-zmk-studio).

Please refer
[react-zmk-studio README](https://github.com/cormoran/react-zmk-studio/blob/main/README.md).

## Setup (Please edit!)

You can use this zmk-module with below setup.

1. Add dependency to your `config/west.yml`.

   ```yaml:config/west.yml
   # Please update with your account and repository name after create repository from template
   manifest:
   remotes:
       ...
       - name: cormoran
       url-base: https://github.com/cormoran
   projects:
       ...
       - name: zmk-module-template-with-custom-studio-rpc
       remote: cormoran
       revision: main # or latest commit hash
       # import: true # if this module has other dependencies
       ...
       # Below setting required to use unofficial studio custom PRC feature
       - name: zmk
       remote: cormoran
       revision: v0.3+custom-studio-protocol
       import:
           file: app/west.yml
   ```

1. Enable flag in your `config/<shield>.conf`

   ```conf:<shield>.conf
   # Enable standalone features
   CONFIG_ZMK_TEMPLATE_FEATURE=y

   # Optionally enable studio custom RPC features
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_TEMPLATE_FEATURE_STUDIO_RPC=y
   ```

1. Update your `<keyboard>.keymap` like .....

   ```
   / {
       ...
   }
   ```

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
