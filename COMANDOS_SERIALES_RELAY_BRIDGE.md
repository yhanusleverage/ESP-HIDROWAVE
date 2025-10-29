# 🎮 Comandos Seriales - RelayBridge

## 📋 **LISTA COMPLETA DE COMANDOS**

### **🌉 RELAY BRIDGE (Supabase ↔ ESP-NOW)**

| Comando | Descripción | Ejemplo |
|---------|-------------|---------|
| `bridge_stats` | Ver estadísticas del bridge | `bridge_stats` |
| `bridge_enable` | Habilitar polling automático | `bridge_enable` |
| `bridge_disable` | Deshabilitar polling | `bridge_disable` |

---

## 🔧 **EJEMPLOS PRÁCTICOS:**

### **1. Ver Estadísticas del RelayBridge**

```
> bridge_stats
```

**Salida esperada:**
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

---

### **2. Habilitar Polling Automático**

```
> bridge_enable
```

**Salida esperada:**
```
✅ RelayBridge habilitado - Polling automático ativo
🌉 RelayBridge auto-processing: ✅ Habilitado
```

**¿Qué hace?**
- Inicia el polling automático de Supabase cada 5 segundos
- Lee comandos con status "pending"
- Los convierte y envía via ESP-NOW
- Actualiza estados en Supabase

---

### **3. Deshabilitar Polling Automático**

```
> bridge_disable
```

**Salida esperada:**
```
⚠️ RelayBridge deshabilitado - Polling pausado
🌉 RelayBridge auto-processing: ❌ Deshabilitado
```

**¿Cuándo usar?**
- Para debugging manual
- Para ahorrar recursos temporalmente
- Para mantenimiento del sistema

---

## 🎯 **FLUJO COMPLETO DE PRUEBA:**

### **Paso 1: Verificar Estado Inicial**
```
> bridge_stats
```

### **Paso 2: Habilitar Bridge**
```
> bridge_enable
```

### **Paso 3: Crear Comando en Supabase (desde Next.js)**
```sql
INSERT INTO relay_commands (relayNumber, action, durationSeconds, status)
VALUES (3, 'on', 10, 'pending');
```

### **Paso 4: Observar Logs del ESP32**
```
🔔 1 comando(s) pendiente(s) en Supabase

📦 Procesando comando #1
   Relé: 3
   Acción: on
   Duración: 10s
✅ Comando enviado via ESP-NOW
✅ Comando #1 | Relé: 3 | Acción: on | Duración: 10s
```

### **Paso 5: Verificar en Slave**
```
🔌 Comando de relé recebido de: 20:43:A8:64:47:D0
   Relé: 3
   Ação: on
   Duração: 10s
✅ Comando executado com sucesso
```

### **Paso 6: Verificar Estadísticas**
```
> bridge_stats

📊 === RELAY BRIDGE STATS ===
Estado: ✅ Activo
Comandos procesados: 1
Comandos enviados: 1
Comandos completados: 1
Comandos fallidos: 0
=============================
```

---

## 🔍 **COMANDOS DE DEBUGGING:**

### **Ver Slaves Conectados**
```
> list
```

**Salida:**
```
📋 === SLAVES CONHECIDOS ===
   Total: 2 slave(s)

   🟢 ESP-NOW-SLAVE-1
      Tipo: RelayCommandBox
      MAC: 14:33:5C:38:BF:60
      Status: Online
      Última comunicação: 5 segundos atrás

   🟢 ESP-NOW-SLAVE-2
      Tipo: RelayCommandBox
      MAC: 24:6F:28:A2:C4:80
      Status: Online
      Última comunicação: 3 segundos atrás
===========================
```

### **Testar Conexión con Slave**
```
> ping ESP-NOW-SLAVE-1
```

**Salida:**
```
🏓 Enviando ping para: ESP-NOW-SLAVE-1
🏓 Pong recebido de: ESP-NOW-SLAVE-1 (online)
✅ Slave respondeu em 45ms
```

### **Enviar Comando Manual (sin Supabase)**
```
> relay ESP-NOW-SLAVE-1 3 on 10
```

**Salida:**
```
✅ Comando enviado: ESP-NOW-SLAVE-1 -> Relé 3 on
```

---

## 📊 **MONITOREO CONTINUO:**

### **Script de Monitoreo (copiar y pegar en serial)**
```
bridge_stats
list
```

**Ejecutar cada 30 segundos para monitorear:**
- Estado del bridge
- Comandos procesados
- Slaves conectados
- Latencia de comunicación

---

## ⚙️ **CONFIGURACIÓN AVANZADA:**

### **Cambiar Intervalo de Polling**

Editar en `RelayBridge.cpp`:
```cpp
checkInterval(5000)  // 5000ms = 5 segundos
```

Opciones recomendadas:
- **1000ms** (1s) - Respuesta rápida, más CPU
- **5000ms** (5s) - Balance óptimo ✅
- **10000ms** (10s) - Ahorro de recursos
- **30000ms** (30s) - Modo económico

### **Cambiar Máximo de Comandos por Polling**

Editar en `RelayBridge.cpp`:
```cpp
const int MAX_COMMANDS = 10;
```

Opciones:
- **5** - Sistemas pequeños
- **10** - Recomendado ✅
- **20** - Sistemas grandes
- **50** - Sistemas industriales

---

## 🚨 **TROUBLESHOOTING:**

### **Problema: "RelayBridge não inicializado"**

**Solución:**
1. Verificar que ESP-NOW Task esté inicializado
2. Reiniciar ESP32
3. Verificar logs de inicialización

### **Problema: "Comandos não são processados"**

**Solución:**
1. Verificar que bridge esté habilitado: `bridge_enable`
2. Verificar conexión con Supabase
3. Ver logs: `bridge_stats`

### **Problema: "Comandos enviados pero slave no responde"**

**Solución:**
1. Verificar slave online: `list`
2. Hacer ping: `ping ESP-NOW-SLAVE-1`
3. Verificar canal ESP-NOW (debe ser 6)
4. Verificar que slave esté en modo listening

---

## 📝 **LOGS TÍPICOS:**

### **Inicialización Exitosa:**
```
🌉 === INICIALIZANDO RELAY BRIDGE ===
========================================
✅ RelayBridge inicializado
   ✓ ESP-NOW: Activo
   ⚠️ Supabase: No configurado
   💡 Configure Supabase para polling automático
========================================
```

### **Procesamiento de Comando:**
```
🔔 1 comando(s) pendiente(s) en Supabase

📦 Procesando comando #1
   Relé: 3
   Acción: on
   Duración: 10s
✅ Comando enviado via ESP-NOW
✅ Comando #1 | Relé: 3 | Acción: on | Duración: 10s
```

### **Error de Comando:**
```
📦 Procesando comando #2
   Relé: 99
   Acción: on
   Duración: 0s
❌ Número de relé inválido: 99
❌ Comando inválido
```

---

## 🎯 **COMANDOS RÁPIDOS (Cheat Sheet):**

```bash
# Ver estado
bridge_stats

# Habilitar/Deshabilitar
bridge_enable
bridge_disable

# Ver slaves
list

# Ping a slave
ping ESP-NOW-SLAVE-1

# Comando manual
relay ESP-NOW-SLAVE-1 3 on 10

# Help
help
```

---

**¡Sistema completo de comandos seriales funcionando! 🎉**

