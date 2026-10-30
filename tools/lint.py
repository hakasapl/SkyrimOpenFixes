"""Static checks over src/Fixes: the framework and CONTRIBUTING.md rules every fix must follow.

python tools/lint.py [path-filter]

Prints each fix's problems and exits with status 1 if any fix has one, so CI fails on it.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FILTER = sys.argv[1] if len(sys.argv) > 1 else ""
# The original mod's license files, kept in the fix's folder next to its source unless the code is GPL-3.0, which the
# repository's own LICENSE already covers
LICENSE_FILE = re.compile(r"^(LICENSE|LICENCE|COPYING)([-._].*)?$", re.IGNORECASE)
PROJECT_LICENSES = {"GPL-3.0", "GPL-3.0-only", "GPL-3.0-or-later"}

FORBIDDEN = [
    (r"\bRegisterListener\s*\(", "registers its own SKSE listener (use Fix::onMessage)"),
    (r"\bSetUniqueID\s*\(|\bSetSaveCallback\s*\(|\bSetLoadCallback\s*\(|\bSetRevertCallback\s*\(", "sets up serialization itself"),
    (r"\bAllocTrampoline\s*\(|\bGetTrampoline\s*\(", "uses CommonLib's trampoline (use Hooks::call/branch/branchToCode/allocateCode)"),
    (r"\.trampolineBytes\b", "declares trampolineBytes (the framework sizes the trampoline itself)"),
    (r"\breport_and_fail\s*\(", "report_and_fail"),
    (r"\bwrite_vfunc\s*[<(]", "writes vfunc directly (use Hooks::vfunc)"),
    (r"\bwrite_(call|branch|thunk_call|thunk_jump)\s*[<(]", "writes a call or jump directly (use Hooks::call/branch)"),
    (r"\bsafe_(write|fill)\s*\(", "writes memory directly (use Hooks::patch/nop)"),
    (r"\bVirtualProtect(Ex)?\s*\(|\bWriteProcessMemory\s*\(|\bFlushInstructionCache\s*\(",
     "changes code memory itself (use Hooks::patch/nop)"),
    (r"\b(?:std::)?mem(cpy|move|set)\s*\(\s*reinterpret_cast\s*<\s*(?:void|std::u?int8_t|std::byte|u?int8_t|char|unsigned char)\s*\*\s*>",
     "writes to a raw address (use Hooks::patch/nop for code; write game data through a typed pointer)"),
    (r"\.getCode\s*[<(]", "runs Xbyak code from its own buffer (use Hooks::allocateCode/branchToCode)"),
    (r"safetyhook::create_(inline|mid)\s*\(", "creates safetyhook directly (use Hooks::inlineHook/midHook)"),
    (r"\bMH_(Initialize|CreateHook|EnableHook|QueueEnableHook)\b|\bDetour(Attach|TransactionBegin)\b",
     "uses MinHook or Detours (use Hooks::inlineHook/midHook)"),
    (r"\bSKSE::Init\s*\(|SKSEPluginLoad|SKSEPluginInfo", "plugin entry point in a fix"),
    (r"\bGetPapyrusInterface\s*\(\)\s*->\s*Register", "registers Papyrus functions"),
    (r"\bstd::thread\b|\bCreateThread\s*\(", "starts a thread (check it only starts in install/callbacks)"),
]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def select_arg_counts(code):
    """Number of arguments of every OpenFixes::select(...) call"""
    counts = []
    for m in re.finditer(r"\bselect\s*(?:<[^<>()]*(?:<[^<>()]*>[^<>()]*)*>)?\s*\(", code):
        depth, args, i = 1, 1, m.end()
        while depth and i < len(code):
            ch = code[i]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == "," and depth == 1:
                args += 1
            i += 1
        counts.append(args)
    return counts


def namespace_scope_relocations(code):
    """REL::Relocation / RELOCATION_ID objects constructed at namespace or class-static scope"""
    hits = []
    depth_stack = []  # 'ns' | 'class' | 'func' | 'other'
    i = 0
    line = 1
    pending = ""
    for ch in code:
        if ch == "\n":
            line += 1
        if ch == "{":
            head = pending.strip()
            if re.search(r"\bnamespace\b[^;{}]*$", head) or head.endswith("extern \"C\""):
                depth_stack.append("ns")
            elif re.search(r"\b(class|struct|union|enum)\b[^;{}()]*$", head):
                depth_stack.append("class")
            elif re.search(r"\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(final\s*)?(->[^{]*)?$", head) or \
                    re.search(r"\b(if|for|while|switch|else|do|try|catch)\b", head):
                depth_stack.append("func")
            else:
                depth_stack.append("other")
            pending = ""
        elif ch == "}":
            if depth_stack:
                depth_stack.pop()
            pending = ""
        elif ch == ";":
            stmt = pending + ";"
            scope = depth_stack[-1] if depth_stack else "ns"
            in_func = "func" in depth_stack
            if not in_func and scope in ("ns", "class"):
                if re.search(r"(RELOCATION_ID|REL::ID\s*\(|REL::VariantID|REL::RelocationID|REL::Offset\s*\()", stmt) and \
                        not re.search(r"\b(using|typedef|constexpr\s+auto\s+\w+\s*=\s*RELOCATION_ID)", stmt):
                    hits.append((line, stmt.strip()[:120]))
            pending = ""
        else:
            pending += ch
        i += 1
    return hits


def main():
    files = sorted((ROOT / "src" / "Fixes").rglob("*.cpp"))
    problems = 0
    for path in files:
        rel = path.relative_to(ROOT).as_posix()
        if FILTER and FILTER not in rel:
            continue
        raw = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments(raw)
        issues = []
        if "SPDX-License-Identifier" not in raw:
            issues.append("missing SPDX header")
        header = " ".join(l.lstrip("/ ").strip() for l in raw.splitlines()[:15] if l.startswith("//"))
        if not re.search(r'"[^"]+"[^"]{0,40}? by ', header):
            issues.append('header doesn\'t credit the original ("<Mod name>" by <author>)')
        folder = path.parent
        files_here = [p for p in folder.iterdir() if p.is_file()]
        spdx = re.search(r"SPDX-License-Identifier:\s*(\S+)", raw)
        same_as_project = spdx is not None and spdx.group(1) in PROJECT_LICENSES
        lic = re.search(r"Full texts? in (.+?) (in this folder|at the repository root)", header)
        if lic:
            base = folder if lic.group(2) == "in this folder" else ROOT
            for name in re.split(r",\s*|\s+and\s+", lic.group(1)):
                if name and not (base / name).is_file():
                    issues.append(f"header names {name}, which isn't {lic.group(2)}")
        else:
            issues.append('header doesn\'t say where the license is ("Full text in LICENSE in this folder.", or '
                          '"Full text in LICENSE at the repository root." for GPL-3.0)')
        folder_licenses = [p for p in files_here if LICENSE_FILE.match(p.name)]
        if same_as_project:
            for p in folder_licenses:
                if p.read_bytes() == (ROOT / "LICENSE").read_bytes():
                    issues.append(f"{p.name} repeats the repository's GPL-3.0 LICENSE; remove it and point the header there")
        elif not folder_licenses:
            issues.append("no license file (LICENSE, COPYING, ...) in the fix's folder")
        if len([p for p in files_here if p.suffix == ".cpp"]) != 1 or len([p for p in files_here if p.suffix == ".h"]) > 1:
            issues.append("a fix's folder holds one .cpp and at most one .h")
        registrations = len(re.findall(r"\bOPENFIXES_REGISTER\s*\(", code))
        infos = re.findall(r'\.id\s*=\s*"([^"]+)",\s*\.section\s*=\s*"([^"]+)"', code)
        if registrations != 1 or len(infos) != 1:
            issues.append(f"must hold exactly one fix ({registrations} OPENFIXES_REGISTER, {len(infos)} FixInfo)")
        if infos and not re.search(r'\.details\s*=\s*"', code):
            issues.append("FixInfo has no .details (what the fix changes, shown in the README)")
        runtimes = re.search(r"\.runtimes\s*=\s*([^,]+),", code)
        if runtimes and re.search(r"Runtime::(VR|ALL)\b", runtimes.group(1)):
            if any(n != 4 for n in select_arg_counts(code)):
                issues.append("runs on VR but calls select() without a VR value (use select(se, ae, ae17, vr))")
        for fix_id, section in infos:
            if path.stem != fix_id or folder.name != fix_id:
                issues.append(f"fix {fix_id} must be src/Fixes/{section}/{fix_id}/{fix_id}.cpp")
            if folder.parent.name != section:
                issues.append(f"section {section} doesn't match folder {folder.parent.name}")
        # CONTRIBUTING.md: a fix has a single on/off toggle (read by the framework) and no settings of its own
        if re.search(r"\bSettings::get\w*\s*\(", code):
            issues.append("reads the INI; a fix only has its on/off toggle (see CONTRIBUTING.md)")
        for pattern, what in FORBIDDEN:
            if re.search(pattern, code):
                issues.append(what)
        for line, stmt in namespace_scope_relocations(code):
            issues.append(f"namespace/class-scope address resolution at line {line}: {stmt}")
        if issues:
            problems += 1
            print(f"{rel}:")
            for issue in issues:
                print(f"    - {issue}")
    print(f"{len(files)} file(s) checked, {problems} with issues")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
