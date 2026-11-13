"""The builder's child processes (the translator, the compiler, the linker),
started through run() so that a cancel can end them, and the launcher's
cancel itself (make_game.py --launcher, docs/design.md section 22): the
launcher closes the builder's standard input, and the builder stops its
children and exits with CANCELLED. Never while the install folder is being
changed: that waits for hold."""
import contextlib
import os
import signal
import subprocess
import sys
import threading

CANCELLED = 3               # the exit code of a cancelled build

hold = threading.Lock()     # held while the install folder changes: a cancel waits for it
_lock = threading.Lock()
_live = set()
_stopping = False
_installed = False          # the install is done: nothing is left to cancel


class Cancelled(Exception):
    pass


def run(args, timeout=None):
    """subprocess.run(args, capture_output=True, text=True), cancellable"""
    kw = {}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NO_WINDOW      # no console window for each one
    else:
        kw["start_new_session"] = True                          # its own group: a cancel ends its children too
    with _lock:
        if _stopping:
            raise Cancelled()
        p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, encoding="utf-8", errors="replace", **kw)
        _live.add(p)
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        _kill(p)
        out, err = p.communicate()
        raise
    finally:
        with _lock:
            _live.discard(p)
    if _stopping:
        raise Cancelled()
    return subprocess.CompletedProcess(args, p.returncode, out, err)


def _kill(p):
    try:
        if os.name == "nt":
            # the tree: llvm-mingw's clang is a wrapper that starts clang.exe
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        else:
            os.killpg(p.pid, signal.SIGKILL)
    except OSError:
        pass
    try:
        p.kill()
    except OSError:
        pass


def stop_all():
    """ends every child; run() raises Cancelled from then on"""
    global _stopping
    with _lock:
        _stopping = True
        live = list(_live)
    for p in live:
        _kill(p)


@contextlib.contextmanager
def installing():
    """around the install: a cancel waits for its end, and after it the build
    is done (there is nothing left to cancel)"""
    global _installed
    with hold:
        yield
        _installed = True


def watch_stdin(log):
    """--launcher: standard input closing (the launcher's Cancel, or the
    launcher ending) cancels the build"""
    def wait():
        try:
            while os.read(0, 4096):
                pass
        except OSError:
            return              # no standard input to watch: nothing to cancel with
        with hold:              # not while the install folder changes
            if _installed:
                return
            log("cancelled")
            stop_all()
            sys.stdout.flush()
            os._exit(CANCELLED)
    threading.Thread(target=wait, daemon=True, name="cancel").start()
