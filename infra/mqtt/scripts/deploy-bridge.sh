#!/bin/bash
# Copia bridge para /opt/hidrowave-bridge na Lightsail (rodar na sua máquina com scp ou na VM após git pull)
set -e
DEST="${1:-/opt/hidrowave-bridge}"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)/bridge"

echo "Deploy bridge from $SCRIPT_DIR to $DEST"
sudo mkdir -p "$DEST"
sudo cp "$SCRIPT_DIR/index.js" "$SCRIPT_DIR/package.json" "$DEST/"
sudo cp "$SCRIPT_DIR/hidrowave-bridge.service" /etc/systemd/system/ 2>/dev/null || true
cd "$DEST"
sudo npm install --production
echo "Configure $DEST/.env then: sudo systemctl enable --now hidrowave-bridge"
