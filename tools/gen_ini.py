"""Generates package/OpenFixes.ini from the FixInfo literals in src/Fixes.

Every fix gets one line, "<FixId> = <default> ; <description> - <origin>", in its section; fixes have no other
settings. Run from anywhere: python tools/gen_ini.py [--check]
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SECTIONS = ["Crashes", "Limits", "Performance", "SaveLoad", "Papyrus", "AI", "Combat", "Magic", "Gameplay", "Movement",
            "Camera", "Graphics", "Audio", "Input", "Interface"]
HEADER = """\
; OpenFixes settings
; Each line turns one fix on (true) or off (false). A fix that is off installs nothing, as if it wasn't there.
; The comment on each line names the bug it fixes, then the mod the fix comes from and its author(s).
; Fixes marked with game versions ([SE], [AE], [1.7], [VR]) only exist for those versions and do nothing on others.
; A fix whose original mod (or another mod with the same fix) is installed is skipped automatically; the log says so.

[General]
"""
GENERAL = [("DebugLogging", "false", "Log every patched address to OpenFixes.log")]
RUNTIME_LABELS = {"SE": "SE", "AE": "AE", "AE17": "1.7", "VR": "VR"}
# Runtime groups in Fix.h; FLAT is FixInfo's default
RUNTIME_GROUPS = {"FLAT": ["SE", "AE", "AE17"], "ALL": ["SE", "AE", "AE17", "VR"]}


def literal(block, field):
    """Concatenated string literal(s) assigned to .field in block, or None"""
    m = re.search(r"\." + field + r'\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', block)
    if not m:
        return None
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))).replace('\\"', '"')


def fix_blocks(text):
    """Yields the text of each Fix({ ... }) initializer"""
    for m in re.finditer(r"\bFix\s*\(\s*\{", text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        yield text[m.end():i - 1]


def collect():
    fixes = []
    for path in sorted((ROOT / "src" / "Fixes").rglob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        for block in fix_blocks(text):
            info = {k: literal(block, k) for k in ("id", "section", "description", "origin")}
            missing = [k for k, v in info.items() if not v]
            if missing:
                sys.exit(f"{path}: FixInfo is missing literal field(s): {', '.join(missing)}")
            info["details"] = literal(block, "details") or ""
            if info["section"] not in SECTIONS:
                sys.exit(f"{path}: unknown section {info['section']!r}; use one of {SECTIONS}")
            info["default"] = "false" if re.search(r"\.enabledByDefault\s*=\s*false", block) else "true"
            runtimes = re.search(r"\.runtimes\s*=\s*([^,]+),", block)
            names = re.findall(r"Runtime::(\w+)", runtimes.group(1)) if runtimes else ["FLAT"]
            covered = {r for n in names for r in RUNTIME_GROUPS.get(n, [n])}
            # Fixes on every runtime carry no tag; the others list the versions they exist for
            info["runtimes"] = [] if covered >= set(RUNTIME_LABELS) else [
                label for key, label in RUNTIME_LABELS.items() if key in covered]
            info["path"] = path.relative_to(ROOT).as_posix()
            fixes.append(info)
    ids = [f["id"] for f in fixes]
    dupes = {i for i in ids if ids.count(i) > 1}
    if dupes:
        sys.exit(f"duplicate fix ids: {sorted(dupes)}")
    return fixes


def lines_for(entries):
    """Aligns 'key = value ; comment' lines"""
    width = max((len(f"{k} = {v}") for k, v, _ in entries), default=0)
    return [f"{k} = {v}".ljust(width) + f" ; {c}" for k, v, c in entries]


def render(fixes):
    out = [HEADER + "\n".join(lines_for(GENERAL)) + "\n"]
    for section in SECTIONS:
        members = sorted((f for f in fixes if f["section"] == section), key=lambda f: f["id"].lower())
        if not members:
            continue
        entries = []
        for f in members:
            tag = f" [{', '.join(f['runtimes'])}]" if f["runtimes"] else ""
            entries.append((f["id"], f["default"], f"{f['description']}{tag} - {f['origin']}"))
        out.append(f"\n[{section}]\n" + "\n".join(lines_for(entries)) + "\n")
    return "".join(out)


def main():
    text = render(collect())
    target = ROOT / "package" / "OpenFixes.ini"
    if "--check" in sys.argv:
        if not target.exists() or target.read_text(encoding="utf-8") != text:
            sys.exit("package/OpenFixes.ini is out of date; run tools/gen_ini.py")
        return
    target.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {target}")


if __name__ == "__main__":
    main()
