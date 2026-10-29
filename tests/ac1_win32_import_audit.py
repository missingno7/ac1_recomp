"""Read-only PE import audit against committed source and the adapter chain."""
import argparse
import json
from pathlib import Path
import subprocess
import pefile


def audit(binary, baseline="HEAD"):
    paths = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", baseline, "src"], text=True
    ).splitlines()
    source = "\n".join(subprocess.check_output(
        ["git", "show", f"{baseline}:{path}"], text=True, errors="replace"
    ) for path in paths)
    pe = pefile.PE(str(binary))
    adapter = Path("src/runtime/ac1_win32_compat.inc").read_text()
    runtime = Path("src/runtime/ac1_runtime.c").read_text()
    assert '#include "ac1_win32_compat.inc"' in runtime
    assert "handle_win32_compat_import(cpu, import) ||" in runtime
    result = {"baseline": baseline, "binary": str(binary), "literal_absent": {}, "disposition": {}}
    seh = {"RtlUnwind", "UnhandledExceptionFilter", "DebugBreak"}
    pure_network = {"htons", "htonl", "ntohs", "ntohl", "__WSAFDIsSet"}
    bink_count = 0
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode()
        for item in entry.imports:
            name = item.name.decode() if item.name else f"#{item.ordinal}"
            if dll.lower() == "binkw32.dll":
                bink_count += 1
            if item.name and f'"{name}"' in source:
                continue
            result["literal_absent"].setdefault(dll, []).append(name)
            if dll.lower() in {"eax.dll", "dsound.dll"}:
                state = "existing ordinal handler (native DS8 on Windows; optional EAX failure)"
            elif name in seh:
                state = "explicit SEH diagnostic"
            elif dll.lower() in {"wininet.dll", "ws2_32.dll"} and name not in pure_network:
                state = "explicit offline diagnostic: caller fallback unverified"
            elif dll.lower() == "kernel32.dll" and name in {"ExitProcess", "TerminateProcess", "FatalAppExitA"}:
                state = "controlled guest stop: code/caller logged; host status remains 8"
            else:
                state = "compatibility handler (platform subsets documented in source)"
            if item.name:
                assert f'"{name}"' in adapter, f"missing adapter/diagnostic for {dll}!{name}"
            result["disposition"][f"{dll}!{name}"] = state
    result["counts"] = {dll: len(names) for dll, names in result["literal_absent"].items()}
    result["absent_total"] = sum(result["counts"].values())
    result["bink_imports_already_named"] = bink_count
    result["xinput_exports"] = {}
    expected = {2: "XInputGetState", 3: "XInputSetState", 4: "XInputGetCapabilities"}
    for directory in ("System32", "SysWOW64"):
        path = Path("C:/Windows") / directory / "xinput1_3.dll"
        exports = pefile.PE(str(path)).DIRECTORY_ENTRY_EXPORT.symbols
        found = {e.ordinal: e.name.decode() for e in exports if e.ordinal in expected and e.name}
        assert found == expected, (path, found)
        result["xinput_exports"][str(path)] = found
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(
        "C:/Program Files (x86)/GOG.com/Assassins Creed/AssassinsCreed_Dx9.exe"))
    parser.add_argument("--baseline", default="HEAD")
    args = parser.parse_args()
    data = audit(args.binary, args.baseline)
    out = Path("work/win32_compat/import-audit.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, indent=2) + "\n")
    print(f"PASS: {data['absent_total']} literal-absent imports; {data['counts']}; XInput ordinals verified")
