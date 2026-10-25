#!/usr/bin/env python3
# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Cross-platform build backend behind the dd gateway. dd.ps1 owns the Windows release
# machinery (signing, MSIX, GitHub); this owns the CMake/Ninja build on both hosts, and is the only
# place that knows how to bring a machine up to being able to build Diffractor.
#
# Invoke through the gateway rather than directly: ./dd <command> or .\dd.ps1 <command>.

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Build tooling. Everything Diffractor cannot be compiled without.
APT_TOOLCHAIN = [
    "build-essential",
    "cmake",
    "ninja-build",
    "pkg-config",
    "ccache",
    "nasm",  # FFmpeg and dav1d assembly
    "python3",
    "git",
    "git-lfs",  # exe/test fixtures are LFS objects, see .gitattributes
]

# System copies of fetched libraries, used during bring-up. These are a convenience: the shipped
# build fetches and builds its own (see docs/third-party.md). FFmpeg is deliberately absent because
# it is a Diffractor fork and must never come from a distribution.
APT_DEPENDENCIES = [
    "zlib1g-dev",
    "libpng-dev",
    "libjpeg-dev",  # virtual: resolves to libjpeg-turbo on Debian and Ubuntu
    "libsqlite3-dev",
    "libwebp-dev",
    "libarchive-dev",
    "libhunspell-dev",  # util_spell.cpp
    "libminizip-dev",  # util_zip.cpp
]


def is_windows() -> bool:
    return platform.system() == "Windows"


def build_dir(args: argparse.Namespace) -> Path:
    """One directory per host, architecture and product, not per configuration.

    The configuration is a CMake cache entry rather than part of the path, so that `dd test` after
    `dd build --config Release` finds the binary that was just built. cmd_build reconfigures when
    the cached configuration differs from the requested one, which is what makes that safe.

    The Store variant does get its own directory. It differs by a CMake option rather than by build
    type, so sharing one would need that option in the same staleness check, and a Store build that
    silently came out without WINSTORE would ship an application-owned updater into the Store.
    """
    if not is_windows():
        return REPO_ROOT / "tmp" / "linux-build"

    name = f"win-build-{getattr(args, 'arch', 'x64')}"
    if getattr(args, "winstore", False):
        name += "-store"

    return REPO_ROOT / "tmp" / name


def binary_path(args: argparse.Namespace) -> Path:
    """Where the build puts the executable, under the name the rest of the toolchain expects."""
    if not is_windows():
        return build_dir(args) / "diffractor"

    stem = {"x64": "diffractor64", "x86": "diffractor32"}.get(args.arch, "diffractor-arm64")

    # The Store package ships one binary called diffractor.exe, and it is Release only.
    if getattr(args, "winstore", False) and args.arch != "arm64":
        stem = "diffractor"
    elif args.config == "Debug":
        stem += "-d"

    return REPO_ROOT / "exe" / f"{stem}.exe"


# The Visual Studio architecture argument for a given target, host x64 assumed.
VCVARS_ARCH = {"x64": "x64", "x86": "x64_x86", "arm64": "x64_arm64"}

_msvc_env_cache: dict[str, dict[str, str]] = {}


def msvc_environment(arch: str) -> dict[str, str]:
    """The environment cl.exe, link.exe, rc.exe and the SDK need.

    Ninja does not set this up the way a Visual Studio generator does, and one generator on both
    hosts is worth more than avoiding this: it means a Linux failure and a Windows failure are the
    same kind of failure. vcvarsall is run once and its environment captured, rather than shelling
    through cmd for every compiler invocation.
    """
    if arch in _msvc_env_cache:
        return _msvc_env_cache[arch]

    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"

    if not vswhere.exists():
        sys.exit(f"{vswhere} not found. Install Visual Studio with the C++ workload.")

    found = subprocess.run(
        [str(vswhere), "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True,
    )
    install = found.stdout.strip().splitlines()

    if not install:
        sys.exit("No Visual Studio install with the C++ tools was found.")

    vcvarsall = Path(install[0]) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"

    if not vcvarsall.exists():
        sys.exit(f"{vcvarsall} not found.")

    say(f"  $ \"{vcvarsall}\" {VCVARS_ARCH[arch]}")

    # The marker separates vcvarsall's own chatter from the environment dump; without it a banner
    # line containing '=' would be read as a variable.
    #
    # Passed as one string with shell=True rather than as a list: a list goes through
    # list2cmdline, which backslash-escapes the quotes around the path and leaves cmd looking for
    # a program called '\"C:\Program'.
    marker = "__DIFFRACTOR_ENV__"
    dumped = subprocess.run(
        f'"{vcvarsall}" {VCVARS_ARCH[arch]} >nul && echo {marker} && set',
        shell=True, capture_output=True, text=True,
    )

    if dumped.returncode != 0:
        sys.exit(f"vcvarsall failed:\n{dumped.stdout}\n{dumped.stderr}")

    environment = dict(os.environ)
    seen_marker = False

    for line in dumped.stdout.splitlines():
        if not seen_marker:
            seen_marker = line.strip() == marker
            continue
        name, separator, value = line.partition("=")
        if separator:
            environment[name] = value

    if not seen_marker:
        sys.exit("vcvarsall produced no environment.")

    _msvc_env_cache[arch] = environment
    return environment


def build_environment(args: argparse.Namespace) -> dict[str, str] | None:
    return msvc_environment(args.arch) if is_windows() else None


def say(message: str) -> None:
    print(message, flush=True)


def run(command: list[str], cwd: Path | None = None, check: bool = True,
        env: dict[str, str] | None = None) -> int:
    say("  $ " + " ".join(command))
    result = subprocess.run(command, cwd=str(cwd or REPO_ROOT), env=env)
    if check and result.returncode != 0:
        sys.exit(result.returncode)
    return result.returncode


def missing_apt_packages(packages: list[str]) -> list[str]:
    missing = []
    for package in packages:
        installed = subprocess.run(
            ["dpkg-query", "-W", "-f=${Status}", package],
            capture_output=True,
            text=True,
        )
        if "install ok installed" not in installed.stdout:
            missing.append(package)
    return missing


def configure_git_for_mirrored_tree() -> None:
    """Stop git misreporting a working tree copied from Windows.

    Two things make the whole tree look modified, and neither is a real change: the Windows
    checkout has core.autocrlf=true so every text file holds CRLF, and exe/test is Git LFS, so
    without the filter git compares real bytes against a pointer blob. Left alone, a commit from
    here would rewrite every text file and replace every LFS pointer with a raw binary.
    """
    if not (REPO_ROOT / ".git").exists():
        return

    run(["git", "config", "core.autocrlf", "true"], check=False)

    if shutil.which("git-lfs"):
        run(["git", "lfs", "install", "--local"], check=False)
    else:
        say("  git-lfs not found: exe/test fixtures will look modified and must not be committed.")


# -------------------------------------------------------------------------------------------
# Commands
# -------------------------------------------------------------------------------------------

def cmd_setup(args: argparse.Namespace) -> None:
    """Install the toolchain and the system packages the build resolves against."""
    if is_windows():
        say("")
        say("Windows setup is Visual Studio with the C++ workload, plus CMake and Ninja, which")
        say("that workload installs. nasm is fetched while configuring when none is on the path.")
        say("Nothing to do here.")
        say("")
        cmd_info(args)
        return

    if not shutil.which("apt-get"):
        sys.exit("setup currently understands apt only. Install the equivalents by hand:\n"
                 f"  toolchain:    {' '.join(APT_TOOLCHAIN)}\n"
                 f"  dependencies: {' '.join(APT_DEPENDENCIES)}")

    wanted = APT_TOOLCHAIN + APT_DEPENDENCIES
    missing = missing_apt_packages(wanted)

    say("")
    say(f"Diffractor setup  ({len(wanted) - len(missing)}/{len(wanted)} already present)")
    say("")

    for package in wanted:
        say(f"  {'MISSING' if package in missing else 'ok     '}  {package}")

    say("")

    if not missing:
        say("Nothing to install.")
        return

    if args.check:
        say(f"{len(missing)} package(s) missing. Run './dd setup' to install them.")
        sys.exit(1)

    say("Installing. This needs sudo and will prompt for your password.")
    say("")
    run(["sudo", "apt-get", "update"])
    run(["sudo", "apt-get", "install", "-y", *missing])
    say("")
    say("Configuring git for a working tree mirrored from Windows.")
    configure_git_for_mirrored_tree()
    say("")
    say("Setup complete. Next: ./dd build")


def find_ninja() -> str | None:
    """Ninja ships inside the Visual Studio C++ workload but is not put on the path."""
    found = shutil.which("ninja")
    if found or not is_windows():
        return found

    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        pattern = "Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
        for candidate in sorted(Path(root).glob(pattern), reverse=True):
            return str(candidate)

    return None


def find_cmake() -> str:
    found = shutil.which("cmake")
    if found or not is_windows():
        return found or "cmake"

    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        pattern = "Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
        for candidate in sorted(Path(root).glob(pattern), reverse=True):
            return str(candidate)

    return "cmake"


def cmd_configure(args: argparse.Namespace) -> None:
    ninja = find_ninja()
    generator = "Ninja" if ninja else ("NMake Makefiles" if is_windows() else "Unix Makefiles")

    if not ninja:
        say("ninja not found, falling back. Install it for the faster build.")

    command = [
        find_cmake(),
        "-S", str(REPO_ROOT),
        "-B", str(build_dir(args)),
        "-G", generator,
        f"-DCMAKE_BUILD_TYPE={args.config}",
    ]

    if ninja and Path(ninja).is_absolute():
        command.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")

    if getattr(args, "winstore", False):
        command.append("-DDIFFRACTOR_WINSTORE=ON")

    command += [f"-D{d}" for d in (args.define or [])]
    run(command, env=build_environment(args))


def cached_build_type(directory: Path) -> str | None:
    cache = directory / "CMakeCache.txt"

    if not cache.exists():
        return None

    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_BUILD_TYPE:"):
            return line.partition("=")[2].strip()

    return None


def cmd_build(args: argparse.Namespace) -> None:
    directory = build_dir(args)

    # Reconfigure when the configuration changes as well as when there is none. A single-config
    # generator bakes the build type into the cache, so building Release over a Debug cache would
    # silently produce Debug again.
    if cached_build_type(directory) != args.config:
        cmd_configure(args)

    run([find_cmake(), "--build", str(directory), "-j", str(args.jobs)],
        env=build_environment(args))
    say("")
    say(f"Built {binary_path(args)}")


def cmd_run(args: argparse.Namespace) -> None:
    binary = binary_path(args)
    if not binary.exists():
        sys.exit(f"{binary} does not exist. Run 'dd build' first.")
    run([str(binary), *args.rest], check=False)


def cmd_test(args: argparse.Namespace) -> None:
    binary = binary_path(args)
    if not binary.exists():
        sys.exit(f"{binary} does not exist. Run 'dd build' first.")
    sys.exit(run([str(binary), "/test", *args.rest], check=False))


def cmd_clean(args: argparse.Namespace) -> None:
    directory = build_dir(args)
    if directory.exists():
        say(f"Removing {directory}")
        shutil.rmtree(directory)
    else:
        say("Nothing to clean.")


def cmd_info(args: argparse.Namespace) -> None:
    say("")
    say(f"  repo         {REPO_ROOT}")
    say(f"  build dir    {build_dir(args)}")
    say(f"  binary       {binary_path(args)}")
    say(f"  host         {platform.system()} {platform.release()}")
    say(f"  target       {args.arch} {args.config}")
    say(f"  python       {platform.python_version()}")
    say("")

    say(f"  {'cmake':<12} {find_cmake()}")
    say(f"  {'ninja':<12} {find_ninja() or 'MISSING'}")

    tools = ("cl", "nasm") if is_windows() else ("g++", "clang++", "pkg-config", "ccache", "nasm")
    for tool in tools:
        found = shutil.which(tool)
        say(f"  {tool:<12} {found or 'MISSING'}")

    say("")


COMMANDS = {
    "setup": cmd_setup,
    "configure": cmd_configure,
    "build": cmd_build,
    "run": cmd_run,
    "test": cmd_test,
    "clean": cmd_clean,
    "info": cmd_info,
}


def main() -> None:
    parser = argparse.ArgumentParser(prog="dd", description="Diffractor build backend")
    parser.add_argument("command", choices=sorted(COMMANDS))
    parser.add_argument("--config", default="Debug", help="CMake build type (default: Debug)")
    parser.add_argument("--arch", default="x64", choices=sorted(VCVARS_ARCH),
                        help="Windows target architecture (default: x64)")
    parser.add_argument("--winstore", action="store_true", help="build the Microsoft Store variant")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--define", "-D", action="append", help="extra -D passed to CMake")
    parser.add_argument("--check", action="store_true", help="report, do not change anything")

    # Not a REMAINDER positional. REMAINDER takes everything after the command, including this
    # script's own options, so `dd build --config Release` silently built Debug -- which is what
    # the Linux CI matrix was doing on both of its legs.
    args, args.rest = parser.parse_known_args()

    # The Store variant is Release by definition; it is the Release configuration plus a define.
    if args.winstore:
        args.config = "Release"

    COMMANDS[args.command](args)


if __name__ == "__main__":
    main()
