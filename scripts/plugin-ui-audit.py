"""Capture a disposable OBS dock and exercise real keyboard navigation.

Requires a stopped portable copy under artifacts, prepared by whep-host-native-e2e.py.
No mouse/UIA Invoke is used to activate plugin controls. UIA supplies names and focus
observations; Tab, Shift+Tab, Space and arrow keys drive the controls.
"""

import argparse
import configparser
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import time

import psutil
import win32clipboard
from pywinauto import Application
from pywinauto.controls.hwndwrapper import HwndWrapper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-review", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--locale", default="en-US")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    review, out = args.obs_review.resolve(), args.output.resolve()
    assert review.is_relative_to(root / "artifacts")
    assert out.is_relative_to(root / "artifacts")
    assert not any(
        p.info["exe"] and Path(p.info["exe"]).is_relative_to(review)
        for p in psutil.process_iter(["exe", "name"])
        if (p.info["name"] or "").lower() == "obs64.exe"
    )
    out.mkdir(parents=True, exist_ok=False)
    translations = {}
    for line in (
        (root / "data/locale" / (args.locale + ".ini"))
        .read_text(encoding="utf-8-sig")
        .splitlines()
    ):
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            assert key not in translations, f"Duplicate translation: {key}"
            translations[key] = value.strip('"')
    for name in ["global.ini", "user.ini"]:
        path = review / "config/obs-studio" / name
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.optionxform = str
        config.read(path, encoding="utf-8-sig")
        config["General"]["Language"] = args.locale
        with path.open("w", encoding="utf-8") as file:
            config.write(file, space_around_delimiters=False)
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
    (out / "state.json").write_text(json.dumps({"pid": obs.pid, "review": str(review)}))
    helper = root / "scripts/guest-return-room-ui.py"
    report = {"locale": args.locale, "focusOrder": []}
    original = None

    def helper_run(*items):
        result = subprocess.run(
            ["python", str(helper), str(out), *items],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert result.returncode == 0, result.stderr
        return result.stdout

    def controls():
        return [
            c
            for w in app.windows()
            for c in w.descendants()
            if "VDONinjaStudioDock" in c.element_info.automation_id and c.is_visible()
        ]

    def find(name):
        return next(
            c for c in controls() if c.element_info.automation_id.split(".")[-1] == name
        )

    def focused():
        return next((c for c in controls() if c.has_keyboard_focus()), None)

    def send_keys(keys):
        # Deliver real Windows keyboard messages to our dock, without sending
        # input into the user's foreground application or locked desktop.
        handle = find("VDONinjaStudioDock").handle
        if keys != "+{TAB}":
            HwndWrapper(handle).send_keystrokes(keys)
            time.sleep(0.15)
            return
        # Keep Shift down until the target has handled Tab; the generic sender
        # can release its modifier state before a busy Qt event loop reads it.
        user32 = ctypes.windll.user32
        target = user32.GetWindowThreadProcessId(handle, None)
        current = ctypes.windll.kernel32.GetCurrentThreadId()
        assert user32.AttachThreadInput(current, target, True)
        saved = (ctypes.c_ubyte * 256)()
        user32.GetKeyboardState(saved)
        held = (ctypes.c_ubyte * 256).from_buffer_copy(saved)
        held[0x10] |= 0x80
        held[0xA0] |= 0x80
        try:
            user32.SetKeyboardState(held)
            user32.SendMessageW(handle, 0x100, 0x10, 1 | (42 << 16))
            user32.SendMessageW(handle, 0x100, 9, 1 | (15 << 16))
            user32.SendMessageW(handle, 0x101, 9, 1 | (15 << 16) | (3 << 30))
            user32.SendMessageW(handle, 0x101, 0x10, 1 | (42 << 16) | (3 << 30))
        finally:
            user32.SetKeyboardState(saved)
            user32.AttachThreadInput(current, target, False)
        time.sleep(0.15)

    def capture(name):
        helper_run("capture", name, "VDONinjaStudioDock")

    def clipboard():
        win32clipboard.OpenClipboard()
        try:
            return (
                win32clipboard.GetClipboardData(win32clipboard.CF_UNICODETEXT)
                if win32clipboard.IsClipboardFormatAvailable(
                    win32clipboard.CF_UNICODETEXT
                )
                else None
            )
        finally:
            win32clipboard.CloseClipboard()

    try:
        original = clipboard()
        time.sleep(5)
        assert obs.poll() is None
        app = Application(backend="uia").connect(process=obs.pid)
        helper_run("open")
        helper_run("resize", "360", "960", "VDONinjaStudioDock")
        # The dock may reopen on the return tab. Select Publish using the tab bar's keys.
        tab = next(c for c in controls() if c.element_info.control_type == "Tab")
        tab.set_focus()
        send_keys("{HOME}")
        time.sleep(0.3)
        modules = [
            m.path
            for m in psutil.Process(obs.pid).memory_maps()
            if Path(m.path).name.lower() == "obs-vdoninja.dll"
        ]
        assert (
            len(modules) == 1
            and Path(modules[0]).resolve()
            == review / "obs-plugins/64bit/obs-vdoninja.dll"
        )
        report["module"] = modules[0]
        report["sha256"] = hashlib.sha256(Path(modules[0]).read_bytes()).hexdigest()
        assert (
            find("PublishCopyWhep").window_text()
            == translations["VDONinja.Dock.CopyWhepLink"]
        )
        assert find("PublishStreamID").element_info.name == translations["StreamID"]
        assert find("PublishPassword").element_info.name == translations["Password"]
        capture("01-publish")
        find("PublishStreamID").set_focus()
        reached = set()
        for _ in range(24):
            control = focused()
            if control:
                identifier = control.element_info.automation_id.split(".")[-1]
                report["focusOrder"].append(
                    {"id": identifier, "name": control.window_text()}
                )
                reached.add(identifier)
                if identifier == "PublishAdvanced":
                    send_keys("{SPACE}")
                    time.sleep(0.2)
                    assert find("VDONinjaUdpPorts").is_visible()
                    capture("02-advanced-keyboard")
                    send_keys("{TAB}")
                    assert focused().element_info.automation_id.endswith(
                        "PublishWhepKey"
                    )
                    assert (
                        find("PublishWhepKey").element_info.name
                        == translations["WhepToken"]
                    )
                    assert find("PublishWhepKey").element_info.element.CurrentIsPassword
                    send_keys("{TAB}")
                    assert focused().element_info.automation_id.endswith(
                        "VDONinjaUdpPorts"
                    )
                    send_keys("+{TAB}")
                    assert focused().element_info.automation_id.endswith(
                        "PublishWhepKey"
                    )
                    send_keys("+{TAB}")
                    assert focused().element_info.automation_id.endswith(
                        "PublishAdvanced"
                    ), focused().element_info.automation_id
                    send_keys("{SPACE}")
                if identifier == "PublishCopyWhep":
                    capture("03-whep-focus")
                    send_keys("{SPACE}")
                    time.sleep(2.1)
                    assert (clipboard() or "").startswith("https://whep.vdo.ninja/")
                    assert (
                        find("PublishLinkFeedback").window_text()
                        == translations["VDONinja.Dock.WhepCopied"]
                    )
                    capture("04-copied-keyboard")
                    break
            send_keys("{TAB}")
            time.sleep(0.1)
        assert {
            "PublishStreamID",
            "PublishRoomID",
            "PublishPassword",
            "PublishAdvanced",
            "PublishStart",
            "PublishCopyWhep",
        } <= reached, reached
        send_keys("+{TAB}")
        assert (
            focused()
            and focused().element_info.automation_id.split(".")[-1] != "PublishCopyWhep"
        )
        helper_run("resize", "280", "900", "VDONinjaStudioDock")
        capture("05-narrow")
        tab = next(c for c in controls() if c.element_info.control_type == "Tab")
        tab.set_focus()
        send_keys("{RIGHT}")
        time.sleep(0.3)
        assert find("GuestReturnAudioTrack").is_visible()
        assert find("GuestReturnBitrate").rectangle().width() >= 140
        capture("06-guest-return")
        find("GuestReturnStreamID").set_focus()
        guest_reached = set()
        for _ in range(22):
            control = focused()
            if control:
                guest_reached.add(control.element_info.automation_id.split(".")[-1])
                if "GuestReturnCopyLink" in guest_reached:
                    break
            send_keys("{TAB}")
            time.sleep(0.1)
        assert {
            "GuestReturnPassword",
            "GuestReturnRoomID",
            "GuestReturnAudioTrack",
            "GuestReturnEncoder",
            "GuestReturnBitrate",
            "GuestReturnSize",
            "GuestReturnFrameRate",
            "GuestReturnMaxViewers",
            "GuestReturnAdvanced",
            "GuestReturnStart",
            "GuestReturnCopyLink",
        } <= guest_reached, guest_reached
        report["guestReturnKeyboardFields"] = sorted(guest_reached)
        find("GuestReturnAudioTrack").set_focus()
        # Restore the value after testing arrow-key editing.
        previous = find("GuestReturnAudioTrack").iface_value.CurrentValue
        send_keys("{UP}{DOWN}")
        assert find("GuestReturnAudioTrack").iface_value.CurrentValue == previous
        find("GuestReturnAdvanced").set_focus()
        send_keys("{SPACE}")
        time.sleep(0.2)
        assert find("GuestReturnSignaling").is_visible()
        capture("07-return-advanced")
        send_keys("{SPACE}")
        report["keyboardPassed"] = True
        report["ok"] = True
    except Exception as error:
        report["error"] = repr(error)
        try:
            capture("failure")
        except Exception:
            pass
        raise
    finally:
        if obs.poll() is None:
            try:
                helper_run("close")
                obs.wait(timeout=20)
            except Exception:
                obs.terminate()
                obs.wait(timeout=10)
        if original is not None:
            win32clipboard.OpenClipboard()
            try:
                win32clipboard.EmptyClipboard()
                win32clipboard.SetClipboardText(original, win32clipboard.CF_UNICODETEXT)
            finally:
                win32clipboard.CloseClipboard()
        (out / "results.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    print(json.dumps(report, ensure_ascii=True))


if __name__ == "__main__":
    main()
