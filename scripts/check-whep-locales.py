"""Check the WHEP/UI audit strings in every shipped locale (UTF-8, keys, placeholders)."""

from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
required = {
    "VDONinja.Dock.CopyWhepLink",
    "VDONinja.Dock.WhepStopFirst",
    "VDONinja.Dock.WhepCopied",
    "VDONinja.Dock.WhepHelp",
    "VDONinja.Dock.Live",
    "VDONinja.Dock.LinkCopied",
    "WhepToken",
    "GuestReturn.Intro",
    "ServiceSetupHint",
    "UDPPorts.Summary",
}
catalogs = {}
for path in sorted((root / "data/locale").glob("*.ini")):
    values = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        match = re.fullmatch(r'([^=]+)="(.*)"', line)
        assert match, f"{path.name}: malformed locale entry: {line}"
        key, value = match.groups()
        assert key not in values, f"{path.name}: duplicate {key}"
        values[key] = value
    assert required <= values.keys(), f"{path.name}: missing {required - values.keys()}"
    for key in required:
        assert values[key].strip() and values[key] != key
        assert "\ufffd" not in values[key], f"{path.name}: broken Unicode in {key}"
        assert "?" not in values[key], f"{path.name}: possible lossy encoding in {key}"
    catalogs[path.stem] = values

for locale, values in catalogs.items():
    for key in required:
        assert re.findall(r"%[1-9]|%[sd]", values[key]) == re.findall(
            r"%[1-9]|%[sd]", catalogs["en-US"][key]
        ), f"{locale}: changed placeholders in {key}"
print(f"PASS: {len(required)} reviewed strings in all {len(catalogs)} locale files")
