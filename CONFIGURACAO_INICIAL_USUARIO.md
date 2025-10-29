# 🌱 Guía de Configuración Inicial - Sistema Hidropônico ESP32

## 📋 Índice
- [Visión General](#-visión-general)
- [Preparación del Hardware](#-preparación-del-hardware)
- [Configuración del Software](#-configuración-del-software)
- [Primera Conexión](#-primera-conexión)
- [Configuración WiFi](#-configuración-wifi)
- [Configuración de la Base de Datos](#-configuración-de-la-base-de-datos)
- [Verificación del Sistema](#-verificación-del-sistema)
- [Múltiples Dispositivos](#-múltiples-dispositivos)
- [Solución de Problemas](#-solución-de-problemas)

---

## 🎯 Visión General

Este sistema utiliza un **método de configuración automática** que permite que cada ESP32 se identifique de manera única y se configure automáticamente sin intervención manual del código.

### ✨ Características Principales:
- **🆔 ID Automático**: Cada ESP32 genera su propio ID único basado en su MAC address
- **🔌 Auto-Registro**: Los dispositivos se registran automáticamente en la base de datos
- **📶 Configuración WiFi**: Interface web para configurar conexión sin cables
- **🌐 Multi-Dispositivo**: Soporte para múltiples ESP32 simultáneamente
- **📱 Interface Web**: Panel de control accesible desde cualquier navegador

---

## 🔧 Preparación del Hardware

### Componentes Necesarios:
- **ESP32 DevKit** (cualquier modelo compatible)
- **Módulo Relé 16 canales** (PCF8574)
- **Sensores**:
  - DHT22 (temperatura/humedad ambiente)
  - Sensor pH (analógico)
  - Sensor TDS (analógico) 
  - DS18B20 (temperatura del agua)
  - Sensor de nivel de agua
- **Display LCD I2C** (opcional)
- **Cables y protoboard**

### Conexiones del Hardware:
```
📍 SENSORES:
- DHT22 → GPIO 15
- DS18B20 → GPIO 4  
- pH Sensor → GPIO 35 (ADC)
- TDS Sensor → GPIO 34 (ADC)
- Nivel Agua → GPIO 32/33

📍 I2C (SDA=21, SCL=22):
- LCD 16x2 → 0x27
- PCF8574 #1 → 0x20 (Relés 0-7)
- PCF8574 #2 → 0x24 (Relés 8-15)

📍 STATUS:
- LED Interno → GPIO 2
```

---

## 💻 Configuración del Software

### 1. Preparar el Entorno de Desarrollo

```bash
# Instalar PlatformIO
pip install platformio

# Clonar el proyecto
git clone <tu-repositorio>
cd ESP-HIDROWAVE-main
```

### 2. Configurar PlatformIO

El archivo `platformio.ini` ya está configurado. Solo verifica:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
```

### 3. Compilar y Subir el Firmware

```bash
# Compilar el proyecto
pio run

# Subir al ESP32
pio run --target upload

# Abrir monitor serial
pio device monitor
```

---

## 🚀 Primera Conexión

### Paso 1: Encender el ESP32

Al encender por primera vez, verás en el Serial Monitor:

```
🚀 === ESP32 HIDROPÔNICO v3.0 ===
🏗️ Arquitetura: Estados Exclusivos
💾 Heap inicial: 180000 bytes
🆔 Device ID: ESP32_HIDRO_A1B2C3  ← ID único generado
📶 MAC Address: 24:6F:28:A1:B2:C3
==================================

🔧 Modo: WiFi Config (primeira vez)
📡 Access Point criado: ESP32-Hidro-Config
🔑 Senha: 12345678
🌐 IP: 192.168.4.1
```

### Paso 2: Identificar tu Device ID

**¡IMPORTANTE!** Anota el **Device ID** que aparece en el monitor serial:
- Formato: `ESP32_HIDRO_XXXXXX`
- Ejemplo: `ESP32_HIDRO_A1B2C3`
- Este ID es **único** para cada ESP32 y se basa en su MAC address

---

## 📶 Configuración WiFi

### Método 1: Via Access Point (Recomendado)

1. **Conectar al WiFi del ESP32:**
   - Red: `ESP32-Hidro-Config`
   - Contraseña: `12345678`

2. **Abrir navegador y ir a:**
   ```
   http://192.168.4.1
   ```

3. **Completar el formulario:**
   ```
   📍 Informaciones Básicas:
   - Nome do Dispositivo: Estufa Principal
   - Localização: Invernadero A - Zona 1
   
   📶 Configurações de Rede:
   - WiFi SSID: Tu_Red_WiFi
   - Senha WiFi: tu_contraseña_wifi
   
   ☁️ Configuração Supabase:
   - URL: https://tu-proyecto.supabase.co
   - API Key: tu_clave_api_supabase
   ```

4. **Hacer clic en "Salvar e Reiniciar"**

### Método 2: Via Serial Monitor

Si prefieres configurar por serial:

```
# En el Serial Monitor, escribir:
wifi

# Seguir las instrucciones:
SSID da rede WiFi: Tu_Red_WiFi
Senha da rede WiFi: tu_contraseña_wifi
URL do servidor: https://tu-proyecto.supabase.co
```

---

## 🗄️ Configuración de la Base de Datos

### Paso 1: Ejecutar el Script SQL

En tu panel de Supabase, ejecuta el script:
```sql
-- Usar: scripts/supabase-multi-device-dynamic.sql
```

Este script:
- ✅ Crea tablas para múltiples dispositivos
- ✅ Configura auto-registro de dispositivos
- ✅ Establece índices optimizados
- ✅ Crea views para el frontend

### Paso 2: Verificar Auto-Registro

Después de configurar WiFi, el ESP32 se registrará automáticamente:

```sql
-- Verificar en Supabase:
SELECT * FROM device_status;

-- Deberías ver tu dispositivo:
device_id: ESP32_HIDRO_A1B2C3
device_name: Estufa Principal  
mac_address: 24:6F:28:A1:B2:C3
is_online: true
```

---

## ✅ Verificación del Sistema

### Paso 1: Monitor Serial

Después de la configuración exitosa:

```
✅ WiFi conectado: 192.168.1.100
🆔 Device ID: ESP32_HIDRO_A1B2C3
📊 Registrado en Supabase: ✅
🔄 Enviando datos de sensores...
🌡️ Temperatura: 25.0°C
💧 Umidade: 65.5%
⚗️ pH: 6.2
```

### Paso 2: Interface Web Local

Accede a la IP del ESP32:
```
http://192.168.1.100
```

Deberías ver:
- 📊 Dashboard con datos de sensores
- 🔌 Estado de los 16 relés
- 📡 Status de conexión
- ⚙️ Opciones de configuración

### Paso 3: Verificar Base de Datos

En Supabase, verifica que los datos lleguen:

```sql
-- Datos de sensores:
SELECT * FROM environment_data ORDER BY created_at DESC LIMIT 5;

-- Datos hidropónicos:
SELECT * FROM hydro_measurements ORDER BY created_at DESC LIMIT 5;

-- Status del dispositivo:
SELECT * FROM device_full_status;
```

---

## 🔄 Múltiples Dispositivos

### Configurar Dispositivos Adicionales

Para agregar más ESP32:

1. **Subir el mismo firmware** a cada ESP32
2. **Cada uno generará su propio ID único**:
   - ESP32 #1: `ESP32_HIDRO_A1B2C3`
   - ESP32 #2: `ESP32_HIDRO_D4E5F6`
   - ESP32 #3: `ESP32_HIDRO_789ABC`

3. **Configurar cada uno individualmente**:
   - Conectar al AP de cada dispositivo
   - Configurar WiFi y ubicación específica
   - El sistema los registrará automáticamente

### Gestión Centralizada

Una vez configurados, todos aparecerán en:

```sql
-- Ver todos los dispositivos:
SELECT device_id, device_name, location, is_online 
FROM device_discovery 
ORDER BY last_seen DESC;
```

### Interface de Gestión

Accede a `/device-config` para:
- 📋 Ver todos los dispositivos
- ⚙️ Configurar cada uno individualmente
- 📊 Ver estadísticas globales
- 🔧 Gestionar grupos y ubicaciones

---

## 🛠️ Solución de Problemas

### Problema: ESP32 no genera Device ID

**Síntomas:**
```
🚨 ERROR: Device ID vacío
📶 MAC Address: 00:00:00:00:00:00
```

**Solución:**
```bash
# 1. Verificar conexión USB
# 2. Reset completo del ESP32
# 3. Verificar en Serial Monitor:
help
wifi
state
```

### Problema: No se conecta al WiFi

**Síntomas:**
```
❌ WiFi falló: No se pudo conectar
🔄 Reintentando en 10 segundos...
```

**Solución:**
1. **Verificar credenciales WiFi**
2. **Reset configuración:**
   ```bash
   # En Serial Monitor:
   wifi
   # Reconfigurar desde cero
   ```
3. **Usar AP mode:**
   - Conectar a `ESP32-Hidro-Config`
   - Reconfigurar en `http://192.168.4.1`

### Problema: No aparece en Supabase

**Síntomas:**
```
✅ WiFi conectado
❌ Supabase: Error de conexión
```

**Solución:**
1. **Verificar URL y API Key de Supabase**
2. **Verificar script SQL ejecutado**
3. **Check RLS policies:**
   ```sql
   -- Verificar políticas:
   SELECT * FROM pg_policies WHERE tablename = 'device_status';
   ```

### Problema: Relés no funcionan

**Síntomas:**
- Comandos enviados pero relés no cambian
- PCF8574 no responde

**Solución:**
1. **Verificar conexiones I2C:**
   - SDA → GPIO 21
   - SCL → GPIO 22
   - VCC → 3.3V o 5V
   - GND → GND

2. **Test I2C en Serial Monitor:**
   ```bash
   # Escribir en Serial Monitor:
   i2c_scan
   # Debería mostrar: 0x20, 0x24
   ```

3. **Test manual de relés:**
   ```bash
   relay 0 on
   relay 0 off
   ```

### Problema: Datos de sensores incorrectos

**Síntomas:**
```
🌡️ Temperatura: -999°C
⚗️ pH: 0.00
```

**Solución:**
1. **Verificar conexiones de sensores**
2. **Calibrar sensores:**
   ```bash
   # En Serial Monitor:
   calibrate_ph
   calibrate_tds
   ```
3. **Verificar alimentación de 5V para sensores**

---

## 📞 Soporte y Ayuda

### Comandos Útiles (Serial Monitor)

```bash
help          # Lista todos los comandos
status        # Estado completo del sistema
sensors       # Leer sensores manualmente
wifi          # Reconfigurar WiFi
reset         # Reiniciar ESP32
memory        # Estado de la memoria
```

### Logs de Depuración

Para obtener más información:

```bash
# En Serial Monitor:
debug on      # Activar logs detallados
debug off     # Desactivar logs
```

### Información del Sistema

```bash
# Obtener información completa:
info          # Device ID, MAC, IP, versión
network       # Estado de la red
database      # Estado de Supabase
hardware      # Estado del hardware
```

---

## 🎉 ¡Sistema Configurado!

Una vez completada la configuración:

✅ **Tu ESP32 tiene un ID único**  
✅ **Se conecta automáticamente al WiFi**  
✅ **Se registra solo en la base de datos**  
✅ **Envía datos de sensores automáticamente**  
✅ **Recibe comandos de relés en tiempo real**  
✅ **Interface web accesible**  
✅ **Listo para producción**  

### Próximos Pasos:

1. **Calibrar sensores** según tu instalación
2. **Configurar automatizaciones** en el panel web
3. **Agregar más dispositivos** si es necesario
4. **Personalizar interface** según tus necesidades

---

**🌱 ¡Tu sistema hidropónico inteligente está listo para funcionar!**

> **Nota:** Este método de configuración inicial está diseñado para ser **user-friendly** y **escalable**. Cada ESP32 se configura una sola vez y luego funciona de manera completamente autónoma.

---

*Desarrollado para el Sistema Hidropónico ESP32 + Next.js + Supabase*  
*Versión: 3.0 | Fecha: Diciembre 2024*
