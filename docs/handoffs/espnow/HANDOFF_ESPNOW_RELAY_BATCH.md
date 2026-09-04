# ESP-NOW relay batch (SET_RELAY_MASK) — handoff

## Baseline funcional (pre-batch, 2026-09-02)

Estado validado em campo **antes** de `ESPNOW_RELAY_BATCH_ENABLED`:

| Métrica | Valor típico sob carga |
|---------|------------------------|
| `heap` | ~97 KB |
| `min` | ~84 KB |
| `loop/s` | 48–71 |
| `mqtt` | 1 estável |
| `errno:11` / `relay/state publish failed` | 0 (coalesce MQTT 300 ms) |
| Comandos slave | `cloud_closed=true`, ACK OK |
| Ráfaga 8 relés | ~7–10 s (1 ESP-NOW/comando + cola retry) |

**Coalesce MQTT** (`MQTT_RELAY_STATE_DEBOUNCE_MS=300`) — mantido; batch ESP-NOW é camada **radio**, não cloud.

---

## O que o batch muda

- Clips **manual + slave + instant + dur=0 + cycle=0 + on/off** na mesma MAC dentro de `ESPNOW_RELAY_BATCH_MS` (default 300 ms).
- Flush → `RelayCoordinator::requestMask` → um `SET_RELAY_MASK` (0x0F).
- Vários `supabase_id` → `registerPendingSlaveAck` cada; fecho cloud via ACK ou `ALL_RELAYS` (como hoje).

## O que NÃO entra no batch

- Timer / ciclo / toggle / on_all / off_all
- Relés locais (master PCF)
- DecisionEngine / diluição / HMI (continuam `actuateSlave` directo)

## Flags (`Config.h`)

| Flag | Default |
|------|---------|
| `ESPNOW_RELAY_BATCH_ENABLED` | 1 |
| `ESPNOW_RELAY_BATCH_MS` | 300 |
| `ESPNOW_RELAY_BATCH_COMPACT_LOG` | 1 |

Desactivar batch: `#define ESPNOW_RELAY_BATCH_ENABLED 0` — regressão ao caminho 1-comando-1-ESP-NOW.

## Serial (compact)

```
[CMD mqtt] supabase_id=1625 slave R1 off ...
[BATCH] queue mac=14:33:5C:38:BF:60 R1 off id=1625 n=1 flush_in=300ms
[BATCH] flush mac=... mask=0xA5 n=4 esp=46 (+312ms)
[SYNC periodic] full relay_states mac=...   ← sync 60s, não é clic UI
```

## Contrato slave

Ver `HANDOFF_SET_RELAY_MASK.md` — opcode `0x0F`, payload `mask` + `durationSec` + `commandId`.
