"""Builds TaskbarTuner with cl.exe directly, without CMake.

CMake is not installed on every machine, but the compiler used for this project
ships as a plain file tree, so the two targets are easy to drive by hand. The
result is identical to what CMakeLists.txt describes: one DLL and one EXE with
the DLL embedded as a resource.

    python tools/build_cl.py --vc <VC/Tools/MSVC/x.y.z> --sdk <sdk root> \
        [--rc <path to rc.exe>] [--config Release]

--sdk accepts an installed Windows SDK (the folder that holds Include/ and
Lib/) as well as the flat layout produced by `xwin splat`:

    <sdk>/crt/include, <sdk>/crt/lib/x64
    <sdk>/sdk/include/{ucrt,um,shared,winrt,cppwinrt}
    <sdk>/sdk/lib/{ucrt,um}/x64

rc.exe is looked up in the SDK when --rc is omitted.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFINES = [
    "UNICODE",
    "_UNICODE",
    "WIN32_LEAN_AND_MEAN",
    "NOMINMAX",
    "_WIN32_WINNT=0x0A00",
]

COMMON = [
    "/nologo",
    "/c",
    "/EHsc",
    "/permissive-",
    "/utf-8",
    "/W4",
    "/std:c++20",
    # Static CRT: the program is shipped as a single file, so it must not need
    # the redistributable, and the injected DLL must not add a second copy of
    # msvcp140 into explorer.exe.
    "/MT",
]

HOOK_SOURCES = [
    "src/hook/dllmain.cpp",
    "src/hook/TaskbarHost.cpp",
    "src/hook/VisualTree.cpp",
    "src/hook/BackdropBrush.cpp",
    "src/shared/Config.cpp",
    "src/shared/Log.cpp",
]

HOST_SOURCES = [
    "src/host/main.cpp",
    "src/host/Panel.cpp",
    "src/host/Injector.cpp",
    "src/shared/Config.cpp",
    "src/shared/Log.cpp",
]

HOOK_LIBS = [
    "windowsapp.lib",
    "comctl32.lib",
    "ole32.lib",
    "user32.lib",
    "gdi32.lib",
    "advapi32.lib",
]

HOST_LIBS = [
    "shell32.lib",
    "comctl32.lib",
    "dwmapi.lib",
    "uxtheme.lib",
    "advapi32.lib",
    "user32.lib",
    "gdi32.lib",
]


def first_existing(*paths):
    for path in paths:
        if os.path.exists(path):
            return path
    return None


def run(command, cwd=None, env=None):
    printable = " ".join(f'"{part}"' if " " in part else part for part in command)
    print("  " + printable)
    result = subprocess.run(command, cwd=cwd, env=env)
    if result.returncode != 0:
        sys.exit(f"failed with exit code {result.returncode}")


def sdk_layout(sdk):
    """Finds the include and lib roots of a Windows SDK.

    Two layouts are supported: the flat one produced by `xwin splat`
    (`<sdk>/sdk/include/um`) and a normal installed SDK
    (`<sdk>/Include/<version>/um`). Returns (include, lib) or exits.
    """
    candidates = []
    if os.path.isdir(os.path.join(sdk, "sdk", "include", "um")):
        candidates.append((os.path.join(sdk, "sdk", "include"),
                           os.path.join(sdk, "sdk", "lib")))
    if os.path.isdir(os.path.join(sdk, "include", "um")):
        candidates.append((os.path.join(sdk, "include"),
                           os.path.join(sdk, "lib")))
    include_root = os.path.join(sdk, "Include")
    if os.path.isdir(include_root):
        for version in sorted(os.listdir(include_root), reverse=True):
            if os.path.isdir(os.path.join(include_root, version, "um")):
                candidates.append((os.path.join(include_root, version),
                                   os.path.join(sdk, "Lib", version)))
                break

    for include, lib in candidates:
        if os.path.isdir(os.path.join(lib, "um", "x64")):
            return include, lib

    sys.exit(f"no usable Windows SDK layout under {sdk}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vc", required=True, help="VC/Tools/MSVC/<version> directory")
    parser.add_argument("--sdk", required=True, help="Windows SDK root or sysroot")
    parser.add_argument("--rc", help="path to rc.exe (found in the SDK if omitted)")
    parser.add_argument("--build", default=os.path.join(ROOT, "build"))
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()

    vc = os.path.abspath(args.vc)
    sdk = os.path.abspath(args.sdk)
    build = os.path.abspath(args.build)

    cl = os.path.join(vc, "bin", "Hostx64", "x64", "cl.exe")
    if not os.path.exists(cl):
        sys.exit(f"cl.exe not found at {cl}")

    include_root, lib_root = sdk_layout(sdk)

    include_dirs = [
        os.path.join(vc, "include"),
        os.path.join(sdk, "crt", "include"),  # absent in an installed SDK
        os.path.join(include_root, "ucrt"),
        os.path.join(include_root, "shared"),
        os.path.join(include_root, "um"),
        os.path.join(include_root, "winrt"),
        os.path.join(include_root, "cppwinrt"),
        os.path.join(ROOT, "src"),
        os.path.join(ROOT, "res"),
    ]
    lib_dirs = [
        os.path.join(vc, "lib", "x64"),
        os.path.join(sdk, "crt", "lib", "x64"),
        os.path.join(lib_root, "ucrt", "x64"),
        os.path.join(lib_root, "um", "x64"),
    ]
    include_dirs = [path for path in include_dirs if os.path.isdir(path)]
    lib_dirs = [path for path in lib_dirs if os.path.isdir(path)]
    for required in (os.path.join(vc, "include"), os.path.join(include_root, "um"),
                     os.path.join(include_root, "cppwinrt")):
        if not os.path.isdir(required):
            sys.exit(f"missing toolchain directory: {required}")

    rc = args.rc
    if not rc:
        for version in sorted(os.listdir(os.path.join(sdk, "bin")), reverse=True) \
                if os.path.isdir(os.path.join(sdk, "bin")) else []:
            candidate = os.path.join(sdk, "bin", version, "x64", "rc.exe")
            if os.path.exists(candidate):
                rc = candidate
                break
    if not rc or not os.path.exists(rc):
        rc = shutil.which("rc.exe")
    if not rc or not os.path.exists(rc):
        sys.exit("rc.exe not found; pass --rc <path>")

    env = dict(os.environ)
    env["INCLUDE"] = ";".join(include_dirs)
    env["LIB"] = ";".join(lib_dirs)
    env["PATH"] = os.path.dirname(cl) + os.pathsep + env.get("PATH", "")

    optimize = ["/O2"] if args.config == "Release" else ["/Od", "/Zi"]
    defines = [f"/D{name}" for name in DEFINES]

    obj_root = os.path.join(build, "obj")
    hook_out = os.path.join(build, "hook")
    bin_out = os.path.join(build, "bin")
    for path in (obj_root, hook_out, bin_out):
        os.makedirs(path, exist_ok=True)

    print("[1/4] compiling the hook")
    hook_objects = []
    for source in HOOK_SOURCES:
        name = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(obj_root, f"hook_{name}.obj")
        run([cl] + COMMON + optimize + defines + [f"/Fo{obj}", os.path.join(ROOT, source)], env=env)
        hook_objects.append(obj)

    print("[2/4] linking TaskbarTunerHook.dll")
    hook_dll = os.path.join(hook_out, "TaskbarTunerHook.dll")
    run(
        [cl, "/nologo", "/LD", f"/Fe{hook_dll}"]
        + hook_objects
        + ["/link", "/SUBSYSTEM:WINDOWS", "/DLL", "/NOLOGO"]
        + HOOK_LIBS,
        env=env,
    )

    print("[3/4] compiling resources")
    generated = os.path.join(build, "generated")
    os.makedirs(generated, exist_ok=True)
    template = os.path.join(ROOT, "res", "TaskbarTuner.rc.in")
    with open(template, encoding="utf-8") as handle:
        text = handle.read()
    text = text.replace("@TUNER_HOOK_DLL@", hook_dll.replace("\\", "\\\\"))
    text = text.replace("@TUNER_ICON@", os.path.join(ROOT, "res", "TaskbarTuner.ico").replace("\\", "\\\\"))
    text = text.replace("@TUNER_MANIFEST@", os.path.join(ROOT, "res", "app.manifest").replace("\\", "\\\\"))
    rc_path = os.path.join(generated, "TaskbarTuner.rc")
    with open(rc_path, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(text)

    res = os.path.join(generated, "TaskbarTuner.res")
    run([os.path.abspath(rc), "/nologo", "/fo", res, f"/I{os.path.join(ROOT, 'res')}", f"/I{os.path.join(ROOT, 'src')}", rc_path])

    print("[4/4] compiling and linking TaskbarTuner.exe")
    host_objects = []
    for source in HOST_SOURCES:
        name = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(obj_root, f"host_{name}.obj")
        run([cl] + COMMON + optimize + defines + [f"/Fo{obj}", os.path.join(ROOT, source)], env=env)
        host_objects.append(obj)

    exe = os.path.join(bin_out, "TaskbarTuner.exe")
    run(
        [cl, "/nologo", f"/Fe{exe}"]
        + host_objects
        + [res, "/link", "/SUBSYSTEM:WINDOWS", "/NOLOGO"]
        + HOST_LIBS,
        env=env,
    )

    size = os.path.getsize(exe)
    print(f"\nbuilt {exe} ({size / 1024:.0f} KiB)")
    print(f"intermediate files in {build}")


if __name__ == "__main__":
    main()
