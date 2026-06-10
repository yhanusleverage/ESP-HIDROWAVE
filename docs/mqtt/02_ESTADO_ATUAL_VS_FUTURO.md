# 02 — Estado atual vs plano (snapshot)

**Plano completo:** [00_PLANO_MESTRE.md](./00_PLANO_MESTRE.md)  
**Atualizado:** maio/2026

---

## Legenda

| | |
|-|-|
| ✅ | Implementado |
| 🟡 | Parcial |
| ⬜ | Gap |
| 📄 | Só documentação |
| ❌ | Desligado de propósito |

---

## Por camada

### Broker (Lightsail)
✅ Mosquitto, auth, ACL lab · 🟡 user por device · ⬜ IP fixo, firewall 1883, rotacionar senha

### Bridge
📄 spec · ⬜ código Node, systemd, heartbeat→Supabase

### Firmware cloud
✅ Supabase HTTPS, poll 5s, ack, sensores 30s, status 60s, sync relés 10s, rules 30s  
❌ admin :80 · ✅ portal AP  
⬜ MQTT, ENABLE_MQTT, dedup id, intervalos 2b

### Confiabilidade
✅ reboot_count NVS, markCommandCompleted, NetworkWatchdog, mutex SSL  
🟡 NVS relés (save ok, **load não aplica no boot**)  
⬜ esp_reset_reason, política pós-reboot, dedup, reboot remoto ESP

### Frontend
✅ Supabase only, last_seen 5min · ⬜ Realtime · ❌ MQTT browser

---

## Carga REST hoje (~1 ESP / hora)

| Operação | Intervalo | REST/h |
|----------|-----------|--------|
| Sensores (2 POST) | 30 s | 240 |
| device_status + relay_master | 60 s | 120 |
| Sync relés | 10 s | 360 |
| Poll comandos | 5 s | **720** |
| decision_rules | 30 s | 120 |
| **Total** | | **~1.560** |

Meta fase **2b:** ~540/h · Meta fase **4:** ~310/h — ver [09](./09_INTERVALOS_REST_VS_MQTT.md).

---

## Gaps P0–P2 (resumo)

| P | Gap |
|---|-----|
| P0 | Bridge; dedup por id |
| P1 | NVS restore boot; reset reason; user MQTT/device |
| P2 | Reboot remoto ESP; IP fixo |

Detalhe: [00 §7](./00_PLANO_MESTRE.md#7-gaps-prioritários-backlog-ordenado).
