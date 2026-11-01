"""Exercise WHEP controls in a disposable OBS copy prepared by whep-host-native-e2e.py.

Checks readable feedback, stream-state priority, clipboard URLs, and persistence
across OBS restart. Captures only the owned process and restores text clipboard
contents. Never point this at your everyday OBS installation.
Run clipboard checks serially: the Windows clipboard is shared across OBS processes.
"""

import argparse
import json
import subprocess
import time
import hashlib
from pathlib import Path
import psutil
import win32clipboard
import win32gui
from pywinauto import Application
from pywinauto.controls.hwndwrapper import HwndWrapper

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--obs-review", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--expected-version", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
review = args.obs_review.resolve()
out = args.output.resolve()
assert review.is_relative_to(
    root / "artifacts"
), "Use a disposable review under this checkout/artifacts"
assert out.is_relative_to(
    root / "artifacts"
), "Write test evidence under this checkout/artifacts"
out.mkdir(parents=True, exist_ok=False)
assert not any(
    p.info["exe"] and Path(p.info["exe"]).is_relative_to(review)
    for p in psutil.process_iter(["exe", "name"])
    if (p.info["name"] or "").lower() == "obs64.exe"
)
service = review / "config/obs-studio/basic/profiles/Untitled/service.json"
settings = json.loads(service.read_text(encoding="utf-8-sig"))
settings["settings"]["whep_token"] = ""
service.write_text(json.dumps(settings), encoding="utf-8")
helper = root / "scripts/guest-return-room-ui.py"
logdir = review / "config/obs-studio/logs"
state = {}
obs = None
report = {"expectedVersion": args.expected_version}
original = None


def run(*args):
    if args[0] == "click-name":
        app = Application(backend="uia").connect(process=state["pid"])
        control = next(
            c
            for w in app.windows()
            for c in w.descendants(control_type=args[2])
            if c.window_text() == args[1]
        )
        assert control.is_enabled()
        handle = focus_control(control)
        # Synchronous key messages avoid racing a floating window's activation.
        win32gui.SendMessage(handle, 0x100, 0x20, 1 | (0x39 << 16))
        win32gui.SendMessage(handle, 0x101, 0x20, 1 | (0x39 << 16) | (3 << 30))
        time.sleep(2.1 if args[1] == "Copy WHEP Link" else 0.4)
        return ""
    r = subprocess.run(
        ["python", str(helper), str(out), *args],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert r.returncode == 0, r.stderr
    return r.stdout


def launch():
    global obs, state
    obs = subprocess.Popen(
        [
            str(review / "bin/64bit/obs64.exe"),
            "--portable",
            "--multi",
            "--profile",
            "Untitled",
            "--disable-updater",
            "--disable-shutdown-check",
        ],
        cwd=review / "bin/64bit",
        creationflags=subprocess.CREATE_NO_WINDOW,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    state = {"pid": obs.pid, "review": str(review)}
    (out / "state.json").write_text(json.dumps(state))
    end = time.monotonic() + 45
    while time.monotonic() < end:
        assert obs.poll() is None
        try:
            app = Application(backend="uia").connect(process=obs.pid)
            if any(w.window_text().startswith("OBS ") for w in app.windows()):
                break
        except Exception:
            pass
        time.sleep(0.25)
    else:
        raise AssertionError("OBS window missing")
    time.sleep(2)
    run("open")
    assert "v" + args.expected_version in run("dump")
    modules = [
        m.path
        for m in psutil.Process(obs.pid).memory_maps()
        if Path(m.path).name.lower() == "obs-vdoninja.dll"
    ]
    assert (
        len(modules) == 1
        and Path(modules[0]).resolve() == review / "obs-plugins/64bit/obs-vdoninja.dll"
    )
    report["loadedModule"] = modules[0]
    report["pluginSha256"] = hashlib.sha256(Path(modules[0]).read_bytes()).hexdigest()


def clipboard():
    win32clipboard.OpenClipboard()
    try:
        return (
            win32clipboard.GetClipboardData(win32clipboard.CF_UNICODETEXT)
            if win32clipboard.IsClipboardFormatAvailable(win32clipboard.CF_UNICODETEXT)
            else None
        )
    finally:
        win32clipboard.CloseClipboard()


def waitlog(marker):
    end = time.monotonic() + 30
    while time.monotonic() < end:
        latest = max(logdir.glob("*.txt"), key=lambda p: p.stat().st_mtime)
        if marker in latest.read_text(encoding="utf-8", errors="replace"):
            return
        time.sleep(0.25)
    raise AssertionError(marker)


def capture(name):
    run("capture", name, "VDONinjaStudioDock")


def dock_control(identifier):
    app = Application(backend="uia").connect(process=state["pid"])
    return next(
        c
        for w in app.windows()
        for c in w.descendants()
        if c.element_info.automation_id.split(".")[-1] == identifier
    )


def keypress(identifier, keys):
    handle = focus_control(dock_control(identifier))
    HwndWrapper(handle).send_keystrokes(keys)
    time.sleep(0.3)


def focus_control(control):
    handle = dock_control("VDONinjaStudioDock").handle
    win32gui.SendMessage(handle, 0x06, 1, 0)  # WM_ACTIVATE / WA_ACTIVE
    control.set_focus()
    time.sleep(0.2)
    assert control.has_keyboard_focus(), "Keyboard focus did not reach the target"
    return handle


try:
    original = clipboard()
    launch()
    capture("00-stopped")
    run("click-name", "Go Live", "Button")
    waitlog("VDO.Ninja output started successfully")
    time.sleep(2)
    run("click-name", "Copy WHEP Link", "Button")
    assert "Stop streaming to enable WHEP." in run("dump")
    assert "Text 'LIVE'" in run("dump")
    assert not json.loads(service.read_text(encoding="utf-8-sig"))["settings"].get(
        "whep_token"
    )
    capture("01-stop-first")
    report["stopFirstMessage"] = True
    time.sleep(7.2)
    assert "Text 'LIVE'" in run("dump")
    report["liveStatusAlwaysVisible"] = True
    run("click-name", "Stop", "Button")
    time.sleep(2)
    run("click-name", "Copy WHEP Link", "Button")
    first = clipboard()
    token = json.loads(service.read_text(encoding="utf-8-sig"))["settings"][
        "whep_token"
    ]
    assert token and first == "https://whep.vdo.ninja/" + token
    capture("02-link-ready")
    report["generatedAndSaved"] = True
    run("click-name", "Go Live", "Button")
    waitlog("WHEP link hosting connected to wss://whep.vdo.ninja")
    time.sleep(3)
    capture("03-live")
    run("click-name", "Copy WHEP Link", "Button")
    assert clipboard() == first
    assert "Text 'LIVE'" in run("dump")
    report["liveCopyUsesSameLink"] = True
    capture("04-copy-live")
    run("click-name", "Stop", "Button")
    time.sleep(2)
    capture("05-stopped")
    run("close")
    assert obs.wait(timeout=30) == 0
    launch()
    run("click-name", "Copy WHEP Link", "Button")
    assert clipboard() == first
    report["sameLinkAfterObsRestart"] = True
    capture("06-after-reopen")
    keypress("PublishAdvanced", "{SPACE}")
    assert dock_control("PublishWhepKey").element_info.element.CurrentIsPassword
    keypress("PublishWhepKey", "{HOME}" + "{DELETE}" * 80 + "{TAB}")
    assert not json.loads(service.read_text(encoding="utf-8-sig"))["settings"].get(
        "whep_token"
    )
    capture("07-disabled")
    assert "Link copied. Go live to watch." not in run("dump")
    run("click-name", "Go Live", "Button")
    time.sleep(3)
    assert not dock_control("PublishWhepKey").is_enabled()
    run("click-name", "Copy WHEP Link", "Button")
    assert "Stop streaming to enable WHEP." in run("dump")
    assert "Text 'LIVE'" in run("dump")
    capture("08-disabled-while-live")
    report["disablePersistsAndAppliesOnStart"] = True
    run("click-name", "Stop", "Button")
    time.sleep(2)
    keypress("PublishAdvanced", "{SPACE}")
    run("click-name", "Copy WHEP Link", "Button")
    replacement = clipboard()
    assert (
        replacement
        and replacement != first
        and replacement.startswith("https://whep.vdo.ninja/")
    )
    capture("09-replacement-link")
    run("close")
    assert obs.wait(timeout=30) == 0
    launch()
    run("click-name", "Copy WHEP Link", "Button")
    assert clipboard() == replacement
    report["replacementLinkPersists"] = True
    capture("10-replacement-after-reopen")
    run("close")
    assert obs.wait(timeout=30) == 0
    report["ok"] = True
except Exception as error:
    report["error"] = repr(error)
    try:
        capture("failure")
    except Exception:
        pass
    raise
finally:
    if obs and obs.poll() is None:
        try:
            try:
                run("click-name", "Stop", "Button")
                time.sleep(2)
            except Exception:
                pass  # Already stopped, or startup failed before the dock appeared.
            run("close")
            obs.wait(timeout=20)
        except Exception:
            obs.terminate()
    if original is not None:
        win32clipboard.OpenClipboard()
        try:
            win32clipboard.EmptyClipboard()
            win32clipboard.SetClipboardText(original, win32clipboard.CF_UNICODETEXT)
        finally:
            win32clipboard.CloseClipboard()
    (out / "results.json").write_text(json.dumps(report, indent=2))
print(json.dumps(report))
