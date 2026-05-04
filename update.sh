#!/bin/bash
set -e
REPO="pvaczi/myio_bridge"
SRC_DIR="/opt/myio-src"
INSTALL_DIR="/opt/myio-bridge"
[ -f /etc/myio-bridge.env ] && source /etc/myio-bridge.env

# --- forrás frissítése ---
if [[ -d "$SRC_DIR/.git" ]]; then
    git -C "$SRC_DIR" pull
else
    git clone "https://${GITHUB_TOKEN}@github.com/${REPO}.git" "$SRC_DIR"
fi

# --- web UI frissítése ---
cp -r "$SRC_DIR/web" "$INSTALL_DIR/"
chown -R myio:myio "$INSTALL_DIR/web"
usermod -aG systemd-journal myio 2>/dev/null || true
systemctl restart myio-web

# --- bináris frissítése ---
ASSET_ID=$(curl -sf -H "Authorization: Bearer $GITHUB_TOKEN" \
  "https://api.github.com/repos/$REPO/releases/latest" \
  | grep -B3 '"myio-bridge-linux-x86_64\.tar\.gz"' \
  | grep '"id":' | tail -1 | grep -o '[0-9]*')

if [ -z "$ASSET_ID" ]; then
    echo "Hiba: nem sikerült meghatározni az asset ID-t" >&2
    exit 1
fi

curl -fL -H "Authorization: Bearer $GITHUB_TOKEN" \
     -H "Accept: application/octet-stream" \
     "https://api.github.com/repos/$REPO/releases/assets/$ASSET_ID" \
     | tar -xz -C /opt/myio-bridge/bin/

systemctl restart myio-bridge
echo "Frissítve: $(date)"
