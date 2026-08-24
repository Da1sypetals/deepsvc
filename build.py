import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ENGINE_DIR = ROOT / "native"
PLUGIN_DIR = ROOT / "plugin"
CHECKPOINTS_DIR = ROOT / "checkpoints"
BUILD_DIR = ROOT / "build"
SOXR_SRC = ROOT / "modules" / "soxr"
SOXR_BUILD = BUILD_DIR / "soxr-build"
SOXR_INSTALL = BUILD_DIR / "soxr-install"
ARTEFACTS = BUILD_DIR / "deepsvc_artefacts" / "Release"
PLUGINS_DIR = Path.home() / "Library" / "Audio" / "Plug-Ins"

# 与 native/src/worker.rs 中引擎加载的文件一一对应
REQUIRED_MODELS = [
    "rmvpe.safetensors",
    "fcpe.safetensors",
    "whisper.safetensors",
    "campplus.safetensors",
    "yingmusic_step_000640.safetensors",
    "pupu-vocoder-large.safetensors",
    "pc-nsf-hifigan.safetensors",
]


def run(command: list, cwd: Path | None = None, env: dict | None = None) -> None:
    print("+", " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=cwd, check=True, env=env)


def build_soxr() -> None:
    # libsoxr 静态链接进插件，保证插件自包含
    if (SOXR_INSTALL / "lib" / "libsoxr.a").is_file():
        return
    run([
        "cmake", "-S", SOXR_SRC, "-B", SOXR_BUILD,
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_TESTS=OFF",
        "-DBUILD_EXAMPLES=OFF",
        "-DWITH_OPENMP=OFF",
        "-DWITH_LSR_BINDINGS=OFF",
        f"-DCMAKE_INSTALL_PREFIX={SOXR_INSTALL}",
    ])
    run(["cmake", "--build", SOXR_BUILD, "-j", str(os.cpu_count() or 4)])
    run(["cmake", "--build", SOXR_BUILD, "--target", "install"])


def build_engine() -> Path:
    env = dict(os.environ)
    # libsoxr-sys 经 pkg-config 找到静态库
    env["PKG_CONFIG_PATH"] = str(SOXR_INSTALL / "lib" / "pkgconfig")
    env["PKG_CONFIG_ALL_STATIC"] = "1"
    run(["cargo", "build", "--release"], cwd=ENGINE_DIR, env=env)
    library = ENGINE_DIR / "target" / "release" / "libdeepsvc_engine.a"
    if not library.is_file():
        sys.exit(f"引擎静态库构建产物不存在: {library}")
    return library


def check_models() -> None:
    missing = [name for name in REQUIRED_MODELS if not (CHECKPOINTS_DIR / name).is_file()]
    if missing:
        sys.exit(f"checkpoints/ 缺少模型文件: {', '.join(missing)}")


def build_plugin() -> None:
    run([
        "cmake", "-S", PLUGIN_DIR, "-B", BUILD_DIR,
        "-DCMAKE_BUILD_TYPE=Release",
    ])
    run(["cmake", "--build", BUILD_DIR, "--config", "Release", "-j", str(os.cpu_count() or 4)])


def install() -> None:
    bundles = [
        (ARTEFACTS / "AU" / "deepsvc.component", PLUGINS_DIR / "Components" / "deepsvc.component"),
        (ARTEFACTS / "VST3" / "deepsvc.vst3", PLUGINS_DIR / "VST3" / "deepsvc.vst3"),
    ]
    for source, target in bundles:
        if not source.exists():
            sys.exit(f"插件构建产物不存在: {source}")
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(source, target)
        print(f"已安装 {target}")


def main() -> None:
    check_models()
    build_soxr()
    build_engine()
    build_plugin()
    install()
    print("完成：插件已构建并安装，重启宿主后即可使用")


if __name__ == "__main__":
    main()
