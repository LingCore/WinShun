"""Generates src/plugins-sdk/winshun-pinyin-table.h, the pinyin table the
optional SDK header src/plugins-sdk/winshun-pinyin.h matches against.

Same source data and same readings as tools/make_pinyin.py (which builds
src/core/PinyinTable.cpp for the app), but packed for a single-header SDK:

  - one array of packed readings instead of readings + initials (the initials
    are one bit mask per character; they are derived from the readings at
    runtime, so the 174 KB in PinyinTable.cpp is not carried here)
  - syllables joined with NUL so the whole set is one string literal
  - the readings as a hex blob, eight digits per character

Usage: python tools/make_sdk_pinyin.py
"""
import pathlib
import re
import unicodedata

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATA = ROOT / "tools" / "data"
OUT = ROOT / "src" / "plugins-sdk" / "winshun-pinyin-table.h"

FIRST, LAST = 0x4E00, 0x9FFF
MAX_READINGS = 3
BITS = 10
DIAERESIS = "\u0308"


def toneless(reading):
    out = []
    for ch in unicodedata.normalize("NFD", reading):
        if ch == DIAERESIS and out and out[-1] == "u":
            out[-1] = "v"
        elif unicodedata.category(ch) != "Mn":
            out.append(ch)
    syllable = "".join(out)
    if not re.fullmatch(r"[a-z]+", syllable):
        raise SystemExit(f"unexpected reading {reading!r}")
    return syllable


def load(name):
    table = {}
    line_re = re.compile(r"^U\+([0-9A-F]+):\s*([^#]+)")
    for line in (DATA / name).read_text(encoding="utf-8").splitlines():
        m = line_re.match(line)
        if not m:
            continue
        code = int(m.group(1), 16)
        if not FIRST <= code <= LAST:
            continue
        found = []
        for reading in m.group(2).strip().split(","):
            syllable = toneless(reading.strip())
            if syllable not in found:
                found.append(syllable)
        table[code] = found
    return table


def readings():
    common = load("pinyin.txt")
    standard = load("kTGHZ2013.txt")
    table = {}
    for code, ordered in common.items():
        if code in standard:
            allowed = standard[code]
            chosen = [s for s in ordered if s in allowed] + [s for s in allowed if s not in ordered]
            table[code] = chosen[:MAX_READINGS]
        else:
            table[code] = ordered[:1]
    return table


def main():
    table = readings()
    syllables = sorted({s for found in table.values() for s in found})
    assert len(syllables) < (1 << BITS) - 1
    number = {s: i + 1 for i, s in enumerate(syllables)}

    packed = []
    for code in range(FIRST, LAST + 1):
        value = 0
        for k, s in enumerate(table.get(code, [])):
            value |= number[s] << (BITS * k)
        packed.append(value)

    # Syllables separated by NUL, as escaped octal: a source file with a raw
    # NUL in a literal is not something every compiler and editor survives.
    joined = "\0".join(syllables) + "\0"
    syllable_literal = "".join(
        "\\0" if ch == "\0" else ch for ch in joined
    )

    def hex_blob(values):
        return "".join(f"{v:08X}" for v in values)

    lines = [
        "// winshun-pinyin-table.h -- generated, do not edit.",
        "//",
        "// The readings behind winshun-pinyin.h, alone in their own header so that",
        "// the table is compiled once even when several translation units include",
        "// the matcher. Generate with tools/make_sdk_pinyin.py.",
        "//",
        "// pinyin-data (https://github.com/mozillazg/pinyin-data, MIT licence):",
        f"// {len(syllables)} syllables, U+{FIRST:04X}..U+{LAST:04X}, up to {MAX_READINGS} readings",
        f"// of {BITS} bits each (syllable number + 1, 0 = none), most common first.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <string_view>",
        "",
        "namespace winshun::pinyin::detail {",
        "",
        "// The syllables, NUL separated, numbered in this order. A char array,",
        "// not a string_view: a view built from a literal stops at the first NUL.",
        f'inline constexpr char kPinyinSyllables[] = "{syllable_literal}";',
        "",
        "// One 8-hex-digit group per character, U+4E00 upwards.",
        "inline constexpr std::string_view kPinyinReadingsHex =",
    ]
    blob = hex_blob(packed)
    for i in range(0, len(blob), 96):
        lines.append(f'    "{blob[i:i + 96]}"')
    lines[-1] += ";"
    lines.append("")
    lines.append("} // namespace winshun::pinyin::detail")
    lines.append("")
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    counted = sum(1 for found in table.values() if len(found) > 1)
    print(f"syllables={len(syllables)} chars={len(table)} multi={counted} bytes={OUT.stat().st_size}")


if __name__ == "__main__":
    main()
