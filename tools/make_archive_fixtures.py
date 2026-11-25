"""Makes the small archives tests/tst_archive.cpp reads (tests/fixtures/archives).

    python tools/make_archive_fixtures.py [libarchive source folder]

Needs py7zr (pip install py7zr) for the 7z ones. The two .rar files cannot be
made here (nothing free writes RAR): they are taken from libarchive's own
test data, given its source folder (build/<dir>/_deps/libarchive/archive/s
once the plugin was built), and otherwise left as they are.

The files are checked in; run this only to change them.
"""

import binascii
import io
import os
import struct
import sys
import tarfile
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'fixtures', 'archives')
WHEN = (2026, 3, 14, 9, 26, 52)  # every member's time
WHEN_UNIX = 1773480412  # the same, as local time +08:00 (tar, 7z)

# name -> text; a name ending in / is a folder
MEMBERS = {
    '合同/': None,
    '合同/2026 年度采购合同.docx': '采购合同正文\n',
    '合同/附件/报价单.xlsx': '报价单\n',
    'notes/Read Me.TXT': 'hello from the archive\n',
    'notes/会议纪要.md': '# 会议纪要\n\n下周三评审。\n',
}


def write(name, data):
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(data)
    print(f'{name}: {len(data)} bytes')


def zip_bytes(names_as=None):
    """A zip of MEMBERS. names_as: an encoding to store the names in without
    the UTF-8 flag (what Windows' own zip and old tools write: GBK here)."""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, text in MEMBERS.items():
            info = zipfile.ZipInfo(name, WHEN)
            info.compress_type = zipfile.ZIP_STORED if text is None else zipfile.ZIP_DEFLATED
            z.writestr(info, b'' if text is None else text.encode('utf-8'))
    data = buffer.getvalue()
    if not names_as:
        return data
    # Rewritten by hand: zipfile always flags non-ASCII names as UTF-8.
    out = io.BytesIO()
    central = []
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            raw = info.filename.encode(names_as)
            body = z.read(info) if not info.is_dir() else b''
            packed = body  # stored: sizes and CRC stay what they are
            offset = out.tell()
            date = (WHEN[0] - 1980) << 9 | WHEN[1] << 5 | WHEN[2]
            time = WHEN[3] << 11 | WHEN[4] << 5 | WHEN[5] // 2
            crc = binascii.crc32(body) & 0xFFFFFFFF
            out.write(struct.pack('<IHHHHHIIIHH', 0x04034B50, 20, 0, 0, time, date, crc, len(packed), len(body), len(raw), 0))
            out.write(raw)
            out.write(packed)
            central.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, 20, 20, 0, 0, time, date, crc, len(packed), len(body),
                                       len(raw), 0, 0, 0, 0, 0x10 if info.is_dir() else 0x20, offset) + raw)
    start = out.tell()
    for record in central:
        out.write(record)
    size = out.tell() - start
    out.write(struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, len(central), len(central), size, start, 0))
    return out.getvalue()


def tar_bytes(mode):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode=mode, format=tarfile.PAX_FORMAT) as t:
        for name, text in MEMBERS.items():
            info = tarfile.TarInfo(name.rstrip('/'))
            info.mtime = WHEN_UNIX
            if text is None:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                t.addfile(info)
            else:
                body = text.encode('utf-8')
                info.size = len(body)
                info.mode = 0o644
                t.addfile(info, io.BytesIO(body))
    return buffer.getvalue()


def seven_zip_bytes(password=None, hide_names=False):
    import py7zr
    import tempfile
    buffer = io.BytesIO()
    # From files on the disk, for their times (writestr stamps them now).
    with tempfile.TemporaryDirectory() as temp, py7zr.SevenZipFile(buffer, 'w', password=password, header_encryption=hide_names) as z:
        for number, (name, text) in enumerate(MEMBERS.items()):
            if text is None:
                continue  # no entries for folders: the files' paths name them
            source = os.path.join(temp, str(number))
            with open(source, 'wb') as f:
                f.write(text.encode('utf-8'))
            os.utime(source, (WHEN_UNIX, WHEN_UNIX))
            z.write(source, name)
    return buffer.getvalue()


def uudecode(path):
    out = bytearray()
    with open(path, 'rb') as f:
        lines = f.read().splitlines()
    for line in lines[1:]:
        if line.strip() in (b'end', b'`', b''):
            if line.strip() == b'end':
                break
            continue
        out += binascii.a2b_uu(line)
    return bytes(out)


def main():
    os.makedirs(OUT, exist_ok=True)
    write('names-utf8.zip', zip_bytes())
    write('names-gbk.zip', zip_bytes('gbk'))
    write('names-utf8-unflagged.zip', zip_bytes('utf-8'))  # as a Mac or Linux tool writes them
    write('files.tar', tar_bytes('w'))
    write('files.tar.gz', tar_bytes('w:gz'))
    write('files.tar.xz', tar_bytes('w:xz'))
    write('files.7z', seven_zip_bytes())
    write('locked-files.7z', seven_zip_bytes('correct horse', hide_names=False))
    write('locked-names.7z', seven_zip_bytes('correct horse', hide_names=True))
    write('not-an-archive.zip', b'This is a text file somebody called .zip.\n' * 8)
    whole = zip_bytes()
    write('cut-short.zip', whole[:len(whole) // 2])
    if len(sys.argv) > 1:
        tests = os.path.join(sys.argv[1], 'libarchive', 'test')
        # stored.rar: RAR5, one file "helloworld.txt"; rar4: test_read_format_rar.rar, "test.txt" and folders.
        for source, name in (('test_read_format_rar5_stored.rar.uu', 'rar5.rar'), ('test_read_format_rar.rar.uu', 'rar4.rar'),
                             ('test_read_format_rar_encryption_header.rar.uu', 'locked-names.rar')):
            path = os.path.join(tests, source)
            if os.path.exists(path):
                write(name, uudecode(path))
            else:
                print(f'{name}: no {source} in {tests}')


if __name__ == '__main__':
    main()
