# ESP-HIDROWAVE

Firmware **ESP32 Master** da hidroponia HIDROWAVE (PlatformIO / Arduino).

O dashboard web **não** está neste repositório. UI: **[HIDROWAVE](https://github.com/yhanusleverage/HIDROWAVE)**. Relés remotos: **[ESPNOW-SLAVE-TASK](https://github.com/yhanusleverage/ESPNOW-SLAVE-TASK)** (ESP-NOW).

[![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)](https://docs.platformio.org/)
[![MQTT](https://img.shields.io/badge/MQTT-Mosquitto%20Lightsail-blue.svg)](docs/mqtt/README.md)

## O que este repo faz

| Peça | Função |
|------|--------|
| `HydroSystemCore` + `HydroControl` | Loop Master: sensores, Auto EC/pH, diluição, relés locais (PCF8574) |
| MQTT (`ENABLE_MQTT` via `secrets.ini`) | Telemetria, heartbeat, status, dose EC/pH, comandos |
| Bridge Node (`infra/mqtt/bridge/`) | Mosquitto → Supabase (`hidrowave-bridge.service` na VM) |
| ESP-NOW | Relés no slave (não MQTT no slave) |
| HTTPS Supabase | Fallback / config / Relés se MQTT cair |

**Não** misturar frontend Next.js aqui. Clone **HIDROWAVE** à parte.

## Arquitetura (estado real)

```
ESP32 Master ──WiFi──► Mosquitto (Lightsail 1 GB)
       │                      │
       │                      ▼
       │              hidrowave-bridge (Node)
       │                      │
       │                      ▼
       │                 Supabase
       │                      ▲
       │                      │
       └── ESP-NOW ──► Slave relés
                              │
Next.js (HIDROWAVE / Railway) ─┴── MQTT publish comandos + REST
```

Broker atual (IP estática): ver `secrets.ini` → `mqtt_host` (não commitar senhas). Análise de serviço: [docs/mqtt/11_ANALISE_BROKER_JOURNAL.md](docs/mqtt/11_ANALISE_BROKER_JOURNAL.md).

## Hardware (Master)

O mapa canónico está em `include/Config.h` / `docs/engineering/` — **não** copiar pinouts de READMEs antigos.

- Relés Master: PCF8574 I2C (SDA 21, SCL 22)
- EC: ADC analógico + janela
- pH: Modbus RS485 (`USE_PH_MODBUS_SENSOR`)
- Níveis: banco discreto (não simular em produção: `HIDRO_SIMULATE_WATER_LEVELS=0`)
- Fluxo diluição: YF-B5 (GPIO do firmware)

DHT/DS18 podem existir em `lib_deps` mas **não** são o caminho de produto EC/pH.

## Build

```bash
git clone https://github.com/yhanusleverage/ESP-HIDROWAVE.git
cd ESP-HIDROWAVE
cp secrets.ini.example secrets.ini   # preencher Supabase + MQTT
pio run -e esp32dev
pio run -t upload
pio device monitor
```

`secrets.ini` está no `.gitignore`. `mqtt_enabled = 1` no Master de bancada/produção.

## MQTT / Lightsail

- Mosquitto + `hidrowave-bridge` na **mesma** VM (`MQTT_HOST=127.0.0.1` no bridge).
- ESP e Railway usam a **IP pública** do broker (`mqtt_host` / `MQTT_HOST`).
- Plano **1 GB + swap**; 512 MB esgota RAM (OOM / reboot).
- Ver tópicos e payloads: [docs/mqtt/04_MODELAGEM_TOPICOS_PAYLOADS.md](docs/mqtt/04_MODELAGEM_TOPICOS_PAYLOADS.md)
- Índice MQTT: [docs/mqtt/README.md](docs/mqtt/README.md)

```bash
# Na VM — análise de servidor (não é dump de tópicos)
sudo journalctl -u hidrowave-bridge -f --no-pager
```

## Repos da família

| Repo | Papel |
|------|--------|
| **ESP-HIDROWAVE** (este) | Firmware Master + bridge |
| [HIDROWAVE](https://github.com/yhanusleverage/HIDROWAVE) | Next.js + API + MQTT publish |
| [ESPNOW-SLAVE-TASK](https://github.com/yhanusleverage/ESPNOW-SLAVE-TASK) | Firmware slave relés |

## Licença

MIT — ver [LICENSE](LICENSE) se existir no repo.
