# Documentação MQTT — HIDROWAVE

**Fonte de verdade do plano:** [00_PLANO_MESTRE.md](./00_PLANO_MESTRE.md) (v2.0 — maio/2026)

Arquitetura **híbrida MQTT + Supabase**: integridade e sync primeiro; WebSocket **não** no ESP; browser **não** usa MQTT.

## Comece aqui

| Ordem | Documento | Para quê |
|-------|-----------|----------|
| 1 | **[00_PLANO_MESTRE.md](./00_PLANO_MESTRE.md)** | Plano completo: fases, ✅/🟡/⬜, gaps, KPIs |
| 2 | [10_REQUISITOS_CONFIABILIDADE.md](./10_REQUISITOS_CONFIABILIDADE.md) | R1–R9, dedup, QoS, reboot, drift |
| 3 | [09_INTERVALOS_REST_VS_MQTT.md](./09_INTERVALOS_REST_VS_MQTT.md) | Quanto REST retirar por fase |

## Índice completo

| Documento | Conteúdo |
|-----------|----------|
| [00_PLANO_MESTRE.md](./00_PLANO_MESTRE.md) | **Plano reescrito** — estado real + gaps + fases 0–5 + 3b |
| [01_ARQUITETURA_E_DECISAO.md](./01_ARQUITETURA_E_DECISAO.md) | Por que híbrido; o que não muda |
| [02_ESTADO_ATUAL_VS_FUTURO.md](./02_ESTADO_ATUAL_VS_FUTURO.md) | Snapshot código vs plano (resumo) |
| [03_PLANO_IMPLEMENTACAO_FASES.md](./03_PLANO_IMPLEMENTACAO_FASES.md) | Checklist operacional por fase |
| [04_MODELAGEM_TOPICOS_PAYLOADS.md](./04_MODELAGEM_TOPICOS_PAYLOADS.md) | Tópicos, JSON, QoS, LWT |
| [05_SEGURANCA_PRODUCAO.md](./05_SEGURANCA_PRODUCAO.md) | ACL, plain vs TLS |
| [06_BRIDGE_MQTT_SUPABASE.md](./06_BRIDGE_MQTT_SUPABASE.md) | Bridge Lightsail |
| [07_FIRMWARE_ESP32.md](./07_FIRMWARE_ESP32.md) | `MqttClient` + `HydroSystemCore` |
| [08_FERRAMENTAS_MQTTX.md](./08_FERRAMENTAS_MQTTX.md) | Dev only |
| [09_INTERVALOS_REST_VS_MQTT.md](./09_INTERVALOS_REST_VS_MQTT.md) | REST/h e timers ms |
| [10_REQUISITOS_CONFIABILIDADE.md](./10_REQUISITOS_CONFIABILIDADE.md) | Critérios de aceite |

## Templates infra

```
ESP-HIDROWAVE-main/infra/mqtt/
  mosquitto/hidrowave.conf.example
  mosquitto/acl.example
  bridge/.env.example
```

## Status rápido (maio/2026)

| Área | Status |
|------|--------|
| Mosquitto Lightsail | ✅ rodando |
| Bridge Node | ✅ código em `infra/mqtt/bridge/` |
| Firmware MQTT | ✅ `MqttClient` (ENABLE_MQTT=0 default) |
| Supabase HTTPS + ack | ✅ |
| MVP soak hydro | 📄 [MVP_SOAK_CHECKLIST.md](./MVP_SOAK_CHECKLIST.md) |
| **MVP device_status (AGORA)** | 📄 [MVP_DEVICE_STATUS_HEARTBEAT.md](./MVP_DEVICE_STATUS_HEARTBEAT.md) |
| Próximo: comandos híbridos | Fase 3 em [03_PLANO_IMPLEMENTACAO_FASES.md](./03_PLANO_IMPLEMENTACAO_FASES.md) |
| Auto pH MQTT (paridad EC) | `ph_operation` + `ph_dose` — ver [04_MODELAGEM](./04_MODELAGEM_TOPICOS_PAYLOADS.md) §3.7–3.8 y handoff pH en HIDROWAVE-main |

## Regra do projeto

Código firmware ou bridge só após **“pode aplicar”**.
