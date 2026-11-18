"""A stand-in for the update service, on this computer only (127.0.0.1).

For tools/verify-installed-upgrade.ps1: Win顺 started with
WINSHUN_UPDATE_FEED=http://127.0.0.1:<port>/stable.json (and
WINSHUN_UPDATE_AUTOINSTALL=1) asks this for the latest version and downloads the
package from it, as it does from lcstp.com. Serves the Setup, or the portable
zip for ...?package=portable, of the release folder given (the output folder of
scripts/release.ps1), with the size and SHA-256 of the file as it is on disk.
The delta packages in that folder (WinShun-<version>-x64-delta-from-*.zip)
are named in the notes' hidden line and served beside it, as on GitHub
(src/app/UpdateDelta.h); --no-delta leaves them out. --push-now adds the
line of a version pushed at once (release::Push::Now in src/core/Release.h);
without it the version counts as published when this started, so a copy's
automatic notice waits for its slot within the next day. The answer has an
ETag, and a request with that If-None-Match gets a 304, as from lcstp.com.

    python tools/update-feed.py <release folder> <version> [port] [--no-delta] [--push-now]

Writes one line per request to stdout; ends when killed.
"""
import datetime
import hashlib
import http.server
import json
import pathlib
import sys
import urllib.parse

flags = {"--no-delta", "--push-now"}
args = [a for a in sys.argv[1:] if a not in flags]
published = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
push_line = "\n<!-- winshun-push now -->\n" if "--push-now" in sys.argv else ""
folder = pathlib.Path(args[0]).resolve()
version = args[1]
port = int(args[2]) if len(args) > 2 else 47631
packages = {False: f"WinShun-{version}-x64-setup.exe", True: f"WinShun-{version}-x64-portable.zip"}
for name in packages.values():
    if not (folder / name).is_file():
        sys.exit(f"missing {folder / name}")
deltas = [] if "--no-delta" in sys.argv else sorted(p.name for p in folder.glob(f"WinShun-{version}-x64-delta-from-*.zip"))
digests = {name: hashlib.sha256((folder / name).read_bytes()).hexdigest() for name in list(packages.values()) + deltas}
delta_line = ""
if deltas:
    prefix = f"WinShun-{version}-x64-delta-from-"
    delta_line = "\n\n<!-- winshun-delta 1 " + " ".join(
        f"{name[len(prefix):-len('.zip')]}:{name}:{(folder / name).stat().st_size}:{digests[name]}" for name in deltas) + " -->\n"


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
                        "## 新功能 · What's new\n\n- 🧪 **测试** 本机升级实测。\n\n  **Test** The upgrade test on this PC.\n"
                        + push_line + delta_line,
                "published_at": published,
                "assets": [{"name": package, "browser_download_url": f"http://127.0.0.1:{port}/{package}",
                            "size": (folder / package).stat().st_size, "digest": "sha256:" + digests[package]}],
            }).encode("utf-8")
            etag = '"' + hashlib.sha256(body).hexdigest() + '"'
            if self.headers.get("If-None-Match") == etag:
                self.send_response(304)
                self.send_header("ETag", etag)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("ETag", etag)
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


print(f"serving {version} from {folder} on http://127.0.0.1:{port}/stable.json, deltas: {deltas or 'none'}, "
      f"push: {'now' if push_line else 'normal'}", flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
