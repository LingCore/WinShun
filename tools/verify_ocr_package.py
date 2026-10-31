"""Verify enhanced OCR in an isolated production App using a local HTTP fixture.

The deployed directory must be a fresh standard deployment, without OCR files.
Each --probe-runtime invocation owns and removes its own temporary profile.
"""

import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.app = args.app.resolve()
    args.output = args.output.resolve()
    if args.output.exists():
        raise ValueError("Choose a new output directory to preserve earlier evidence")
    if (args.app.parent / "onnxruntime.dll").exists():
        raise ValueError("Use a standard deployment without bundled OCR")
    args.output.mkdir(parents=True)
    payload = args.package.read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass

        def do_GET(self):
            if self.path == "/unavailable":
                self.send_error(503)
                return
            body = b"damaged OCR archive" if self.path == "/corrupt" else payload
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                for start in range(0, len(body), 128 * 1024):
                    self.wfile.write(body[start:start + 128 * 1024])
                    self.wfile.flush()
                    time.sleep(0.02 if self.path == "/slow" else 0.001)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    url = f"http://127.0.0.1:{server.server_port}"
    local = args.output / "离线 包" / args.package.name
    local.parent.mkdir()
    shutil.copy2(args.package, local)
    cases = [
        ("download", ["--ocr-source", url + "/valid"], [True]),
        ("network-retry", ["--ocr-source", url + "/unavailable", "--ocr-retry-source", url + "/valid"], [False, True]),
        ("corrupt-retry", ["--ocr-source", url + "/corrupt", "--ocr-retry-source", url + "/valid"], [False, True]),
        ("cancel-retry", ["--ocr-source", url + "/slow", "--ocr-cancel", "--ocr-retry-source", url + "/valid"], [False, True]),
        ("local-unicode", ["--ocr-local", str(local), "--dark", "--scale", "1.5"], [True]),
        ("english-compact", ["--ocr-local", str(local), "--english", "--compact", "--scale", "1.25"], [True]),
    ]
    results = []
    environment = os.environ.copy()
    # The app-local deployment supplies Qt and the C++ runtime.
    environment["PATH"] = os.pathsep.join([str(args.app.parent), os.path.join(os.environ["WINDIR"], "System32"), os.environ["WINDIR"]])
    for name in ("QML2_IMPORT_PATH", "QML_IMPORT_PATH", "QT_PLUGIN_PATH", "WINSHUN_OCR_PACKAGE"):
        environment.pop(name, None)
    try:
        for name, flags, expected in cases:
            output = args.output / name
            command = [str(args.app), "--probe-runtime", str(output), "--runtime-ui", "--ocr-probe", "--settle-ms", "300", *flags]
            run = subprocess.run(command, env=environment, capture_output=True, timeout=60,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
            report = json.loads((output / "runtime.json").read_text(encoding="utf-8"))
            attempts = report["ocr_attempts"]
            assert run.returncode == 0, (name, run.returncode, report)
            assert report["warnings"] == 0 and report["ocr_before"] is False, (name, report)
            assert report["ocr_button_inside_row"], (name, report)
            assert [item["available"] for item in attempts] == expected, (name, attempts)
            assert all(not item["busy"] for item in attempts), (name, attempts)
            assert attempts[-1]["progress"] == 1, (name, attempts)
            if name == "english-compact":
                assert report["ocr_title"] == "Enhanced text recognition", report
                assert report["ocr_button"] == "Download and install", report
            results.append({"case": name, "passed": True, "attempts": attempts})
            print(f"PASS {name}", flush=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)
    (args.output / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{len(results)}/{len(cases)} isolated production App cases passed")


if __name__ == "__main__":
    main()
