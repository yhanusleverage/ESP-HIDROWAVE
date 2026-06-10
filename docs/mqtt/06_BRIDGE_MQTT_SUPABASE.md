# 06 — Bridge MQTT → Supabase

## Papel

Processo **sempre ligado** na Lightsail que:

1. Subscreve tópicos `hidrowave/+/...`
2. Valida JSON (`v`, `device_id`)
3. Escreve no Supabase com **`service_role`** (nunca expor ao ESP nem ao browser)

O frontend continua usando **anon key** + RLS.

---

## Por que existe

| Sem bridge | Com bridge |
|------------|------------|
| ESP precisaria HTTPS para cada heartbeat | ESP publica 50–100 bytes MQTT |
| UI não veria online rápido | `last_seen` atualiza em segundos |
| Duplicar lógica no Next.js | Um lugar server-side para persistência |

---

## Layout sugerido no servidor

```
/opt/hidrowave-bridge/
  package.json
  index.js
  .env                 # chmod 600
  README.md            # link para docs/mqtt
```

Systemd: `/etc/systemd/system/hidrowave-bridge.service`

---

## Variáveis de ambiente (`.env.example` em `infra/mqtt/bridge/`)

```env
MQTT_HOST=127.0.0.1
MQTT_PORT=1883
MQTT_USER=bridge_internal
MQTT_PASS=

SUPABASE_URL=https://xxxx.supabase.co
SUPABASE_SERVICE_ROLE_KEY=

HEARTBEAT_STALE_MS=120000
TELEMETRY_THROTTLE_MS=60000
```

- `MQTT_HOST=127.0.0.1`: bridge na mesma VM que Mosquitto (menos exposição).
- `TELEMETRY_THROTTLE_MS`: evita flood na fase 2 (HTTPS + MQTT paralelos).

---

## Lógica por tópico (pseudocódigo)

### `hidrowave/+/heartbeat`

```
device_id = parseTopic(topic)
payload = JSON.parse(message)
if payload.device_id != device_id: drop
await supabase.from('device_status')
  .update({ last_seen: new Date().toISOString(), is_online: true })
  .eq('device_id', device_id)
```

### `hidrowave/+/status` (LWT retain)

```
if payload.online === false:
  update is_online = false
else:
  update is_online = true (opcional)
```

### `hidrowave/+/telemetry`

```
if throttle[device_id] < TELEMETRY_THROTTLE_MS: return
map ph, tds, ... → hydro_measurements INSERT
```

### Publicar comando (fase 3)

Opções:

1. **Supabase Database Webhook** em INSERT `relay_commands` → HTTP local bridge → `mqtt.publish(command)`.
2. Bridge faz poll leve em `relay_commands` `status=pending` (menos elegante).

---

## ACL do usuário `bridge_internal`

Ver `infra/mqtt/mosquitto/acl.example`.

---

## Tratamento de erros

| Erro | Comportamento |
|------|---------------|
| Supabase 5xx | Retry exponencial; manter fila em memória limitada |
| JSON inválido | Log + discard |
| MQTT disconnect | Reconnect automático (`mqtt.js`) |
| `device_id` desconhecido | Log; opcional ignorar ou criar alerta |

---

## Segurança

- Processo roda como usuário Linux sem shell (`hidrowave`)
- `.env` não em backup público
- Logs não imprimem keys nem senhas

---

## Teste manual (fase 1)

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -u bridge_test -P '...' \
  -t 'hidrowave/ESP32_HIDRO_269844/heartbeat' \
  -m '{"v":1,"device_id":"ESP32_HIDRO_269844","ts":0,"online":true}'
```

Verificar no SQL Editor Supabase:

```sql
select device_id, last_seen, is_online
from device_status
where device_id = 'ESP32_HIDRO_269844';
```

---

## Implementação

Código do bridge **ainda não está no repositório** — apenas especificação. Ao implementar, adicionar pasta `infra/mqtt/bridge/` com `index.js` e pedir aprovação antes do deploy.
