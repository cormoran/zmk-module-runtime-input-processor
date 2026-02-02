import platform
import shutil
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

THIS_DIR = Path(__file__).parent.resolve()

def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )

@dataclass
class NotFound:
    text: str

class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(subprocess.run(
            ["west", "topdir"],
            capture_output=True,
            text=True,
        ).stdout.strip())
        cls.BUILD_DIR = cls.WEST_TOPDIR / "build"

    @unittest.skipUnless(platform.system() == "Linux", "zmk-test is only supported on Linux")
    def test_zmk_test(self):
        tests_build = self.BUILD_DIR / "tests"
        shutil.rmtree(tests_build, ignore_errors=True)

        result = run_west(["zmk-test", "tests", '-m', '.'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout)

    def test_zmk_build(self):
        artifacts_and_expected_config: dict[str, list[str | NotFound]] = {
            "my_awesome_keyboard_with_custom_rpc_support": [
                "CONFIG_MY_AWESOME_KEYBOARD_SPECIAL_FEATURE=y",
                "CONFIG_ZMK_STUDIO=y",
                "CONFIG_ZMK_TEMPLATE_FEATURE=y",
                "CONFIG_ZMK_TEMPLATE_FEATURE_STUDIO_RPC=y",
            ],
            "my_awesome_keyboard_without_custom_rpc_support": [
                "CONFIG_MY_AWESOME_KEYBOARD_SPECIAL_FEATURE=y",
                "# CONFIG_ZMK_STUDIO is not set",
                "CONFIG_ZMK_TEMPLATE_FEATURE=y",
                NotFound("CONFIG_ZMK_TEMPLATE_FEATURE_STUDIO_RPC"),
            ]
        }

        for artifact in artifacts_and_expected_config.keys():
            shutil.rmtree(self.BUILD_DIR / artifact, ignore_errors=True)

        result = run_west(["zmk-build", "tests/zmk-config/config", "-m", "tests/zmk-config", ".", "-q"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_config.items():
            config_path = self.BUILD_DIR / artifact / "zephyr" / ".config"
            self.assertTrue(config_path.exists(), f"{artifact} .config is missing")
            config_text = config_path.read_text()
            for entry in entries:
                if isinstance(entry, NotFound):
                    if entry.text in config_text:
                        self.fail(f"{entry.text} found in {config_path} for {artifact}, but it should not be present")
                else:
                    if entry not in config_text:
                        self.fail(f"{entry} not found in {config_path} for {artifact}")
            self.assertTrue((config_path.parent / "zmk.uf2").exists(), f"{artifact} zmk.uf2 is missing in {config_path.parent}")

if __name__ == "__main__":
    unittest.main()
