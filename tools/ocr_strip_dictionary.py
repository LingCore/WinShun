"""Make a reading model without its characters, and the dictionary beside it.

RapidOCR's PP-OCR files carry their characters in the model (metadata key
"character"); PaddleOCR's own ONNX exports do not and come with a dictionary
file. tst_ocrpackage reads with such a pair when the build folder has one:

    python tools/ocr_strip_dictionary.py build/<task>/ocr-precise/PP-OCRv6_rec_small.onnx build/<task>/ocr-precise/nodict.onnx

writes nodict.onnx and nodict.txt (one character a line). Needs the `onnx` package.
"""
import pathlib
import sys

import onnx


def main() -> int:
    source, target = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    model = onnx.load(str(source))
    characters = [item.value for item in model.metadata_props if item.key == "character"]
    if not characters:
        print("the model has no characters of its own")
        return 1
    kept = [item for item in model.metadata_props if item.key != "character"]
    del model.metadata_props[:]
    model.metadata_props.extend(kept)
    onnx.save(model, str(target))
    target.with_suffix(".txt").write_text(characters[0] + "\n", encoding="utf-8", newline="\n")
    print(f"{target.name}: {len(characters[0].splitlines())} characters moved to {target.with_suffix('.txt').name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
