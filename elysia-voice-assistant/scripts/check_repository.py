"""Check source syntax, the character pack, and the clean publication files."""
import ast
import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {".git", "__pycache__", ".venv", "venv", "cache", "output", "SpeakerVerification_DIR", "managed_components", "reference_audio"}


def files():
    for path in ROOT.rglob("*"):
        relative = path.relative_to(ROOT)
        if not path.is_file() or any(p in SKIP_DIRS or p.startswith("build") for p in relative.parts[:-1]):
            continue
        if relative.parts[:2] == ("backend", "models") and path.name != "README.md":
            continue
        if path.name in {"sdkconfig", "sdkconfig.old"} or path.name.endswith(".local.h") or path.name == ".env" or path.name.endswith(".env"):
            continue
        yield path


def main():
    failures = []
    count = 0
    for path in files():
        count += 1
        relative = path.relative_to(ROOT)
        if path.suffix == ".py":
            try:
                ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(relative))
            except SyntaxError as exc:
                failures.append(f"{relative}: Python syntax error at line {exc.lineno}")
        if path.suffix == ".json":
            json.loads(path.read_text(encoding="utf-8-sig"))
        if path.stat().st_size > 50 * 1024 * 1024:
            failures.append(f"{relative}: unexpectedly large publication file")
        if path.suffix in {".wav", ".pyc", ".zip", ".bin", ".elf", ".bak", ".fixed"}:
            failures.append(f"{relative}: generated or private file in publication set")
        if path.suffix in {".py", ".h", ".c", ".md", ".json"} or path.name.endswith(".example"):
            text = path.read_text(encoding="utf-8-sig")
            if re.search(r"\bsk-[A-Za-z0-9_-]{16,}\b", text):
                failures.append(f"{relative}: possible API key; value suppressed")
            if re.search(r'(?m)^\w+_IN_CODE\s*=\s*[\x22\x27][^\x22\x27]+', text):
                failures.append(f"{relative}: nonempty inline cloud configuration")
    pack = ROOT / "backend" / "ElysiaSkill_real"
    spec = importlib.util.spec_from_file_location("elysia_pack_check", pack / "loader.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    loaded = module.load_elysia_skill(pack)
    if not loaded["system_prompt"] or not loaded["example_messages"]:
        failures.append("Character pack is empty")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"PASS: {count} publication files; Python syntax, JSON and character pack checked")
    print("Local secrets, recordings, models and build directories must remain ignored by Git.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
