import hmac
import json
import os
import re
import subprocess
import tempfile
from functools import wraps
from flask import Flask, Response, jsonify, render_template, request

_JOURNAL_LINE = re.compile(r'^(\S+)\s+\S+\s+\S+:\s+(.*)')
_OWN_TS = re.compile(r'^\[?\d{4}-\d{2}-\d{2}')

app = Flask(__name__)

DATA_DIR = os.environ.get("MYIO_DATA_DIR", "/opt/myio-bridge/data")
BIN_DIR = os.environ.get("MYIO_BIN_DIR", "/opt/myio-bridge/bin")

BRIDGE_JSON = os.path.join(DATA_DIR, "bridge.json")
DISCOVERY_JSON = os.path.join(DATA_DIR, "discovery_input.json")
PASSWORD_FILE = os.path.join(DATA_DIR, "webui_password")


def _load_credentials():
    """The install script generates this file (chmod 600, owner myio) in
    "username:password" format. A line without a colon is treated as a bare
    password with the legacy username "admin". If the file is missing or
    unreadable, the UI runs unauthenticated — matching the pre-auth behaviour
    so a manual install without the file does not lock the user out."""
    try:
        with open(PASSWORD_FILE) as f:
            line = f.read().strip()
    except OSError:
        return None
    if not line:
        return None
    user, sep, password = line.partition(":")
    if not sep:
        return ("admin", line)
    if not user or not password:
        return None
    return (user, password)


_CREDENTIALS = _load_credentials()


@app.before_request
def _require_auth():
    if _CREDENTIALS is None:
        return None
    auth = request.authorization
    if auth and \
            hmac.compare_digest(auth.username or "", _CREDENTIALS[0]) and \
            hmac.compare_digest(auth.password or "", _CREDENTIALS[1]):
        return None
    return Response("Authentication required", 401,
                    {"WWW-Authenticate": 'Basic realm="myIO bridge"'})


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/version")
def version():
    bridge_bin = os.path.join(BIN_DIR, "bridge")
    try:
        result = subprocess.run(
            [bridge_bin, "--version"],
            capture_output=True, text=True, timeout=5
        )
        return jsonify({"ok": result.returncode == 0, "version": result.stdout.strip()})
    except Exception as e:
        return jsonify({"ok": False, "version": "?", "error": str(e)})


@app.route("/api/bridge/status")
def bridge_status():
    result = subprocess.run(
        ["systemctl", "is-active", "myio-bridge"],
        capture_output=True, text=True
    )
    return jsonify({"status": result.stdout.strip()})


@app.route("/api/bridge/start", methods=["POST"])
def bridge_start():
    result = subprocess.run(
        ["sudo", "systemctl", "start", "myio-bridge"],
        capture_output=True, text=True
    )
    ok = result.returncode == 0
    return jsonify({"ok": ok, "error": result.stderr.strip() if not ok else ""})


@app.route("/api/bridge/stop", methods=["POST"])
def bridge_stop():
    result = subprocess.run(
        ["sudo", "systemctl", "stop", "myio-bridge"],
        capture_output=True, text=True
    )
    ok = result.returncode == 0
    return jsonify({"ok": ok, "error": result.stderr.strip() if not ok else ""})


@app.route("/api/discovery/run", methods=["POST"])
def discovery_run():
    discovery_bin = os.path.join(BIN_DIR, "discovery")
    try:
        result = subprocess.run(
            [discovery_bin, DISCOVERY_JSON, BRIDGE_JSON],
            capture_output=True, text=True, timeout=60
        )
        output = result.stdout + result.stderr
        return jsonify({"ok": result.returncode == 0, "output": output})
    except subprocess.TimeoutExpired:
        return jsonify({"ok": False, "output": "Időtúllépés (60s)"}), 500


@app.route("/api/config/bridge")
def config_bridge_get():
    try:
        with open(BRIDGE_JSON) as f:
            return jsonify({"ok": True, "content": f.read()})
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.route("/api/config/bridge", methods=["POST"])
def config_bridge_set():
    return _save_config(BRIDGE_JSON)


@app.route("/api/config/discovery")
def config_discovery_get():
    try:
        with open(DISCOVERY_JSON) as f:
            return jsonify({"ok": True, "content": f.read()})
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.route("/api/config/discovery", methods=["POST"])
def config_discovery_set():
    return _save_config(DISCOVERY_JSON)


@app.route("/api/bridge/log")
def bridge_log():
    result = subprocess.run(
        ["journalctl", "-u", "myio-bridge", "-n", "200", "--no-pager", "--output=short-iso-precise"],
        capture_output=True, text=True
    )
    raw = result.stdout or result.stderr or "(nincs log)"
    lines = []
    for line in raw.splitlines():
        m = _JOURNAL_LINE.match(line)
        if m:
            ts, msg = m.group(1), m.group(2)
            lines.append(msg if _OWN_TS.match(msg) else f"{ts} {msg}")
        else:
            lines.append(line)
    return jsonify({"ok": True, "lines": "\n".join(lines)})


def _check_bridge_config(tmp_path):
    """Runs the bridge's own config parser on the candidate file, so a value the
    bridge would reject can never be saved. Returns the error text or None."""
    bridge_bin = os.path.join(BIN_DIR, "bridge")
    try:
        result = subprocess.run(
            [bridge_bin, "--check-config", tmp_path],
            capture_output=True, text=True, timeout=10
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"az ellenőrzés nem futott le: {e}"
    if result.returncode != 0:
        return (result.stderr or result.stdout).strip() or f"exit {result.returncode}"
    return None


def _save_config(path):
    content = request.get_data(as_text=True)
    try:
        json.loads(content)
    except (json.JSONDecodeError, ValueError) as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    try:
        # Atomic write: an interrupted save must never leave a truncated config
        # behind — the bridge would fail to start on it.
        fd, tmp_path = tempfile.mkstemp(dir=os.path.dirname(path), suffix=".tmp")
        try:
            with os.fdopen(fd, "w") as f:
                f.write(content)
                f.flush()
                os.fsync(f.fileno())
            os.chmod(tmp_path, 0o600)
            if path == BRIDGE_JSON:
                err = _check_bridge_config(tmp_path)
                if err:
                    os.unlink(tmp_path)
                    return jsonify({"ok": False, "error": f"Érvénytelen konfig: {err}"}), 400
            os.replace(tmp_path, path)
        except BaseException:
            os.unlink(tmp_path)
            raise
        return jsonify({"ok": True})
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 500


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080)
