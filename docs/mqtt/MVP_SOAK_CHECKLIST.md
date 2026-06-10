# MVP Telemetria — checklist soak 24h

Após flash com `mqtt_enabled=1` e bridge rodando.

## Antes do soak

- [ ] Bridge `journalctl -u hidrowave-bridge` sem erros
- [ ] Pub manual → linha em `hydro_measurements`
- [ ] Serial ESP: `[MQTT] Connected` e `telemetry ph=...` a cada 30s
- [ ] `mqtt_hydro_only=0` (HTTPS paralelo)

## Durante 24h

| Hora | Verificar |
|------|-----------|
| 0h | Heap livre no serial (anotar baseline) |
| 1h | SQL: ≥2 linhas/device via MQTT path (throttle 30s) |
| 6h | Sem reboot espontâneo |
| 24h | Heap ≥ baseline −5KB; gráfico HIDROWAVE contínuo |

## SQL útil

```sql
SELECT device_id, ph, temperature, tds, created_at
FROM hydro_measurements
WHERE device_id = 'ESP32_HIDRO_XXXXXX'
  AND created_at > NOW() - INTERVAL '24 hours'
ORDER BY created_at DESC;
```

## Passo 3 (opcional)

Em `secrets.ini`: `mqtt_hydro_only = 1` — recompilar e flash.  
HTTPS sensores para; MQTT + bridge única fonte hydro.

## Rollback

- `mqtt_enabled = 0` → rebuild flash
- `systemctl stop hidrowave-bridge`
