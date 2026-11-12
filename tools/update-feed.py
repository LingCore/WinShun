"""A stand-in for the update service, on this computer only (127.0.0.1).

For tools/verify-installed-upgrade.ps1: Win顺 started with
WINSHUN_UPDATE_FEED=http://127.0.0.1:<port>/stable.json (and
WINSHUN_UPDATE_AUTOINSTALL=1) asks this for the latest version and downloads the
package from it, as it does from lcstp.com. Serves the Setup, or the portable
zip for ...?package=portable, of the release folder given (the output folder of
scripts/release.ps1), with the size and SHA-256 of the file as it is on disk.

    python tools/update-feed.py <release folder> <version> [port]

Writes one line per request to stdout; ends when killed.
"""
import hashlib
import http.server
import json
import pathlib
import sys
import urllib.parse

folder = pathlib.Path(sys.argv[1]).resolve()
version = sys.argv[2]
port = int(sys.argv[3]) if len(sys.argv) > 3 else 47631
packages = {False: f"WinShun-{version}-x64-setup.exe", True: f"WinShun-{version}-x64-portable.zip"}
for name in packages.values():
    if not (folder / name).is_file():
        sys.exit(f"missing {folder / name}")
digests = {name: hashlib.sha256((folder / name).read_bytes()).hexdigest() for name in packages.values()}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        name = url.path.lstrip("/")
        if name == "stable.json":
            package = packages["package=portable" in url.query]
            body = json.dumps({
                "tag_name": f"v{version}",
                "html_url": "https://lcstp.com/projects/winshun",
                "body": f"Win顺 {version}（本机升级实测）。 WinShun {version} (upgrade test on this PC).\n\n"
                        "## 新功能 · What's new\n\n- 🧪 **测试** 本机升级实测。\n\n  **Test** The upgrade test on this PC.\n",
                "assets": [{"name": package, "browser_download_url": f"http://127.0.0.1:{port}/{package}",
                            "size": (folder / package).stat().st_size, "digest": "sha256:" + digests[package]}],
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
        elif name in digests:
            body = (folder / name).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
        else:
            self.send_error(404)
            return
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        print(self.address_string(), format % args, flush=True)


print(f"serving {version} from {folder} on http://127.0.0.1:{port}/stable.json", flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
