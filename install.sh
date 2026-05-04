#!/bin/bash
set -euo pipefail

INSTALL_DIR="/opt/myio-bridge"
SERVICE_USER="myio"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

info()  { echo "[INFO]  $*"; }
ok()    { echo "[OK]    $*"; }
error() { echo "[ERROR] $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || error "Root jogosultság szükséges (sudo bash install.sh)."

# --- token ---
[[ -f /etc/myio-bridge.env ]] && source /etc/myio-bridge.env
if [[ -z "${GITHUB_TOKEN:-}" ]]; then
    read -rsp "GitHub PAT: " GITHUB_TOKEN; echo
fi
[[ -n "${GITHUB_TOKEN:-}" ]] || error "GITHUB_TOKEN nem lehet üres."

# --- deps ---
info "Függőségek telepítése..."
apt-get update -qq
apt-get install -y -qq curl python3-venv
ok "Függőségek kész."

# --- user ---
if ! id "$SERVICE_USER" &>/dev/null; then
    useradd -r -s /usr/sbin/nologin "$SERVICE_USER"
    ok "User létrehozva: $SERVICE_USER"
else
    info "User már létezik: $SERVICE_USER"
fi

# --- directories ---
mkdir -p "$INSTALL_DIR"/{bin,data}
ok "Könyvtárak: $INSTALL_DIR"

# --- env file ---
if [[ ! -f /etc/myio-bridge.env ]]; then
    printf 'GITHUB_TOKEN=%s\n' "$GITHUB_TOKEN" > /etc/myio-bridge.env
    chmod 600 /etc/myio-bridge.env
    ok "Env fájl létrehozva: /etc/myio-bridge.env"
else
    info "Env fájl már létezik, megtartva."
fi

# --- web UI ---
info "Web UI másolása..."
cp -r "$SCRIPT_DIR/web" "$INSTALL_DIR/"
if [[ ! -d "$INSTALL_DIR/venv" ]]; then
    python3 -m venv "$INSTALL_DIR/venv"
fi
"$INSTALL_DIR/venv/bin/pip" install -q flask
ok "Web UI kész."

# --- config fájlok (csak ha még nincs) ---
for f in bridge.json discovery_input.json; do
    src="$SCRIPT_DIR/data/$f"
    dst="$INSTALL_DIR/data/$f"
    if [[ -f "$src" && ! -f "$dst" ]]; then
        cp "$src" "$dst"
        ok "Config másolva: $f"
    elif [[ -f "$dst" ]]; then
        info "Config már létezik, megtartva: $f"
    fi
done

# --- systemd service-ek ---
info "Systemd service-ek telepítése..."
cp "$SCRIPT_DIR/myio-bridge.service" /etc/systemd/system/

cat > /etc/systemd/system/myio-web.service << 'EOF'
[Unit]
Description=myIO Bridge Web UI
After=network-online.target

[Service]
Type=simple
User=myio
WorkingDirectory=/opt/myio-bridge
ExecStart=/opt/myio-bridge/venv/bin/python web/app.py
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=myio-web

[Install]
WantedBy=multi-user.target
EOF

# --- sudoers ---
cat > /etc/sudoers.d/myio << 'EOF'
myio ALL=(ALL) NOPASSWD: /bin/systemctl start myio-bridge, /bin/systemctl stop myio-bridge, /bin/systemctl restart myio-bridge
EOF
chmod 440 /etc/sudoers.d/myio
ok "Sudoers kész."

# --- ownership ---
chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"

# --- service enable (start nélkül, az update.sh indítja) ---
systemctl daemon-reload
systemctl enable myio-bridge myio-web
ok "Service-ek engedélyezve."

# --- bináris letöltés (update.sh elvégzi a myio-bridge restart-ot is) ---
info "Bináris letöltése a GitHub release-ből..."
bash "$SCRIPT_DIR/update.sh"
ok "Binárisok letöltve, myio-bridge elindult."

# --- web UI indítása ---
systemctl start myio-web
ok "myio-web elindult."

echo
echo "================================================"
echo " Telepítés kész!"
echo " Web UI: http://$(hostname -I | awk '{print $1}'):8080"
echo "================================================"
echo
systemctl status myio-bridge myio-web --no-pager
