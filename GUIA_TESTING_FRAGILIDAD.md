# 🧪 Guía de Testing de Fragilidad del Sistema ESP32

## 📊 Estado Actual del Sistema
- **Heap libre**: ~98KB (32.9%) ✅ Saludable
- **Uptime**: 492s sin reboot ✅
- **Supabase**: Conectado ✅
- **ESP-NOW**: 1 slave online ✅

---

## 🔍 Puntos Críticos de Fragilidad

### 1. Mutex en HydroControl (DosingTask)
| Métrica | Valor Actual | Umbral Crítico |
|---------|--------------|----------------|
| Uso de mutex | 15 operaciones | >50 = riesgo |
| Timeout mutex | 0 | >3 = problema |
| Deadlock | No | Sí = crítico |

**Test**: Activar `auto_enabled` + dosagem simultánea

### 2. ⚠️ SSL Memory Allocation (MÁS CRÍTICO)
| Métrica | Valor Actual | Umbral Crítico |
|---------|--------------|----------------|
| Heap normal | 98KB (32.9%) | >60KB = saludable |
| Heap bajo carga | 57KB (19.5%) | <60KB = SSL falla |
| SSL failures | Recurrente bajo carga | >3 consecutivos = crítico |
| HTTP timeouts | 1 (recuperado) | >5 consecutivos = problema |

**Causa**: Múltiples conexiones SSL simultáneas agotan heap
**Test**: Polling EC config + envío de sensores + sync slaves + comandos rápidos

### 3. Queues ESP-NOW (MasterSlaveManager)
| Métrica | Valor Actual | Umbral Crítico |
|---------|--------------|----------------|
| Queue overflow | 0 | >0 = problema |
| Mensajes perdidos | 0 | >5% = problema |
| Latencia ping/pong | <100ms | >500ms = problema |

**Test**: Enviar comandos rápidos a múltiples relés del slave

### 4. ObjectPool (SupabaseClient)
| Métrica | Valor Actual | Umbral Crítico |
|---------|--------------|----------------|
| Pool exhaustion | 0 | >0 = problema |
| Memory fragmentation | 38.9KB max alloc | <20KB = crítico |

---

## 🧪 Protocolo de Testing

### Fase 1: Testing Individual (5 min cada)

#### A) Control Proporcional EC
1. Activar `auto_enabled = true` en frontend
2. Configurar `ec_setpoint = 1500`
3. Observar logs de dosagem
4. **Métricas**: Tiempo de respuesta, heap antes/después

#### B) Acionamiento de Relays Slave
1. Activar/desactivar relés 0-7 del slave rápidamente
2. Verificar sincronización con Supabase
3. **Métricas**: Latencia ESP-NOW, queue overflow

#### C) Polling HTTP intensivo
1. **NOTA**: `intervalo_auto_ec` NO es polling HTTP
   - `intervalo_auto_ec` = intervalo entre verificaciones del control proporcional EC (cuando `auto_enabled = true`)
   - Polling HTTP de config = cada 5 segundos (hardcoded)
2. Para testar: Activar `auto_enabled` con `intervalo_auto_ec = 1` segundo
3. Observar heap y timeouts
4. **Métricas**: HTTP success rate, heap trend

### Fase 2: Testing Combinado (10 min)

1. **Activar todo simultáneamente**:
   - Auto EC habilitado
   - Comandos de relés desde frontend
   - Slave respondiendo pings
   - Polling EC config cada 5s

2. **Observar**:
   - Heap libre (debe mantenerse >50KB)
   - Timeouts HTTP (<3 consecutivos)
   - Latencia ESP-NOW (<500ms)
   - Watchdog triggers (0)

---

## 📈 Métricas a Monitorear

```
🔍 DASHBOARD DE SALUD
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Heap Libre:        [████████░░] 98KB / 300KB (32.9%)
Max Alloc:         [████░░░░░░] 38.9KB (fragmentación moderada)
HTTP Success:      [██████████] 100% (último timeout recuperado)
ESP-NOW Latency:   [██████████] <100ms
Mutex Contention:  [░░░░░░░░░░] 0 deadlocks
Queue Overflow:    [░░░░░░░░░░] 0 eventos
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## ⚠️ Problemas Detectados en el Log

### 1. NVS Guardando 4 veces (Redundante) ✅ CONFIRMADO
```
╔═══════════════════════════════════════════════════╗
║   💾 ATUALIZANDO EC_CONFIG NO NVS                 ║  ← Se repite 4 veces
╚═══════════════════════════════════════════════════╝
```
**Causa**: Cada setter guarda automáticamente:
1. `setECSetpoint()` → guarda
2. `setAutoECEnabled()` → guarda  
3. `setAutoECInterval()` → guarda
4. Llamada final `saveECControllerConfig()` → guarda

**Impacto**: Desgaste de flash, tiempo de CPU, fragmentación heap
**Solución**: Guardar solo una vez al final, después de todos los setters

### 2. HTTP Timeout en RPC MASTER
```
[460926][W][HTTPClient.cpp:1483] returnError(): error(-11): read Timeout
❌ [RPC MASTER] Erro HTTP: -11
⏱️ [NETWORK_WDT] Operación completada en 14012 ms
```
**Impacto**: 14 segundos de bloqueo
**Solución**: Reducir timeout HTTP a 5-8 segundos

---

## 🔧 Optimizaciones Sugeridas

### Prioridad Alta
1. **Reducir timeout HTTP** de 14s a 8s
2. **Evitar NVS duplicado** - solo guardar si cambió algo
3. **Agregar métricas de salud** en el log periódico

### Prioridad Media
4. **Aumentar intervalo de discovery** ESP-NOW (muy frecuente)
5. **Reducir logs verbosos** de relay sync (spam)

### Prioridad Baja
6. **Implementar circuit breaker** para HTTP failures
7. **Agregar health check** periódico con métricas

---

## 📋 Checklist de Watchdog

Si el sistema hace reboot por watchdog, buscar en el log:

- [ ] `Guru Meditation Error` - Stack overflow o null pointer
- [ ] `Task watchdog got triggered` - Task bloqueada
- [ ] `Brownout detector was triggered` - Problema de alimentación
- [ ] `assert failed` - Error de mutex/queue
- [ ] `SSL - Memory allocation failed` - Heap agotado

---

## 🎯 Conclusión Actual

**El sistema está ESTABLE pero FRÁGIL bajo carga alta**.

### ✅ Lo que funciona bien:
- No hay watchdog reboots
- ESP-NOW funciona correctamente
- Sistema se recupera automáticamente

### ⚠️ Problemas detectados:
1. **SSL Memory Allocation Failed** (MÁS CRÍTICO)
   - Heap baja de 98KB → 57KB cuando presionas muchos botones
   - SSL necesita ~40KB → falla
   - Se recupera solo, pero es frágil

2. **NVS guardando 4 veces** (Ineficiente)
   - Cada setter guarda automáticamente
   - Desgaste de flash innecesario

3. **HTTP timeouts** (Menor)
   - 1 timeout recuperado automáticamente

### Para testar fragilidad:
1. Activar `auto_enabled = true` con `intervalo_auto_ec = 1` segundo
2. Enviar comandos rápidos a múltiples relés del slave
3. Observar heap (debe mantenerse >60KB para SSL)
4. Verificar que no hay watchdog reboots

