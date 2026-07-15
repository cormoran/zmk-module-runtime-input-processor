# Design: migrate persistence to zmk-feature-custom-settings + reflect template improvements

Status: design (implementation delegated). Owner decisions captured below.

## Goal

Replace this module's hand-rolled Zephyr `settings_save_one` / `SETTINGS_STATIC_HANDLER`
persistence with [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings)
as the storage backend, and reflect the recently-adopted
`zmk-module-template-with-custom-studio-rpc` improvements where they apply.

### Owner decisions (do not re-litigate)

1. **Adoption depth = storage backend swap ONLY.** Keep this module's own custom
   Studio RPC subsystem: its proto (`proto/cormoran/rip/custom.proto`), its
   handler (`src/studio/custom_handler.c`, all ~18 `Set*` / list / get / reset
   RPCs), its `zmk_input_processor_state_changed` event + listener, and its own
   `web/` UI. custom-settings is used purely as a typed persistence library
   underneath the existing handlers — NOT as the editing RPC surface.
2. **Proto backward compatibility MUST be preserved.** `custom.proto` is NOT
   modified (not even additively) as part of this change. Because Design A does
   not touch the RPC layer at all, this is satisfied trivially — the web↔firmware
   wire contract is unchanged.
3. **Keep the layer-info RPC** (`GetLayerInfo` / `LayerInfo`) exactly as-is.
4. No backward compatibility for the *stored on-flash format* is required (old
   `input_proc/<name>` blobs may be abandoned).

## Why this is a small, contained change

Under Design A the RPC layer is untouched. The device runtime struct
`runtime_processor_data` remains the runtime source of truth (behaviors keep
mutating `data->*` directly for temporary effects; `data->persistent_*` keeps
the persisted baseline). Only the *persistence mechanism* beneath the handlers
changes:

- OLD: `struct processor_settings` + debounced `save_processor_settings_work_handler`
  → `settings_save_one("input_proc/<name>", &settings, sizeof)`; load via
  `SETTINGS_STATIC_HANDLER_DEFINE(input_proc, ...)` → `load_processor_settings_cb`.
- NEW: one custom-settings entry per processor holding the same field set as a
  packed blob; save via `zmk_custom_setting_write_by_key(..., PERSIST)`; load via
  a deferred boot-apply that reads the entry back.

Everything in `src/studio/`, `proto/`, `src/events/`, `src/behaviors/`, and the
input-processing hot path in `input_processor_runtime.c` is unchanged.

## Storage model

**One custom-settings entry per processor**, not one per field. Rationale:
Design A keeps the bespoke RPC as the only editing surface, so per-field typed
settings would create a redundant second surface; a single opaque blob per
processor is the minimal, closest analogue of today's single-struct-per-processor
storage and avoids 15×N registration boilerplate.

- Subsystem id: `"cormoran__rip"` (≤ `CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN`=32).
- Key: the processor's `processor_label` DT string literal (already ≤
  `CONFIG_ZMK_RUNTIME_INPUT_PROCESSOR_NAME_MAX_LEN`, well under
  `CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN`=48). No runtime key buffer needed —
  unlike pmw3610's `<field>@<id>`, our key is a compile-time DT literal.
- Value type: `ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES`.
- **Confidentiality: `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE`** — so if
  a firmware also enables custom-settings' *generic* Studio RPC, these entries are
  never listed/edited/exported there. This module's own RPC stays the sole editor.
- Default value: an empty (zero-length) `ZMK_CUSTOM_SETTING_VALUE_BYTES()`, meaning
  "nothing persisted → keep DT defaults".
- Size: the blob is `1 + sizeof(struct rip_persist_v1)` (~40 bytes for the 15
  fields, see below) — comfortably under the fixed
  `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`=64 carrier. **No `LARGE_VALUES` /
  `DEFINE_SIZED` needed.** A `BUILD_ASSERT` enforces this at build time; if the
  struct ever exceeds the carrier, switch to `ZMK_CUSTOM_SETTING_DEFINE_SIZED`.

### Blob format: `[version][raw struct]`

Store a version byte followed by a raw `memcpy` of the on-flash struct
(`struct rip_persist_v1`, the same 15 fields sourced from `data->persistent_*`):

```
byte 0:      version  (start at 1)
bytes 1..:   raw memcpy of struct rip_persist_v1 (sizeof(struct) bytes)

total size = 1 + sizeof(struct rip_persist_v1)
```

The version byte is written *separately* from the struct `memcpy`
(`buf[0] = version; memcpy(&buf[1], &settings, sizeof(settings))`, total size
`1 + sizeof`), rather than memcpy'ing a `{uint8_t version; struct ...;}` wrapper
whole, so no wrapper-struct padding enters the layout.

This is safe because the blob is only ever written and read back by this same
firmware image — the in-memory struct layout is a valid wire format for our own
use. A `BUILD_ASSERT(1 + sizeof(struct rip_persist_v1) <=
CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE)` fails the build (not silently) if the
struct ever outgrows the 64-byte carrier.

On load:
- read size == 0 → no persisted value, keep DT defaults;
- otherwise require BOTH `buf[0] == RIP_SETTINGS_BLOB_VERSION` AND total
  `size == 1 + sizeof(struct rip_persist_v1)`; on any mismatch (e.g. a future
  firmware changed the struct and bumped the version, or a truncated record) →
  `LOG_WRN` and keep DT defaults (never fail boot).

A firmware that changes `struct rip_persist_v1` must bump
`RIP_SETTINGS_BLOB_VERSION`; the old blob is then rejected by the version/size
check and DT defaults are used. There is no on-flash backward-compat requirement
with the old `settings_save_one` blob.

## Save path

Keep the existing per-processor debounced `k_work_delayable save_work` (coalesces
Studio slider spam). Its handler now:
1. Packs `data->persistent_*` into the blob buffer above.
2. `zmk_custom_setting_write_by_key("cormoran__rip", cfg->name, &bytes_value,
   ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST)`.

`schedule_save_processor_settings()` is unchanged (still reschedules `save_work`
with `CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE`). Every `zmk_input_processor_runtime_set_*`
persistent path already calls it — leave those call sites alone.

## Load / boot-apply path — the one tricky part

custom-settings' load handler populates its own RAM value during `settings_load()`
(called from ZMK `main()` **after all SYS_INIT levels**) and does **not** raise a
`zmk_custom_setting_changed` event on load. So the persisted value is only readable
*after* boot completes, and an ordinary `SYS_INIT` hook is too early. The sanctioned
hook is the **`zmk_custom_settings_initialized`** event, which custom-settings raises
exactly once right after that boot `settings_load` pass finishes (i.e. once every
persisted value is populated and readable):

- A `ZMK_LISTENER` / `ZMK_SUBSCRIPTION(zmk_custom_settings_initialized)` runs the
  apply once the event fires. The event is dispatched synchronously from
  custom-settings' boot settings-commit, which does **not** hold
  `custom_settings_lock`, so `zmk_custom_setting_read_by_key(...)` reads back the
  persisted value without deadlocking. (An earlier revision used an
  `APPLICATION`-level `SYS_INIT` that submitted a delayed `k_work` to sidestep the
  race; the event replaces that guess-a-delay hack with a deterministic signal.)
- The listener iterates all processors (`zmk_input_processor_runtime_foreach`),
  reads each one's blob, and if present unpacks into `data->persistent_*` **and**
  the current active values, then `update_rotation_values(data)` — exactly what the
  old `load_processor_settings_cb` did.

**Do NOT register a `zmk_custom_setting_changed` listener that re-applies to the
device.** In Design A the device is the runtime source of truth and the bespoke
RPC handlers already mutate `data->*` directly; a re-apply listener would (a)
clobber behavior-set *temporary* values when an unrelated field is saved, and (b)
risk a write→changed→apply→write recursion. The device→settings direction (save)
plus a one-shot settings→device boot apply is sufficient and matches the "storage
backend only" decision. (This is the deliberate difference from pmw3610/animation,
which use custom-settings as their editing surface and therefore need the listener.)

Because `DEVICE_PRIVATE` entries are not reachable by the generic RPC, generic
save/discard/reset cannot touch them, so nothing external mutates them behind the
module's back — reinforcing that no listener is needed.

## Build / dependency wiring

- **Kconfig**: gate the new persistence on `CONFIG_ZMK_CUSTOM_SETTINGS` instead of
  (or in addition to) `CONFIG_SETTINGS`. Add `select ZMK_CUSTOM_SETTINGS` under
  `ZMK_RUNTIME_INPUT_PROCESSOR` (persistence is core to the feature), or make it a
  documented `depends on`. Keep the code compiling when custom-settings is absent
  (persistence simply unavailable) if that is cheap; otherwise `select` it.
- **west (tests)**: add `zmk-feature-custom-settings` (remote `cormoran`, revision
  `main`) to `west/west-dependency/west-test-dependency.yml` alongside the existing
  `zmk` custom-studio-protocol project.
- **README**: document the new dependency in the dependent's `config/west.yml`
  example and the `CONFIG_ZMK_CUSTOM_SETTINGS=y` requirement.
- **tests/studio/native_sim.conf**: add `CONFIG_ZMK_CUSTOM_SETTINGS=y` (+ the RPC
  option only if a test needs the generic surface — it does not here). The existing
  `CONFIG_SETTINGS=y` stays.

## Tests

- Unit/native_sim: verify a `Set*` RPC persists and survives a simulated reload
  (write → save → re-read via boot-apply). Adapt the existing `tests/studio/`
  snapshot expectations if log lines change (the `input_proc` save/load `LOG_INF`
  strings are replaced).
- Confirm the build enables `CONFIG_ZMK_CUSTOM_SETTINGS` (assert in `test.py`).

## Secondary: reflect template improvements ("as far as possible")

These are independent of the storage swap and lower priority. Do them AFTER the
core swap lands (separate commits; can be a follow-up PR if large). FIRST inspect
the existing `origin/chore/template_sync_0bbe51e` and
`origin/chore/template_sync_921ccb8` branches — they may already carry much of this.

1. **Web modernization** — adopt the template/pmw3610 react-zmk-studio flow: dual
   transport (USB + BLE), unlock prompt/retry, last-connected-serial-port memory,
   footer template credit, BLE "Studio service advertised only when unlocked" hint.
   This is connection-layer only; the rip proto client and editing UI are unchanged
   (proto compat preserved). Bump `react-zmk-studio` / `zmk-studio-ts-client` pins to
   match the template.
2. **Hardware-free Renode CI test** (`tests/renode/`) — boot + core Studio RPC +
   this module's custom RPC, mirroring the template's `tests/renode/renode_test.py`
   and the `renode_smoke_test` build artifact. Wire it into CI.
3. **Dev docs/skills sync** — optionally sync `AGENTS.md`/`CLAUDE.md` and the
   `skills/zmk-module-dev` + `skills/zmk-module-design` guidance from the template
   (this repo currently uses `.github/agents/*`). Lowest priority; only if cheap.

## Files touched (core swap)

- `src/pointing/input_processor_runtime.c` — replace the `#if CONFIG_SETTINGS`
  persistence block (struct, save handler, `SETTINGS_STATIC_HANDLER`, load cb) with
  the pack/write + deferred boot-apply described above. May factor the settings
  glue into a new `src/settings/rip_settings.c` (+ CMake entry) if it reads cleaner,
  but keeping it in the same file is fine.
- `Kconfig`, `west/west-dependency/west-test-dependency.yml`, `README.md`,
  `tests/studio/native_sim.conf`, and snapshot/test files as needed.
- NOT touched: `proto/`, `src/studio/`, `src/events/`, `src/behaviors/`.
</content>
</invoke>
