#!/usr/bin/env python3
"""Hardware-free functional test: boot this module's firmware in the Renode
emulator and exercise its own custom Studio RPC subsystem (`cormoran_rip`)
end to end.

How it's wired together (see README.md's "Hardware-free Renode testing"
section for the full story):
  - `ZMK_RENODE_ELF` (env var) points at the firmware ELF the action already
    built with the Renode Studio-RPC-over-UART overlay + transport (real
    hardware normally carries Studio RPC over USB; Renode's USB model is a
    non-functional register stub, so testing under emulation swaps in a
    wired-UART carrier with identical RPC framing -- see zmk-workspace's
    skills/test-zmk-renode/SKILL.md for why).
  - `renode_harness` (a module from that same zmk-workspace checkout) is
    importable via PYTHONPATH -- the action sets this up. It provides
    RenodeSession/boot_single/wait_for_text/proto compiling, so this file
    doesn't need to reimplement any of the Renode-specific plumbing.
  - The custom RPC "envelope": ZMK Studio's `zmk.custom` subsystem is a
    generic pass-through -- a module's own proto messages travel as opaque
    `bytes` inside `zmk.custom.CallRequest.payload`/`CallResponse.payload`,
    addressed by a runtime-assigned `subsystem_index` (see dependencies'
    zmk-studio-messages proto/zmk/custom.proto). This module registers
    itself under the fixed string identifier "cormoran_rip"
    (src/studio/custom_handler.c,
    `ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran_rip, ...)`), always as the first (and,
    in the test build, only) registered subsystem, i.e. index 0.

*** KNOWN RENODE-ENVIRONMENT LIMITATION (see zmk-module-template-with-
custom-studio-rpc's and zmk-feature-fast-keymap's tests/renode/renode_test.py,
found 2026-07-08; reproduced here 2026-07-09 with a twist specific to this
module -- see below) ***
Under Renode -- and, as far as we know, ONLY under Renode -- the Studio RPC
TX path can stall and stop delivering frames once enough data (or enough
separate messages) goes out back-to-back over the emulated UART. The
template and fast-keymap found this triggered by a single callback-encoded
custom-subsystem *response* past a few tens of bytes. During the hang the
firmware is NOT crashed: Renode's `sysbus.cpu ExecutedInstructions` keeps
growing steadily and `sysbus.cpu PC` samples land inside
`ring_buf_area_claim`/`ring_buf_area_finish`
(dependencies/zephyr/lib/utils/ring_buffer.c), consistent with the TX path
waiting on a ring buffer that never drains -- see the template's
tests/renode/renode_test.py module docstring for the full investigation. It
does not affect real hardware.

Empirically confirmed for this module's own RPC surface (2026-07-09, against
the test build's 3 configured input processors and its single, unnamed
keymap layer):
  - `GetLayerInfoRequest` round-trips reliably: its response carries real
    per-layer data, but the test keymap has exactly one layer with no
    explicit display name, so the encoded response stays tiny (well under
    the observed threshold) -- this is the real custom-subsystem round trip
    this file asserts (test_custom_rpc_get_layer_info_round_trip).
  - `ListInputProcessorsRequest`, despite its own RPC *response* being
    deliberately empty by design (handle_list_input_processors() in
    src/studio/custom_handler.c reports the real processor data out-of-band,
    as `InputProcessorChangedNotification`s per processor, instead of in the
    response -- see proto/cormoran/rip/custom.proto), reliably times out
    under Renode: it makes the firmware enqueue one
    callback-encoded notification (each carrying a full, ~15-field
    `InputProcessorInfo`) per configured processor (3 in the test build)
    over the same custom-subsystem response-encoding machinery, back to
    back, before/around the small RPC response itself -- reproducing the
    same TX-path stall from cumulative traffic rather than from a single
    large message. Small, callback-free responses (core GetDeviceInfo,
    meta.simple_error) are unaffected by either pattern and stay reliable
    indefinitely.

Run locally (from this repo's root, with a west workspace already set up --
see README.md):

    python3 tests/renode/renode_test.py -v

(Named `renode_test.py`, not `test_renode.py`, on purpose: the existing
`python3 -m unittest -v` build-job step at the repo root auto-discovers
every `test*.py`, and this file needs a real firmware ELF + PYTHONPATH the
build job doesn't set up -- keeping it out of that pattern keeps the two
test surfaces independent. The `zmk-renode-test` action instead runs
everything under `tests/renode/` explicitly, with `ZMK_RENODE_ELF` and
PYTHONPATH already set.)

The Renode-testable ELF must already be built first -- see README.md for
the exact build invocation, or let the composite action / CI do it.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# renode_harness comes from the zmk-workspace checkout the zmk-renode-test
# action provides on PYTHONPATH (zmk-workspace is no longer a west dependency
# -- the buildable renode module/snippet now come from zmk-west-commands).
# Support running this file directly too by falling back to a sibling
# `zmk-workspace` checkout next to this repo.
try:
    import renode_harness
except ImportError:  # pragma: no cover - convenience fallback for local dev
    fallback_candidates = [
        REPO_ROOT.parent / "zmk-workspace" / "skills" / "test-zmk-renode" / "scripts",
    ]
    for fallback in fallback_candidates:
        if fallback.is_dir():
            sys.path.insert(0, str(fallback))
            import renode_harness

            break
    else:
        raise


SUBSYSTEM_IDENTIFIER = "cormoran_rip"
# This module registers exactly one custom subsystem in the test build, so
# its index is deterministically 0 -- but see the KNOWN RENODE-ENVIRONMENT
# LIMITATION note above: `ListCustomSubsystemRequest` (the normal way to
# discover this at runtime) returns a large response (identifier + UI URL,
# well past the observed size threshold) and so also times out under
# Renode; this test hardcodes the index rather than discovering it.
KNOWN_SUBSYSTEM_INDEX = 0
# Always out of range regardless of how many custom subsystems a given
# module registers -- used to exercise the *working* fast-path dispatch
# (see test_custom_rpc_invalid_index_dispatch).
INVALID_SUBSYSTEM_INDEX = 99


class RenodeRuntimeInputProcessorTests(unittest.TestCase):
    """Boots this module's own Renode-testable ELF once for the whole class
    (like the skill's own T0/T1 tests, boot is the slow part) and exercises
    the cormoran_rip custom subsystem's envelope/dispatch machinery."""

    renode_path: str
    elf: Path
    studio_pb2 = None
    rip_pb2 = None

    @classmethod
    def setUpClass(cls):
        cls.renode_path = renode_harness.find_or_install_renode()
        if cls.renode_path is None:
            raise unittest.SkipTest(
                "Renode is not installed and could not be auto-installed"
            )

        elf_env = os.environ.get("ZMK_RENODE_ELF")
        if not elf_env:
            raise unittest.SkipTest(
                "ZMK_RENODE_ELF not set -- build the Renode-testable ELF first (see README.md)"
            )
        cls.elf = Path(elf_env)
        if not cls.elf.is_file():
            raise unittest.SkipTest(f"ZMK_RENODE_ELF does not exist: {cls.elf}")

        # Core zmk.studio.* messages (Request/Response envelope, core.proto,
        # custom.proto for the generic custom-subsystem envelope).
        studio_proto_dir = renode_harness.find_studio_proto_dir(REPO_ROOT)
        cls.studio_pb2 = renode_harness.load_studio_pb2(studio_proto_dir)

        # This module's own proto (proto/cormoran/rip/custom.proto, package
        # cormoran.rip) -- compiled separately since it lives outside
        # zmk-studio-messages.
        out_dir = renode_harness.compile_protos(
            [REPO_ROOT / "proto" / "cormoran" / "rip" / "custom.proto"],
            include_dirs=[REPO_ROOT / "proto"],
        )
        sys.path.insert(0, str(out_dir))
        import cormoran.rip.custom_pb2 as rip_pb2  # type: ignore

        cls.rip_pb2 = rip_pb2

    def setUp(self):
        self.session, self.console, self.rpc = renode_harness.boot_single(
            self.renode_path, self.elf
        )
        self.addCleanup(self.session.stop)
        self.addCleanup(self.console.close)
        self.addCleanup(self.rpc.close)

        banner = renode_harness.wait_for_text(
            self.console._sock, "Welcome to ZMK", timeout=15
        )
        self.assertIn(
            "Welcome to ZMK", banner, f"never saw ZMK boot banner; got:\n{banner}"
        )

    def _send_call(self, subsystem_index: int, payload: bytes, request_id: int = 1):
        req = self.studio_pb2.Request()
        req.request_id = request_id
        req.custom.call.subsystem_index = subsystem_index
        req.custom.call.payload = payload
        self.rpc.send(req.SerializeToString())

    # -- Affirmative proof the custom-subsystem envelope works -----------

    def test_custom_rpc_invalid_index_dispatch(self):
        """`custom.call` to a subsystem index that doesn't exist proves the
        whole custom-subsystem *envelope* round-trips correctly end to end
        (Request.custom oneof selection, CallRequest field encoding,
        subsystem-count/index validation, meta.simple_error response
        encoding/decoding)."""
        self._send_call(INVALID_SUBSYSTEM_INDEX, b"", request_id=7)

        resp_bytes = self.rpc.read_frame(timeout=10.0)
        self.assertIsNotNone(
            resp_bytes, "no response to custom.call with an invalid index (timeout)"
        )
        resp = self.studio_pb2.Response()
        resp.ParseFromString(resp_bytes)
        self.assertEqual(resp.WhichOneof("type"), "request_response")
        self.assertEqual(resp.request_response.request_id, 7)
        self.assertEqual(resp.request_response.WhichOneof("subsystem"), "meta")
        self.assertEqual(
            resp.request_response.meta.WhichOneof("response_type"), "simple_error"
        )
        # zmk.meta.ErrorConditions.RPC_NOT_FOUND == 2
        self.assertEqual(resp.request_response.meta.simple_error, 2)

    # -- Affirmative proof cormoran_rip's own handler responds ------------

    def test_custom_rpc_get_layer_info_round_trip(self):
        """Sends a real GetLayerInfoRequest to this module's own registered
        subsystem (index 0) and checks the response round-trips as a real
        GetLayerInfoResponse listing the test build's keymap layer(s). The
        test keymap has exactly one layer (see
        tests/zmk-config/boards/shields/my_awesome_keyboard/my_awesome_keyboard.keymap)
        with no explicit display name, so the encoded response stays tiny
        (well under the known Renode-only threshold documented in this
        file's module docstring) -- this is a genuine, reliable end-to-end
        proof that cormoran_rip's own handler (not just the generic
        custom-subsystem envelope) runs and responds under Renode."""
        inner_req = self.rip_pb2.Request()
        inner_req.get_layer_info.SetInParent()
        self._send_call(KNOWN_SUBSYSTEM_INDEX, inner_req.SerializeToString())

        resp_bytes = self.rpc.read_frame(timeout=10.0)
        self.assertIsNotNone(
            resp_bytes,
            "no response to a real GetLayerInfoRequest against cormoran_rip (timeout)",
        )
        resp = self.studio_pb2.Response()
        resp.ParseFromString(resp_bytes)
        self.assertEqual(resp.WhichOneof("type"), "request_response")
        self.assertEqual(resp.request_response.request_id, 1)
        self.assertEqual(resp.request_response.WhichOneof("subsystem"), "custom")

        inner_resp = self.rip_pb2.Response()
        inner_resp.ParseFromString(resp.request_response.custom.call.payload)
        self.assertEqual(inner_resp.WhichOneof("response_type"), "get_layer_info")
        layers = inner_resp.get_layer_info.layers
        self.assertEqual(len(layers), 1, f"expected exactly one layer, got {layers}")
        self.assertEqual(layers[0].index, 0)

    # -- Known Renode limitation: documented, asserted, not silently skipped --

    def test_custom_rpc_list_input_processors_round_trip_KNOWN_BROKEN_UNDER_RENODE(
        self,
    ):
        """Documents the known Renode-environment limitation (see this
        file's module docstring): sending a real ListInputProcessorsRequest
        to this module's own registered subsystem should get back the
        (deliberately empty) ListInputProcessorsResponse, followed by one
        InputProcessorChangedNotification per configured processor (3 in the
        test build) -- and does, on real hardware -- but under Renode the
        burst of callback-encoded notifications this request triggers stalls
        the TX path before/around the small RPC response itself,
        reproducing the same kind of read timeout as the template's
        SampleResponse and fast-keymap's Snapshot before it (see the module
        docstring's 2026-07-09 findings for why this differs from a single
        large response). This test asserts *that exact failure* (a read
        timeout) so it will start failing -- loudly, as a signal to update
        this test to assert the real round trip (and, ideally, drain and
        assert the 3 expected notifications too) -- the day the underlying
        emulation/harness limitation is fixed."""
        inner_req = self.rip_pb2.Request()
        inner_req.list_input_processors.SetInParent()
        self._send_call(KNOWN_SUBSYSTEM_INDEX, inner_req.SerializeToString())

        resp_bytes = self.rpc.read_frame(timeout=10.0)
        self.assertIsNone(
            resp_bytes,
            "custom.call ListInputProcessorsRequest got a response under Renode -- the "
            "known Renode-only limitation documented in this file's module docstring "
            "appears to be fixed! Update this test to assert the real "
            "ListInputProcessorsResponse round trip instead (see "
            "test_custom_rpc_get_layer_info_round_trip for the "
            "request-building/response-parsing pattern), and consider draining/asserting "
            "the 3 expected InputProcessorChangedNotification frames and re-adding "
            "subsystem discovery via ListCustomSubsystemRequest.",
        )


if __name__ == "__main__":
    unittest.main()
