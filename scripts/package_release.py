#!/usr/bin/env python3
"""Local multi-platform CMake release packager for nijinWVDC."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile


ALL_TARGETS = (
    "macos-arm64",
    "linux-x86_64",
    "windows-x86_64",
)


def read_project_version(root: Path) -> str:
    """Parse project(nijinWVDC VERSION X.Y.Z ...) from CMakeLists.txt."""
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\(\s*nijinWVDC\s+VERSION\s+(\d+\.\d+\.\d+)\b",
        cmake_text,
    )
    if not match:
        raise RuntimeError("Could not parse project VERSION from CMakeLists.txt")
    return match.group(1)


def run_cmd(cmd: list[str], cwd: Path) -> None:
    """Run a build/test subprocess and propagate non-zero exit status."""
    print(f"+ {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def jobs_count() -> str:
    """Return parallel build concurrency string."""
    return str(max(1, os.cpu_count() or 4))


def find_tool(candidates: list[str], label: str) -> str:
    """Resolve the first executable from PATH or explicit fallback paths."""
    for item in candidates:
        resolved = shutil.which(item) if "/" not in item else (item if os.access(item, os.X_OK) else None)
        if resolved:
            return resolved
    raise RuntimeError(f"Missing required {label} tool (tried: {', '.join(candidates)})")


def create_tar_gz(pkg_dir: Path, out_path: Path) -> None:
    """Create a .tar.gz archive preserving symlinks atomically."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_name(out_path.name + ".tmp")
    try:
        with tarfile.open(tmp_path, "w:gz") as archive:
            archive.add(pkg_dir, arcname=pkg_dir.name)
        tmp_path.replace(out_path)
    finally:
        tmp_path.unlink(missing_ok=True)


def create_zip(pkg_dir: Path, out_path: Path) -> None:
    """Create a .zip archive with top-level package directory atomically."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_name(out_path.name + ".tmp")
    try:
        files = sorted(p for p in pkg_dir.rglob("*") if p.is_file())
        with zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for item in files:
                archive.write(item, item.relative_to(pkg_dir.parent).as_posix())
        tmp_path.replace(out_path)
    finally:
        tmp_path.unlink(missing_ok=True)


def generate_windows_import_lib(def_path: Path, lib_out: Path, mingw_prefix: str) -> None:
    """Generate COFF import library (.lib) from .def using llvm-dlltool or GNU dlltool."""
    llvm_dlltool = find_tool(
        ["llvm-dlltool", "/opt/homebrew/opt/llvm/bin/llvm-dlltool", f"{mingw_prefix}dlltool"],
        "dlltool",
    )
    if Path(llvm_dlltool).name.startswith("llvm-"):
        cmd = [llvm_dlltool, "-m", "i386:x86-64", "-D", "nijinWVDC.dll", "-d", str(def_path), "-l", str(lib_out)]
    else:
        cmd = [llvm_dlltool, "-D", "nijinWVDC.dll", "-d", str(def_path), "-l", str(lib_out)]
    run_cmd(cmd, def_path.parent)


def configure_build_install(
    root: Path,
    bdir: Path,
    stage_pkg: Path,
    cmake_args: list[str],
    run_tests: bool,
) -> None:
    """Configure, build, optionally test, and install a CMake configuration."""
    if bdir.exists():
        shutil.rmtree(bdir)
    if stage_pkg.exists():
        shutil.rmtree(stage_pkg)
    bdir.mkdir(parents=True, exist_ok=True)
    stage_pkg.parent.mkdir(parents=True, exist_ok=True)

    run_cmd(
        ["cmake", "-S", str(root), "-B", str(bdir), "-DCMAKE_BUILD_TYPE=Release", "-DNIJINWVDC_BUILD_TESTS=ON"]
        + cmake_args,
        root,
    )
    run_cmd(["cmake", "--build", str(bdir), "-j", jobs_count()], root)
    if run_tests:
        run_cmd(["ctest", "--test-dir", str(bdir), "--output-on-failure"], root)
    run_cmd(["cmake", "--install", str(bdir), "--prefix", str(stage_pkg)], root)


def build_macos_target(
    root: Path,
    tag: str,
    build_root: Path,
    stage_root: Path,
    out_dir: Path,
) -> Path:
    """Build, test, and package macOS arm64."""
    if sys.platform != "darwin":
        raise RuntimeError("Target macos-arm64 requires a macOS host")
    pkg_name = f"nijinWVDC-{tag}-macos-arm64"
    bdir = build_root / "macos-arm64"
    stage_pkg = stage_root / pkg_name
    cmake_args = [
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0",
        "-DCMAKE_OSX_ARCHITECTURES=arm64",
    ]
    configure_build_install(root, bdir, stage_pkg, cmake_args, run_tests=True)
    out_path = out_dir / f"{pkg_name}.tar.gz"
    create_tar_gz(stage_pkg, out_path)
    return out_path


def build_linux_target(
    root: Path,
    tag: str,
    build_root: Path,
    stage_root: Path,
    out_dir: Path,
    linux_prefix: str,
) -> Path:
    """Build and package Linux x86_64 with static libstdc++/libgcc runtime linkage."""
    pkg_name = f"nijinWVDC-{tag}-linux-x86_64"
    bdir = build_root / "linux-x86_64"
    stage_pkg = stage_root / pkg_name

    cc = find_tool([f"{linux_prefix}gcc"], "Linux C compiler")
    cxx = find_tool([f"{linux_prefix}g++"], "Linux C++ compiler")
    cmake_args = [
        "-DCMAKE_SYSTEM_NAME=Linux",
        "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
        f"-DCMAKE_C_COMPILER={cc}",
        f"-DCMAKE_CXX_COMPILER={cxx}",
        "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc",
        "-DCMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -static-libgcc",
    ]
    for tool_var, suffix in (("CMAKE_AR", "ar"), ("CMAKE_RANLIB", "ranlib")):
        found = shutil.which(f"{linux_prefix}{suffix}")
        if found:
            cmake_args.append(f"-D{tool_var}={found}")

    native_linux_x64 = sys.platform.startswith("linux") and platform.machine().lower() in ("x86_64", "amd64")
    configure_build_install(root, bdir, stage_pkg, cmake_args, run_tests=native_linux_x64 and not linux_prefix)
    out_path = out_dir / f"{pkg_name}.tar.gz"
    create_tar_gz(stage_pkg, out_path)
    return out_path


def build_windows_target(
    root: Path,
    tag: str,
    build_root: Path,
    stage_root: Path,
    out_dir: Path,
    mingw_prefix: str,
) -> Path:
    """Build and package Windows x86_64 via MinGW-w64 GCC with static runtime linkage."""
    pkg_name = f"nijinWVDC-{tag}-windows-x86_64"
    bdir = build_root / "windows-x86_64"
    stage_pkg = stage_root / pkg_name
    def_path = bdir.resolve() / "nijinWVDC.def"

    cc = find_tool([f"{mingw_prefix}gcc"], "MinGW C compiler")
    cxx = find_tool([f"{mingw_prefix}g++"], "MinGW C++ compiler")
    rc = find_tool([f"{mingw_prefix}windres"], "MinGW windres")
    cmake_args = [
        "-DCMAKE_SYSTEM_NAME=Windows",
        "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
        f"-DCMAKE_C_COMPILER={cc}",
        f"-DCMAKE_CXX_COMPILER={cxx}",
        f"-DCMAKE_RC_COMPILER={rc}",
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++",
        f"-DCMAKE_SHARED_LINKER_FLAGS=-static -static-libgcc -static-libstdc++ -Wl,--output-def,{def_path.as_posix()}",
    ]
    native_win_x64 = sys.platform == "win32" and platform.machine().lower() in ("amd64", "x86_64")
    configure_build_install(root, bdir, stage_pkg, cmake_args, run_tests=native_win_x64)

    if not def_path.is_file():
        raise RuntimeError(f"Expected export definition file missing: {def_path}")
    lib_dir = stage_pkg / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(def_path, lib_dir / "nijinWVDC.def")
    generate_windows_import_lib(def_path, lib_dir / "nijinWVDC.lib", mingw_prefix)

    out_path = out_dir / f"{pkg_name}.zip"
    create_zip(stage_pkg, out_path)
    return out_path


def write_checksums(out_dir: Path, tag: str) -> Path:
    """Write SHA256SUMS.txt over nijinWVDC-<tag>-* archives in out_dir."""
    prefix = f"nijinWVDC-{tag}-"
    archives = sorted(
        p for p in out_dir.iterdir()
        if p.is_file() and p.name.startswith(prefix) and (p.name.endswith(".tar.gz") or p.name.endswith(".zip"))
    )
    if not archives:
        raise RuntimeError(f"No {prefix}* archives found in {out_dir}")
    lines = []
    for path in archives:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        lines.append(f"{digest.hexdigest()}  {path.name}\n")
    sums_path = out_dir / "SHA256SUMS.txt"
    sums_path.write_text("".join(lines), encoding="utf-8")
    return sums_path


def parse_args(default_tag: str) -> argparse.Namespace:
    """Parse CLI arguments."""
    default_linux_prefix = (
        "" if sys.platform.startswith("linux") and platform.machine().lower() in ("x86_64", "amd64")
        else "x86_64-unknown-linux-gnu-"
    )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default=default_tag, help="Release tag name (default: v<VERSION>)")
    parser.add_argument("--out-dir", default="release_dist", help="Output archive directory")
    parser.add_argument("--build-root", default="build/release-pkg", help="Intermediate CMake build directory")
    parser.add_argument(
        "--targets",
        default=",".join(ALL_TARGETS),
        help=f"Comma-separated subset of: {', '.join(ALL_TARGETS)}",
    )
    parser.add_argument(
        "--linux-prefix",
        default=default_linux_prefix,
        help="Toolchain prefix for linux-x86_64 (default: %(default)r)",
    )
    parser.add_argument(
        "--mingw-prefix",
        default="x86_64-w64-mingw32-",
        help="MinGW-w64 toolchain prefix for windows-x86_64 (default: %(default)r)",
    )
    parser.add_argument(
        "--keep-build",
        action="store_true",
        help="Keep intermediate CMake build directories under --build-root",
    )
    parser.add_argument(
        "--checksums-only",
        action="store_true",
        help="Only regenerate SHA256SUMS.txt over existing archives in --out-dir",
    )
    return parser.parse_args()


def main() -> int:
    """Entry point."""
    root = Path(__file__).resolve().parent.parent
    version = read_project_version(root)
    args = parse_args(f"v{version}")
    if args.tag != f"v{version}":
        raise RuntimeError(f"--tag ({args.tag}) must match CMakeLists.txt version (v{version})")

    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = (root / out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.checksums_only:
        sums_path = write_checksums(out_dir, args.tag)
        print(f"Wrote {sums_path}")
        return 0

    targets = [t.strip() for t in args.targets.split(",") if t.strip()]
    unknown = [t for t in targets if t not in ALL_TARGETS]
    if not targets or unknown:
        raise RuntimeError(f"Unknown target(s): {', '.join(unknown)}")

    build_root = Path(args.build_root)
    if not build_root.is_absolute():
        build_root = (root / build_root).resolve()
    stage_root = build_root / "_stage"

    built: list[Path] = []
    for target in targets:
        if target == "macos-arm64":
            built.append(build_macos_target(root, args.tag, build_root, stage_root, out_dir))
        elif target == "linux-x86_64":
            built.append(build_linux_target(root, args.tag, build_root, stage_root, out_dir, args.linux_prefix))
        elif target == "windows-x86_64":
            built.append(build_windows_target(root, args.tag, build_root, stage_root, out_dir, args.mingw_prefix))

    if stage_root.exists():
        shutil.rmtree(stage_root)
    if not args.keep_build and build_root.exists():
        shutil.rmtree(build_root)

    sums_path = write_checksums(out_dir, args.tag)
    for item in built:
        print(f"Packaged {item}")
    print(f"Checksums {sums_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
