#!/bin/bash
set -euo pipefail
REPO="pvaczi/myio_bridge"
SRC_DIR="/opt/myio-src"
INSTALL_DIR="/opt/myio-bridge"
BIN_DIR="$INSTALL_DIR/bin"
ASSET_NAME="myio-bridge-linux-x86_64.tar.gz"
MIN_FREE_KB=20000   # ~20 MB szabad hely kell, mielőtt bármihez hozzányúlnánk

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
# A repó publikus: a clone/pull és a release-letöltés token nélkül megy.
if [[ -d "$SRC_DIR/.git" ]]; then
    # Korábbi telepítések tokenes URL-jének tisztítása.
    git -C "$SRC_DIR" remote set-url origin "https://github.com/${REPO}.git"
    git -C "$SRC_DIR" pull
else
    git clone "https://github.com/${REPO}.git" "$SRC_DIR"
fi

# --- web UI frissítése ---
cp -r "$SRC_DIR/web" "$INSTALL_DIR/"
chown -R myio:myio "$INSTALL_DIR/web"
usermod -aG systemd-journal myio 2>/dev/null || true
systemctl restart myio-web

# --- bináris letöltése IDEIGLENES helyre (soha nem közvetlenül élesbe) ---
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A letöltés/kicsomagolás a TMP partícióján történik (tipikusan /tmp, ami lehet
# külön mountpoint) — ott is kell hely, nem csak az INSTALL_DIR-en.
TMP_AVAIL_KB=$(df --output=avail "$TMP" | tail -1 | tr -d ' ')
if [ "$TMP_AVAIL_KB" -lt "$MIN_FREE_KB" ]; then
    echo "Hiba: kevés a szabad hely a temp könyvtárban (${TMP_AVAIL_KB} KB < ${MIN_FREE_KB} KB)." >&2
    exit 1
fi

RELEASE_INFO=$(curl -sf \
  "https://api.github.com/repos/$REPO/releases/latest" \
  | python3 -c "
import sys, json
a = [(x['id'], x.get('digest') or '') for x in json.load(sys.stdin)['assets']
     if x['name'] == '$ASSET_NAME']
print(a[0][0] if a else '')
print(a[0][1] if a else '')")
ASSET_ID=$(echo "$RELEASE_INFO" | sed -n 1p)
ASSET_DIGEST=$(echo "$RELEASE_INFO" | sed -n 2p)

if [ -z "$ASSET_ID" ]; then
    echo "Hiba: nem sikerült meghatározni az asset ID-t ($ASSET_NAME)" >&2
    exit 1
fi

curl -fsSL -H "Accept: application/octet-stream" \
     "https://api.github.com/repos/$REPO/releases/assets/$ASSET_ID" \
     -o "$TMP/rel.tgz"

# --- integritás-ellenőrzés a csere ELŐTT ---
# 0) sha256 összevetés a GitHub API digestjével (ha az API adja),
# 1) az archívum ép-e (tar -tzf), 2) a kicsomagolt bináris tényleg fut-e.
# Ez fogja el a csonka/hibás letöltést, mielőtt élesbe kerülne.
if [ -n "$ASSET_DIGEST" ]; then
    ACTUAL_DIGEST="sha256:$(sha256sum "$TMP/rel.tgz" | cut -d' ' -f1)"
    if [ "$ACTUAL_DIGEST" != "$ASSET_DIGEST" ]; then
        echo "Hiba: sha256 eltérés (várt: $ASSET_DIGEST, kapott: $ACTUAL_DIGEST). Régi bináris marad." >&2
        exit 1
    fi
fi
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
# A set -e miatt az install-hibát explicit kell kezelni, különben a script úgy
# lép ki, hogy törött bináris marad élesben és a rollback sosem fut le.
rollback() {
    for f in bridge discovery; do
        [ -f "$BIN_DIR/$f.bak" ] && mv -f "$BIN_DIR/$f.bak" "$BIN_DIR/$f"
    done
}
INSTALL_OK=1
for f in bridge discovery; do
    [ -f "$BIN_DIR/$f" ] && cp -a "$BIN_DIR/$f" "$BIN_DIR/$f.bak"
    if ! install -o myio -g myio -m 755 "$TMP/$f" "$BIN_DIR/$f"; then
        INSTALL_OK=0
        break
    fi
done
if [ "$INSTALL_OK" -ne 1 ]; then
    echo "Hiba: a bináris cseréje megszakadt — visszaállítás a mentett binárisra." >&2
    rollback
    exit 1
fi

# --- unit fájlok szinkronizálása (a restart ELŐTT) ---
# Az install.sh ugyanezeket másolja (templating nélkül), a repó a forrás.
# A régi unit a $BIN_DIR-be kerül mentésre: rollbacknél a régi bináris a régi
# unittal indul (egy Type=notify unit alatt a READY-t nem küldő bináris
# start-timeoutba futna).
UNITS_CHANGED=0
CHANGED_UNITS=""
for u in myio-bridge.service myio-web.service; do
    if ! cmp -s "$SRC_DIR/$u" "/etc/systemd/system/$u"; then
        [ -f "/etc/systemd/system/$u" ] && cp -a "/etc/systemd/system/$u" "$BIN_DIR/$u.bak"
        cp "$SRC_DIR/$u" "/etc/systemd/system/$u"
        echo "Unit frissítve: $u"
        UNITS_CHANGED=1
        CHANGED_UNITS="$CHANGED_UNITS $u"
    fi
done
if [ "$UNITS_CHANGED" -eq 1 ]; then
    systemctl daemon-reload
fi
rollback_units() {
    [ "$UNITS_CHANGED" -eq 1 ] || return 0
    for u in $CHANGED_UNITS; do
        [ -f "$BIN_DIR/$u.bak" ] && mv -f "$BIN_DIR/$u.bak" "/etc/systemd/system/$u"
    done
    systemctl daemon-reload
}

# --- restart + rollback, ha az új bináris nem indul ---
# A .bak fájlokat sikeres frissítés után is MEGTARTJUK (last-known-good) —
# a következő frissítés úgyis felülírja őket.
if [ "$BRIDGE_WAS_RUNNING" -eq 1 ]; then
    # Kétlépcsős health-check: az is-active közvetlenül restart után akkor is
    # igaz, ha a processz pár másodperccel később hal el.
    if systemctl restart myio-bridge && sleep 3 && systemctl is-active --quiet myio-bridge \
       && sleep 5 && systemctl is-active --quiet myio-bridge; then
        echo "Bridge újraindítva: $NEWVER (backup: $BIN_DIR/*.bak)"
    else
        echo "Hiba: az új bridge nem indult el — visszaállítás a mentett binárisra." >&2
        rollback
        rollback_units
        systemctl restart myio-bridge || true
        exit 1
    fi
else
    echo "myio-bridge nem futott — frissítés után sem indítjuk el."
fi
echo "Frissítve: $NEWVER ($(date))"
