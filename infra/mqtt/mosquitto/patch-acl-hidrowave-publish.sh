#!/usr/bin/env bash
# Garante user hidrowave com write em hidrowave/+/command (Railway / UI publish).
# Uso (Lightsail): sudo bash patch-acl-hidrowave-publish.sh

set -euo pipefail

ACL_FILE="${ACL_FILE:-/var/lib/mosquitto/acl}"

if [[ ! -f "$ACL_FILE" ]]; then
  echo "ACL não encontrado: $ACL_FILE" >&2
  exit 1
fi

if grep -q '^user hidrowave$' "$ACL_FILE"; then
  if grep -A8 '^user hidrowave$' "$ACL_FILE" | grep -qE 'topic write hidrowave/(\+/command|ESP32_[^/]+/#)'; then
    echo "ACL user hidrowave já permite publish command (wildcard ou device-specific)."
    exit 0
  fi
fi

cp "$ACL_FILE" "${ACL_FILE}.bak.$(date +%Y%m%d%H%M%S)"

python3 - "$ACL_FILE" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
block = """
# --- UI / Railway publish (Fase 3 comandos) ---
user hidrowave
topic write hidrowave/+/command
topic read hidrowave/+/#

"""
if "user hidrowave" in text and "topic write hidrowave/+/command" in text:
    print("hidrowave write command já presente (formato diferente).")
    sys.exit(0)
text = text.rstrip() + "\n" + block + "\n"
open(path, "w", encoding="utf-8").write(text)
print("ACL atualizado: user hidrowave + write command.")
PY

systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto
echo "Mosquitto recarregado."
echo "Verificar: sudo grep -A3 '^user hidrowave' $ACL_FILE"
