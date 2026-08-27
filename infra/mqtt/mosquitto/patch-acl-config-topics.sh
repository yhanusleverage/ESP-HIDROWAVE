#!/usr/bin/env bash
# ACL: Railway publica ec/config + ph/config; ESP lê esses tópicos.
# Lightsail: sudo bash patch-acl-config-topics.sh
set -euo pipefail
ACL_FILE="${ACL_FILE:-/var/lib/mosquitto/acl}"

if [[ ! -f "$ACL_FILE" ]]; then
  echo "ACL não encontrado: $ACL_FILE" >&2
  exit 1
fi

cp "$ACL_FILE" "${ACL_FILE}.bak.$(date +%Y%m%d%H%M%S)"

python3 - "$ACL_FILE" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
changed = False
if "topic write hidrowave/+/ec/config" not in text:
    needle = "topic write hidrowave/+/command"
    if needle not in text:
        raise SystemExit("user hidrowave command write não encontrado")
    text = text.replace(
        needle,
        needle + "\ntopic write hidrowave/+/ec/config\ntopic write hidrowave/+/ph/config",
        1,
    )
    changed = True
# ESP: após cada "topic read .../command" adicionar ec/ph config se faltar
import re
def add_reads(m):
    block = m.group(0)
    prefix = m.group(1)
    extra = ""
    if f"topic read {prefix}/ec/config" not in text[m.start():m.end()+200]:
        extra += f"\ntopic read {prefix}/ec/config"
        extra += f"\ntopic read {prefix}/ph/config"
    if extra and f"{prefix}/ec/config" not in block:
        return block + extra
    return block
new_text, n = re.subn(
    r"(topic read (hidrowave/[^/\s]+)/command)",
    lambda m: m.group(0)
    if f"{m.group(2)}/ec/config" in text
    else m.group(0) + f"\ntopic read {m.group(2)}/ec/config\ntopic read {m.group(2)}/ph/config",
    text,
)
if n:
    text = new_text
    changed = True
open(path, "w", encoding="utf-8").write(text)
print("ACL config topics atualizado." if changed or n else "ACL já tinha ec/ph config.")
PY

systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto
echo "Mosquitto recarregado."
