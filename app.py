import os
import sqlite3
from datetime import datetime

import requests as req_lib
from flask import Flask, render_template, jsonify, request, send_file, Response, redirect
from flask_socketio import SocketIO

app = Flask(__name__)
app.config["SECRET_KEY"] = os.urandom(24).hex()
socketio = SocketIO(app, cors_allowed_origins="*")

DB_PATH = os.path.join(os.path.dirname(__file__), "mons_iot.db")
CAMERA_DIR = os.path.join(os.path.dirname(__file__), "camera_captures")
CAMERA_STATE_FILE = os.path.join(os.path.dirname(__file__), "camera_state.json")
CERT_FILE = os.path.join(os.path.dirname(__file__), "cert.pem")
KEY_FILE = os.path.join(os.path.dirname(__file__), "key.pem")
os.makedirs(CAMERA_DIR, exist_ok=True)

# In-memory camera state (restored from disk if available)
camera_info = {"ip": None, "stream_url": None, "last_capture": None, "photo_count": 0}

import json

def save_camera_state():
    with open(CAMERA_STATE_FILE, "w") as f:
        json.dump(camera_info, f)

def load_camera_state():
    if os.path.exists(CAMERA_STATE_FILE):
        try:
            with open(CAMERA_STATE_FILE, "r") as f:
                data = json.load(f)
                camera_info.update(data)
        except Exception:
            pass

load_camera_state()


def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS sensor_readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_type TEXT    NOT NULL DEFAULT 'unknown',
            soil_moisture  INTEGER NOT NULL,
            soil_percent   INTEGER NOT NULL DEFAULT 0,
            temperature    REAL    NOT NULL,
            humidity       REAL    NOT NULL,
            ph_value       REAL    NOT NULL DEFAULT 0,
            ph_raw         INTEGER NOT NULL DEFAULT 0,
            pump_status    INTEGER NOT NULL DEFAULT 0,
            created_at     TEXT    NOT NULL
        )
    """)
    # Add device_type column to existing tables
    try:
        conn.execute("ALTER TABLE sensor_readings ADD COLUMN device_type TEXT NOT NULL DEFAULT 'unknown'")
    except sqlite3.OperationalError:
        pass  # column already exists
    # Add pH columns to existing tables that lack them
    try:
        conn.execute("ALTER TABLE sensor_readings ADD COLUMN ph_value REAL NOT NULL DEFAULT 0")
    except sqlite3.OperationalError:
        pass  # column already exists
    try:
        conn.execute("ALTER TABLE sensor_readings ADD COLUMN ph_raw INTEGER NOT NULL DEFAULT 0")
    except sqlite3.OperationalError:
        pass  # column already exists
    try:
        conn.execute("ALTER TABLE sensor_readings ADD COLUMN soil_percent INTEGER NOT NULL DEFAULT 0")
    except sqlite3.OperationalError:
        pass  # column already exists
    conn.commit()
    conn.close()


# ─── Pages ────────────────────────────────────────────────────────────

@app.route("/")
def dashboard():
    return render_template("dashboard.html")


# ─── API: receive data from ESP8266/ESP32 ──────────────────────────────────

@app.route("/api/sensor-data", methods=["POST"])
def receive_sensor_data():
    data = request.get_json(force=True)

    soil = data.get("soil_moisture")
    soil_pct = data.get("soil_percent", 0)
    temp = data.get("temperature")
    hum = data.get("humidity")
    ph = data.get("ph_value", 0)
    ph_raw = data.get("ph_raw", 0)
    pump = 1 if data.get("pump_status") else 0

    if soil is None or temp is None or hum is None:
        return jsonify({"error": "Missing fields"}), 400

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    conn = get_db()
    conn.execute(
        "INSERT INTO sensor_readings (soil_moisture, soil_percent, temperature, humidity, ph_value, ph_raw, pump_status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        (soil, soil_pct, temp, hum, ph, ph_raw, pump, now),
    )
    conn.commit()
    conn.close()

    payload = {
        "soil_moisture": soil,
        "soil_percent": soil_pct,
        "temperature": temp,
        "humidity": hum,
        "ph_value": ph,
        "ph_raw": ph_raw,
        "pump_status": bool(pump),
        "created_at": now,
    }
    socketio.emit("new_reading", payload)

    return jsonify({"status": "ok"}), 201


@app.route("/api/sensor-data/esp32", methods=["POST"])
def receive_esp32_data():
    data = request.get_json(force=True)

    temp = data.get("temperature")
    hum = data.get("humidity")
    ph = data.get("ph_value", 0)
    ph_raw = data.get("ph_raw", 0)

    if temp is None or hum is None:
        return jsonify({"error": "Missing temperature or humidity"}), 400

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    conn = get_db()
    # Store ESP32 data with device_type marker
    conn.execute(
        "INSERT INTO sensor_readings (device_type, soil_moisture, soil_percent, temperature, humidity, ph_value, ph_raw, pump_status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ('esp32', 0, 0, temp, hum, ph, ph_raw, 0, now),
    )
    conn.commit()
    conn.close()

    payload = {
        "device": "esp32",
        "temperature": temp,
        "humidity": hum,
        "ph_value": ph,
        "ph_raw": ph_raw,
        "created_at": now,
    }
    socketio.emit("esp32_reading", payload)

    return jsonify({"status": "ok"}), 201


# ─── API: NodeMCU Sensor Data (Soil Moisture) ──────────────────────────────────

@app.route("/api/sensor-data/nodemcu", methods=["POST"])
def receive_nodemcu_data():
    data = request.get_json(force=True)

    soil = data.get("soil_moisture")
    soil_pct = data.get("soil_percent", 0)
    pump = 1 if data.get("pump_status") else 0

    if soil is None:
        return jsonify({"error": "Missing soil_moisture"}), 400

    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    conn = get_db()
    # Store NodeMCU data with device_type marker
    conn.execute(
        "INSERT INTO sensor_readings (device_type, soil_moisture, soil_percent, temperature, humidity, ph_value, ph_raw, pump_status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ('nodemcu', soil, soil_pct, 0, 0, 0, 0, pump, now),
    )
    conn.commit()
    conn.close()

    payload = {
        "device": "nodemcu",
        "soil_moisture": soil,
        "soil_percent": soil_pct,
        "pump_status": bool(pump),
        "created_at": now,
    }
    socketio.emit("nodemcu_reading", payload)

    return jsonify({"status": "ok"}), 201


# ─── API: fetch history ──────────────────────────────────────────────

@app.route("/api/readings")
def get_readings():
    limit = request.args.get("limit", 100, type=int)
    limit = min(limit, 1000)

    conn = get_db()
    rows = conn.execute(
        "SELECT * FROM sensor_readings ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()

    readings = [dict(r) for r in rows]
    for r in readings:
        r["pump_status"] = bool(r["pump_status"])
    readings.reverse()
    return jsonify(readings)


@app.route("/api/readings/latest")
def get_latest():
    conn = get_db()
    row = conn.execute(
        "SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 1"
    ).fetchone()
    conn.close()

    if row:
        return jsonify(dict(row))
    return jsonify(None)


# ─── API: Camera endpoints ────────────────────────────────────────────

# ─── API: Weather (Open-Meteo, no key needed) ────────────────────────

WEATHER_CACHE = {"data": None, "ts": 0}
WEATHER_TTL = 300  # seconds

WMO_CODES = {
    0: "Clear sky", 1: "Mainly clear", 2: "Partly cloudy", 3: "Overcast",
    45: "Foggy", 48: "Rime fog", 51: "Light drizzle", 53: "Drizzle",
    55: "Dense drizzle", 61: "Light rain", 63: "Rain", 65: "Heavy rain",
    71: "Light snow", 73: "Snow", 75: "Heavy snow", 80: "Light showers",
    81: "Showers", 82: "Heavy showers", 95: "Thunderstorm",
    96: "Thunderstorm w/ hail", 99: "Severe thunderstorm",
}

@app.route("/api/weather")
def get_weather():
    import time
    now = time.time()
    if WEATHER_CACHE["data"] and (now - WEATHER_CACHE["ts"]) < WEATHER_TTL:
        return jsonify(WEATHER_CACHE["data"])
    try:
        # Butuan City, Philippines
        r = req_lib.get(
            "https://api.open-meteo.com/v1/forecast",
            params={
                "latitude": 8.9475,
                "longitude": 125.5406,
                "current_weather": "true",
                "current": "temperature_2m,relative_humidity_2m,weather_code",
            },
            timeout=8,
        )
        j = r.json()
        current = j.get("current", {})
        cw = j.get("current_weather", {})
        temp = current.get("temperature_2m", cw.get("temperature"))
        hum = current.get("relative_humidity_2m")
        code = current.get("weather_code", cw.get("weathercode", 0))
        desc = WMO_CODES.get(code, "Unknown")
        result = {"temperature": temp, "humidity": hum, "description": desc, "weather_code": code}
        WEATHER_CACHE["data"] = result
        WEATHER_CACHE["ts"] = now
        return jsonify(result)
    except Exception as e:
        return jsonify({"error": str(e)}), 502


@app.route("/api/camera/register", methods=["POST"])
def camera_register():
    data = request.get_json(force=True)
    camera_info["ip"] = data.get("ip")
    camera_info["stream_url"] = data.get("stream_url")
    save_camera_state()
    print(f"ESP32-CAM registered: {camera_info['ip']}")
    socketio.emit("camera_status", {
        "ip": camera_info["ip"],
        "stream_url": camera_info["stream_url"],
        "status": "online",
    })
    return jsonify({"status": "ok"}), 201


@app.route("/api/camera/upload", methods=["POST"])
def camera_upload():
    if request.content_type != "image/jpeg":
        return jsonify({"error": "Expected image/jpeg"}), 400

    image_data = request.get_data()
    if not image_data or len(image_data) < 100:
        return jsonify({"error": "No image data"}), 400

    now = datetime.now()
    filename = now.strftime("%Y%m%d_%H%M%S") + ".jpg"
    filepath = os.path.join(CAMERA_DIR, filename)

    with open(filepath, "wb") as f:
        f.write(image_data)

    # Also keep a "latest.jpg" for quick access
    latest_path = os.path.join(CAMERA_DIR, "latest.jpg")
    with open(latest_path, "wb") as f:
        f.write(image_data)

    camera_info["last_capture"] = now.strftime("%Y-%m-%d %H:%M:%S")
    camera_info["photo_count"] = len([
        f for f in os.listdir(CAMERA_DIR)
        if f.endswith(".jpg") and f != "latest.jpg"
    ])

    socketio.emit("camera_capture", {
        "filename": filename,
        "timestamp": camera_info["last_capture"],
        "photo_count": camera_info["photo_count"],
    })

    save_camera_state()

    return jsonify({"status": "ok", "filename": filename}), 201


@app.route("/api/camera/latest")
def camera_latest_image():
    latest_path = os.path.join(CAMERA_DIR, "latest.jpg")
    if os.path.exists(latest_path):
        return send_file(latest_path, mimetype="image/jpeg")
    return jsonify({"error": "No captures yet"}), 404


@app.route("/api/camera/info")
def camera_get_info():
    return jsonify({
        "ip": camera_info["ip"],
        "stream_url": camera_info["stream_url"],
        "last_capture": camera_info["last_capture"],
        "photo_count": camera_info["photo_count"],
        "online": camera_info["ip"] is not None,
    })


@app.route("/api/camera/proxy-stream")
def camera_proxy_stream():
    """Proxy the MJPEG stream from ESP32-CAM through Flask for smooth playback."""
    cam_ip = camera_info.get("ip")
    if not cam_ip:
        return jsonify({"error": "Camera not registered"}), 503

    def generate():
        try:
            stream = req_lib.get(
                f"http://{cam_ip}:81/stream",
                stream=True,
                timeout=10,
            )
            for chunk in stream.iter_content(chunk_size=4096):
                yield chunk
        except Exception:
            pass

    return Response(
        generate(),
        mimetype="multipart/x-mixed-replace;boundary=123456789000000000000987654321",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Access-Control-Allow-Origin": "*",
        },
    )


@app.route("/api/camera/proxy-capture")
def camera_proxy_capture():
    """Proxy a single JPEG snapshot from ESP32-CAM through Flask."""
    cam_ip = camera_info.get("ip")
    if not cam_ip:
        return jsonify({"error": "Camera not registered"}), 503

    try:
        r = req_lib.get(f"http://{cam_ip}/capture", timeout=5)
        if r.status_code == 200:
            return Response(r.content, mimetype="image/jpeg",
                            headers={"Cache-Control": "no-cache"})
        return jsonify({"error": "Capture failed"}), 502
    except Exception as e:
        return jsonify({"error": str(e)}), 502


# ─── Phone Camera ─────────────────────────────────────────────────────

@app.route("/phone-cam")
def phone_cam_page():
    """Redirect HTTP to HTTPS:5001 for camera access, or serve page if already on HTTPS."""
    if not request.is_secure:
        https_url = f"https://{request.host.split(':')[0]}:5001/phone-cam"
        return redirect(https_url)
    return render_template("phone_cam.html")


@app.route("/api/phone-cam/frame", methods=["POST"])
def phone_cam_frame():
    """Receive a JPEG frame from the phone and broadcast to dashboard viewers."""
    if not request.data or len(request.data) < 100:
        return jsonify({"error": "No frame data"}), 400
    import base64
    b64 = base64.b64encode(request.data).decode("ascii")
    socketio.emit("phone_cam_frame", {"data": b64})
    return jsonify({"status": "ok"}), 200


# ─── SocketIO events ─────────────────────────────────────────────────

@socketio.on("connect")
def handle_connect():
    print("Client connected")


@socketio.on("disconnect")
def handle_disconnect():
    print("Client disconnected")


@socketio.on("phone_cam_frame")
def handle_phone_cam_frame(data):
    """Relay phone camera frame from phone client to dashboard clients."""
    socketio.emit("phone_cam_frame", data, include_self=False)


# ─── Main ────────────────────────────────────────────────────────────

def generate_self_signed_cert():
    """Generate a self-signed SSL cert if one doesn't exist yet."""
    if os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE):
        return
    from OpenSSL import crypto
    key = crypto.PKey()
    key.generate_key(crypto.TYPE_RSA, 2048)
    cert = crypto.X509()
    cert.get_subject().CN = "Mons IoT"
    cert.set_serial_number(1000)
    cert.gmtime_adj_notBefore(0)
    cert.gmtime_adj_notAfter(365 * 24 * 60 * 60)  # 1 year
    cert.set_issuer(cert.get_subject())
    cert.set_pubkey(key)
    cert.sign(key, "sha256")
    with open(CERT_FILE, "wb") as f:
        f.write(crypto.dump_certificate(crypto.FILETYPE_PEM, cert))
    with open(KEY_FILE, "wb") as f:
        f.write(crypto.dump_privatekey(crypto.FILETYPE_PEM, key))
    print("Generated self-signed SSL certificate")


if __name__ == "__main__":
    init_db()
    generate_self_signed_cert()

    # Start HTTPS server on port 5001 for phone camera (getUserMedia needs secure context)
    import eventlet
    import eventlet.wsgi

    def _https_server():
        ssl_sock = eventlet.wrap_ssl(
            eventlet.listen(('0.0.0.0', 5001)),
            certfile=CERT_FILE, keyfile=KEY_FILE, server_side=True,
        )
        eventlet.wsgi.server(ssl_sock, app, log_output=False)

    eventlet.spawn(_https_server)
    print("Starting Mons IoT Dashboard on http://0.0.0.0:5000")
    print("Phone camera HTTPS available on https://0.0.0.0:5001/phone-cam")
    socketio.run(
        app, host="0.0.0.0", port=5000, debug=True,
        use_reloader=False, allow_unsafe_werkzeug=True,
    )
