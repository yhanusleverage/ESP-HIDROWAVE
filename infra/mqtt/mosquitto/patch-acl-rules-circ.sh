#!/usr/bin/env bash
# ACL: Railway publica circ/config + rules/#; ESP lê esses tópicos (subscribe).
# Mosquitto: write …/# NÃO dá subscribe — precisa topic read explícito.
# Uso (Lightsail):
#   sed -i 's/\r$//' /tmp/patch-acl-rules-circ.sh && sudo bash /tmp/patch-acl-rules-circ.sh

set -euo pipefail

ACL_FILE="${ACL_FILE:-/var/lib/mosquitto/acl}"

if [[ ! -f "$ACL_FILE" ]]; then
  echo "ACL não encontrado: $ACL_FILE" >&2
  exit 1
fi

cp "$ACL_FILE" "${ACL_FILE}.bak.$(date +%Y%m%d%H%M%S)"

python3 - "$ACL_FILE" <<'PY'
import re
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
changed = False

# --- Railway / UI: write circ + rules ---
if "topic write hidrowave/+/circ/config" not in text:
    needle = "topic write hidrowave/+/ph/config"
    if needle not in text:
        needle = "topic write hidrowave/+/command"
    if needle not in text:
        raise SystemExit("user hidrowave write base não encontrado")
    text = text.replace(
        needle,
        needle
        + "\ntopic write hidrowave/+/circ/config"
        + "\ntopic write hidrowave/+/rules/#",
        1,
    )
    changed = True
elif "topic write hidrowave/+/rules/#" not in text:
    needle = "topic write hidrowave/+/circ/config"
    text = text.replace(needle, needle + "\ntopic write hidrowave/+/rules/#", 1)
    changed = True

# --- Cada master mqtt_ESP32_*: read circ/config + rules/# ---
def patch_master_block(m: re.Match) -> str:
    global changed
    block = m.group(0)
    device = m.group(1)
    circ = f"topic read hidrowave/{device}/circ/config"
    rules = f"topic read hidrowave/{device}/rules/#"
    extra = ""
    if circ not in block:
        extra += f"\n{circ}"
    if rules not in block:
        extra += f"\n{rules}"
    if not extra:
        return block
    # Inserir antes do write …/# do device
    write_line = f"topic write hidrowave/{device}/#"
    if write_line in block:
        block = block.replace(write_line, extra.lstrip("\n") + "\n" + write_line, 1)
    else:
        block = block.rstrip() + "\n" + extra.lstrip("\n") + "\n"
    changed = True
    return block

# Blocos: user mqtt_ESP32_… — group 1 = device_id (sem prefixo mqtt_)
# Só MULTILINE (não DOTALL): senão topic .+ engole vários masters
new_text, n = re.subn(
    r"(?m)^user mqtt_(ESP32_HIDRO_[0-9A-Fa-f]+)\n(?:topic .+\n)*",
    patch_master_block,
    text,
)
if n:
    text = new_text

open(path, "w", encoding="utf-8", newline="\n").write(text)
if changed:
    print("ACL atualizado: circ/config + rules/# (Railway write + ESP read).")
else:
    print("ACL já tinha circ/config + rules/# — nada a fazer.")
PY

systemctl reload mosquitto 2>/dev/null || systemctl restart mosquitto
echo "Mosquitto recarregado."
grep -n 'circ/config\|rules' "$ACL_FILE" || true
