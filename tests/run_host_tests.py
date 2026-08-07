"""Build and run platform-independent MAKCM tests when a C++ compiler exists.

The CI/Windows development image may not carry a compiler.  In that case the
script exits successfully with a clear SKIP so an IDF-only checkout remains
usable; native builds still compile these sources through the component.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMMON = ROOT / "common"
CORE_INCLUDE = ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" / "makcm_core" / "include"


def compiler() -> str | None:
    # Prefer the project-local, pinned toolchain when present so the offline
    # suite does not depend on a developer's global PATH.
    local_root = ROOT / ".tools" / "llvm-mingw"
    for candidate in sorted(local_root.glob("*/bin/clang++.exe")):
        if candidate.is_file():
            return str(candidate)
    for name in ("c++", "g++", "clang++", "cl"):
        found = shutil.which(name)
        if found:
            return found
    return None


def main() -> int:
    cxx = compiler()
    if cxx is None:
        print("SKIP: no host C++ compiler found")
        return 0
    if Path(cxx).name.lower() == "cl.exe":
        print("SKIP: run_host_tests.py currently expects GCC/Clang command line")
        return 0

    # Keep the host suite in lockstep with the platform-independent test files.
    # Most tests are header-only; ProfileStore additionally needs its parser
    # translation unit linked in.
    test_sources = sorted((ROOT / "tests").glob("test_*.cpp"))
    if not test_sources:
        print("FAIL: no host test sources found")
        return 1

    with tempfile.TemporaryDirectory(prefix="makcm-host-") as temp:
        # The pinned llvm-mingw clang++ may emit host executables that depend
        # on libc++.dll beside the compiler.  Keep the test runner portable by
        # making that runtime discoverable without requiring a global PATH
        # edit; system GCC/Clang paths are unchanged.
        run_env = os.environ.copy()
        compiler_dir = str(Path(cxx).resolve().parent)
        run_env["PATH"] = compiler_dir + os.pathsep + run_env.get("PATH", "")
        for source in test_sources:
            name = source.stem
            exe = Path(temp) / name
            include_dirs = [COMMON, CORE_INCLUDE]
            extra_sources: list[Path] = []
            if name == "test_controller_profile_registry":
                # The registry is an IDF translation unit (and normally gets
                # mbedTLS/FreeRTOS headers from the target build).  Compile it
                # here with the test's tiny host stubs so the exact allow-list
                # branches remain covered by the offline suite.
                include_dirs.insert(0, ROOT / "tests" / "registry_host_stubs")
                command_define = "-DMAKCM_TEST_CONTROLLER_PROFILES"
                extra_sources.append(
                    ROOT
                    / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
                    / "components"
                    / "makcm_core"
                    / "controller_profile_registry.cpp"
                )
            else:
                command_define = None
            command = [
                cxx,
                "-std=c++17",
                *[f"-I{directory}" for directory in include_dirs],
                str(source),
            ]
            if command_define is not None:
                command.insert(2, command_define)
            if name in ("test_profile_store", "test_profile_blob",
                        "test_input_policy", "test_four_in_two_out_profile",
                        "test_generic_profile_routing"):
                command.append(str(
                    ROOT
                    / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
                    / "components"
                    / "makcm_core"
                    / "profile_store.cpp"
                ))
            if name == "test_stick_runtime":
                command.append(str(
                    ROOT
                    / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
                    / "components"
                    / "makcm_core"
                    / "stick_runtime.cpp"
                ))
            command.extend(str(extra_source) for extra_source in extra_sources)
            command.extend(["-o", str(exe)])
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT, env=run_env)
            print(f"PASS: {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
