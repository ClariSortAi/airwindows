"""Check portable docs, source-package inputs and accidental local artifacts."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
paths = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT,
).decode().split("\0")
errors = []
for name in filter(None, paths):
    path = ROOT / name
    if not path.is_file():
        continue
    if any(part in {".deps", ".venv", "__pycache__", "build", "dist", ".local"} for part in path.relative_to(ROOT).parts):
        errors.append(f"Generated directory tracked: {name}")
    if path.suffix.lower() in {".exe", ".dll", ".obj", ".pdb", ".pcap", ".pcapng", ".pfx", ".key"}:
        errors.append(f"Local artifact tracked: {name}")
    # Historical AirDrop research has its own evidence and path conventions.
    scoped = name.startswith(("app/", "native-ble-prototype/")) or name in {
        "README.md", "AGENTS.md", "CLAUDE.md", "FOR-AGENTS.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE.md",
        "docs/CLAUDE-CODE-REVIEW.md",
    }
    if not scoped or path.suffix.lower() not in {".md", ".txt", ".py", ".ps1", ".cpp", ".h", ".cs", ".json"}:
        continue
    text = path.read_text(encoding="utf-8-sig")
    if re.search(r"(?i)C:[\\/]Users[\\/]|C:[\\/]Dev[\\/]|b31ac1219e25a061|b75de3b6de19880e|JTRMAIN", text):
        errors.append(f"Machine-specific content: {name}")
    if path.suffix.lower() == ".md":
        if len(re.findall(r"(?m)^```", text)) % 2:
            errors.append(f"Unclosed code fence: {name}")
        for target in re.findall(r"\]\(([^)]+)\)", text):
            if "://" in target or target.startswith(("#", "mailto:")):
                continue
            target = target.split("#", 1)[0]
            if target and not (path.parent / target).exists():
                errors.append(f"Missing link in {name}: {target}")
for name in (
    "app/LICENSE", "native-ble-prototype/LICENSE-iphone-use",
    "native-ble-prototype/native_hid_backend.cpp", "app/scripts/observe-phone.py",
    ".github/workflows/ci.yml", "CONTRIBUTING.md", "SECURITY.md",
    "AGENTS.md", "CLAUDE.md", "FOR-AGENTS.md", "requirements-agent.txt",
    "app/packaging/Start-AirWindows.cmd",
    "app/tools/iosctl/Program.cs", "app/tools/iosctl/README.md",
    "native-ble-prototype/subscriber_selection.h",
):
    if not (ROOT / name).is_file():
        errors.append(f"Missing repository input: {name}")
if errors:
    raise SystemExit("\n".join(errors))
print("Repository checks passed: portable docs, links, package inputs and artifact exclusions.")
