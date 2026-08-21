#!/usr/bin/env python3
"""Serve a .pfrm blob over HTTP with proper ETag/304 handling, for bench testing.

This stands in for the photoframe-webhook service so you can exercise the whole
firmware image path from a laptop, with no cluster and no phone:

    python tools/encode_image.py photo.jpg -o /tmp/latest.pfrm
    python tools/serve_test.py /tmp/latest.pfrm --port 8080

then point IMAGE_URL in secrets.h at http://<your-laptop>:8080/latest.pfrm.

The file is re-read and re-hashed on every request, so you can re-encode a different
photo over the top of it and the next wake will see a new ETag and re-render.

Fault injection for bring-up stage 6 -- these are the paths that actually matter:

    --truncate N   serve only the first N bytes (Content-Length still honest)
    --corrupt      flip one byte in the middle (payload CRC must reject it)
    --status 500   return an error instead
    --stall N      pause N seconds mid-body (exercises the stall timeout)
"""

import argparse
import hashlib
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    args = None  # set in main()

    def log_message(self, fmt, *a):
        print(f"{self.address_string()} {fmt % a}")

    def do_GET(self):
        self._serve(body=True)

    def do_HEAD(self):
        self._serve(body=False)

    def _serve(self, body):
        a = self.args
        if a.status != 200:
            self.send_error(a.status, "fault injection")
            return

        try:
            data = a.path.read_bytes()
        except OSError as e:
            self.send_error(404, str(e))
            return

        etag = '"%s"' % hashlib.sha256(data).hexdigest()[:32]

        if self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.end_headers()
            return

        if a.corrupt:
            data = bytearray(data)
            data[len(data) // 2] ^= 0xFF
            data = bytes(data)
        if a.truncate:
            data = data[: a.truncate]

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("ETag", etag)
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if not body:
            return

        if a.stall:
            half = len(data) // 2
            self.wfile.write(data[:half])
            self.wfile.flush()
            print(f"  ... stalling {a.stall}s mid-body")
            time.sleep(a.stall)
            self.wfile.write(data[half:])
        else:
            self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", type=Path)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--truncate", type=int, default=0)
    ap.add_argument("--corrupt", action="store_true")
    ap.add_argument("--status", type=int, default=200)
    ap.add_argument("--stall", type=int, default=0)
    args = ap.parse_args()

    Handler.args = args
    print(f"serving {args.path} on http://{args.bind}:{args.port}/latest.pfrm")
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
