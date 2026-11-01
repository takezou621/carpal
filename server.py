"""Carpal telemetry service. Run behind an HTTPS reverse proxy."""

import argparse
import base64
import binascii
from contextlib import contextmanager
import hmac
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


ROOT = Path(__file__).resolve().parent
MAX_BODY = 300_000
MAX_IMAGE = 200_000
ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")


def identifier(value):
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise ValueError("Invalid identifier")
    return value


def number(value, low, high):
    if (isinstance(value, bool) or not isinstance(value, (int, float))
            or not low <= value <= high or not math.isfinite(value)):
        raise ValueError("Invalid numeric value")
    return value


class Store:
    def __init__(self, path):
        self.path = str(path)
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS vehicles (
                    id TEXT PRIMARY KEY, lat REAL, lon REAL, speed REAL,
                    position_at REAL, last_seen REAL NOT NULL,
                    parking_armed INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY, vehicle TEXT NOT NULL,
                    event_id TEXT NOT NULL, received_at REAL NOT NULL,
                    acceleration_g REAL NOT NULL, image BLOB,
                    UNIQUE(vehicle, event_id),
                    FOREIGN KEY(vehicle) REFERENCES vehicles(id)
                );
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys = ON")
        try:
            with db:
                yield db
        finally:
            db.close()

    def telemetry(self, data):
        vehicle = identifier(data.get("vehicle"))
        position = data.get("position")
        if position is not None:
            if not isinstance(position, dict):
                raise ValueError("Invalid position")
            lat = number(position.get("lat"), -90, 90)
            lon = number(position.get("lon"), -180, 180)
            speed = number(position.get("speed_kmh"), 0, 1000)
            age = number(position.get("age_seconds", 0), 0, 3600)
        now = time.time()
        with self.connect() as db:
            db.execute("""
                INSERT INTO vehicles(id, last_seen) VALUES (?, ?)
                ON CONFLICT(id) DO UPDATE SET last_seen = excluded.last_seen
            """, (vehicle, now))
            if position is not None:
                db.execute("""
                    UPDATE vehicles SET lat=?, lon=?, speed=?, position_at=?
                    WHERE id=?
                """, (lat, lon, speed, now - age, vehicle))
            armed = db.execute(
                "SELECT parking_armed FROM vehicles WHERE id=?", (vehicle,)
            ).fetchone()[0]
        return {"parking_armed": bool(armed)}

    def event(self, data):
        vehicle = identifier(data.get("vehicle"))
        event_id = identifier(data.get("event_id"))
        acceleration = number(data.get("acceleration_g"), 0, 64)
        encoded = data.get("image_jpeg")
        image = None
        if encoded is not None:
            if not isinstance(encoded, str):
                raise ValueError("Invalid JPEG")
            try:
                image = base64.b64decode(encoded, validate=True)
            except (ValueError, binascii.Error) as exc:
                raise ValueError("Invalid JPEG") from exc
            if (len(image) > MAX_IMAGE or not image.startswith(b"\xff\xd8")
                    or not image.endswith(b"\xff\xd9")):
                raise ValueError("Invalid JPEG")
        with self.connect() as db:
            if not db.execute("SELECT id FROM vehicles WHERE id=?", (vehicle,)).fetchone():
                raise ValueError("Send telemetry before an event")
            db.execute("""
                INSERT INTO events(vehicle, event_id, received_at, acceleration_g, image)
                VALUES (?, ?, ?, ?, ?) ON CONFLICT(vehicle, event_id) DO NOTHING
            """, (vehicle, event_id, time.time(), acceleration, image))
            event = db.execute(
                "SELECT id FROM events WHERE vehicle=? AND event_id=?", (vehicle, event_id)
            ).fetchone()
        return {"id": event[0]}

    def vehicles(self):
        with self.connect() as db:
            rows = db.execute("SELECT * FROM vehicles ORDER BY id").fetchall()
        return [dict(row, parking_armed=bool(row["parking_armed"])) for row in rows]

    def arm(self, vehicle, armed):
        identifier(vehicle)
        if not isinstance(armed, bool):
            raise ValueError("parking_armed must be boolean")
        with self.connect() as db:
            cursor = db.execute(
                "UPDATE vehicles SET parking_armed=? WHERE id=?", (armed, vehicle)
            )
            if not cursor.rowcount:
                raise LookupError("Vehicle not found")
        return {"parking_armed": armed}

    def events(self, vehicle):
        identifier(vehicle)
        with self.connect() as db:
            rows = db.execute("""
                SELECT id, vehicle, event_id, received_at, acceleration_g,
                       image IS NOT NULL AS has_image
                FROM events WHERE vehicle=? ORDER BY id DESC LIMIT 50
            """, (vehicle,)).fetchall()
        return [dict(row, has_image=bool(row["has_image"])) for row in rows]

    def image(self, event):
        with self.connect() as db:
            row = db.execute("SELECT image FROM events WHERE id=?", (event,)).fetchone()
        if row is None or row[0] is None:
            raise LookupError("Image not found")
        return row[0]


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, format, *args):
        # Request URLs may contain identifiers; do not put them in logs.
        pass

    def reply(self, status, body, content_type="application/json"):
        if content_type == "application/json":
            body = json.dumps(body, allow_nan=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; script-src 'self' https://unpkg.com; "
                         "style-src 'self' 'unsafe-inline' https://unpkg.com; "
                         "img-src 'self' blob: data: https://tile.openstreetmap.org "
                         "https://unpkg.com/leaflet@1.9.4/dist/images/; "
                         "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        expected = "Bearer " + self.server.token
        actual = self.headers.get("Authorization", "")
        if not hmac.compare_digest(actual.encode(), expected.encode()):
            self.reply(401, {"error": "Unauthorized"})
            return False
        return True

    def do_GET(self):
        url = urlsplit(self.path)
        static = {"/": ("index.html", "text/html; charset=utf-8"),
                  "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                  "/style.css": ("style.css", "text/css; charset=utf-8")}
        if url.path in static:
            name, mime = static[url.path]
            self.reply(200, (ROOT / "web" / name).read_bytes(), mime)
            return
        if not self.authorized():
            return
        try:
            if url.path == "/api/vehicles":
                self.reply(200, {"vehicles": self.server.store.vehicles()})
            elif url.path == "/api/events":
                vehicle = parse_qs(url.query).get("vehicle", [""])[0]
                self.reply(200, {"events": self.server.store.events(vehicle)})
            elif re.fullmatch(r"/api/events/[0-9]+/image", url.path):
                self.reply(200, self.server.store.image(int(url.path.split("/")[3])),
                           "image/jpeg")
            else:
                self.reply(404, {"error": "Not found"})
        except ValueError as exc:
            self.reply(400, {"error": str(exc)})
        except LookupError as exc:
            self.reply(404, {"error": str(exc)})

    def do_POST(self):
        self.close_connection = True
        if not self.authorized():
            return
        try:
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("Chunked requests are not supported")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_BODY:
                self.reply(413, {"error": "Body must be between 1 and 300000 bytes"})
                return
            if self.headers.get_content_type() != "application/json":
                raise ValueError("Expected application/json")
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError("Incomplete request body")
            data = json.loads(body)
            if not isinstance(data, dict):
                raise ValueError("Expected an object")
            path = urlsplit(self.path).path
            if path == "/api/telemetry":
                self.reply(200, self.server.store.telemetry(data))
            elif path == "/api/events":
                self.reply(200, self.server.store.event(data))
            elif re.fullmatch(r"/api/vehicles/[A-Za-z0-9_-]{1,64}/parking", path):
                self.reply(200, self.server.store.arm(
                    path.split("/")[3], data.get("parking_armed")))
            else:
                self.reply(404, {"error": "Not found"})
        except (ValueError, UnicodeError, RecursionError) as exc:
            self.reply(400, {"error": str(exc)})
        except LookupError as exc:
            self.reply(404, {"error": str(exc)})
        except TimeoutError:
            self.reply(408, {"error": "Request body timeout"})


def make_server(address, database, token):
    if not token or len(token) < 32 or not token.isascii():
        raise ValueError("CARPAL_TOKEN must be at least 32 ASCII characters")
    server = ThreadingHTTPServer(address, Handler)
    server.store = Store(database)
    server.token = token
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--database", default=str(ROOT / "carpal.sqlite3"))
    args = parser.parse_args()
    server = make_server((args.host, args.port), args.database, os.environ.get("CARPAL_TOKEN"))
    print(f"Carpal listening on {args.host}:{server.server_port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
