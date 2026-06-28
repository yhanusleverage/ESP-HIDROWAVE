# Bridge — validar PATCH relay_slaves completo (G4)

**Serviço:** `hidrowave-bridge` na VM Lightsail

## Logs esperados

| Evento | Log OK | Log problema |
|--------|--------|--------------|
| Heartbeat link (45s) | `[bridge] PATCH relay_slaves link-only ESP32_SLAVE_...` | — |
| ALL_RELAYS / toggle OK | `[bridge] PATCH relay_slaves ESP32_SLAVE_...` (upsert **com** estados) | Só `link-only` após toggle |
| Rejeição | — | `Rejected hidrowave/.../relay/state` |

## Comando monitor (SSH)

```bash
journalctl -u hidrowave-bridge -f | grep -E 'relay_slaves|Rejected|command_ack'
```

## Master serial (correlacionar)

Após fix firmware master:

```
[SYNC-MQTT] full relay_states mac=14:33:5C:38:BF:60
[MQTT] relay/state published
```

**Sem** só `[SLAVE-LINK] relay_state_link_heartbeat` durante 60s se slave online.

## Deploy bridge (se código antigo)

```powershell
# De ESP-HIDROWAVE-main/infra/mqtt/bridge/scripts/
.\deploy-lightsail.ps1
```

Depois na VM:

```bash
sudo systemctl restart hidrowave-bridge
journalctl -u hidrowave-bridge -n 20 --no-pager | grep -E 'Subscribed|link-only|PATCH relay_slaves'
```

## Pass G4

Em 2 min de operação com slave online: pelo menos **1** linha `PATCH relay_slaves ESP32_SLAVE_...` **sem** `link-only` no sufixo.
