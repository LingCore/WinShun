"""Build the pinned enhanced OCR archive and optionally expand it for offline Setup."""

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expand-to", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    spec = json.loads((root / "resources/ocr/package.json").read_text(encoding="utf-8"))
    runtime = args.build_dir / "_deps/onnxruntime-win-x64-1.30.0"
    sources = {
        "licenses/onnxruntime/LICENSE.txt": runtime / "LICENSE",
        "licenses/onnxruntime/ThirdPartyNotices.txt": runtime / "ThirdPartyNotices.txt",
        "licenses/paddleocr.txt": root / "third_party/ppocr/LICENSE",
    }
    payload = []
    for item in spec["files"]:
        name = item["path"]
        data = sources.get(name, args.build_dir / name).read_bytes()
        if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"{name}: size/hash differs from the pinned OCR manifest")
        payload.append((name, data))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in payload:
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)
    if args.expand_to:
        for name, data in payload:
            destination = args.expand_to / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    print(f"{args.output}: {args.output.stat().st_size:,} bytes, {len(payload)} verified files")


if __name__ == "__main__":
    main()
