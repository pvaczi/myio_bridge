#!/bin/bash
set -euo pipefail
REPO="pvaczi/myio_bridge"
SRC_DIR="/opt/myio-src"
INSTALL_DIR="/opt/myio-bridge"
BIN_DIR="$INSTALL_DIR/bin"
ASSET_NAME="myio-bridge-linux-x86_64.tar.gz"
MIN_FREE_KB=20000   # ~20 MB szabad hely kell, mielőtt bármihez hozzányúlnánk

[ -f /etc/myio-bridge.env ] && source /etc/myio-bridge.env

# --- preflight: szabad lemezhely ---
# A csonka-letöltés csapda gyökéroka: ha megtelt a rootfs, a `curl | tar` egy
# fél binárist ír ki, ami SIGSEGV-vel száll el. Ezért ELŐBB ellenőrzünk, és ha
# kevés a hely, hozzá se kezdünk — a régi, működő bináris marad.
AVAIL_KB=$(df --output=avail "$INSTALL_DIR" | tail -1 | tr -d ' ')
if [ "$AVAIL_KB" -lt "$MIN_FREE_KB" ]; then
    echo "Hiba: kevés a szabad hely (${AVAIL_KB} KB < ${MIN_FREE_KB} KB). Frissítés megszakítva, a régi bináris marad." >&2
    echo "Tipp: 'journalctl --vacuum-size=8M' + 'apt-get clean', vagy növeld a rootfs-t (host: 'pct resize <vmid> rootfs +1G')." >&2
    exit 1
fi

# --- bridge állapot mentése ---
systemctl is-active --quiet myio-bridge && BRIDGE_WAS_RUNNING=1 || BRIDGE_WAS_RUNNING=0

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

# --- bináris letöltése IDEIGLENES helyre (soha nem közvetlenül élesbe) ---
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ASSET_ID=$(curl -sf -H "Authorization: Bearer $GITHUB_TOKEN" \
  "https://api.github.com/repos/$REPO/releases/latest" \
  | python3 -c "import sys,json; a=[x['id'] for x in json.load(sys.stdin)['assets'] if x['name']=='$ASSET_NAME']; print(a[0] if a else '')")

if [ -z "$ASSET_ID" ]; then
    echo "Hiba: nem sikerült meghatározni az asset ID-t ($ASSET_NAME)" >&2
    exit 1
fi

curl -fsSL -H "Authorization: Bearer $GITHUB_TOKEN" \
     -H "Accept: application/octet-stream" \
     "https://api.github.com/repos/$REPO/releases/assets/$ASSET_ID" \
     -o "$TMP/rel.tgz"

# --- integritás-ellenőrzés a csere ELŐTT ---
# 1) az archívum ép-e (tar -tzf), 2) a kicsomagolt bináris tényleg fut-e.
# Ez fogja el a csonka/hibás letöltést, mielőtt élesbe kerülne.
if ! tar -tzf "$TMP/rel.tgz" >/dev/null 2>&1; then
    echo "Hiba: a letöltött archívum sérült (tar -tzf megbukott). Régi bináris marad." >&2
    exit 1
fi
tar -xzf "$TMP/rel.tgz" -C "$TMP"
if [ ! -s "$TMP/bridge" ] || ! "$TMP/bridge" --version >/dev/null 2>&1; then
    echo "Hiba: a kicsomagolt bridge nem futtatható (csonka/hibás letöltés). Régi bináris marad." >&2
    exit 1
fi
NEWVER=$("$TMP/bridge" --version 2>/dev/null || echo '?')

# --- atomi csere backuppal ---
for f in bridge discovery; do
    [ -f "$BIN_DIR/$f" ] && cp -a "$BIN_DIR/$f" "$BIN_DIR/$f.bak"
    install -o myio -g myio -m 755 "$TMP/$f" "$BIN_DIR/$f"
done

# --- restart + rollback, ha az új bináris nem indul ---
if [ "$BRIDGE_WAS_RUNNING" -eq 1 ]; then
    if systemctl restart myio-bridge && sleep 2 && systemctl is-active --quiet myio-bridge; then
        echo "Bridge újraindítva: $NEWVER"
        rm -f "$BIN_DIR/bridge.bak" "$BIN_DIR/discovery.bak"
    else
        echo "Hiba: az új bridge nem indult el — visszaállítás a mentett binárisra." >&2
        for f in bridge discovery; do
            [ -f "$BIN_DIR/$f.bak" ] && mv "$BIN_DIR/$f.bak" "$BIN_DIR/$f"
        done
        systemctl restart myio-bridge || true
        exit 1
    fi
else
    echo "myio-bridge nem futott — frissítés után sem indítjuk el."
    rm -f "$BIN_DIR/bridge.bak" "$BIN_DIR/discovery.bak"
fi
echo "Frissítve: $NEWVER ($(date))"
