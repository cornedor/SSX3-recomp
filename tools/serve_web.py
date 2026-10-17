#!/usr/bin/env python3
"""Serve the web build (build-web/ by default) cross-origin isolated.

The wasm build's threads share one memory (SharedArrayBuffer), which browsers
only allow on a cross-origin isolated page: COOP same-origin + COEP
require-corp. localhost counts as a secure context, so this is enough to play
on this machine:

    tools/serve_web.py [dir] [--port 8080] [--bind 127.0.0.1] [--game DIR]

--game DIR also serves your rip under /game/ with /game/manifest.json, and
the page offers to copy it from there (or does so at once with
?import=server): handy for automated runs, where no one picks a folder.

--sink DIR accepts what a page started with ?report sends back: its log
(POST /report/log, appended to DIR/web_log.txt) and canvas screenshots
(POST /report/shot?name=N, DIR/N.png) -- so headless runs need no driver.

The page and module contain SSX 3's recompiled code: serve them to yourself
only (the default bind is loopback).
"""
import argparse
import functools
import http.server
from pathlib import Path


import json
import os
import urllib.parse

GAME_DIR = None
SINK_DIR = None


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript"}

    def translate_path(self, path):
        clean = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        if GAME_DIR and (clean == "/game" or clean.startswith("/game/")):
            rel = clean[len("/game"):].lstrip("/")
            full = os.path.realpath(os.path.join(GAME_DIR, rel))
            if full == GAME_DIR or full.startswith(GAME_DIR + os.sep):
                return full
            return os.path.join(GAME_DIR, "\0denied")
        return super().translate_path(path)

    def do_GET(self):
        if GAME_DIR and urllib.parse.urlsplit(self.path).path == "/game/manifest.json":
            files = []
            for root, _dirs, names in os.walk(GAME_DIR, followlinks=True):
                for n in sorted(names):
                    full = os.path.join(root, n)
                    rel = os.path.relpath(full, GAME_DIR).replace(os.sep, "/")
                    if rel.startswith(("UDATA/", "TDATA/")) or rel.endswith(".json"):
                        continue          # saves and analysis output are not the disc
                    files.append({"path": rel, "size": os.path.getsize(full)})
            body = json.dumps({"files": files}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        u = urllib.parse.urlsplit(self.path)
        if not SINK_DIR or not u.path.startswith("/report/"):
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if u.path == "/report/log":
            with open(os.path.join(SINK_DIR, "web_log.txt"), "ab") as f:
                f.write(body)
        elif u.path == "/report/shot":
            name = urllib.parse.parse_qs(u.query).get("name", ["shot"])[0]
            name = "".join(c for c in name if c.isalnum() or c in "-_") or "shot"
            with open(os.path.join(SINK_DIR, name + ".png"), "wb") as f:
                f.write(body)
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        if not SINK_DIR:
            super().log_message(fmt, *args)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def main():
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?", default=str(here / "build-web"))
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--game", help="your rip (default.xbe + data/) to offer under /game/")
    ap.add_argument("--sink", help="directory for ?report logs and screenshots")
    a = ap.parse_args()
    global GAME_DIR, SINK_DIR
    if a.sink:
        SINK_DIR = os.path.realpath(a.sink)
        os.makedirs(SINK_DIR, exist_ok=True)
    if a.game:
        GAME_DIR = os.path.realpath(a.game)
    handler = functools.partial(Handler, directory=a.dir)
    with http.server.ThreadingHTTPServer((a.bind, a.port), handler) as srv:
        print(f"serving {a.dir} on http://{a.bind}:{a.port}/")
        srv.serve_forever()


if __name__ == "__main__":
    main()
