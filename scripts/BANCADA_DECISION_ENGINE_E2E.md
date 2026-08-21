# Bancada E2E — Decision Engine + ScriptRunner (M3/M4)

Checklist para validar motor de decisão local, scripts sequenciais y sync `decision_rules` desde Supabase.

## Pre-requisitos

- [ ] Master ESP32 con WiFi y Supabase conectado (`supabaseConnected=true`)
- [ ] Slave ESP-NOW emparejado (si la regla usa `target_device_id` remoto)
- [ ] LittleFS montado (`DecisionEngine.begin()` OK)
- [ ] NTP sincronizado (para `time_window` con HH:MM)
- [ ] Fila `decision_rules` con al menos 1 regla `enabled=true` para el `device_id` del master

## M3 — ScriptRunner

- [ ] Regla con `rule_json.script.instructions` (sin `actions`) se carga sin error de validación
- [ ] `relay_action` master: relé local cambia según instrucción
- [ ] `relay_action` slave: comando ESP-NOW al `target_device_id` correcto
- [ ] `delay`: script pausa ~N ticks (2 s/tick) antes de la siguiente instrucción
- [ ] `while` + `max_iterations`: loop corta al alcanzar el límite de ticks
- [ ] `time_window` (`procedure_triggers` HH:MM): script solo corre dentro de la ventana
- [ ] Prioridad ≥ 80: ScriptRunner activa `setTankProcedureActive(true)` hasta fin de secuencia (Auto EC/pH pausado sin timer)

## M4 — Sync Supabase → LittleFS

- [ ] `GET decision_rules?device_id=eq.<ID>&enabled=eq.true` retorna array JSON
- [ ] `/rules.json` en LittleFS contiene `{"rules":[...]}`
- [ ] Log: `✅ [REGRAS] N regras sincronizadas do Supabase → LittleFS`
- [ ] Cambio en Supabase (disable regla) se refleja tras ~30 s (`RULES_CHECK_INTERVAL`)
- [ ] `decisionEngine.loadRulesFromFile()` recarga reglas sin reinicio

## E2E — Dreno (ejemplo script)

1. [ ] Crear regla `RULE_DRAIN` priority 80 con script `while level_4 != vazio` + `relay_action` slave
2. [ ] Verificar válvula ON mientras L4 no está vacío
3. [ ] Simular L4 vacío → relé OFF
4. [ ] Confirmar hold P1 durante ejecución del script

## Serial — mensajes esperados

```
✅ Regra carregada: <nombre>
✅ [REGRAS] N regras sincronizadas do Supabase → LittleFS
📋 N regras carregadas do arquivo
⚡ [LOCAL/COORD] Relé X: ON/OFF (regra: script)
✅ [REMOTO] device_id=... relay=X: on/off (regra: script)
```

## Fallos comunes

| Síntoma | Causa probable |
|---------|----------------|
| `Regra inválida: Regra deve ter pelo menos uma ação` | Falta `script.instructions` en `rule_json` |
| Script no corre fuera de ventana | `time_window` / NTP no sincronizado |
| Slave no responde | `target_device_id` no coincide con `deviceName` del slave |
| Sync no actualiza | `hasEnoughMemoryForHTTPS()` false o SSL hot path ocupado |

## Comandos útiles

- Monitor serial: `pio device monitor -b 115200`
- Ver `/rules.json`: endpoint diag o `LittleFS.open` en debug
- Forzar poll: esperar 30 s o reiniciar tras editar regla en Supabase
