"""Cache/cancellation regression tests; no torch, audio device, or downloads required."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("worker", Path(__file__).with_name("worker.py"))
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wds-stem-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.job = self.root / "job"
        self.job.mkdir()
        self.cache = self.root / "entry"
        self.cache.mkdir()
        self.spec = {"source_sha256": "a", "weights": ["b"], "precision": "fp32"}

    def publish(self):
        tracks = {}
        for name in worker.SOURCES:
            path = self.cache / (name + ".wav")
            path.write_bytes(name.encode())
            tracks[name] = {"file": path.name, "sha256": worker.digest(path)}
        data = {"schema": 1, "spec": self.spec, "tracks": tracks}
        (self.cache / "manifest.json").write_text(json.dumps(data))
        return data

    def test_incomplete_directory_is_not_a_hit(self):
        self.assertIsNone(worker.read_cached(self.cache, self.spec, self.job))

    def test_valid_cache_and_corrupt_payload(self):
        expected = self.publish()
        self.assertEqual(worker.read_cached(self.cache, self.spec, self.job), expected)
        (self.cache / "drums.wav").write_bytes(b"corrupt")
        self.assertIsNone(worker.read_cached(self.cache, self.spec, self.job))

    def test_changed_model_settings_invalidate_cache(self):
        self.publish()
        for key, value in (("source_sha256", "new"), ("weights", ["new"]), ("precision", "tf32")):
            changed = dict(self.spec, **{key: value})
            self.assertNotEqual(worker.cache_key(changed), worker.cache_key(self.spec))
            self.assertIsNone(worker.read_cached(self.cache, changed, self.job))

    def test_reject_missing_track_and_external_path(self):
        data = self.publish()
        data["tracks"]["drums"]["file"] = "../drums.wav"
        (self.cache / "manifest.json").write_text(json.dumps(data))
        self.assertIsNone(worker.read_cached(self.cache, self.spec, self.job))
        del data["tracks"]["drums"]
        (self.cache / "manifest.json").write_text(json.dumps(data))
        self.assertIsNone(worker.read_cached(self.cache, self.spec, self.job))

    def test_cancel_interrupts_cache_validation(self):
        self.publish()
        (self.job / "cancel").touch()
        with self.assertRaises(worker.Cancelled):
            worker.read_cached(self.cache, self.spec, self.job)

    def test_cache_key_ignores_dictionary_order(self):
        self.assertEqual(worker.cache_key(self.spec), worker.cache_key(dict(reversed(list(self.spec.items())))))

    def test_protocol_is_single_utf8_json_line(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            worker.emit("progress", message="分轨\n继续", percent=10)
        self.assertEqual(len(output.getvalue().splitlines()), 1)
        self.assertEqual(json.loads(output.getvalue())["protocol"], 1)

    def test_ffmpeg_command_restricts_protocols_before_input(self):
        cmd = worker.ffmpeg_command("ffmpeg", "/a/b.mp3", "/j/decoded.wav")
        i = cmd.index("-i")
        self.assertEqual(cmd[cmd.index("-protocol_whitelist") + 1], "file")
        self.assertLess(cmd.index("-protocol_whitelist"), i)
        self.assertEqual(cmd[i + 1], "/a/b.mp3")
        self.assertEqual(cmd[-1], "/j/decoded.wav")


if __name__ == "__main__":
    unittest.main()
