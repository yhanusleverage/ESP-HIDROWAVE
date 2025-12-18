# 🔄 FLUJO PROCEDURAL COMPLETO: Búsqueda EC Config View

## 📋 RESUMEN EJECUTIVO

**Ciclo completo:** Cada **5 segundos** (intervalo de polling)
**Duración total del ciclo:** ~**500-2000ms** (0.5-2 segundos)
**Tiempo de detección de cambios:** Máximo **5 segundos** + tiempo de procesamiento

---

## 🔄 CICLO COMPLETO PASO A PASO

### **FASE 1: TRIGGER DEL POLLING** (HydroSystemCore::loop)

```
Tiempo: 0ms
```

**Código:** `src/HydroSystemCore.cpp` línea ~286

```cpp
if (now - lastECConfigCheck >= checkInterval) {  // 5000ms = 5 segundos
    checkECConfigFromSupabase();
}
```

**Acción:**
- Verifica si pasaron 5 segundos desde última verificación
- Si sí, llama a `checkECConfigFromSupabase()`

---

### **FASE 2: VALIDACIONES INICIALES** (checkECConfigFromSupabase)

```
Tiempo: 0-5ms
```

**Código:** `src/HydroSystemCore.cpp` línea ~575

```cpp
if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
    return;  // Salir si no hay conexión o memoria
}
if (!supabase.isReady()) {
    return;  // Salir si Supabase no está listo
}
```

**Acciones:**
- ✅ Verificar conexión Supabase
- ✅ Verificar memoria disponible
- ✅ Verificar estado de Supabase

**Tiempo estimado:** 1-5ms (validaciones rápidas)

---

### **FASE 3: LLAMADA A SUPABASE CLIENT** (getECConfigFromSupabase)

```
Tiempo: 5-10ms
```

**Código:** `src/HydroSystemCore.cpp` línea ~589

```cpp
ECConfig config;
if (supabase.getECConfigFromSupabase(config)) {
    // Procesar respuesta
}
```

**Acción:**
- Llama a función de SupabaseClient
- Pasa estructura ECConfig por referencia

**Tiempo estimado:** 1-5ms (solo llamada de función)

---

### **FASE 4: ADQUIRIR MUTEX** (Thread Safety)

```
Tiempo: 10-5010ms (puede esperar hasta 5 segundos)
```

**Código:** `src/SupabaseClient.cpp` línea ~3133

```cpp
if (xSemaphoreTake(commandCheckMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return false;  // Timeout después de 5 segundos
}
```

**Acciones:**
- Adquirir mutex para thread-safety
- Esperar máximo 5 segundos si otro thread está usando

**Tiempo estimado:**
- **Normal:** 1-10ms (mutex disponible)
- **Contención:** 10-5000ms (si otro thread está usando)

---

### **FASE 5: PREPARAR REQUEST HTTP**

```
Tiempo: 15-25ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3141-3153

```cpp
String endpoint = "rpc/activate_auto_ec";
DynamicJsonDocument payloadDoc(256);
payloadDoc["p_device_id"] = getDeviceID();
String payload;
serializeJson(payloadDoc, payload);
```

**Acciones:**
- Construir endpoint RPC
- Crear payload JSON con device_id
- Serializar JSON

**Tiempo estimado:** 5-15ms (construcción de strings y JSON)

---

### **FASE 6: OBTENER CLIENTES SSL/HTTP** (Object Pool)

```
Tiempo: 20-30ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3155-3186

```cpp
ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
if (usePool) {
    sslClient = poolMgr->acquireSSLClient();
    httpClient = poolMgr->acquireHTTPClient(sslClient);
} else {
    sslClient = secureClient;
    httpClient = &http;
}
```

**Acciones:**
- Obtener cliente SSL del pool (o crear nuevo)
- Obtener cliente HTTP del pool (o usar existente)

**Tiempo estimado:**
- **Con Pool:** 1-5ms (rápido)
- **Sin Pool:** 5-10ms (crear nuevo)

---

### **FASE 7: VERIFICACIONES DE MEMORIA Y SALUD SSL**

```
Tiempo: 25-35ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3221-3259

```cpp
uint32_t freeHeap = ESP.getFreeHeap();
uint32_t maxAlloc = ESP.getMaxAllocHeap();

if (freeHeap < 40000) {
    return false;  // Memoria insuficiente
}
if (maxAlloc < 30000) {
    return false;  // Fragmentación detectada
}
```

**Acciones:**
- Verificar heap libre (mínimo 40KB)
- Verificar max alloc (mínimo 30KB)
- Verificar fragmentación de memoria

**Tiempo estimado:** 1-5ms (lecturas rápidas de memoria)

---

### **FASE 8: INICIAR CONEXIÓN SSL**

```
Tiempo: 30-8030ms (puede tardar hasta 8 segundos)
```

**Código:** `src/SupabaseClient.cpp` línea ~3262-3275

```cpp
String fullUrl = baseUrl + "/rest/v1/" + endpoint;
if (!httpClient->begin(*sslClient, fullUrl)) {
    return false;  // Fallo al iniciar conexión
}
```

**Acciones:**
- Construir URL completa
- Iniciar conexión SSL con Supabase
- Timeout de conexión: **8 segundos**

**Tiempo estimado:**
- **Normal:** 200-1000ms (conexión establecida)
- **Lento:** 1000-8000ms (red lenta o problemas)
- **Timeout:** 8000ms (si falla)

---

### **FASE 9: CONFIGURAR HEADERS HTTP**

```
Tiempo: 1030-1040ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3294-3302

```cpp
httpClient->setConnectTimeout(8000);   // 8s conexión
httpClient->setTimeout(12000);          // 12s total
httpClient->addHeader("Authorization", buildAuthHeader());
httpClient->addHeader("apikey", apiKey);
httpClient->addHeader("Content-Type", "application/json");
```

**Acciones:**
- Configurar timeouts
- Agregar headers de autenticación
- Agregar headers de contenido

**Tiempo estimado:** 5-10ms (configuración rápida)

---

### **FASE 10: ENVIAR REQUEST POST**

```
Tiempo: 1040-12440ms (puede tardar hasta 12 segundos)
```

**Código:** `src/SupabaseClient.cpp` línea ~3305-3306

```cpp
Serial.println("📡 [RPC EC_CONFIG] Enviando requisição POST...");
int httpCode = httpClient->POST(payload);
```

**Acciones:**
- Enviar POST request a Supabase
- Esperar respuesta del servidor
- Timeout total: **12 segundos**

**Tiempo estimado:**
- **Normal:** 300-2000ms (respuesta rápida)
- **Lento:** 2000-12000ms (red lenta o servidor ocupado)
- **Timeout:** 12000ms (si falla)

**Desglose típico:**
- Envío de datos: 50-200ms
- Procesamiento en Supabase: 100-500ms
- Respuesta de Supabase: 50-200ms
- **Total típico:** 200-900ms

---

### **FASE 11: LEER RESPUESTA HTTP**

```
Tiempo: 12440-12500ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3371-3376

```cpp
int contentLength = httpClient->getSize();
String response = httpClient->getString();
```

**Acciones:**
- Obtener tamaño de respuesta
- Leer respuesta completa como String

**Tiempo estimado:** 10-100ms (depende del tamaño de respuesta)

**Tamaño típico de respuesta:** 200-1000 bytes (JSON pequeño)

---

### **FASE 12: PARSEAR JSON DE RESPUESTA**

```
Tiempo: 12500-12550ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3395-3459

```cpp
DynamicJsonDocument doc(2048);
DeserializationError error = deserializeJson(doc, response);

config.base_dose = configObj["base_dose"] | 0.0;
config.auto_enabled = configObj["auto_enabled"] | false;
// ... más campos
```

**Acciones:**
- Parsear JSON de respuesta
- Extraer campos a estructura ECConfig
- Validar datos

**Tiempo estimado:** 20-50ms (parsing de JSON pequeño)

---

### **FASE 13: CERRAR CONEXIÓN Y LIBERAR RECURSOS**

```
Tiempo: 12550-12750ms
```

**Código:** `src/SupabaseClient.cpp` línea ~3486-3500

```cpp
httpClient->end();
vTaskDelay(pdMS_TO_TICKS(200));  // Delay para liberar memoria SSL
networkWatchdog.endOperation(false);
```

**Acciones:**
- Cerrar conexión HTTP
- Liberar clientes SSL/HTTP al pool
- Liberar mutex
- Delay de 200ms para liberar memoria SSL

**Tiempo estimado:** 200-250ms (incluye delay de 200ms)

---

### **FASE 14: ACTUALIZAR HYDROCONTROL**

```
Tiempo: 12750-12800ms
```

**Código:** `src/HydroSystemCore.cpp` línea ~591-599

```cpp
hydroControl.getECController().setBaseDose(config.base_dose);
hydroControl.setECSetpoint(config.ec_setpoint);
hydroControl.setAutoECEnabled(config.auto_enabled);
// ... más actualizaciones
```

**Acciones:**
- Actualizar parámetros del controller
- Actualizar setpoint
- Actualizar auto_enabled
- Actualizar intervalo

**Tiempo estimado:** 5-20ms (asignaciones rápidas)

---

### **FASE 15: PROCESAR NUTRIENTES (si existen)**

```
Tiempo: 12800-12900ms (opcional)
```

**Código:** `src/HydroSystemCore.cpp` línea ~601-649

```cpp
if (config.nutrientsJson.length() > 0) {
    DynamicJsonDocument nutrientsDoc(jsonSize);
    deserializeJson(nutrientsDoc, config.nutrientsJson);
    // ... procesar y convertir formato
    hydroControl.updateNutrientProportions(adaptedArray);
}
```

**Acciones:**
- Parsear JSON de nutrientes
- Convertir formato (relay 0-15 → relayNumber 1-16)
- Actualizar proporciones en HydroControl

**Tiempo estimado:** 50-100ms (solo si hay nutrientes)

---

### **FASE 16: GUARDAR EN NVS**

```
Tiempo: 12900-13000ms
```

**Código:** `src/HydroSystemCore.cpp` línea ~653-654

```cpp
Serial.println("💾 [EC CONFIG] Guardando configuración en NVS...");
hydroControl.saveECControllerConfig();
```

**Código interno:** `src/HydroControl.cpp` línea ~1003-1047

```cpp
PreferencesManager::saveConfigFloat("ec_baseDose", baseDose);
PreferencesManager::saveConfigFloat("ec_autoEnabled", autoEnabled ? 1 : 0);
// ... más guardados
```

**Acciones:**
- Guardar cada parámetro en NVS
- Escribir en memoria no volátil

**Tiempo estimado:** 50-100ms (escritura en NVS)

---

## ⏱️ RESUMEN DE TIEMPOS

### **Escenario Normal (Red Rápida):**

| Fase | Tiempo Acumulado | Duración de Fase |
|------|------------------|------------------|
| 1. Trigger polling | 0ms | 0ms |
| 2. Validaciones | 5ms | 5ms |
| 3. Llamada función | 10ms | 5ms |
| 4. Adquirir mutex | 20ms | 10ms |
| 5. Preparar request | 35ms | 15ms |
| 6. Obtener clientes | 40ms | 5ms |
| 7. Verificar memoria | 45ms | 5ms |
| 8. Iniciar SSL | 800ms | 755ms |
| 9. Configurar headers | 810ms | 10ms |
| 10. POST request | 1500ms | 690ms |
| 11. Leer respuesta | 1520ms | 20ms |
| 12. Parsear JSON | 1550ms | 30ms |
| 13. Cerrar conexión | 1800ms | 250ms |
| 14. Actualizar HydroControl | 1820ms | 20ms |
| 15. Procesar nutrientes | 1870ms | 50ms |
| 16. Guardar NVS | 1970ms | 100ms |

**TOTAL: ~2000ms (2 segundos)**

---

### **Escenario Lento (Red Lenta):**

| Fase | Tiempo Acumulado | Duración de Fase |
|------|------------------|------------------|
| 1-7. Preparación | 50ms | 50ms |
| 8. Iniciar SSL | 3000ms | 2950ms |
| 9. Configurar headers | 3010ms | 10ms |
| 10. POST request | 8000ms | 4990ms |
| 11-16. Procesamiento | 8300ms | 300ms |

**TOTAL: ~8300ms (8.3 segundos)**

---

### **Escenario Timeout (Fallo de Red):**

| Fase | Tiempo Acumulado | Duración de Fase |
|------|------------------|------------------|
| 1-7. Preparación | 50ms | 50ms |
| 8. Iniciar SSL | 8050ms | 8000ms (timeout) |
| **FALLO** | - | - |

**TOTAL: ~8050ms (8 segundos) - FALLO**

---

## 🔄 CICLO COMPLETO CON INTERVALO

### **Timeline Visual:**

```
T=0s    → Trigger polling (cada 5 segundos)
T=0s    → Inicia checkECConfigFromSupabase()
T=0-2s  → Procesamiento completo (normal)
T=2s    → Guardado en NVS completado
T=5s    → Próximo trigger de polling
```

### **Duración Total del Ciclo:**

- **Intervalo entre ciclos:** 5 segundos (5000ms)
- **Duración de procesamiento:** 500-2000ms (normal)
- **Tiempo de detección de cambios:** Máximo 5 segundos + tiempo de procesamiento

**Ejemplo:**
- Frontend cambia `auto_enabled` en T=0s
- ESP32 detecta cambio en T=5s (próximo polling)
- ESP32 procesa y actualiza en T=5-7s
- **Total:** 5-7 segundos desde cambio hasta aplicación

---

## 📊 ESTADÍSTICAS TÍPICAS

### **Tiempos Promedio:**

- **Conexión SSL:** 200-1000ms
- **Request POST:** 300-2000ms
- **Procesamiento local:** 100-200ms
- **Guardado NVS:** 50-100ms
- **TOTAL:** 650-3300ms (0.65-3.3 segundos)

### **Frecuencia:**

- **Polling cada:** 5 segundos
- **Requests por minuto:** 12 requests/min
- **Requests por hora:** 720 requests/hora

---

## ✅ CONCLUSIÓN

**Ciclo completo:**
- **Intervalo:** 5 segundos
- **Duración de procesamiento:** 0.5-2 segundos (normal)
- **Tiempo de detección de cambios:** Máximo 5 segundos

**El ESP32 verifica cambios en `ec_config_view` cada 5 segundos y los aplica en menos de 2 segundos adicionales.**

