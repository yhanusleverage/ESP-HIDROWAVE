# Handoff geral (sessão 21–22 ago 2026)

**Para:** retomar o projeto **sem** esta conversa.  
**Idioma do time:** espanhol no chat; docs MQTT misturam PT/ES.  
**Não commitar** `secrets.ini`.

---

## 1. O que é o sistema (3 rádios / 3 nuvens)

| Peça | Repo | Papel |
|------|------|--------|
| **Core (Master)** | `ESP-HIDROWAVE-main` | WiFi STA + MQTT + HTTPS + ESP-NOW. Device `ESP32_HIDRO_1A575C`. MAC `EC:E3:34:1A:57:5C`. |
| **Atlas (Slave)** | `ESPNOW-SLAVE-TASK-main` | Só ESP-NOW + relés PCF8574. MAC `14:33:5C:38:BF:60`. Sem WiFi de produção. |
| **Web** | `HIDROWAVE-main` | Next.js + Supabase. Automação, Atlas, Auto EC/pH. |

Nuvem:

- **Supabase** = fonte da UI (tabelas + Realtime WSS).
- **MQTT** Lightsail `15.175.109.90:1883` → **hidrowave-bridge** PATCH nas tabelas.
- O browser **não** fala MQTT. O Atlas **não** fala MQTT. Só o Core.

```
UI  ←WSS/REST→  Supabase  ←bridge←  Mosquitto  ←  Core  ←ESP-NOW→  Atlas
```

---

## 2. Bancada (fato)

- Router WiFi do Core: **canal 5** (`YAGO_2.4`). ESP-NOW do Master **segue** esse canal. Não forçar 11.
- Slave NVS: canal **5** (depois do scan). Lock = ficar no 5.
- Serial Master: slave **Online: Sim**, ping/pong, comando **chega**.
- Relé físico: `PCF8574 não inicializado` → ACK `success=0` `state=OFF`. Enlace OK, chip não.
- UI “sala 1 Atlas Offline”: **não** é ESP-NOW. É `relay_slaves.last_update` (ou `device_status.last_seen`) com limiar **90 s**.
- MQTT no Serial: `mqtt=0` / `Failed rc=-2` quando o broker **não abre 1883** (firewall/instância). Teste PC: portas **22 e 1883 fechadas** para `15.175.109.90`.
- `ENABLE_LOCAL_ADMIN_HTTP=0` → UI não lê cache RAM do Core.

** mentira no firmware:** texto fixo “mesmo canal ESP-NOW (canal 1)”. O canal real é `Canal WiFi detectado: N` no boot.

---

## 3. Auto EC / Auto pH (web) — já no disco

**Problema antigo:** botão na web OFF, ESP ON (poll HTTPS pulava com `doseCycleBusy`; apply sem NVS).

**Entrega UI-only (não conserta o Master):**

- Optimistic toggle + Realtime `ec_config_view` / `ph_config_view`.
- Arquivos: `HIDROWAVE-main/src/lib/realtime/auto-controller.ts`, `AutoEcControllerPanel.tsx`, `PhControllerPanel.tsx`.
- SQL: `HIDROWAVE-main/scripts/ENABLE_AUTO_CONTROLLER_REALTIME.sql`.
- Badge **ao vivo** = canal `SUBSCRIBED`. Sem F5.
- **Não** sincroniza o ESP. MQTT/firmware Auto EC = outro trabalho.

---

## 4. ESP-NOW — o que está no código

**Padrão Espressif:** um rádio. Scan **uma vez** se NVS vazio. Se NVS tem canal → **ficar**. Heartbeat app 15 s. Não comparar `millis()` entre placas; usar `lastRxAgeMs` local (e `seq` depois).

**Fase 1 (implementada no firmware):**

| Relógio | Onde | Comportamento |
|---------|------|----------------|
| 1 | Master `addTrustedSlave` | Janela **5 s** sem GET EC / poll comandos / telemetry MQTT |
| 2 | Slave `discoverMaster` | NVS 2–13: **não** hop 1/6/11 |
| 3 | `SafetyWatchdog` 15 s | Só depois do lock |
| 4 | `HydroSystemCore::update` | HTTPS/MQTT diferidos se janela ativa |
| 5 | `performRediscoveryIfNeeded` | Não hop se já no canal NVS; 180 s se NVS vazio |

Docs: [ESPNOW_LOCK_WINDOW.md](espnow/ESPNOW_LOCK_WINDOW.md).  
Provisioning ciclo 11→op: [HANDOFF_PROVISIONING_CICLO_11.md](espnow/HANDOFF_PROVISIONING_CICLO_11.md) (`ESPNOW_PROVISIONING_STA_SUSPEND=1`).  
Debug: `ESPNOW_LOCK_DEBUG` / `[LOCK]` `[SCAN]`. Depois: `0` só no Master.

**Bug que já se corrigiu no Slave:** `waitForMasterResponse()` zerava `masterFound` a cada espera.

**Não fazer:** canal fixo 11 no Master; reconectar ESP-NOW cada 30 s.

---

## 5. Atlas Offline na web — MQTT (não o scan)

A UI lê Supabase (`resolveSlaveOnline`, limiar 1,5 min). RAM do Core `is_online:true` **não** chega à Vercel.

Ordem combinada:

1. Lightsail: Mosquitto + `hidrowave-bridge` **active**, portas **22 e 1883** abertas.
2. `secrets.ini`: `mqtt_enabled=1`, host `15.175.109.90`. User no **firmware** = `mqtt_` + device_id (a linha `mqtt_user` do ini **não manda** no CONNECT).
3. Flash **só Core**. Serial: `[MQTT] Connected` `mqtt=1`.
4. Bridge PATCH `relay_slaves.last_update` → UI Online.

**Não subir `.bin` ao Lightsail.** Broker = contas + ACL + systemd.

Plano MQTT bancada: `mqtt_ESP32_HIDRO_1A575C` no `passwd` (hoje).  
Futuro flota: ver §6.

---

## 6. ACL flota `%c` (desenho; não em produção)

Handoff de testes: [HANDOFF_ACL_FLEET_VALIDATION.md](../mqtt/HANDOFF_ACL_FLEET_VALIDATION.md).  
Comentário no [acl.example](../../infra/mqtt/mosquitto/acl.example).

**passwd futuro:** 3 users — `bridge_internal`, `hidrowave`, `mqtt_esp`.

**ACL ESP:**

```
user mqtt_esp
topic read hidrowave/%c/command
topic write hidrowave/%c/#
```

`%c` = client id = `ESP32_HIDRO_XXXXXX` = “só o teu andar”.  
`+` no bridge = o empregado vê todos os andares.

Email **não** entra no ACL. Web: `device_id` ↔ `user_email` + RLS.

**Passo 2 firmware:** login MQTT = `mqtt_esp` (hoje ainda `mqtt_`+id). Sem isso a ACL flota não casa.

**Não** escrever um serviço que insere MAC no ACL. Uma regra `%c` chega.

---

## 7. IDs e paths

| Nome | Valor |
|------|--------|
| Core | `ESP32_HIDRO_1A575C` |
| Atlas MAC | `14:33:5C:38:BF:60` |
| MQTT host | `15.175.109.90` |
| IP antiga (não usar) | `99.79.36.220` |
| Canal RF bancada | **5** |

SSH: `ubuntu@15.175.109.90` (chave Lightsail). Deste ambiente Cursor as portas **não** responderam.

---

## 8. Próximo (ordem)

1. **Rede MQTT:** firewall Lightsail 22/1883; `systemctl status mosquitto hidrowave-bridge`.
2. Flash Core se `mqtt_enabled=1` e Serial `mqtt=1`.
3. Confirmar UI Atlas Online.
4. (Opcional) ACL `%c` + firmware `mqtt_esp`.
5. PCF8574 `0x20` se quiser relé físico.
6. Auto EC Master MQTT (ESP ainda pode ficar ON com web OFF).

---

## 9. O que não misturar

- Realtime Quantidade (`pump_quantity`) ≠ Realtime Auto EC (só `auto_enabled`).
- `Operacional` ≠ `Online` (enum handshake vs flag DEVICE_INFO).
- Canal WiFi ≠ IP `192.168.1.x`.
- Canal ESP-NOW do Master = canal do router, sempre.
