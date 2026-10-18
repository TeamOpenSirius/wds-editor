"""Installer decision tests; no network, pip, GPU or torch required."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("install", Path(__file__).with_name("install.py"))
install = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install)
INDEX = install.TORCH_INDEX


class InterpreterTests(unittest.TestCase):
    def test_no_upper_python_bound(self):
        for minor in (10, 13, 14, 15, 16, 20):
            self.assertIsNone(install.interpreter_problem((3, minor), True, "Linux", "x86_64"))
        self.assertIsNone(install.interpreter_problem((3, 15), True, "Darwin", "arm64"))
        self.assertIsNone(install.interpreter_problem((3, 13), True, "Windows", "ARM64"))

    def test_unusable_interpreters_explain_why(self):
        self.assertIn("3.10", install.interpreter_problem((3, 9), True, "Windows", "AMD64"))
        self.assertIn("32 位", install.interpreter_problem((3, 12), False, "Windows", "AMD64"))
        self.assertIn("arm64", install.interpreter_problem((3, 12), True, "Darwin", "x86_64"))

    def test_missing_venv_names_debian_package(self):
        self.assertIn("python3.15-venv", install.venv_problem((3, 15), "Linux", False))
        self.assertIsNone(install.venv_problem((3, 15), "Linux", True))


class TorchIndexTests(unittest.TestCase):
    def test_nvidia_smi_parsing(self):
        self.assertEqual(install.parse_nvidia_smi("595.91.07, 12.0\n595.91.07, 8.6\n"), ((595, 91, 7), (12, 0)))
        self.assertEqual(install.parse_nvidia_smi("536.99\n"), ((536, 99), None))
        self.assertEqual(install.parse_nvidia_smi("470.256.02, [N/A]\n"), ((470, 256, 2), None))
        self.assertIsNone(install.parse_nvidia_smi("No devices were found\n"))
        self.assertIsNone(install.parse_nvidia_smi("NVML library version: 580.95\n"))

    def test_gpu_selects_newest_supported_cuda_build(self):
        choose = install.choose_torch_index
        self.assertEqual(choose("Linux", "x86_64", ((595, 91, 7), (12, 0)))[0], INDEX + "cu130")
        self.assertEqual(choose("Windows", "AMD64", ((581, 57), (8, 6)))[0], INDEX + "cu130")
        # Driver predates CUDA 13, or the GPU predates Turing: CUDA 12.6.
        self.assertEqual(choose("Windows", "AMD64", ((566, 36), (8, 9)))[0], INDEX + "cu126")
        self.assertEqual(choose("Linux", "x86_64", ((580, 95, 5), (6, 1)))[0], INDEX + "cu126")
        # cu126 has no Blackwell kernels; Kepler is unsupported everywhere.
        self.assertEqual(choose("Linux", "x86_64", ((570, 133, 7), (12, 0)))[0], INDEX + "cpu")
        self.assertEqual(choose("Linux", "x86_64", ((470, 256, 2), (3, 5)))[0], INDEX + "cpu")
        self.assertEqual(choose("Linux", "x86_64", None), (INDEX + "cpu", "CPU"))
        self.assertEqual(choose("Windows", "ARM64", ((581, 57), (8, 6)))[0], INDEX + "cpu")
        self.assertEqual(choose("Darwin", "arm64", None)[0], INDEX + "cpu")

    def test_override(self):
        choose = install.choose_torch_index
        gpu = ((595, 91, 7), (12, 0))
        self.assertEqual(choose("Linux", "x86_64", gpu, "rocm7.2")[0], INDEX + "rocm7.2")
        self.assertEqual(choose("Linux", "x86_64", gpu, "https://mirror.example/whl/cu130")[0],
                         "https://mirror.example/whl/cu130")
        self.assertIsNone(choose("Linux", "x86_64", gpu, "default")[0])

    def test_installed_build_must_match_index(self):
        matches = install.torch_matches_index
        self.assertTrue(matches(INDEX + "cpu", ["2.14.1+cpu", None, None]))
        self.assertTrue(matches(INDEX + "cu130", ["2.14.1", "13.0", None]))
        self.assertFalse(matches(INDEX + "cu130", ["2.14.1+cpu", None, None]))
        self.assertFalse(matches(INDEX + "cu126", ["2.14.1", "13.0", None]))
        self.assertFalse(matches(INDEX + "cpu", ["2.14.1", "13.0", None]))
        self.assertFalse(matches(INDEX + "cpu", None))
        self.assertTrue(matches("https://mirror.example/simple", ["2.14.1", "13.0", None]))
        self.assertTrue(matches(None, ["2.14.1+cpu", None, None]))


class PipFailureTests(unittest.TestCase):
    def test_unreachable_index_is_a_network_error(self):
        output = ("WARNING: Retrying ... Max retries exceeded with url: /whl/cu130/torch/\n"
                  "ERROR: No matching distribution found for torch>=2.6\n")
        self.assertTrue(install.is_network_failure(output))
        self.assertIn("网络", install.describe_pip_failure("PyTorch", output, INDEX + "cu130"))

    def test_missing_wheels_name_python_and_override(self):
        message = install.describe_pip_failure(
            "PyTorch", "ERROR: No matching distribution found for torch>=2.6\n", INDEX + "cpu")
        self.assertIn("Python %d.%d" % sys.version_info[:2], message)
        self.assertIn(install.platform_label(), message)
        self.assertIn("WDS_STEM_TORCH_INDEX", message)


@unittest.skipIf(os.name == "nt", "fake interpreter is a shell script")
class RuntimeTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="wds-stem-install-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.job = self.root / "job"
        self.job.mkdir()
        self.runtime = self.root / "runtime"
        (self.runtime / "bin").mkdir(parents=True)

    def fake_python(self, version):
        python = self.runtime / "bin" / "python"
        python.write_text("#!/bin/sh\necho '[%d, %d]'\n" % version)
        python.chmod(0o755)
        return python

    def test_runtime_for_this_python_is_reused(self):
        python = self.fake_python(sys.version_info[:2])
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(install.ensure_runtime(self.runtime, self.job), python)

    def test_never_deletes_a_directory_that_is_not_a_venv(self):
        self.fake_python((2, 7))
        keep = self.runtime / "user-data.txt"
        keep.write_text("keep")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(install.InstallError):
                install.ensure_runtime(self.runtime, self.job)
        self.assertTrue(keep.exists())


if __name__ == "__main__":
    unittest.main()
