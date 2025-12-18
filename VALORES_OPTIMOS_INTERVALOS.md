# ⚙️ Valores Óptimos de Intervalos y Manejo de Recursos

## 📊 Los 3 Intervalos Diferentes

### 1. **`intervalSeconds`** (Entre Nutrientes)
- **Qué es**: Tiempo de espera entre cada nutriente en una secuencia de dosagem
- **Valor actual**: 3 segundos (hardcoded)
- **Ejemplo**: Dosificar grow → esperar 3s → dosificar micro → esperar 3s → dosificar bloom
- **Recomendado**: **3 segundos** ✅ (ya está bien)

### 2. **`intervalo_auto_ec`** (Entre Verificaciones del Control)
- **Qué es**: Intervalo entre verificaciones del control proporcional EC
- **Valor actual**: 3 segundos
- **Ejemplo**: Cada 3 segundos verifica si EC < setpoint y calcula si necesita dosificar
- **⚠️ IMPORTANTE**: Solo se aplica DESPUÉS de que termine `tempo_recirculacao`
- **Recomendado**: **5 minutos (300 segundos)** ⚠️ Para reducir carga

### 3. **`tempo_recirculacao`** (Tiempo Muerto Después de Dosagem)
- **Qué es**: Tiempo de espera DESPUÉS de una dosagem completa antes de verificar EC de nuevo
- **Valor actual**: 600 segundos (10 minutos)
- **Ejemplo**: Dosagem completa → esperar 10 minutos → verificar EC de nuevo
- **⚠️ IMPORTANTE**: Este es un BLOQUEO ABSOLUTO - durante este tiempo, `intervalo_auto_ec` NO se aplica
- **Recomendado**: **10 minutos (600 segundos)** ✅ (ya está bien)

### 🔍 **¿Son Redundantes?**
**SÍ, son prácticamente lo mismo en la práctica**:

```
Flujo Real en el código:
1. checkAutoEC() se llama cada loop
2. PRIMERO verifica tempo_recirculacao (línea 567):
   - Si está en tiempo muerto → return INMEDIATAMENTE
   - NO verifica intervalo_auto_ec
3. DESPUÉS verifica intervalo_auto_ec (línea 600):
   - Solo si NO está en tiempo muerto
```

**Problema**: Si `tempo_recirculacao = 600` y `intervalo_auto_ec = 3`:
- Después de dosificar, espera 10 minutos (bloqueo absoluto)
- Durante esos 10 minutos, `intervalo_auto_ec` NO se aplica (se ignora)
- Después de 10 minutos, verifica cada 3 segundos
- **El `intervalo_auto_ec = 3` solo importa después del tiempo muerto**

**Conclusión**: Son redundantes. Si `tempo_recirculacao` es alto (600s), el `intervalo_auto_ec` solo importa después del tiempo muerto.

**Recomendación Simplificada**:
- **Usar solo `tempo_recirculacao`** como intervalo principal
- **`intervalo_auto_ec` puede ser igual o mayor** que `tempo_recirculacao`
- **Ejemplo óptimo**: `tempo_recirculacao = 600`, `intervalo_auto_ec = 300` (5 min)

---

## 🎯 Valores Óptimos Recomendados

### Para Producción (Estable):
```json
{
  "intervalo_auto_ec": 300,        // 5 minutos entre verificaciones
  "tempo_recirculacao": 600,       // 10 minutos después de dosagem
  "intervalSeconds": 3              // 3 segundos entre nutrientes (hardcoded)
}
```

### Para Testing (Más Rápido):
```json
{
  "intervalo_auto_ec": 30,          // 30 segundos entre verificaciones
  "tempo_recirculacao": 60,         // 1 minuto después de dosagem
  "intervalSeconds": 3              // 3 segundos entre nutrientes
}
```

### Para Debug (Muy Rápido - ⚠️ FRÁGIL):
```json
{
  "intervalo_auto_ec": 3,           // 3 segundos (actual)
  "tempo_recirculacao": 30,         // 30 segundos
  "intervalSeconds": 3              // 3 segundos
}
```

---

## 🔧 Optimización de Recursos (Queue, Mutex, Object Pool)

### Problema Actual:
- **Heap fragmentado** → SSL falla cuando heap < 60KB
- **Múltiples verificaciones** → CPU waste
- **NVS guardando 4 veces** → Flash wear

### Soluciones Propuestas:

#### 1. **Aumentar `intervalo_auto_ec` a 5 minutos**
**Beneficio**: Reduce verificaciones de 20/min → 0.2/min (100x menos carga)

**Código a modificar**: `src/HydroSystemCore.cpp` línea 287
```cpp
// ACTUAL (cada 5 segundos):
int intervalSeconds = 5;

// OPTIMIZADO (cada 5 minutos):
int intervalSeconds = 300;  // 5 minutos
```

#### 2. **Optimizar NVS - Guardar Solo Una Vez**
**Beneficio**: Reduce flash wear y fragmentación heap

**Código a modificar**: `src/HydroSystemCore.cpp` líneas 601-604
```cpp
// ACTUAL: Cada setter guarda automáticamente
hydroControl.setECSetpoint(config.ec_setpoint);      // → guarda
hydroControl.setAutoECEnabled(config.auto_enabled);   // → guarda
hydroControl.setAutoECInterval(config.intervalo_auto_ec); // → guarda
hydroControl.saveECControllerConfig();                // → guarda (4 veces total)

// OPTIMIZADO: Guardar solo al final
hydroControl.setECSetpoint(config.ec_setpoint, false);      // false = no guardar
hydroControl.setAutoECEnabled(config.auto_enabled, false); // false = no guardar
hydroControl.setAutoECInterval(config.intervalo_auto_ec, false); // false = no guardar
hydroControl.saveECControllerConfig();                // → guarda UNA vez
```

#### 3. **Optimizar Mutex Timeout**
**Beneficio**: Reduce CPU waste cuando mutex está ocupado

**Código actual**: `src/HydroControl.cpp` línea 1349
```cpp
if (xSemaphoreTake(dosingMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;  // Timeout 10ms
}
```
✅ **Ya está optimizado** - timeout corto previene bloqueos

#### 4. **Optimizar Object Pool para HTTPClient**
**Beneficio**: Reduce fragmentación heap

**Verificar**: `src/ObjectPoolManager.cpp` y `src/SupabaseClient.cpp`
- Asegurar que HTTPClient se libera correctamente después de cada request
- Verificar que no hay leaks de memoria

#### 5. **Agregar Circuit Breaker para SSL Failures**
**Beneficio**: Evita intentar SSL cuando heap es bajo

**Implementar en**: `src/SupabaseClient.cpp`
```cpp
// Si heap < 60KB, no intentar SSL
if (ESP.getFreeHeap() < 60000) {
    Serial.println("⚠️ [SSL] Heap muy bajo - saltando request");
    return false;  // Circuit breaker
}
```

---

## 📋 Plan de Implementación

### Prioridad Alta (Crítico):
1. ✅ **Aumentar `intervalo_auto_ec` a 300 segundos (5 minutos)**
   - Impacto: Reduce carga 100x
   - Riesgo: Bajo (solo cambia frecuencia de verificación)

2. ✅ **Optimizar NVS - Guardar solo una vez**
   - Impacto: Reduce flash wear y fragmentación
   - Riesgo: Bajo (solo cambia cuándo se guarda)

### Prioridad Media:
3. ⚠️ **Agregar Circuit Breaker para SSL**
   - Impacto: Previene SSL failures cuando heap es bajo
   - Riesgo: Medio (necesita testing)

4. ⚠️ **Verificar Object Pool leaks**
   - Impacto: Reduce fragmentación heap
   - Riesgo: Medio (requiere análisis profundo)

### Prioridad Baja:
5. ℹ️ **Optimizar logs verbosos**
   - Impacto: Reduce CPU/Serial overhead
   - Riesgo: Bajo

---

## 🧪 Test de Valores Óptimos

### Test 1: `intervalo_auto_ec = 300` (5 minutos)
**Esperado**:
- Verificaciones: 0.2/min (vs 20/min actual)
- Heap: Se mantiene >80KB
- SSL: No falla
- Latencia: Aceptable (5 min es razonable para hidroponía)

### Test 2: `intervalo_auto_ec = 60` (1 minuto)
**Esperado**:
- Verificaciones: 1/min
- Heap: Se mantiene >70KB
- SSL: Ocasionalmente falla bajo carga alta
- Latencia: Buena (1 min es rápido)

### Test 3: `intervalo_auto_ec = 30` (30 segundos)
**Esperado**:
- Verificaciones: 2/min
- Heap: Puede bajar a 60KB bajo carga
- SSL: Puede fallar si hay otros procesos
- Latencia: Muy buena

---

## ✅ Recomendación Final (SIMPLIFICADA)

**Para Producción**:
```json
{
  "intervalo_auto_ec": 300,        // 5 minutos - Solo se aplica DESPUÉS de tempo_recirculacao
  "tempo_recirculacao": 600,       // 10 minutos - BLOQUEO ABSOLUTO después de dosagem
  "intervalSeconds": 3             // 3 segundos - Hardcoded, está bien
}
```

**Justificación Simplificada**:
- **`tempo_recirculacao = 600`** es el intervalo REAL (bloqueo absoluto después de dosagem)
- **`intervalo_auto_ec = 300`** solo importa después del tiempo muerto (puede ser igual o mayor)
- **En la práctica**: Después de dosificar → espera 10 min → luego verifica cada 5 min
- Reduce carga del sistema significativamente
- Mantiene heap >80KB
- Previene SSL failures

**Alternativa Más Simple** (si quieres un solo intervalo):
```json
{
  "intervalo_auto_ec": 600,        // Igual que tempo_recirculacao
  "tempo_recirculacao": 600,       // Mismo valor
  "intervalSeconds": 3             // 3 segundos
}
```
**Resultado**: Verifica cada 10 minutos (después del tiempo muerto)

---

## 🔍 Monitoreo Recomendado

Después de cambiar `intervalo_auto_ec` a 300 segundos, monitorear:

1. **Heap libre**: Debe mantenerse >80KB
2. **SSL failures**: Debe ser 0
3. **HTTP timeouts**: Debe ser <1 por minuto
4. **Latencia de respuesta**: EC debe ajustarse en <10 minutos

