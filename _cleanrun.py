"""_cleanrun.py <program> [args...] -- run a command with case-deduplicated environment.

Why this exists
---------------
MSBuild's ToolTask.GetProcessStartInfo copies the process environment into a
**case-sensitive** Hashtable. The WorkBuddy Bash tool's environment carries names
that differ only in case (HTTP_PROXY + http_proxy, HTTPS_PROXY + https_proxy),
so MSBuild throws System.ArgumentException *before* cl.exe is ever spawned, and
CMake only reports the downstream symptom:

    error MSB6001: "CL.exe" 的命令行开关无效
    No CMAKE_CXX_COMPILER could be found.

Fix: launch the build with a deduplicated environment. Dropping the duplicates
once at the top is enough, because this same block is what ProcessStartInfo
reads several processes down the chain.

Usage (note: pass *Windows* paths, not MSYS /c/... paths):
    PY="C:/Users/<user>/.workbuddy/binaries/python/versions/3.13.12/python.exe"
    "$PY" _cleanrun.py "C:/.../cmake.exe" -S "E:/proj" -B "E:/proj/build" -G "Visual Studio 17 2022" -A x64
"""
import ctypes
import subprocess
import sys


def raw_env_entries():
    """Read the process environment block verbatim (GetEnvironmentStringsW).

    os.environ / $env: collapse case on Windows and therefore *hide* the bug.
    """
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.GetEnvironmentStringsW.restype = ctypes.c_void_p
    k.FreeEnvironmentStringsW.argtypes = [ctypes.c_void_p]
    p = k.GetEnvironmentStringsW()
    addr = p
    out = []
    while True:
        s = ctypes.c_wchar_p(addr).value
        if not s:
            break
        out.append(s)
        addr += (len(s) + 1) * ctypes.sizeof(ctypes.c_wchar)
    k.FreeEnvironmentStringsW(ctypes.c_void_p(p))
    return out


def clean_env():
    env, seen = {}, {}
    for e in raw_env_entries():
        name, sep, value = e.partition("=")
        if not sep or not name:
            continue  # skips the "=F:=F:\\..." current-dir pseudo-vars
        if name.lower() in seen:
            sys.stderr.write("[cleanrun] drop %r (keeping %r)\n" % (name, seen[name.lower()]))
            continue
        seen[name.lower()] = name
        env[name] = value
    return env


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    # return the child's exit code, or the caller sees success no matter what
    sys.exit(subprocess.call(sys.argv[1:], env=clean_env()))
