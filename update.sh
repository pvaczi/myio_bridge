#!/bin/bash
set -e
REPO="pvaczi/myio_bridge"
[ -f /etc/myio-bridge.env ] && source /etc/myio-bridge.env

ASSET_URL=$(curl -sf -H "Authorization: Bearer $GITHUB_TOKEN" \
  "https://api.github.com/repos/$REPO/releases/latest" \
  | grep -o '"browser_download_url": "[^"]*myio-bridge-linux-x86_64\.tar\.gz"' \
  | cut -d'"' -f4)

if [ -z "$ASSET_URL" ]; then
    echo "Hiba: nem sikerült meghatározni a letöltési URL-t" >&2
    exit 1
fi

curl -fL -H "Authorization: Bearer $GITHUB_TOKEN" \
     -H "Accept: application/octet-stream" \
     "$ASSET_URL" | tar -xz -C /opt/myio-bridge/bin/

systemctl restart myio-bridge
echo "Frissítve: $(date)"
