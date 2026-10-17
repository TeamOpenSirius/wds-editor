"""Exercise the launcher/build CLI without requiring a Qt SDK or compiling code."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LinuxScriptsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wds linux scripts ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / "cache"
        self.build = self.root / "build with spaces"
        self.calls = self.root / "calls.jsonl"
        self.env = os.environ.copy()
        for name in ("WDS_LINUX_CACHE", "WDS_LINUX_BUILD_DIR", "WDS_CMAKE",
                     "WDS_QT_LINUX_ROOT", "WDS_BASS_LINUX_DIR", "Qt6_ROOT",
                     "CMAKE_PREFIX_PATH", "CMAKE_GENERATOR", "CMAKE_BUILD_PARALLEL_LEVEL"):
            self.env.pop(name, None)
        self.env.update(WDS_LINUX_CACHE=str(self.cache), WDS_LINUX_BUILD_DIR=str(self.build),
                        WDS_SCRIPT_TEST_CALLS=str(self.calls))
        self.cmake = self.root / "fake cmake"
        self.executable(self.cmake, """import json, os, sys
with open(os.environ['WDS_SCRIPT_TEST_CALLS'], 'a') as f:
    f.write(json.dumps({'args': sys.argv[1:], 'prefix': os.environ.get('CMAKE_PREFIX_PATH')}) + '\\n')
sys.exit(int(os.environ.get('WDS_SCRIPT_TEST_EXIT', '0')))
""")
        self.env["WDS_CMAKE"] = str(self.cmake)

    def executable(self, path, body):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!" + sys.executable + "\n" + body)
        path.chmod(0o755)

    def run_script(self, name, *args):
        return subprocess.run(["bash", str(ROOT / "scripts" / name), *args],
                              cwd=self.root, env=self.env, text=True, capture_output=True)

    def recorded(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def test_configure_and_build_preserve_paths_and_prefix_environment(self):
        self.env["CMAKE_PREFIX_PATH"] = "/custom sdk:/another prefix"
        result = self.run_script("build-linux.sh", "--", "-DEXAMPLE=path with spaces")
        self.assertEqual(result.returncode, 0, result.stderr)
        configure, build = self.recorded()
        self.assertIn(str(ROOT), configure["args"])
        self.assertIn(str(self.build), configure["args"])
        self.assertIn("-DEXAMPLE=path with spaces", configure["args"])
        self.assertEqual(configure["prefix"], self.env["CMAKE_PREFIX_PATH"])
        self.assertEqual(build["args"], ["--build", str(self.build), "--parallel", "4"])

    def test_reconfigure_keeps_cached_generator_type_and_bass_path(self):
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\nCMAKE_GENERATOR:INTERNAL=Unix Makefiles\n"
            "CMAKE_BUILD_TYPE:STRING=Debug\nWDS_BASS_LINUX_DIR:PATH=/custom bass\n")
        result = self.run_script("build-linux.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.recorded()[0]["args"]
        self.assertNotIn("-G", args)
        self.assertFalse(any(a.startswith(("-DCMAKE_BUILD_TYPE=", "-DWDS_BASS_LINUX_DIR=")) for a in args))

    def test_another_checkout_cache_is_rejected_by_both_scripts(self):
        self.build.mkdir()
        (self.build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/another checkout\n")
        for script in ("build-linux.sh", "run-linux.sh"):
            result = self.run_script(script)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("WDS_LINUX_BUILD_DIR", result.stderr)
        self.assertFalse(self.calls.exists())

    def test_explicit_sdk_preserves_other_search_prefixes(self):
        sdk = self.root / "Qt SDK"
        config = sdk / "lib/cmake/Qt6/Qt6Config.cmake"
        config.parent.mkdir(parents=True)
        config.touch()
        self.env.update(WDS_QT_LINUX_ROOT=str(sdk), CMAKE_PREFIX_PATH="/custom deps",
                        WDS_BASS_LINUX_DIR="/custom bass", CMAKE_GENERATOR="Unix Makefiles")
        result = self.run_script("build-linux.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        configure = self.recorded()[0]
        self.assertEqual(configure["prefix"], str(sdk) + ":/custom deps")
        self.assertIn("-DWDS_BASS_LINUX_DIR=/custom bass", configure["args"])
        self.assertNotIn("-G", configure["args"])

    def test_failed_configure_does_not_launch_build(self):
        self.env["WDS_SCRIPT_TEST_EXIT"] = "23"
        self.assertEqual(self.run_script("build-linux.sh").returncode, 23)
        self.assertEqual(len(self.recorded()), 1)

    def test_launcher_preserves_arguments_runtime_path_and_exit_status(self):
        runtime = self.cache / "deps/runtime/lib"
        runtime.mkdir(parents=True)
        self.env["LD_LIBRARY_PATH"] = "/existing libraries"
        self.executable(self.build / "ui/wds_editor", """import json, os, sys
print(json.dumps({'args': sys.argv[1:], 'libs': os.environ['LD_LIBRARY_PATH']}))
sys.exit(19)
""")
        result = self.run_script("run-linux.sh", "--open", "/music files/song.wdsproject")
        self.assertEqual(result.returncode, 19)
        output = json.loads(result.stdout)
        self.assertEqual(output["args"], ["--open", "/music files/song.wdsproject"])
        self.assertEqual(output["libs"], str(runtime) + ":/existing libraries")

    def test_help_and_missing_editor(self):
        for script in ("build-linux.sh", "run-linux.sh"):
            result = self.run_script(script, "--help")
            self.assertEqual(result.returncode, 0)
            self.assertIn("Usage:", result.stdout)
        result = self.run_script("run-linux.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.build), result.stderr)

    def test_bad_explicit_sdk_fails_before_configure(self):
        self.env["WDS_QT_LINUX_ROOT"] = str(self.root / "missing SDK")
        result = self.run_script("build-linux.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Qt SDK not found", result.stderr)
        self.assertFalse(self.calls.exists())


if __name__ == "__main__":
    unittest.main()
