"""Restore CAM++ from a deployment ZIP, or explicitly download it from ModelScope."""
import argparse
import shutil
import tempfile
import zipfile
from pathlib import Path

MODEL_ID = "iic/speech_campplus_sv_zh-cn_16k-common"
FILES = ("configuration.json", "campplus_cn_common.bin")
DEFAULT_TARGET = Path(__file__).resolve().parents[1] / "backend" / "models" / "CAM++"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--archive", type=Path, help="Original deployment ZIP; personal audio is skipped")
    source.add_argument("--download", action="store_true", help="Download the two files from ModelScope")
    parser.add_argument("--target", type=Path, default=DEFAULT_TARGET)
    args = parser.parse_args()
    target = args.target.resolve()
    if any((target / name).exists() for name in FILES):
        parser.error("Target already contains model files; choose another --target to avoid overwriting")
    target.mkdir(parents=True, exist_ok=True)
    if args.archive:
        with zipfile.ZipFile(args.archive) as archive:
            names = archive.namelist()
            selected = {}
            for name in FILES:
                candidates = [p for p in names if p.replace("\\", "/").endswith("models/CAM++/" + name)]
                if len(candidates) != 1:
                    parser.error(f"Archive must contain exactly one models/CAM++/{name}")
                selected[name] = candidates[0]
            # Read only exact required members; do not extract the archive tree or enrollment audio.
            for name, member in selected.items():
                with archive.open(member) as src, (target / name).open("wb") as dst:
                    shutil.copyfileobj(src, dst)
    else:
        from modelscope import snapshot_download
        with tempfile.TemporaryDirectory(prefix="elysia-camplusplus-") as staging:
            downloaded = Path(snapshot_download(MODEL_ID, local_dir=staging, allow_file_pattern=list(FILES)))
            missing = [name for name in FILES if not (downloaded / name).is_file()]
            if missing:
                raise SystemExit("Downloaded model is incomplete: " + ", ".join(missing))
            for name in FILES:
                shutil.copyfile(downloaded / name, target / name)
    for name in FILES:
        if not (target / name).is_file() or (target / name).stat().st_size == 0:
            raise SystemExit("Model file missing or empty: " + name)
    print(f"CAM++ ready: {target}")


if __name__ == "__main__":
    main()
