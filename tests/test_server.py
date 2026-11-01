import base64
import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest

from server import MAX_BODY, Store, make_server


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.database = Path(self.temp.name) / "test.sqlite3"
        self.token = "test-only-" + "x" * 32
        self.server = make_server(("127.0.0.1", 0), self.database, self.token)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def request(self, method, path, data=None, authenticated=True, raw=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        headers = {"Content-Type": "application/json"}
        if authenticated:
            headers["Authorization"] = "Bearer " + self.token
        body = raw if raw is not None else (json.dumps(data) if data is not None else None)
        connection.request(method, path, body, headers)
        response = connection.getresponse()
        payload = response.read()
        result = (response.status, payload, dict(response.getheaders()))
        connection.close()
        return result

    def telemetry(self, position=None):
        status, body, _ = self.request("POST", "/api/telemetry",
                                      {"vehicle": "car-1", "position": position})
        self.assertEqual(status, 200, body)
        return json.loads(body)

    def test_api_requires_authentication(self):
        for method, path, data in [
            ("GET", "/api/vehicles", None),
            ("GET", "/api/events?vehicle=car-1", None),
            ("GET", "/api/events/1/image", None),
            ("POST", "/api/telemetry", {"vehicle": "car-1"}),
            ("POST", "/api/vehicles/car-1/parking", {"parking_armed": True}),
            ("POST", "/api/events", {}),
        ]:
            with self.subTest(path=path):
                self.assertEqual(self.request(method, path, data, False)[0], 401)

    def test_tracking_and_parking_state(self):
        self.assertFalse(self.telemetry({"lat": 35.1, "lon": 139.2,
                                        "speed_kmh": 12.5})["parking_armed"])
        self.assertEqual(self.request("POST", "/api/vehicles/car-1/parking",
                                      {"parking_armed": True})[0], 200)
        self.assertTrue(self.telemetry()["parking_armed"])
        status, body, _ = self.request("GET", "/api/vehicles")
        self.assertEqual(status, 200)
        vehicle = json.loads(body)["vehicles"][0]
        self.assertEqual((vehicle["lat"], vehicle["lon"], vehicle["speed"]),
                         (35.1, 139.2, 12.5))
        self.assertTrue(vehicle["parking_armed"])
        self.assertGreaterEqual(vehicle["last_seen"], vehicle["position_at"])
        self.assertTrue(Store(self.database).vehicles()[0]["parking_armed"])
        self.request("POST", "/api/vehicles/car-1/parking", {"parking_armed": False})
        self.assertFalse(self.telemetry()["parking_armed"])

    def test_missing_fix_never_creates_zero_location(self):
        self.telemetry()
        vehicle = self.server.store.vehicles()[0]
        self.assertIsNone(vehicle["lat"])
        self.assertIsNone(vehicle["position_at"])

    def test_slow_cellular_upload_keeps_actual_fix_age(self):
        self.telemetry({"lat": 35, "lon": 139, "speed_kmh": 0, "age_seconds": 60})
        vehicle = self.server.store.vehicles()[0]
        self.assertAlmostEqual(vehicle["last_seen"] - vehicle["position_at"], 60)

    def test_coordinates_are_validated_atomically(self):
        for position in [
            {"lat": 91, "lon": 0, "speed_kmh": 0},
            {"lat": 0, "lon": -181, "speed_kmh": 0},
            {"lat": True, "lon": 0, "speed_kmh": 0},
            {"lat": 0, "lon": 0, "speed_kmh": -1},
            {"lat": float("nan"), "lon": 0, "speed_kmh": 0},
            {"lat": 0, "lon": 0, "speed_kmh": float("inf")},
            {"lat": 10 ** 400, "lon": 0, "speed_kmh": 0},
            {"lat": 0, "lon": 0, "speed_kmh": 0, "age_seconds": -1},
            [],
        ]:
            with self.subTest(position=position):
                self.assertEqual(self.request("POST", "/api/telemetry",
                    {"vehicle": "car-1", "position": position})[0], 400)
        self.assertEqual(self.server.store.vehicles(), [])

    def test_events_and_authenticated_jpeg_with_idempotent_retry(self):
        self.telemetry()
        jpeg = b"\xff\xd8test-jpeg\xff\xd9"
        data = {"vehicle": "car-1", "event_id": "boot-1",
                "acceleration_g": 1.2,
                "image_jpeg": base64.b64encode(jpeg).decode()}
        status, body, _ = self.request("POST", "/api/events", data)
        self.assertEqual(status, 200)
        event_id = json.loads(body)["id"]
        self.assertEqual(self.request("POST", "/api/events", data)[1], body)
        events = self.server.store.events("car-1")
        self.assertEqual(len(events), 1)
        self.assertTrue(events[0]["has_image"])
        self.assertNotIn("image", events[0])
        status, body, headers = self.request("GET", f"/api/events/{event_id}/image")
        self.assertEqual((status, body), (200, jpeg))
        self.assertEqual(headers["Content-Type"], "image/jpeg")
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        self.assertEqual(headers["Cache-Control"], "no-store")

    def test_event_without_camera(self):
        self.telemetry()
        self.assertEqual(self.request("POST", "/api/events", {
            "vehicle": "car-1", "event_id": "boot-2", "acceleration_g": 0.8
        })[0], 200)
        self.assertFalse(self.server.store.events("car-1")[0]["has_image"])
        self.assertEqual(self.request("GET", "/api/events/1/image")[0], 404)

    def test_full_scale_vector_shock_is_accepted(self):
        self.telemetry()
        self.assertEqual(self.request("POST", "/api/events", {
            "vehicle": "car-1", "event_id": "full-scale", "acceleration_g": 55.5
        })[0], 200)

    def test_invalid_event_data(self):
        self.telemetry()
        for image in ["not base64!", base64.b64encode(b"not jpeg").decode(),
                      base64.b64encode(b"\xff\xd8" + b"x" * 200_000 + b"\xff\xd9").decode(),
                      12]:
            self.assertEqual(self.request("POST", "/api/events", {
                "vehicle": "car-1", "event_id": "boot-3", "acceleration_g": 1,
                "image_jpeg": image
            })[0], 400)
        self.assertEqual(self.request("POST", "/api/events", {
            "vehicle": "unknown", "event_id": "boot-3", "acceleration_g": 1
        })[0], 400)
        self.assertEqual(self.server.store.events("car-1"), [])

    def test_invalid_requests(self):
        for raw in ["{", "[]", "null", '{"vehicle":"../car"}', '{"vehicle": 123}']:
            self.assertEqual(self.request("POST", "/api/telemetry", raw=raw)[0], 400)
        self.assertEqual(self.request("POST", "/api/telemetry",
                                      raw="[" * 2000 + "]" * 2000)[0], 400)
        self.assertEqual(self.request("POST", "/api/telemetry",
                                      raw="x" * (MAX_BODY + 1))[0], 413)
        self.assertEqual(self.request("POST", "/api/vehicles/missing/parking",
                                      {"parking_armed": True})[0], 404)
        self.telemetry()
        self.assertEqual(self.request("POST", "/api/vehicles/car-1/parking",
                                      {"parking_armed": "false"})[0], 400)

    def test_static_routes_do_not_expose_files(self):
        for path in ["/server.py", "/../server.py", "/carpal.sqlite3"]:
            self.assertEqual(self.request("GET", path)[0], 404)
        status, body, headers = self.request("GET", "/", authenticated=False)
        self.assertEqual(status, 200)
        self.assertIn(b"Carpal", body)
        self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])

    def test_event_list_is_bounded_and_isolated_by_vehicle(self):
        self.telemetry()
        for index in range(51):
            self.server.store.event({"vehicle": "car-1", "event_id": f"event-{index}",
                                     "acceleration_g": 1})
        events = self.server.store.events("car-1")
        self.assertEqual(len(events), 50)
        self.assertEqual(events[0]["event_id"], "event-50")
        self.assertEqual(self.server.store.events("another-car"), [])


if __name__ == "__main__":
    unittest.main()
