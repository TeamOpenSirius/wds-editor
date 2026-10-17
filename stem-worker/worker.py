"""Optional local HTDemucs worker. stdout is versioned JSONL; diagnostics use stderr.

The editor launches one process per song. No Qt/BASS dependencies, chart data,
training code, or network service. Only the first model download needs a network.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

SCHEMA = 1
SOURCES = ("drums", "bass", "other", "vocals")


class Cancelled(Exception):
    pass


def emit(event, **fields):
    print(json.dumps({"protocol": SCHEMA, "event": event, **fields}, ensure_ascii=False), flush=True)


def check_cancel(job):
    if (job / "cancel").exists():
        raise Cancelled()


def digest(path, job=None):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        while data := f.read(1024 * 1024):
            if job is not None:
                check_cancel(job)
            h.update(data)
    return h.hexdigest()


def cache_key(spec):
    return hashlib.sha256(json.dumps(spec, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def read_cached(folder, spec, job):
    """A directory alone is not a cache hit. Verify every published payload."""
    try:
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
        if manifest["schema"] != SCHEMA or manifest["spec"] != spec:
            return None
        if set(manifest["tracks"]) != set(SOURCES):
            return None
        for name in SOURCES:
            track = manifest["tracks"][name]
            path = folder / (name + ".wav")
            if track["file"] != path.name or digest(path, job) != track["sha256"]:
                return None
        return manifest
    except (OSError, ValueError, KeyError, TypeError):
        return None


def decode(source, job, sf):
    try:
        info = sf.info(source)
        path = source
    except (RuntimeError, OSError):
        import imageio_ffmpeg
        path = job / "decoded.wav"
        with (job / "decode.log").open("wb") as log:
            proc = subprocess.Popen(
                [imageio_ffmpeg.get_ffmpeg_exe(), "-nostdin", "-v", "error", "-y",
                 "-i", str(source), "-map", "0:a:0", "-vn", "-ac", "2",
                 "-t", "1801", "-c:a", "pcm_f32le", str(path)],
                stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            try:
                while proc.poll() is None:
                    check_cancel(job)
                    time.sleep(.05)
                if proc.returncode:
                    raise RuntimeError("音频解码失败：" + (job / "decode.log").read_text(errors="replace")[-2000:])
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
        info = sf.info(path)
    if info.duration < .01 or info.duration > 1800 or info.channels not in (1, 2):
        raise ValueError("支持 10 毫秒到 30 分钟的单声道或立体声音频")
    check_cancel(job)
    audio, rate = sf.read(path, dtype="float32", always_2d=True)
    return audio, rate


def separate(args):
    job, cache, source = args.job.resolve(), args.cache.resolve(), args.input.resolve()
    job.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    check_cancel(job)
    emit("progress", percent=0, message="正在准备本地分轨…")
    os.environ["OMP_NUM_THREADS"] = str(args.threads)
    os.environ["MKL_NUM_THREADS"] = str(args.threads)
    # Cooperative scheduling also works on platforms where process niceness is unavailable.
    if args.background:
        try:
            if os.name == "posix":
                os.nice(5)
            elif os.name == "nt":
                import ctypes
                ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
        except OSError:
            pass
    with contextlib.redirect_stdout(sys.stderr):
        import numpy as np
        import torch
        import julius
        import soundfile as sf
        import yaml
        from filelock import FileLock
        from demucs.apply import apply_model, TensorChunk
        from demucs.pretrained import get_model, REMOTE_ROOT, _parse_remote_files

    torch.set_num_threads(args.threads)
    torch.set_num_interop_threads(1)
    torch.manual_seed(42)
    device = args.device
    if device == "auto":
        device = "cuda" if torch.cuda.is_available() else "mps" if torch.backends.mps.is_available() else "cpu"
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("当前运行环境没有可用的 CUDA；请选择自动或 CPU")
    if device == "mps" and not torch.backends.mps.is_available():
        raise RuntimeError("当前运行环境没有可用的 Apple MPS；请选择自动或 CPU")
    if device == "cuda":
        torch.backends.cuda.matmul.allow_tf32 = True
        torch.backends.cudnn.allow_tf32 = True
        # Fixed shape chunks reuse cuDNN plans. Leave memory for the editor's Vulkan renderer.
        torch.backends.cudnn.benchmark = True
        if args.background:
            torch.cuda.set_per_process_memory_fraction(.65)
    precision = "tf32" if device == "cuda" else "fp32"
    emit("progress", percent=2, message="正在校验音源和模型…", device=device)
    audio_hash = digest(source, job)
    urls = _parse_remote_files(REMOTE_ROOT / "files.txt")
    signatures = yaml.safe_load((REMOTE_ROOT / "htdemucs.yaml").read_text())["models"]
    checkpoints = [Path(torch.hub.get_dir()) / "checkpoints" / urls[s].rsplit("/", 1)[-1] for s in signatures]
    bag = None
    if not all(p.is_file() for p in checkpoints):
        emit("progress", percent=3, message="首次使用：正在下载 HTDemucs 模型…")
        with contextlib.redirect_stdout(sys.stderr):
            bag = get_model("htdemucs")
    check_cancel(job)
    spec = {"source_sha256": audio_hash, "model": "htdemucs",
            "weights": [digest(p, job) for p in checkpoints],
            "worker_sha256": digest(__file__), "device": device, "precision": precision,
            "overlap": .25, "shifts": 0, "seed": 42,
            "sndfile": sf.__libsndfile_version__,
            "packages": {n: importlib.metadata.version(n) for n in
                         ("torch", "demucs", "julius", "soundfile", "numpy")}}
    key = cache_key(spec)
    folder = cache / key
    with FileLock(str(cache / (key + ".lock")), timeout=0):
        cached = read_cached(folder, spec, job)
        if cached is not None:
            if digest(source, job) != audio_hash:
                raise RuntimeError("音源在读取期间发生变化，请重新生成")
            emit("complete", manifest=str(folder / "manifest.json"), cached=True)
            return
        emit("progress", percent=5, message="正在解码音频…", device=device)
        raw, rate = decode(source, job, sf)
        if not np.isfinite(raw).all():
            raise ValueError("音频包含无效样本")
        original_samples = len(raw)
        if raw.shape[1] == 1:
            raw = np.repeat(raw, 2, axis=1)
        with contextlib.redirect_stdout(sys.stderr):
            if bag is None:
                bag = get_model("htdemucs")
        if len(bag.models) != 1 or set(bag.sources) != set(SOURCES):
            raise RuntimeError("不兼容的 HTDemucs 权重")
        model = bag.models[0].eval().to(device)
        model.requires_grad_(False)
        check_cancel(job)
        start = time.monotonic()
        with torch.inference_mode():
            wave = julius.resample_frac(torch.from_numpy(raw.T.copy()), rate, model.samplerate)
            del raw
            ref = wave.mean(0)
            mean, scale = ref.mean(), ref.std()
            if scale < 1e-8:
                scale = wave.std().clamp_min(1e-8)
            wave = (wave - mean) / scale
            length = wave.shape[-1]
            segment = float(model.segment)
            size = int(segment * model.samplerate)
            stride = int(size * .75)
            # Whole-song storage stays on CPU. GPU memory only holds one fixed-size chunk.
            result = torch.zeros(len(SOURCES), 2, length)
            weights = torch.zeros(length)
            window = torch.cat((torch.arange(1, size // 2 + 1), torch.arange(size - size // 2, 0, -1))).float()
            window /= window.max()
            count = math.ceil(length / stride)
            for index, offset in enumerate(range(0, length, stride)):
                check_cancel(job)
                tick = time.monotonic()
                # Use the upstream TensorChunk padding/center trimming, including end-of-song context.
                part = apply_model(model, TensorChunk(wave[None], offset, size),
                                   shifts=0, split=False, segment=segment, device=device)[0].cpu()
                n = part.shape[-1]
                if not torch.isfinite(part).all():
                    raise RuntimeError("模型输出包含无效样本")
                result[..., offset:offset+n].add_(part * window[:n])
                weights[offset:offset+n].add_(window[:n])
                del part
                emit("progress", percent=10 + int(80 * (index + 1) / count),
                     message=f"正在分轨 {index + 1}/{count}（{device.upper()}）", device=device)
                if args.background:
                    # CPU copy above synchronizes GPU work before yielding to the renderer.
                    time.sleep(min(.15, (time.monotonic() - tick) * .15))
            result.div_(weights.clamp_min(1e-12)).mul_(scale).add_(mean)
            del wave, weights
            stage = job / "result"
            stage.mkdir(exist_ok=False)
            tracks = {}
            for index, name in enumerate(model.sources):
                check_cancel(job)
                # Preserve original duration exactly. No chart delay, trimming, or per-stem gain.
                output = julius.resample_frac(result[index], model.samplerate, rate, full=True)
                output = torch.nn.functional.pad(output, (0, max(0, original_samples - output.shape[-1])))
                output = output[:, :original_samples].T.contiguous().numpy()
                if not np.isfinite(output).all():
                    raise RuntimeError("重采样输出包含无效样本")
                path = stage / (name + ".wav")
                sf.write(path, output, rate, subtype="FLOAT")
                info = sf.info(path)
                if info.frames != original_samples or info.samplerate != rate or info.channels != 2:
                    raise RuntimeError("分轨文件长度校验失败")
                tracks[name] = {"file": path.name, "sha256": digest(path, job)}
        if digest(source, job) != audio_hash:
            raise RuntimeError("音源在推理期间发生变化，请重新生成")
        manifest = {"schema": SCHEMA, "spec": spec, "tracks": tracks,
                    "source": str(source), "sample_rate": rate, "samples": original_samples,
                    "channels": 2, "time_origin": "original_audio_sample_zero",
                    "elapsed_seconds": round(time.monotonic() - start, 3)}
        (stage / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        check_cancel(job)
        # Only remove a corrupt entry under this exact key, while holding its lock.
        if folder.exists():
            shutil.rmtree(folder)
        stage.rename(folder)  # job directory is created on the same filesystem as the cache
        emit("complete", manifest=str(folder / "manifest.json"), cached=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--job", type=Path, required=True)
    parser.add_argument("--device", choices=("auto", "cuda", "mps", "cpu"), default="auto")
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--background", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.threads <= 16:
        parser.error("threads must be between 1 and 16")
    try:
        separate(args)
        return 0
    except Cancelled:
        emit("cancelled")
        return 2
    except Exception as exc:
        import traceback
        traceback.print_exc(file=sys.stderr)
        message = str(exc)
        if isinstance(exc, ModuleNotFoundError):
            message = "缺少分轨依赖，请先安装运行环境或选择已安装 Demucs 的 Python。" + message
        if "out of memory" in message.lower():
            message = "显存不足；可关闭其他 GPU 程序，或切换 CPU 后重试。"
        emit("error", message=message)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
