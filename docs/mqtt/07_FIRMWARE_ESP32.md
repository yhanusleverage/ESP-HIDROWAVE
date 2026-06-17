# 07 — Plano de firmware ESP32 (MQTT)

**Status:** MQTT **implementado** para EC + pH (jun/2026). Este doc mantém notas gerais; procedimentos Auto pH em handoffs serial.

---

## Auto pH — procedimentos firmware

| Paso | Doc | Conteúdo |
|------|-----|----------|
| S04 | [`HIDROWAVE-main/docs/handoffs/ph/S04_FLUJO_POLL_CONFIG.md`](../../../HIDROWAVE-main/docs/handoffs/ph/S04_FLUJO_POLL_CONFIG.md) | Poll `ph_config_view`, apply config |
| S05 | [`HIDROWAVE-main/docs/handoffs/ph/S05_FLUJO_CICLO_ADAPTATIVO.md`](../../../HIDROWAVE-main/docs/handoffs/ph/S05_FLUJO_CICLO_ADAPTATIVO.md) | Ciclo adaptativo, `ph_dose`, heartbeat 12s/30s |
| S07 | [`HIDROWAVE-main/docs/handoffs/ph/S07_BRIDGE_MQTT.md`](../../../HIDROWAVE-main/docs/handoffs/ph/S07_BRIDGE_MQTT.md) | Bridge Lightsail, ACL, test publish |

Código: `MqttClient::publishPhOperation`, `publishPhDose`; `HydroSystemCore::syncPhOperationStateToSupabase`, `handlePhDoseEvent`; NVS boot `HydroControl::loadPHControllerConfig()`.

Índice completo: [`HIDROWAVE-main/docs/handoffs/ph/00_INDICE_SERIAL.md`](../../../HIDROWAVE-main/docs/handoffs/ph/00_INDICE_SERIAL.md)

---

## Por que ainda não está no código

1. Broker precisava estar validado (Fase 0).
2. Bridge deve existir antes de depender de MQTT para `last_seen` na UI.
3. Regra do projeto: diff revisado antes de aplicar.

---

## Dependências planejadas

`platformio.ini`:

```ini
lib_deps =
  knolleary/PubSubClient @ ^2.8
```

Usar `WiFiClient` (TCP plain), não `WiFiClientSecure`, na fase MVP.

---

## Configuração (`secrets.ini` + `Config.h`)

`secrets.ini` (local, gitignored) — ver `secrets.ini.example`:

```ini
mqtt_host=SEU_IP_ESTATICO
mqtt_port=1883
mqtt_user=mqtt_ESP32_HIDRO_XXXXXX
mqtt_pass=
```

`Config.h` (proposto):

```c
#ifndef ENABLE_MQTT
#define ENABLE_MQTT 0   // 1 após validação
#endif
```

Build flags via `platformio.ini` podem injetar de `secrets.ini` (mesmo padrão Supabase).

---

## Arquivos novos (proposta)

| Arquivo | Responsabilidade |
|---------|------------------|
| `include/MqttClient.h` | API: `begin`, `loop`, `publishHeartbeat`, `publishTelemetry`, `setCommandCallback` |
| `src/MqttClient.cpp` | PubSubClient, reconnect, LWT |
| `HydroSystemCore.cpp` | Hooks no `loop()` após WiFi |

---

## Integração em `HydroSystemCore::loop()`

Ordem sugerida (não bloquear HTTPS):

```
1. mqtt.loop()                    // rápido
2. lógica existente sensores/relés
3. se ENABLE_MQTT && WiFi.connected():
     - a cada 30s → publishHeartbeat()
     - a cada 60s → publishTelemetry()  // ou 30s alinhado SENSOR_SEND_INTERVAL
4. supabase checks (existente)
```

**Mutex:** não chamar `mqtt.publish` dentro de `SupabaseClient::makeRequest` — usar flags `pendingMqttHeartbeat` se necessário.

---

## Intervalos MQTT vs HTTPS (fase 2)

| Dado | MQTT (novo) | HTTPS (mantém fase 2) |
|------|-------------|------------------------|
| Heartbeat | 30 s | `device_status` 60 s até fase 4 |
| Telemetria | 60 s | 30 s `sendHydroData` |
| Relés | on change | sync 10 s |
| Comandos | — | poll 5 s |

Fase 4: desligar HTTPS redundante.

---

## Memória

- `MIN_HEAP_FOR_HTTPS` = 30 KB (`HydroSystemCore.h`) — manter.
- Antes de `mqtt.connect`, checar `ESP.getFreeHeap() > 45000` (ajustar em teste).
- Buffer PubSubClient: 512 bytes (suficiente para doc 04).

---

## Connect / LWT

```cpp
mqttClient.connect(
  clientId.c_str(),      // ex: "ESP32_HIDRO_269844"
  mqtt_user,
  mqtt_pass,
  statusTopic.c_str(),   // LWT topic
  1,                     // QoS
  true,                  // retain
  "{\"v\":1,\"online\":false}"
);
```

Após connect OK: publish retain `online:true` em `.../status`.

---

## Callback de comando (fase 3)

```cpp
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  // parse JSON → delegar para mesma função que processa relay_commands
}
```

Reutilizar validação de índice de relé e `duration_s` já existente em `SupabaseClient` / `HydroSystemCore`.

---

## Logs esperados (debug)

```
[MQTT] Connecting to 99.x.x.x:1883 ...
[MQTT] Connected clientId=ESP32_HIDRO_269844
[MQTT] Published heartbeat
[MQTT] Reconnect in 5s (rc=-2)
```

---

## Rollback

- `ENABLE_MQTT 0` → compilação idêntica ao comportamento atual.
- Remover credenciais `mqtt_*` de `secrets.ini` em campo se desativar broker.

---

## Checklist antes do merge

- [ ] `device_id` em tópicos = `getDeviceID()`
- [ ] ACL no broker para esta placa
- [ ] LWT testado
- [ ] 24 h soak sem queda de heap
- [ ] Aprovação explícita do usuário
