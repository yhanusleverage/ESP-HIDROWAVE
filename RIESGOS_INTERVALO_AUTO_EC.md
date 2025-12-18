# ⚠️ Riesgos Críticos de `intervalo_auto_ec` Muy Bajo

## 📊 Cómo Funciona `intervalo_auto_ec`

- **NO es polling HTTP** (ese es cada 5 segundos)
- **SÍ es intervalo entre verificaciones del control proporcional EC**
- Se ejecuta en `hydroControl.loop()` → `checkAutoEC()`

### Flujo Normal:
```
Cada intervalo_auto_ec segundos:
  1. Verificar si EC < setpoint
  2. Si sí → Calcular dosagem (u(t))
  3. Si dosagem > 0.1 ml → Iniciar dosagem sequencial
  4. Después de dosagem → Esperar tempo_recirculacao (60s)
  5. Durante tempo_recirculacao → NO verifica EC
```

---

## 🚨 RIESGOS CRÍTICOS

### 1. **Mutex Contention** (ALTO RIESGO)

**Escenario**: `intervalo_auto_ec = 1` segundo

```
T=0s:  checkAutoEC() → EC bajo → Inicia dosagem (adquiere mutex)
T=1s:  checkAutoEC() → Dosagem ainda ativa → Ignora (currentState != IDLE) ✅
T=2s:  checkAutoEC() → Dosagem ainda ativa → Ignora ✅
...
T=15s: Dosagem completa → Libera mutex
T=16s: checkAutoEC() → EC ainda baixo → Tenta nova dosagem
```

**Problema**:
- Si dosagem dura 15 segundos y `intervalo_auto_ec = 1`, hay 15 llamadas inútiles
- Cada llamada hace cálculos y verificaciones (CPU waste)
- Si `tempo_recirculacao` es muy bajo, puede intentar dosificar inmediatamente después

**Código actual** (línea 650):
```cpp
if (currentState == IDLE) {
    startSimpleSequentialDosage(dosageML, ecSetpoint, ec);
} else {
    Serial.println("⚠️  Auto EC: Sistema sequencial já ativo - aguardando conclusão");
}
```
✅ **Protección**: No inicia nueva dosagem si hay una activa

---

### 2. **Overdosing** (CRÍTICO)

**Escenario**: `intervalo_auto_ec = 1` + `tempo_recirculacao = 5` segundos

```
T=0s:   Dosagem 1 (10ml) → EC sube de 200 → 250
T=15s:  Dosagem completa
T=20s:  Tempo morto termina (5s)
T=21s:  checkAutoEC() → EC ainda baixo (250 < 1400) → Dosagem 2 (10ml)
T=36s:  Dosagem completa
T=41s:  Tempo morto termina
T=42s:  Dosagem 3 (10ml)
...
```

**Problema**:
- EC no sube instantáneamente después de dosificar
- Si `tempo_recirculacao` es muy bajo, dosifica antes de que EC se estabilice
- Puede dosificar múltiples veces seguidas → **OVERDOSE**

**Código actual** (línea 567):
```cpp
if (lastDosageCompleteTime > 0 && tempoRecirculacao > 0) {
    unsigned long elapsedSeconds = (millis() - lastDosageCompleteTime) / 1000;
    if (elapsedSeconds < tempoRecirculacao) {
        return;  // Ainda em tempo morto - não medir EC
    }
}
```
✅ **Protección**: `tempo_recirculacao` previene overdosing

---

### 3. **Heap Fragmentation** (ALTO RIESGO)

**Escenario**: `intervalo_auto_ec = 1` + múltiples dosagens

**Cada dosagem crea**:
- Objetos `SimpleNutrient` (array)
- Cálculos temporales (DynamicJsonDocument si hay nutrientes)
- Logs (String objects)

**Problema**:
- Si hay muchas dosagens seguidas, heap se fragmenta
- Ya vimos que bajo carga, heap baja de 98KB → 57KB
- SSL necesita ~40KB → **FALLA** cuando heap < 60KB

**Evidencia del log**:
```
Heap normal: 98KB (32.9%)
Heap bajo carga: 57KB (19.5%) ← SSL falla aquí
[851167][E][ssl_client.cpp:37] SSL - Memory allocation failed
```

---

### 4. **Task Starvation** (MEDIO RIESGO)

**Escenario**: `intervalo_auto_ec = 1` + dosagem activa

**DosingTask** (Core 1, prioridad 3):
- Procesa dosagem cada 20ms cuando activa
- Usa mutex con timeout de 10ms
- Si mutex está ocupado, retorna (no bloquea)

**Problema**:
- Si `checkAutoEC()` se ejecuta muy frecuentemente, puede competir por recursos
- Aunque no bloquea (timeout 10ms), desperdicia CPU

**Código actual** (línea 1349):
```cpp
if (xSemaphoreTake(dosingMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;  // Não conseguiu mutex, tentar novamente
}
```
✅ **Protección**: Timeout previene deadlock

---

### 5. **Watchdog Reboot** (BAJO RIESGO, pero posible)

**Escenario**: `intervalo_auto_ec = 1` + heap fragmentado + SSL fallando

**Causa**:
- Múltiples verificaciones cada segundo
- Heap fragmentado → SSL falla
- HTTP requests bloquean loop principal
- Si loop principal se bloquea > 5 segundos → **Watchdog reboot**

**Evidencia del log**:
```
⚠️ [NETWORK_WDT] Fallo #4 consecutivo
```

---

## 📋 Recomendaciones

### Valores Seguros:
- **`intervalo_auto_ec`**: Mínimo **3 segundos** (recomendado: 5-10 segundos)
- **`tempo_recirculacao`**: Mínimo **30 segundos** (recomendado: 60 segundos)

### Valores Peligrosos:
- **`intervalo_auto_ec = 1`**: ⚠️ Solo para testing, no producción
- **`tempo_recirculacao < 30`**: ⚠️ Riesgo de overdosing

### Si necesitas `intervalo_auto_ec = 1`:
1. Aumentar `tempo_recirculacao` a 60+ segundos
2. Monitorear heap (debe mantenerse >60KB)
3. Verificar que no hay múltiples dosagens seguidas
4. Usar solo para testing, no producción

---

## 🧪 Test de Fragilidad

### Test 1: `intervalo_auto_ec = 1` + `tempo_recirculacao = 60`
**Esperado**: Sistema estable, pero muchas verificaciones inútiles

### Test 2: `intervalo_auto_ec = 1` + `tempo_recirculacao = 5`
**Esperado**: ⚠️ Riesgo de overdosing, múltiples dosagens seguidas

### Test 3: `intervalo_auto_ec = 1` + múltiples comandos de relés
**Esperado**: ⚠️ Heap baja → SSL falla → HTTP timeouts

---

## ✅ Conclusión

**`intervalo_auto_ec = 1` es FRÁGIL pero NO causa crash inmediato** porque:
1. ✅ Protección contra múltiples dosagens (`currentState != IDLE`)
2. ✅ Protección contra overdosing (`tempo_recirculacao`)
3. ✅ Mutex con timeout (no bloquea)
4. ⚠️ PERO: Fragmenta heap y puede causar SSL failures

**Recomendación**: Usar mínimo 3 segundos en producción.

