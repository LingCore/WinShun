from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import textwrap

root = Path(sys.argv[1]).resolve()
repo = Path(sys.argv[2]).resolve()
packages = Path(sys.argv[3]).resolve()
fixture = root / "publish-simulation"
(fixture / "tools").mkdir(exist_ok=True)
(fixture / "plugins").mkdir(exist_ok=True)
source = fixture / "dist/desktop"
source.mkdir(exist_ok=True)
for name in ("check.py", "registry.py"):
    shutil.copyfile(repo / "tools" / name, fixture / "tools" / name)
for path in packages.iterdir():
    shutil.copyfile(path, source / path.name)
for plugin in ("day.ocr", "day.rename"):
    entry = json.loads((repo / "plugins" / f"{plugin}.json").read_text(encoding="utf-8"))
    entry["version"] = "0.0.0"
    (fixture / "plugins" / f"{plugin}.json").write_text(json.dumps(entry), encoding="utf-8")
workflow = (repo / ".github/workflows/publish.yml").read_text(encoding="utf-8")
section = workflow.split("      - name: Verify and include the Windows desktop packages\n", 1)[1]
body = section.split("          python -I - <<'PY'\n", 1)[1].split("          PY\n", 1)[0]
body = textwrap.dedent(body)
baseline = (fixture / "dist/legacy.tsv").read_bytes()
environment = dict(os.environ)
temporary = fixture / "temporary"
temporary.mkdir(exist_ok=True)
environment.update(TEMP=str(temporary), TMP=str(temporary), PYTHONDONTWRITEBYTECODE="1")

def run():
    return subprocess.run([sys.executable, "-I", "-c", body], cwd=fixture, env=environment,
                          capture_output=True, text=True, encoding="utf-8")

accepted = run()
assert accepted.returncode == 0, accepted.stderr
rows = (fixture / "dist/legacy.tsv").read_text(encoding="utf-8").splitlines()
assert len(rows) == 3
assert {row.split("\t")[0] for row in rows[1:]} == {"day.ocr", "day.rename"}
for plugin, version in (("day.ocr", "1.1.0"), ("day.rename", "1.2.0")):
    path = fixture / "plugins" / f"{plugin}.json"
    entry = json.loads(path.read_text(encoding="utf-8"))
    entry["version"] = version
    path.write_text(json.dumps(entry), encoding="utf-8")
(fixture / "dist/legacy.tsv").write_bytes(baseline)
skipped = run()
assert skipped.returncode == 0, skipped.stderr
assert (fixture / "dist/legacy.tsv").read_bytes() == baseline

receipt = source / "day.ocr-1.1.0.build.json"
original = receipt.read_bytes()
tampered = json.loads(original)
tampered["sourceCommit"] = "a" * 40
receipt.write_text(json.dumps(tampered), encoding="utf-8")
rejected_receipt = run()
assert rejected_receipt.returncode != 0 and "receipt differs from package" in rejected_receipt.stderr
receipt.write_bytes(original)
checksum = source / "day.ocr-1.1.0.wsplugin.sha256"
original = checksum.read_bytes()
checksum.write_text("0" * 64 + "  day.ocr-1.1.0.wsplugin\n", encoding="ascii")
rejected_checksum = run()
assert rejected_checksum.returncode != 0 and "checksum attachment mismatch" in rejected_checksum.stderr
checksum.write_bytes(original)
report = {"validPackagesAccepted": True, "sameOrNewerVersionsSkipped": True,
          "tamperedReceiptRejected": True, "tamperedChecksumRejected": True}
(root / "publisher-validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps(report))
