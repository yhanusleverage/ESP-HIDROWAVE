#!/usr/bin/env bash
# Garante ACL Mosquitto para bridge_internal ler dose + ph_dose.
# Uso (Lightsail): sudo bash patch-acl-dose-topics.sh

set -euo pipefail

ACL_FILE="${ACL_FILE:-/var/lib/mosquitto/acl}"
MARKER_BEGIN="# HIDROWAVE bridge dose topics (auto)"
MARKER_END="# END HIDROWAVE bridge dose topics"

if [[ ! -f "$ACL_FILE" ]]; then
  echo "ACL não encontrado: $ACL_FILE" >&2
  exit 1
fi

if grep -q 'topic read hidrowave/+/dose' "$ACL_FILE" && grep -q 'topic read hidrowave/+/ph_dose' "$ACL_FILE"; then
  echo "ACL já inclui dose e ph_dose — nada a fazer."
  exit 0
fi

cp "$ACL_FILE" "${ACL_FILE}.bak.$(date +%Y%m%d%H%M%S)"

python3 - "$ACL_FILE" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
block = """
# HIDROWAVE bridge dose topics (auto)
topic read hidrowave/+/dose
topic read hidrowave/+/ph_dose
# END HIDROWAVE bridge dose topics
"""
needle = "user bridge_internal"
if needle not in text:
    raise SystemExit("user bridge_internal não encontrado no ACL")
if "topic read hidrowave/+/dose" in text:
    raise SystemExit("dose já presente (parcial)")
idx = text.index(needle)
# inserir após bloco bridge_internal (após próxima linha user ou fim)
rest = text[idx:]
next_user = rest.find("\nuser ", 1)
insert_at = idx + (next_user if next_user != -1 else len(rest))
new_text = text[:insert_at].rstrip() + "\n" + block + text[insert_at:]
open(path, "w", encoding="utf-8").write(new_text)
print("ACL atualizado com dose + ph_dose.")
PY

systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto
echo "Mosquitto recarregado."
