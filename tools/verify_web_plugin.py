"""Exercise the production web provider on the runtime probe's private profile."""
import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import urllib.parse
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--qt-bin', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--dark', action='store_true')
    parser.add_argument('--english', action='store_true')
    parser.add_argument('--scale', default='1')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    requests = []

    class Suggestions(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            words = urllib.parse.parse_qs(url.query).get('q', [''])[0]
            requests.append({'path': url.path, 'words': words})
            body = json.dumps([words, [words + ' 建议一', words + ' 建议二']], ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Suggestions)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    env['QT_FORCE_STDERR_LOGGING'] = '1'
    source = Path(__file__).resolve().parents[1] / 'src/web-plugin/package'
    try:
        with tempfile.TemporaryDirectory(prefix='WinShun-web-package-') as fixture:
            shutil.copytree(source, Path(fixture) / 'winshun.web')
            archive = args.archive.resolve() if args.archive else Path(fixture) / 'web-reinstall.wsplugin'
            if not args.archive:
                with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as package:
                    for file in sorted(source.iterdir()):
                        if file.is_file():
                            package.write(file, file.name)
            command = [str(args.exe.resolve()), '--probe-runtime', str(args.out.resolve()),
                       '--runtime-plugins', fixture, '--web-plugin-check', '--renderer', 'd3d11',
                       '--web-plugin-archive', str(archive),
                       '--web-suggest-url', f'http://127.0.0.1:{server.server_port}/suggest?q=%s',
                       '--settle-ms', '100', '--scale', args.scale]
            if args.dark:
                command.append('--dark')
            if args.english:
                command.append('--english')
            with (args.out / 'process.log').open('w', encoding='utf-8') as log:
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        (args.out / 'suggestion-requests.json').write_text(json.dumps(requests, ensure_ascii=False, indent=2), encoding='utf-8')
    if result.returncode:
        raise SystemExit(f'Runtime probe failed: {result.returncode}; see {args.out}')
    report = json.loads((args.out / 'runtime.json').read_text(encoding='utf-8'))
    assert report.get('web_plugin_passed'), report.get('web_plugin')
    assert requests and any('中文 & spaces' in request['words'] for request in requests), requests
    assert all('disabled' not in request['words'] and 'uninstalled' not in request['words'] for request in requests), requests
    print(f"PASS: {len(report['web_plugin'])} production checks, live local suggestions, no requests while disabled/uninstalled")


if __name__ == '__main__':
    main()
