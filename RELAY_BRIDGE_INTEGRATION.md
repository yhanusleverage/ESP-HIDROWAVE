# 🌉 RelayBridge - Guía de Integración

## 📋 **¿Qué es RelayBridge?**

**RelayBridge** es la **capa de traducción** entre Supabase y ESP-NOW que permite:

1. ✅ Recibir comandos del dashboard Next.js via Supabase
2. ✅ Convertir formato Supabase → ESP-NOW
3. ✅ Enviar comandos a slaves via ESP-NOW
4. ✅ Actualizar estados en Supabase
5. ✅ Sincronización automática

---

## 🔄 **Flujo Completo:**

```
┌─────────────────┐
│   Next.js       │ Usuario crea comando
│   Dashboard     │ "Ligar relé 3 por 10s"
└────────┬────────┘
         │ HTTP POST
         ▼
┌─────────────────┐
│   SUPABASE      │ INSERT INTO relay_commands
│   Database      │ {id: 1, relayNumber: 3, 
└────────┬────────┘  action: "on", duration: 10}
         │
         │ Polling (cada 5s)
         ▼
┌─────────────────┐
│  RelayBridge    │ 1. Lee comando "pending"
│  (Master ESP32) │ 2. Convierte a ESPNowRelayCommand
└────────┬────────┘ 3. Calcula checksum
         │
         │ ESP-NOW
         ▼
┌─────────────────┐
│  ESPNowTask     │ Envía comando via ESP-NOW
│  (Master ESP32) │ Canal 6, MAC del slave
└────────┬────────┘
         │
         │ ESP-NOW (wireless)
         ▼
┌─────────────────┐
│ ESPNowTaskSlave │ 1. Recibe comando
│ (Slave ESP32)   │ 2. Valida checksum
└────────┬────────┘ 3. Ejecuta en relé físico
         │
         │ GPIO
         ▼
┌─────────────────┐
│  Relé Físico    │ ⚡ Acción ejecutada
│  (Hardware)     │ Relé 3 → ON por 10s
└─────────────────┘
         │
         │ Respuesta
         ▼
┌─────────────────┐
│  RelayBridge    │ Actualiza Supabase:
│  (Master ESP32) │ status: "completed"
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   SUPABASE      │ UPDATE relay_commands
│   Database      │ SET status = 'completed'
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Next.js       │ Usuario ve estado
│   Dashboard     │ "✅ Comando completado"
└─────────────────┘
```

---

## 🔧 **Integración en main.cpp:**

### **1. Incluir el header:**

```cpp
#include "RelayBridge.h"
```

### **2. Declarar instancia global:**

```cpp
// Variables globales
SupabaseClient* supabaseClient = nullptr;
ESPNowTask* espNowTask = nullptr;
RelayBridge* relayBridge = nullptr;  // ← NUEVO
```

### **3. Inicializar en setup():**

```cpp
void setup() {
    // ... código existente ...
    
    // Inicializar Supabase
    supabaseClient = new SupabaseClient();
    if (supabaseClient->begin(SUPABASE_URL, SUPABASE_KEY)) {
        Serial.println("✅ Supabase conectado");
    }
    
    // Inicializar ESP-NOW Task
    espNowTask = new ESPNowTask();
    if (espNowTask->begin()) {
        Serial.println("✅ ESP-NOW Task inicializado");
    }
    
    // Inicializar RelayBridge
    relayBridge = new RelayBridge(supabaseClient, espNowTask);
    if (relayBridge->begin()) {
        Serial.println("✅ RelayBridge inicializado");
        relayBridge->setAutoProcessing(true);  // Habilitar procesamiento automático
    }
    
    // ... resto del código ...
}
```

### **4. Actualizar en loop():**

```cpp
void loop() {
    // ... código existente ...
    
    // Actualizar RelayBridge (procesa comandos de Supabase)
    if (relayBridge) {
        relayBridge->update();
    }
    
    // ... resto del código ...
}
```

### **5. Comandos seriales opcionales:**

```cpp
void handleMasterSerialCommands() {
    // ... comandos existentes ...
    
    else if (command == "bridge_stats") {
        if (relayBridge) {
            relayBridge->printStats();
        }
    }
    else if (command == "bridge_enable") {
        if (relayBridge) {
            relayBridge->setAutoProcessing(true);
        }
    }
    else if (command == "bridge_disable") {
        if (relayBridge) {
            relayBridge->setAutoProcessing(false);
        }
    }
}
```

---

## 📊 **Estructura de Datos:**

### **Supabase (RelayCommand):**
```cpp
struct RelayCommand {
    int id;                    // ID único del comando
    int relayNumber;           // 0-15
    String action;             // "on", "off", "toggle", "on_forever"
    int durationSeconds;       // Duración en segundos
    String status;             // "pending", "sent", "completed", "failed"
    unsigned long timestamp;
};
```

### **ESP-NOW (ESPNowRelayCommand):**
```cpp
struct ESPNowRelayCommand {
    uint8_t relayNumber;      // 0-7 (1 byte)
    char action[16];          // "on", "off", etc. (16 bytes)
    uint32_t duration;        // Segundos (4 bytes)
    uint8_t checksum;         // Validación (1 byte)
};
// Total: 22 bytes (eficiente para ESP-NOW)
```

---

## 🎯 **Conversión Automática:**

El RelayBridge hace la conversión automáticamente:

```cpp
// Supabase → ESP-NOW
RelayCommand supaCmd = {
    .id = 1,
    .relayNumber = 3,
    .action = "on",
    .durationSeconds = 10,
    .status = "pending"
};

// RelayBridge convierte a:
ESPNowRelayCommand espCmd = {
    .relayNumber = 3,
    .action = "on",
    .duration = 10,
    .checksum = 0xAB  // Calculado automáticamente
};
```

---

## ⚙️ **Configuración:**

### **Intervalo de Polling:**
Por defecto: **5 segundos**

Cambiar en `RelayBridge.cpp`:
```cpp
checkInterval(5000)  // 5000ms = 5 segundos
```

### **Máximo de Comandos por Polling:**
Por defecto: **10 comandos**

Cambiar en `RelayBridge.cpp`:
```cpp
const int MAX_COMMANDS = 10;
```

---

## 🔍 **Debugging:**

### **Ver estadísticas:**
```
bridge_stats
```

**Salida:**
```
📊 === RELAY BRIDGE STATS ===
Estado: ✅ Activo
Comandos procesados: 25
Comandos enviados: 23
Comandos completados: 20
Comandos fallidos: 2
Intervalo de polling: 5000ms
=============================
```

### **Logs automáticos:**
El RelayBridge imprime logs detallados:
```
🔔 3 comando(s) pendiente(s) en Supabase

📦 Procesando comando #1
   Relé: 3
   Acción: on
   Duración: 10s
✅ Comando enviado via ESP-NOW
✅ Comando #1 | Relé: 3 | Acción: on | Duración: 10s
```

---

## 🚀 **Ventajas:**

1. ✅ **Automático** - No requiere intervención manual
2. ✅ **Eficiente** - Polling cada 5s (configurable)
3. ✅ **Robusto** - Manejo de errores y reintentos
4. ✅ **Trazable** - Logs detallados de cada comando
5. ✅ **Escalable** - Procesa múltiples comandos en batch
6. ✅ **Sincronizado** - Estados actualizados en tiempo real

---

## 📝 **Próximos Pasos:**

1. ✅ Integrar en main.cpp
2. ✅ Compilar y probar
3. ✅ Crear comandos desde Next.js
4. ✅ Verificar ejecución en slaves
5. ✅ Monitorear estados en Supabase

---

## 🎯 **Resultado Esperado:**

```
Usuario (Next.js) → Crea comando
     ↓
Supabase → Guarda comando (pending)
     ↓
RelayBridge → Lee comando (polling)
     ↓
ESP-NOW → Envía a slave
     ↓
Slave → Ejecuta en relé físico
     ↓
RelayBridge → Actualiza Supabase (completed)
     ↓
Usuario (Next.js) → Ve confirmación ✅
```

---

**¡Sistema completo de automatización funcionando! 🎉**

