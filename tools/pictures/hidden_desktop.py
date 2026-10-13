"""Run a program on a Windows desktop nobody is looking at, and wait for it.

A documentation picture is taken by the program drawing its own window with the real Windows
platform plugin: the offscreen plugin has no system font fallback, so a glyph the bundled face
lacks (the drop-down arrow of a combo box) comes out as a missing-glyph box. A real window on the
desktop a person is using appears in front of their work and can be clicked. So the program is
started on a second desktop of the same window station (CreateDesktopW, and STARTUPINFO.lpDesktop
for the process): it draws there as it would anywhere, and nothing shows on the screen.

Windows only, stdlib only (ctypes). take_pictures.py is the caller.
"""

from __future__ import annotations

import ctypes
import subprocess
import sys
from ctypes import wintypes
from pathlib import Path

DESKTOP_NAME = "iclforge-doc-pictures"
TIMED_OUT = -9

_GENERIC_ALL = 0x10000000
_CREATE_UNICODE_ENVIRONMENT = 0x00000400
_STARTF_USESTDHANDLES = 0x00000100
_HANDLE_FLAG_INHERIT = 0x00000001
_GENERIC_WRITE = 0x40000000
_FILE_SHARE_READ = 0x00000001
_FILE_SHARE_WRITE = 0x00000002
_CREATE_ALWAYS = 2
_FILE_ATTRIBUTE_NORMAL = 0x80
_WAIT_OBJECT_0 = 0
_INVALID_HANDLE = ctypes.c_void_p(-1).value


class _StartupInfo(ctypes.Structure):
    _fields_ = (
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.c_void_p),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    )


class _ProcessInformation(ctypes.Structure):
    _fields_ = (
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    )


class _SecurityAttributes(ctypes.Structure):
    _fields_ = (
        ("nLength", wintypes.DWORD),
        ("lpSecurityDescriptor", ctypes.c_void_p),
        ("bInheritHandle", wintypes.BOOL),
    )


def _libraries() -> tuple[ctypes.WinDLL, ctypes.WinDLL]:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.CreateDesktopW.restype = wintypes.HANDLE
    user32.CreateDesktopW.argtypes = (
        wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
        ctypes.c_void_p)
    user32.CloseDesktop.argtypes = (wintypes.HANDLE,)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = (
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(_SecurityAttributes),
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel32.CreateProcessW.argtypes = (
        wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL,
        wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(_StartupInfo),
        ctypes.POINTER(_ProcessInformation))
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    kernel32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    return user32, kernel32


def _environment_block(env: dict[str, str]) -> ctypes.Array:
    """CreateProcessW's environment: NAME=value strings, each ended by a null, then one more."""
    text = "".join(f"{name}={value}\0" for name, value in sorted(env.items())) + "\0"
    return ctypes.create_unicode_buffer(text, len(text))


class HiddenDesktop:
    """The desktop, kept open for as long as programs on it are wanted.

    Programs started on one desktop see each other's windows, which a picture of a program that
    lists the others (Crucible's application list) depends on, so the desktop outlives a single
    run. `with HiddenDesktop() as desktop:` closes it; the programs must have ended by then.
    """

    def __init__(self, name: str = DESKTOP_NAME) -> None:
        if sys.platform != "win32":
            raise OSError("a hidden desktop is a Windows desktop: this runs on Windows only")
        self._name = name
        self._user32, self._kernel32 = _libraries()
        self._handle = self._user32.CreateDesktopW(name, None, None, 0, _GENERIC_ALL, None)
        if not self._handle:
            raise ctypes.WinError(ctypes.get_last_error())

    def __enter__(self) -> HiddenDesktop:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        if self._handle:
            self._user32.CloseDesktop(self._handle)
            self._handle = None

    def start(self, command: list[str], env: dict[str, str], cwd: Path,
              log: Path | None = None) -> int:
        """Starts the program and returns its process handle; stop() or wait() ends the hold.

        What it writes to stdout and stderr goes to `log`: a windowed program has no console of
        its own, and what a failed capture said is the only account of why it failed.
        """
        kernel32 = self._kernel32
        startup = _StartupInfo()
        startup.cb = ctypes.sizeof(_StartupInfo)
        startup.lpDesktop = f"WinSta0\\{self._name}"
        log_handle = None
        if log is not None:
            inheritable = _SecurityAttributes(ctypes.sizeof(_SecurityAttributes), None, True)
            log_handle = kernel32.CreateFileW(
                str(log), _GENERIC_WRITE, _FILE_SHARE_READ | _FILE_SHARE_WRITE,
                ctypes.byref(inheritable), _CREATE_ALWAYS, _FILE_ATTRIBUTE_NORMAL, None)
            if log_handle in (None, _INVALID_HANDLE):
                raise ctypes.WinError(ctypes.get_last_error())
            startup.dwFlags = _STARTF_USESTDHANDLES
            startup.hStdOutput = log_handle
            startup.hStdError = log_handle
        process = _ProcessInformation()
        line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
        started = kernel32.CreateProcessW(
            None, line, None, None, log_handle is not None, _CREATE_UNICODE_ENVIRONMENT,
            _environment_block(env), str(cwd), ctypes.byref(startup), ctypes.byref(process))
        error = ctypes.get_last_error()
        if log_handle is not None:
            kernel32.CloseHandle(log_handle)
        if not started:
            raise ctypes.WinError(error)
        kernel32.CloseHandle(process.hThread)
        return process.hProcess

    def wait(self, process: int, timeout_s: float) -> int:
        """The program's exit code, or TIMED_OUT after ending it."""
        kernel32 = self._kernel32
        try:
            if kernel32.WaitForSingleObject(process, int(timeout_s * 1000)) != _WAIT_OBJECT_0:
                kernel32.TerminateProcess(process, 1)
                kernel32.WaitForSingleObject(process, 5000)
                return TIMED_OUT
            code = wintypes.DWORD()
            kernel32.GetExitCodeProcess(process, ctypes.byref(code))
            return ctypes.c_int32(code.value).value
        finally:
            kernel32.CloseHandle(process)

    def stop(self, process: int) -> None:
        """Ends a program that was started to be in the picture, not to take it."""
        kernel32 = self._kernel32
        kernel32.TerminateProcess(process, 0)
        kernel32.WaitForSingleObject(process, 5000)
        kernel32.CloseHandle(process)

    def run(self, command: list[str], env: dict[str, str], cwd: Path, timeout_s: float,
            log: Path | None = None) -> int:
        return self.wait(self.start(command, env, cwd, log), timeout_s)
