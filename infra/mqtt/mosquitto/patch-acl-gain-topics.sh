#!/usr/bin/env bash
# Garante ACL Mosquitto: bridge_internal lê ec_gain + ph_gain (K aprendido ESP → bridge).
# Uso (Lightsail): sed -i 's/\r$//' patch-acl-gain-topics.sh && sudo bash patch-acl-gain-topics.sh

set -euo pipefail

ACL_FILE="${ACL_FILE:-/var/lib/mosquitto/acl}"

if [[ ! -f "$ACL_FILE" ]]; then
  echo "ACL não encontrado: $ACL_FILE" >&2
  exit 1
fi

if grep -q 'topic read hidrowave/+/ec_gain' "$ACL_FILE" && grep -q 'topic read hidrowave/+/ph_gain' "$ACL_FILE"; then
  echo "ACL já inclui ec_gain e ph_gain — nada a fazer."
  exit 0
fi

cp "$ACL_FILE" "${ACL_FILE}.bak.$(date +%Y%m%d%H%M%S)"

python3 - "$ACL_FILE" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
block = """
# HIDROWAVE bridge gain topics (auto) — ec_config_view / ph_config_view k_*
topic read hidrowave/+/ec_gain
topic read hidrowave/+/ph_gain
# END HIDROWAVE bridge gain topics
"""
needle = "user bridge_internal"
if needle not in text:
    raise SystemExit("user bridge_internal não encontrado no ACL")
if "topic read hidrowave/+/ec_gain" in text:
    raise SystemExit("ec_gain já presente (parcial)")
idx = text.index(needle)
rest = text[idx:]
next_user = rest.find("\nuser ", 1)
insert_at = idx + (next_user if next_user != -1 else len(rest))
new_text = text[:insert_at].rstrip() + "\n" + block + text[insert_at:]
open(path, "w", encoding="utf-8").write(new_text)
print("ACL atualizado com ec_gain + ph_gain.")
PY

systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto
echo "Mosquitto recarregado."
