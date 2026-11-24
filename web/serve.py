#!/usr/bin/env python3
"""Local test server for the browser build (emu8's, adapted).

    python web/serve.py [port] [--all]     -> http://localhost:8766/

Serves web/dist with the COOP/COEP headers SharedArrayBuffer needs, plus (local only) the repo's
sdcard/ folder: /__sdcard_list returns its file list and /__sdcard/<path> the files. On localhost the
page copies the ones it lacks into its /sdcard (IndexedDB) at startup. By default only the files git
tracks (the BIOS, the free disks, the tests); --all lists every file up to 64 MB, the local MS-DOS
and Windows disks and games included. Nothing of this exists on the published site.
"""
import functools
import http.server
import json
import os
import subprocess
import sys
import urllib.parse

WEB = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(WEB)
DIST = os.path.join(WEB, "dist")
SD = os.path.join(REPO, "sdcard")
MAX = 64 * 1024 * 1024
ALL = "--all" in sys.argv


def sd_files():
    if ALL:
        out = []
        for root, dirs, names in os.walk(SD):
            dirs[:] = [d for d in dirs if not d.startswith(".")]     # .unzipped, the folder disks' temporaries
            out += [os.path.relpath(os.path.join(root, n), SD).replace(os.sep, "/") for n in names]
    else:
        r = subprocess.run(["git", "ls-files", "sdcard"], cwd=REPO, capture_output=True, text=True)
        out = [p[len("sdcard/"):] for p in r.stdout.splitlines() if p.startswith("sdcard/")]
    files = []
    for rel in sorted(out):
        full = os.path.join(SD, rel)
        if os.path.isfile(full) and os.path.getsize(full) <= MAX:
            files.append({"path": rel, "size": os.path.getsize(full)})
    return files


class H(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript",
                      ".webmanifest": "application/manifest+json"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        if p.startswith("/__sdcard/"):
            full = os.path.normpath(os.path.join(SD, p[len("/__sdcard/"):]))
            return full if full.startswith(SD) else SD
        if p == "/send":
            return os.path.join(DIST, "send.html")
        return super().translate_path(path)

    def do_GET(self):
        if self.path.startswith("/__sdcard_list"):
            body = json.dumps(sd_files()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if args else 8766
    print(f"pico-386 local: http://localhost:{port}/  (sdcard: {SD}, {'all files' if ALL else 'git-tracked files'})", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=DIST)).serve_forever()
