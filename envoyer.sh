#!/bin/sh
# envoyer.sh - envoie un fichier a LocalSend pour Wii depuis le Mac (sans l'app LocalSend)
#
#   ./envoyer.sh 192.168.1.42                 -> envoie le .dol compile (mise a jour)
#   ./envoyer.sh 192.168.1.42 fichier.wad     -> envoie n'importe quel fichier
#   PORT=53318 ./envoyer.sh ...               -> si le port a ete change sur la Wii
#
# Un .dol de LocalSend pour Wii est reconnu par la Wii : elle propose
# "Installer la vX et redemarrer" (A sur la Wiimote).

set -e
IP="$1"
FILE="$2"
PORT="${PORT:-53317}"

if [ -z "$IP" ]; then
    echo "Usage : $0 <ip-de-la-wii> [fichier]" >&2
    exit 1
fi
if [ -z "$FILE" ]; then
    FILE="$(ls -t ./*.dol 2>/dev/null | head -n 1)"
    [ -n "$FILE" ] || { echo "Aucun .dol trouve : lance d'abord make" >&2; exit 1; }
fi
[ -f "$FILE" ] || { echo "Fichier introuvable : $FILE" >&2; exit 1; }

NAME="$(basename "$FILE")"
case "$NAME" in *.dol) NAME="localsend-wii-update.dol" ;; esac
SIZE="$(wc -c < "$FILE" | tr -d ' ')"
URL="http://$IP:$PORT/api/localsend/v2"

echo "Envoi de $FILE ($SIZE octets) vers $IP:$PORT..."
RESP="$(curl -sS --max-time 15 -X POST -H 'Content-Type: application/json' \
  --data "{\"info\":{\"alias\":\"Mac (envoyer.sh)\",\"version\":\"2.0\",\"deviceType\":\"desktop\",\"fingerprint\":\"envoyer-sh\",\"port\":$PORT,\"protocol\":\"http\",\"download\":false},\"files\":{\"f1\":{\"id\":\"f1\",\"fileName\":\"$NAME\",\"size\":$SIZE,\"fileType\":\"application/octet-stream\"}}}" \
  "$URL/prepare-upload")"

SID="$(printf '%s' "$RESP" | sed -n 's/.*"sessionId":"\([^"]*\)".*/\1/p')"
TOK="$(printf '%s' "$RESP" | sed -n 's/.*"f1":"\([^"]*\)".*/\1/p')"
if [ -z "$SID" ] || [ -z "$TOK" ]; then
    echo "La Wii a refuse l'envoi : $RESP" >&2
    exit 1
fi

curl -sS --fail --max-time 600 -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @"$FILE" "$URL/upload?sessionId=$SID&fileId=f1&token=$TOK" > /dev/null
echo "Envoye. Sur la Wii : choisis l'action avec la croix puis A."
