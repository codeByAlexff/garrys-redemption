"""Runs the GMod half of the bridge in the real game, against fake_rdr2.py.

    set GR_GMOD_DIR=<folder that holds gmod_win64.exe>
    python tools/gmod_live_test.py

Starts GMod in a window on gm_flatgrass, with nobody at the keyboard, and checks:

  1. the module loads and waits for RDR2
  2. it connects to the fake RDR2 and both frame counters move
  3. keep-active: a WM_ACTIVATE(WA_INACTIVE) does not drop the frame rate while
     connected, does with gr_keep_active 0, and gr_keep_active 1 brings it back.
     Then the same with a deactivation that comes from Windows: the window is minimised
  4. the fake going away is noticed, and a new one is connected to again

RDR2 is not needed. Install the module and addon first (tools/package.ps1). The game is
killed at the end: it does not close on WM_CLOSE within any useful time.
"""

import ctypes
import os
import re
import subprocess
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))

WM_ACTIVATE = 0x0006
WM_COPYDATA = 0x004A
WA_INACTIVE = 0
SW_MINIMIZE = 6
SMTO_ABORTIFHUNG = 0x0002

user32 = ctypes.WinDLL("user32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, ctypes.c_void_p,
                                       wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
user32.GetForegroundWindow.restype = wintypes.HWND
user32.ShowWindowAsync.argtypes = [wintypes.HWND, ctypes.c_int]


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [("dwData", ctypes.c_size_t), ("cbData", wintypes.DWORD), ("lpData", ctypes.c_char_p)]


class Failed(Exception):
    pass


def find_game_window(pid):
    found = []

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        if owner.value == pid and name.value == "Valve001":
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(WNDENUMPROC(visit), 0)
    return found[0] if found else None


def send(hwnd, message, wparam, lparam):
    result = ctypes.c_size_t()
    if not user32.SendMessageTimeoutW(hwnd, message, wparam, lparam, SMTO_ABORTIFHUNG, 10000, ctypes.byref(result)):
        raise Failed("the game window did not answer message 0x%04X" % message)


def console(hwnd, command):
    """The engine runs a WM_COPYDATA string with dwData 0 as a console command (Hammer uses this)."""
    data = command.encode("ascii") + b"\0"
    cds = COPYDATASTRUCT(0, len(data), data)
    send(hwnd, WM_COPYDATA, 0, ctypes.addressof(cds))


class Tail:
    """Reads what has been appended to a file since this object was made."""

    def __init__(self, path):
        self.path = path
        self.at = os.path.getsize(path) if os.path.exists(path) else 0
        self.seen = ""
        self.mark = 0

    def read(self):
        if not os.path.exists(self.path):
            return ""
        with open(self.path, "rb") as f:
            f.seek(self.at)
            data = f.read()
        self.at += len(data)
        text = data.decode("utf-8", "replace")
        self.seen += text
        return text

    def wait_for(self, needle, seconds, what):
        """Waits for the next needle after the last one matched. Returns the text up to it."""
        deadline = time.monotonic() + seconds
        while True:
            self.read()
            at = self.seen.find(needle, self.mark)
            if at >= 0:
                text = self.seen[self.mark:at + len(needle)]
                self.mark = at + len(needle)
                return text
            if time.monotonic() >= deadline:
                raise Failed("%s: no %r within %d s" % (what, needle, seconds))
            time.sleep(0.25)


def frame_rate(probe, seconds=4):
    """PreRender calls per second from gr_probe's lines, skipping the line in progress."""
    probe.read()
    time.sleep(seconds)
    rates = [int(calls) / float(covered)
             for covered, calls in re.findall(r"(\d+\.\d+)s \|.*?PreRender (\d+)/", probe.read())]
    if len(rates) < 2:
        raise Failed("gr_probe wrote no lines: is the probe on?")
    return sum(rates[1:]) / len(rates[1:])


def cursor_grab(tries=3):
    """Where the game puts the cursor back to, or None if it leaves the cursor alone.

    Nudges the cursor and looks whether it stays where it was put. Counts as grabbed only
    if every try snaps to the same point, so a hand on the mouse is not mistaken for it.
    """
    snapped = set()
    for _ in range(tries):
        start = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(start))
        target = (start.x + 37, start.y + 23)
        user32.SetCursorPos(*target)
        time.sleep(0.2)
        now = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(now))
        if (now.x, now.y) == target:
            user32.SetCursorPos(start.x, start.y)
            return None
        snapped.add((now.x, now.y))
    return snapped.pop() if len(snapped) == 1 else None


def start_fake():
    return subprocess.Popen([sys.executable, os.path.join(HERE, "fake_rdr2.py"), "--quiet"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    gmod_dir = os.environ.get("GR_GMOD_DIR")
    exe = os.path.join(gmod_dir or "", "gmod_win64.exe")
    if not gmod_dir or not os.path.exists(exe):
        print("Set GR_GMOD_DIR to the folder that holds gmod_win64.exe.")
        return 2
    garrysmod = os.path.join(gmod_dir, "garrysmod")
    log = Tail(os.path.join(garrysmod, "console.log"))
    probe = Tail(os.path.join(garrysmod, "data", "gr_probe.txt"))

    results = []

    def check(ok, text):
        results.append(ok)
        print("%s  %s" % ("ok  " if ok else "FAIL", text))

    game = subprocess.Popen([exe, "-windowed", "-w", "1280", "-h", "720", "-novid", "-condebug",
                             "+maxplayers", "1", "+map", "gm_flatgrass"], cwd=gmod_dir)
    fake = None
    try:
        # 1. Load. The first start after a game update takes minutes.
        log.wait_for("bridge: module loaded.", 300, "module load")
        log.wait_for("waiting for RDR2", 15, "first state")
        check(True, "module loaded, waiting for RDR2")
        hwnd = find_game_window(game.pid)
        if not hwnd:
            raise Failed("no Valve001 window in process %d" % game.pid)

        # 2. Connect.
        fake = start_fake()
        log.wait_for("bridge: connected to RDR2", 15, "connect")
        check(True, "connected to the fake RDR2")
        console(hwnd, "gr_status")
        first = log.wait_for("deactivations swallowed", 10, "gr_status")
        time.sleep(1)
        console(hwnd, "gr_status")
        second = log.wait_for("deactivations swallowed", 10, "gr_status")
        ours = [int(re.search(r"frames: ours (\d+)", t).group(1)) for t in (first, second)]
        theirs = [int(re.search(r"RDR2's (\d+)", t).group(1)) for t in (first, second)]
        torn = int(re.search(r"torn reads (\d+)", second).group(1))
        check(ours[1] > ours[0] and theirs[1] > theirs[0],
              "frame counters move: ours %d -> %d, RDR2's %d -> %d, %d torn reads"
              % (ours[0], ours[1], theirs[0], theirs[1], torn))
        check("keep-active on" in second, "gr_status reports keep-active on while connected")

        # 3. Keep-active.
        console(hwnd, "gr_probe 1")
        time.sleep(1)
        in_front = user32.GetForegroundWindow() == hwnd
        base = frame_rate(probe)
        print("      GMod is %sthe foreground window, %.0f frames/s" % ("" if in_front else "not ", base))
        send(hwnd, WM_ACTIVATE, WA_INACTIVE, 0)
        kept = frame_rate(probe)
        check(kept > 60, "deactivated while keeping: %.0f frames/s (throttled would be 20)" % kept)
        console(hwnd, "gr_status")
        status = log.wait_for("deactivations swallowed", 10, "gr_status")
        swallowed = int(re.search(r"(\d+) deactivations swallowed", status).group(1))
        check(swallowed >= 1 and "believes it is active" in status,
              "gr_status: %d deactivations swallowed, engine believes it is active" % swallowed)
        # Not a pass or fail: an engine that is kept active may go on recentring the
        # cursor, which decides whether the module has to shut its mouse off as well.
        in_front = user32.GetForegroundWindow() == hwnd
        grab = cursor_grab()
        print("      kept active, %sthe foreground window: %s" % (
            "" if in_front else "not ", "cursor pulled to %s" % (grab,) if grab else "cursor left alone"))

        console(hwnd, "gr_keep_active 0")
        time.sleep(0.5)
        send(hwnd, WM_ACTIVATE, WA_INACTIVE, 0)
        throttled = frame_rate(probe)
        check(throttled < 30, "deactivated with gr_keep_active 0: %.0f frames/s" % throttled)
        console(hwnd, "gr_keep_active 1")
        woken = frame_rate(probe)
        check(woken > 60, "gr_keep_active 1 wakes the engine: %.0f frames/s" % woken)

        # The same from Windows itself: minimising the window in front hands the
        # foreground to another one. A background process cannot do that by calling
        # SetForegroundWindow on something else.
        user32.ShowWindowAsync(hwnd, SW_MINIMIZE)
        time.sleep(1)
        minimised = frame_rate(probe)
        check(user32.GetForegroundWindow() != hwnd and minimised > 60,
              "minimised, another window in front: %.0f frames/s" % minimised)
        console(hwnd, "gr_status")
        status = log.wait_for("deactivations swallowed", 10, "gr_status")
        print("      " + status.strip().splitlines()[-1].strip())
        grab = cursor_grab()
        print("      kept active, minimised: %s" % ("cursor pulled to %s" % (grab,) if grab else "cursor left alone"))

        # 4. Lose the peer, find a new one.
        fake.kill()
        fake.wait()
        log.wait_for("bridge: waiting for RDR2", 15, "peer loss")
        check(True, "fake RDR2 killed: back to waiting")
        fake = start_fake()
        log.wait_for("bridge: connected to RDR2", 15, "reconnect")
        check(True, "connected to a second fake RDR2")
        console(hwnd, "gr_probe 0")
        time.sleep(0.5)
    except Failed as failure:
        check(False, str(failure))
    finally:
        if fake:
            fake.kill()
        game.kill()
        game.wait()

    errors = [line for line in log.seen.splitlines() if "[ERROR]" in line or "Lua Error" in line]
    check(not errors, "no Lua errors in the console" if not errors else "Lua errors: %s" % errors[:3])
    module_log = os.path.join(garrysmod, "lua", "bin", "GarrysRedemption_gmod.log")
    if os.path.exists(module_log):
        print("\n---- %s" % module_log)
        with open(module_log, encoding="utf-8", errors="replace") as f:
            print(f.read().rstrip())
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
