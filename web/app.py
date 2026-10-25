import hmac
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
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

BRIDGE_JSON_BASENAME = os.path.basename(BRIDGE_JSON)
_BACKUP_NAME = re.compile(r'^bridge\.json\.bak\.\d{8}-\d{6}$')
BACKUP_KEEP = 5
RESTART_DEBOUNCE_SEC = 10
_restart_lock = threading.Lock()
_last_restart = None


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


STATUS_DIR = os.path.join(DATA_DIR, "status")


@app.route("/api/servers/status")
def servers_status():
    """Per-server live status from the files the bridge writes to
    <data dir>/status/<id>.json. Order follows bridge.json, else file names.
    A missing/unreadable file becomes an entry, never an HTTP error."""
    now = time.time()
    ids = []
    try:
        with open(BRIDGE_JSON) as f:
            for s in json.load(f).get("servers", []):
                sid = s.get("id") if isinstance(s, dict) else None
                if isinstance(sid, str) and sid and sid not in ids:
                    ids.append(sid)
    except (OSError, ValueError, AttributeError):
        ids = []
    if not ids:
        try:
            ids = sorted(n[:-5] for n in os.listdir(STATUS_DIR) if n.endswith(".json"))
        except OSError:
            ids = []
    servers = []
    for sid in ids:
        path = os.path.join(STATUS_DIR, sid + ".json")
        entry = {"server_id": sid}
        try:
            mtime = os.stat(path).st_mtime
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            if not isinstance(data, dict):
                raise ValueError("not a JSON object")
            entry = {**data, "server_id": sid, "age_sec": max(0, int(now - mtime))}
        except FileNotFoundError:
            entry["missing"] = True
        except (OSError, ValueError) as e:
            entry["error"] = str(e)[:120]
        servers.append(entry)
    return jsonify({"servers": servers,
                    "now": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))})


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


def _list_backups():
    """Backup file names of bridge.json, newest first (the timestamp sorts)."""
    try:
        names = [n for n in os.listdir(DATA_DIR) if _BACKUP_NAME.match(n)]
    except OSError:
        return []
    return sorted(names, reverse=True)


def _backup_bridge_config(new_content):
    """Copies the current bridge.json aside before it is overwritten; keeps the
    newest BACKUP_KEEP. No backup when nothing would be lost (missing file,
    unchanged content, or a same-second backup already exists) — a duplicate
    would push a real older state out of the kept window."""
    try:
        with open(BRIDGE_JSON) as f:
            if f.read() == new_content:
                return
    except FileNotFoundError:
        return
    name = BRIDGE_JSON_BASENAME + ".bak." + time.strftime("%Y%m%d-%H%M%S")
    dst = os.path.join(DATA_DIR, name)
    if not os.path.exists(dst):
        shutil.copy2(BRIDGE_JSON, dst)
        os.chmod(dst, 0o600)
    for old in _list_backups()[BACKUP_KEEP:]:
        os.unlink(os.path.join(DATA_DIR, old))


def _restart_bridge():
    """Returns the error text or None. A second restart within
    RESTART_DEBOUNCE_SEC is refused: each restart re-polls every myIO server."""
    global _last_restart
    with _restart_lock:
        now = time.monotonic()
        if _last_restart is not None and now - _last_restart < RESTART_DEBOUNCE_SEC:
            return f"az előző újraindítás óta nem telt el {RESTART_DEBOUNCE_SEC} mp"
        _last_restart = now
    result = subprocess.run(
        ["sudo", "systemctl", "restart", "myio-bridge"],
        capture_output=True, text=True
    )
    return None if result.returncode == 0 else (result.stderr.strip() or "restart hiba")


@app.route("/api/config/bridge/backups")
def config_bridge_backups():
    return jsonify({"ok": True, "backups": _list_backups()})


@app.route("/api/config/bridge/restore", methods=["POST"])
def config_bridge_restore():
    name = (request.get_json(silent=True) or {}).get("name", "")
    if not _BACKUP_NAME.match(name):
        return jsonify({"ok": False, "error": "érvénytelen mentésnév"}), 400
    try:
        with open(os.path.join(DATA_DIR, name)) as f:
            content = f.read()
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 404
    return _write_config(BRIDGE_JSON, content)


def _save_config(path):
    return _write_config(path, request.get_data(as_text=True),
                         restart=request.args.get("restart") == "1")


def _write_config(path, content, restart=False):
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
                _backup_bridge_config(content)
            os.replace(tmp_path, path)
        except BaseException:
            os.unlink(tmp_path)
            raise
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 500
    if restart:
        err = _restart_bridge()
        if err:
            return jsonify({"ok": False, "saved": True, "error": f"Mentve, de: {err}"}), 409
    return jsonify({"ok": True})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080)
