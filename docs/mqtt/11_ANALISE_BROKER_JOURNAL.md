# 11 — Analisar o broker com journalctl

Guia de bancada: **formato de servidor** (systemd), sem pager. Não é dump de tópicos MQTT.

SSH na VM Lightsail **1 GB** (IP estática atual `15.175.109.90`). Sem passwords neste doc.

## Comando recomendado (bridge → Supabase)

```bash
sudo journalctl -u hidrowave-bridge -f --no-pager
```

`--no-pager` = saída contínua (não abre `less`; não precisa Q). **Ctrl+C** para parar de olhar; o serviço segue.

O que aparece: data, host, `node[pid]`, depois INSERT/PATCH.

Procurar:

- `INSERT hydro_measurements` — EC/pH/níveis persistidos
- `PATCH device_status` — heartbeat / presença
- `ec_operation` / `ph_operation` — estado Auto EC/pH
- `ESP32_HIDRO_1A575C` — Master desta bancada
- `Throttled telemetry` — **normal** (anti-flood ~30 s)

## Opcional: Mosquitto

Connect/disconnect do broker. **Não** lista cada EC/pH.

```bash
sudo journalctl -u mosquitto -f --no-pager
```

## Os dois

```bash
sudo journalctl -u hidrowave-bridge -u mosquitto -f --no-pager
```

## O que não usar como análise de serviço

`mosquitto_sub -t '#' -v` é dump de tópicos (debug de pacote). Útil no laboratório; **não** substitui o journal do systemd.

## RAM / swap

Não substitui o journal. Só saúde da VM (plano 1G + swap):

```bash
free -h
```

Ver também deploy: [06_BRIDGE_MQTT_SUPABASE.md](./06_BRIDGE_MQTT_SUPABASE.md).
