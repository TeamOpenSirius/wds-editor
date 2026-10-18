"""Install the optional stem runtime into an isolated virtual environment.

The editor runs this with the Python the user selected (or with the runtime itself
when repairing it). stdout is the same versioned JSONL protocol as worker.py; venv
and pip output go to stderr. The syntax stays Python 3.6 compatible so an old
interpreter reports a readable version error instead of a SyntaxError.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

SCHEMA = 1
MIN_PYTHON = (3, 10)
MAX_PYTHON = (3, 14)
TORCH = "torch>=2.6"
# Pure Python, so pinning never blocks a Python version. Exact because it is installed
# with --no-deps (see requirements.txt) and worker.py uses its loader internals.
DEMUCS = "demucs==4.1.0"
TORCH_INDEX = "https://download.pytorch.org/whl/"
# (variant, label, minimum compute capability, minimum Linux driver, minimum Windows driver).
# Newest first; drivers follow NVIDIA's CUDA toolkit release notes.
CUDA_VARIANTS = (
    ("cu130", "CUDA 13.0", (7, 5), (580, 65, 6), (580, 88)),
    ("cu126", "CUDA 12.6", (5, 0), (525, 60, 13), (527, 41)),
)
# cu126 wheels carry no Blackwell (sm_100+) kernels.
CU126_MAX_CAPABILITY = (9, 0)
# pip reports an unreachable index as "from versions: none"; these mark that case.
NETWORK_ERRORS = ("NewConnectionError", "ConnectTimeoutError", "ReadTimeoutError", "ProxyError",
                  "SSLError", "Max retries exceeded", "Could not fetch URL", "Connection reset",
                  "Connection refused", "Temporary failure in name resolution",
                  "Name or service not known", "getaddrinfo failed")
NO_DISTRIBUTION = ("No matching distribution found", "Could not find a version that satisfies")
TORCH_PROBE = """\
import json, importlib.metadata as m
try:
    m.version("torch")
except m.PackageNotFoundError:
    print(json.dumps({"installed": False}))
else:
    try:
        import torch
        build = [torch.__version__, torch.version.cuda, getattr(torch.version, "hip", None)]
    except Exception:
        build = None
    print(json.dumps({"installed": True, "build": build}))
"""


class Cancelled(Exception):
    pass


class InstallError(Exception):
    pass


def emit(event, **fields):
    record = {"protocol": SCHEMA, "event": event}
    record.update(fields)
    print(json.dumps(record, ensure_ascii=False), flush=True)


def check_cancel(job):
    if (job / "cancel").exists():
        raise Cancelled()


def version_tuple(text):
    return tuple(int(part) for part in re.findall(r"\d+", text or ""))


def interpreter_problem(version, is_64bit, system, machine):
    label = "%d.%d" % tuple(version[:2])
    if tuple(version[:2]) < MIN_PYTHON or tuple(version[:2]) > MAX_PYTHON:
        return "所选 Python 为 %s；分轨运行环境需要 Python 3.10～3.14。" % label
    if not is_64bit:
        return "所选 Python %s 是 32 位版本，PyTorch 只提供 64 位安装包；请改用 64 位 Python。" % label
    if system == "Darwin" and machine != "arm64":
        return ("所选 Python 以 x86_64（Intel / Rosetta）方式运行，当前 PyTorch 只提供 Apple Silicon 安装包；"
                "请改用 arm64 原生 Python。")
    if system not in ("Windows", "Darwin", "Linux"):
        return "暂不支持在 %s 上安装分轨运行环境。" % system
    return None


def venv_problem(version, system, has_ensurepip):
    if has_ensurepip:
        return None
    if system == "Linux":
        return ("所选 Python 缺少 venv/ensurepip 组件，无法创建独立运行环境。"
                "Debian/Ubuntu 请先执行：sudo apt install python%d.%d-venv" % tuple(version[:2]))
    return "所选 Python 缺少 ensurepip 组件，无法创建独立运行环境；请改用完整安装的 Python。"


def parse_nvidia_smi(text):
    """First GPU's (driver, compute capability); capability is None when not reported."""
    for line in (text or "").splitlines():
        fields = [field.strip() for field in line.split(",")]
        if re.match(r"^\d+(\.\d+)+$", fields[0]):
            capability = fields[1] if len(fields) > 1 and re.match(r"^\d+\.\d+$", fields[1]) else ""
            return version_tuple(fields[0]), version_tuple(capability) or None
    return None


def nvidia_smi_candidates(system):
    found = shutil.which("nvidia-smi")
    candidates = [found] if found else []
    if system == "Windows":
        candidates.append(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "nvidia-smi.exe"))
        candidates.append(os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                                       "NVIDIA Corporation", "NVSMI", "nvidia-smi.exe"))
    return [path for path in candidates if path and os.path.isfile(path)]


def query_nvidia(system):
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    for exe in nvidia_smi_candidates(system):
        # compute_cap needs driver 510+; older drivers only answer the second query.
        for fields in ("driver_version,compute_cap", "driver_version"):
            try:
                result = subprocess.run([exe, "--query-gpu=" + fields, "--format=csv,noheader"],
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, universal_newlines=True,
                                        timeout=20, creationflags=flags)
            except (OSError, subprocess.SubprocessError):
                break
            gpu = parse_nvidia_smi(result.stdout) if result.returncode == 0 else None
            if gpu:
                return gpu
    return None


def choose_torch_index(system, machine, gpu, override=""):
    """Return (index URL, label); a None URL keeps pip's configured index."""
    override = (override or "").strip()
    if override:
        if override.lower() in ("default", "pypi"):
            return None, "默认软件源"
        if "://" in override:
            return override, override
        return TORCH_INDEX + override, override
    if system == "Darwin":
        return TORCH_INDEX + "cpu", "Apple MPS / CPU"
    if gpu and machine.lower() in ("amd64", "x86_64", "aarch64"):
        driver, capability = gpu
        for variant, label, min_capability, linux_driver, windows_driver in CUDA_VARIANTS:
            if driver < (windows_driver if system == "Windows" else linux_driver):
                continue
            if capability and (capability < min_capability or
                               (variant == "cu126" and capability > CU126_MAX_CAPABILITY)):
                continue
            return TORCH_INDEX + variant, label
        return TORCH_INDEX + "cpu", "CPU；当前 NVIDIA 显卡或驱动版本不受 PyTorch CUDA 版支持"
    return TORCH_INDEX + "cpu", "CPU"


def torch_matches_index(index, build):
    """build is the installed torch's (version, cuda, hip), or None when it fails to import."""
    if build is None:
        return False
    if not index or not index.startswith(TORCH_INDEX):
        return True
    variant = index[len(TORCH_INDEX):].strip("/")
    _, cuda, hip = build
    if variant == "cpu":
        return not cuda and not hip
    if variant.startswith("cu"):
        return bool(cuda) and cuda.replace(".", "") == variant[2:]
    return True


def platform_label():
    # PyTorch wheels also need macOS 14+ / glibc 2.28+; name them so an old OS is visible.
    system, machine = platform.system(), platform.machine()
    if system == "Darwin":
        return "macOS %s %s" % (platform.mac_ver()[0], machine)
    if system == "Linux":
        libc, version = platform.libc_ver()
        return "Linux %s%s" % (machine, ", %s %s" % (libc, version) if libc else "")
    return "%s %s %s" % (system, platform.release(), machine)


def is_network_failure(output):
    return any(marker in output for marker in NETWORK_ERRORS)


def describe_pip_failure(what, output, index):
    if is_network_failure(output):
        return "下载%s失败：无法连接软件源，请检查网络或代理后重试。" % what
    if any(marker in output for marker in NO_DISTRIBUTION):
        version = "%d.%d" % sys.version_info[:2]
        if what == "PyTorch":
            source = index or "当前软件源"
            return ("%s 尚未为 Python %s（%s）提供安装包。请改用 PyTorch 已支持的 Python 或系统版本，"
                    "或设置 WDS_STEM_TORCH_INDEX 指定其他 PyTorch 软件源。" % (source, version, platform_label()))
        return "当前软件源没有适用于 Python %s（%s）的%s安装包。" % (version, platform_label(), what)
    if "No space left on device" in output or "[Errno 28]" in output:
        return "安装%s失败：磁盘空间不足。" % what
    return "安装%s失败，详见日志。" % what


def child_environment():
    env = dict(os.environ)
    # A global PYTHONHOME/PYTHONPATH belongs to some other interpreter and breaks the runtime.
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    env["PIP_NO_INPUT"] = "1"
    return env


def run_logged(command, job):
    """Run with output forwarded to stderr; returns (exit code, output tail)."""
    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=child_environment(),
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    tail = bytearray()

    def pump():
        while True:
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                return
            stream = getattr(sys.stderr, "buffer", None)
            if stream is not None:
                stream.write(chunk)
            else:
                sys.stderr.write(chunk.decode("utf-8", "replace"))
            sys.stderr.flush()
            tail.extend(chunk)
            del tail[:-65536]

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    try:
        while proc.poll() is None:
            check_cancel(job)
            time.sleep(.1)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        reader.join(5)
    return proc.returncode, tail.decode("utf-8", "replace")


def run_json(command, job):
    """Run a helper that prints one JSON object; returns it or None."""
    code, output = run_logged(command, job)
    if code:
        return None
    for line in reversed(output.splitlines()):
        try:
            return json.loads(line)
        except ValueError:
            continue
    return None


def same_path(a, b):
    return os.path.normcase(str(Path(a).resolve())) == os.path.normcase(str(Path(b).resolve()))


def runtime_python(runtime):
    return runtime / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def ensure_runtime(runtime, job):
    if same_path(sys.prefix, runtime):
        return Path(sys.executable)
    python = runtime_python(runtime)
    if python.is_file():
        probe = run_json([str(python), "-c", "import json, sys; print(json.dumps(list(sys.version_info[:2])))"], job)
        if probe == list(sys.version_info[:2]):
            return python
        if not (runtime / "pyvenv.cfg").is_file():
            raise InstallError("运行环境目录不是虚拟环境，未做修改：%s" % runtime)
        # Unusable, or built from another Python: rebuild so pip resolves wheels for this one.
        emit("progress", message="正在为 Python %d.%d 重建运行环境…" % sys.version_info[:2])
        shutil.rmtree(str(runtime))
    try:
        import ensurepip  # noqa: F401
        has_ensurepip = True
    except ImportError:
        has_ensurepip = False
    problem = venv_problem(sys.version_info, platform.system(), has_ensurepip)
    if problem:
        raise InstallError(problem)
    emit("progress", message="正在创建独立运行环境…")
    code, output = run_logged([sys.executable, "-m", "venv", str(runtime)], job)
    if code or not python.is_file():
        shutil.rmtree(str(runtime), ignore_errors=True)
        # Debian keeps `import ensurepip` working but strips its wheels; venv then names it.
        problem = venv_problem(sys.version_info, platform.system(), "ensurepip" not in output)
        raise InstallError(problem or "创建独立运行环境失败，详见日志。")
    return python


def pip(python, arguments, job):
    return run_logged([str(python), "-m", "pip", "--disable-pip-version-check", "install"] + arguments, job)


def install_torch(python, job):
    system, machine = platform.system(), platform.machine()
    override = os.environ.get("WDS_STEM_TORCH_INDEX", "")
    gpu = None if system == "Darwin" or override else query_nvidia(system)
    index, label = choose_torch_index(system, machine, gpu, override)
    installed = run_json([str(python), "-c", TORCH_PROBE], job)
    if installed and installed.get("installed") and not torch_matches_index(index, installed.get("build")):
        # pip keeps a broken, CPU or other-CUDA build that already satisfies the version range.
        emit("progress", message="正在替换与显卡不匹配的 PyTorch…")
        run_logged([str(python), "-m", "pip", "--disable-pip-version-check", "uninstall", "-y", "torch"], job)
    emit("progress", message="正在下载并安装 PyTorch（%s）…" % label)
    code, output = pip(python, [TORCH] + (["--index-url", index] if index else []), job)
    if code and index and not override and is_network_failure(output):
        emit("progress", message="无法访问 PyTorch 官方源，改用默认软件源安装 PyTorch…")
        index = None
        code, output = pip(python, [TORCH], job)
    if code:
        raise InstallError(describe_pip_failure("PyTorch", output, index))


def install(args):
    job = args.job.resolve()
    problem = interpreter_problem(sys.version_info, sys.maxsize > 2 ** 32, platform.system(), platform.machine())
    if problem:
        raise InstallError(problem)
    python = ensure_runtime(args.runtime.resolve(), job)
    check_cancel(job)
    code, output = pip(python, ["--upgrade", "pip"], job)
    if code and is_network_failure(output):
        raise InstallError(describe_pip_failure("pip", output, None))
    install_torch(python, job)
    emit("progress", message="正在下载并安装分轨依赖…")
    code, output = pip(python, ["-r", str(args.requirements)], job)
    if code:
        raise InstallError(describe_pip_failure("分轨依赖", output, None))
    emit("progress", message="正在安装 Demucs…")
    code, output = pip(python, ["--no-deps", DEMUCS], job)
    if code:
        raise InstallError(describe_pip_failure("Demucs", output, None))
    emit("progress", message="正在校验运行环境…")
    runtime = run_json([str(python), str(args.worker), "--check"], job)
    if not runtime or runtime.get("event") != "runtime":
        raise InstallError("运行环境校验失败，详见日志。")
    devices = [name for name, ok in (("CUDA", runtime.get("cuda")), ("Apple MPS", runtime.get("mps"))) if ok]
    emit("complete", python=str(python), torch=runtime.get("torch"),
         message="运行环境已安装（PyTorch %s，%s），可以生成分轨。" %
                 (runtime.get("torch"), " / ".join(devices) + " 可用" if devices else "仅 CPU"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--job", type=Path, required=True)
    parser.add_argument("--requirements", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    args = parser.parse_args()
    try:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    except (ValueError, OSError):
        pass
    try:
        install(args)
        return 0
    except Cancelled:
        emit("cancelled")
        return 2
    except InstallError as exc:
        emit("error", message=str(exc))
        return 1
    except Exception as exc:
        import traceback
        traceback.print_exc(file=sys.stderr)
        emit("error", message="安装运行环境失败：%s" % exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
