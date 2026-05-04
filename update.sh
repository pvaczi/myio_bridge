#!/bin/bash
set -e
REPO="pvaczi/myio_bridge"
[ -f /etc/myio-bridge.env ] && source /etc/myio-bridge.env

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
