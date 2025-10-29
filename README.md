# 🌱 ESP32 Sistema Hidropônico - Integração Web API

Sistema completo de automação hidropônica com ESP32 integrado a sistema web Next.js.

## 📋 Características

### 🔧 Hardware Suportado
- **ESP32 DevKit** - Microcontrolador principal
- **Módulo Relé 8 Canais** - Controle de bombas, luzes, ventiladores
- **DHT22** - Sensor de temperatura e umidade ambiente
- **DS18B20** - Sensor de temperatura da água
- **Sensor pH** - Medição de acidez da solução nutritiva
- **Sensor TDS** - Condutividade elétrica/nutrientes
- **Sensor de Nível** - Monitoramento do reservatório
- **Display LCD I2C** - Interface local
- **LED de Status** - Indicação visual do sistema

### 🌐 Conectividade
- **WiFi** com reconexão automática
- **HTTP Client** para comunicação com API
- **Servidor Web Local** para configuração
- **AP Mode** para configuração inicial
- **JSON** para troca de dados

### 📊 Funcionalidades

#### Sensores (Envio automático a cada 30s)
```json
{
  "device_id": "ESP32_HIDRO_001",
  "hydro_data": {
    "temperature": 22.5,
    "ph": 6.2,
    "tds": 850,
    "water_level_ok": true
  },
  "environment_data": {
    "temperature": 25.0,
    "humidity": 65.5
  }
}
```

#### Controle de Relés (Comandos via API)
```json
{
  "relayNumber": 0,
  "action": "on",
  "duration": 30
}
```

## 🚀 Instalação

### 1. Preparar Ambiente
```bash
# Instalar PlatformIO
pip install platformio

# Clonar projeto
git clone <seu-repositorio>
cd modulorelays
```

### 2. Configurar Hardware

#### Conexões ESP32:
```
Sensores:
- DHT22 → GPIO 22
- DS18B20 → GPIO 33
- pH Sensor → GPIO 35 (ADC)
- TDS Sensor → GPIO 34 (ADC)
- Nível Água → GPIO 23

I2C (SDA=21, SCL=22):
- LCD 16x2 → 0x27
- PCF8574 #1 → 0x20 (Relés 0-7)
- PCF8574 #2 → 0x24 (Expansão)

Status:
- LED Interno → GPIO 2
```

### 3. Compilar e Upload
```bash
# Compilar
pio run

# Upload
pio run --target upload

# Monitor Serial
pio device monitor
```

## ⚙️ Configuração

### Primeira Inicialização

1. **Via Serial Monitor:**
   ```
   SSID da rede WiFi: MinhaRede
   Senha da rede WiFi: MinhaSenh@123
   URL do servidor: http://192.168.1.100:3000
   ```

2. **Via Access Point:**
   - Conecte ao WiFi: `ESP32-Hidro-Config`
   - Senha: `12345678`
   - Acesse: `http://192.168.4.1`
   - Configure rede e servidor

### Configurações Salvas
As configurações são salvas em `/wifi_config.json` no SPIFFS:
```json
{
  "ssid": "MinhaRede",
  "password": "MinhaSenh@123",
  "server_url": "http://192.168.1.100:3000"
}
```

## 🌐 API Endpoints

### Envio de Dados dos Sensores
```http
POST /api/sensor-data
Content-Type: application/json

{
  "device_id": "ESP32_HIDRO_001",
  "hydro_data": {
    "temperature": 22.5,
    "ph": 6.2,
    "tds": 850,
    "water_level_ok": true
  },
  "environment_data": {
    "temperature": 25.0,
    "humidity": 65.5
  }
}
```

### Verificação de Comandos
```http
GET /api/relay/check?device_id=ESP32_HIDRO_001

Response:
{
  "commands": [
    {
      "relayNumber": 0,
      "action": "on",
      "duration": 30
    }
  ]
}
```

### Controle de Relés
```http
POST /api/relay
Content-Type: application/json

{
  "relayNumber": 0,
  "action": "on",
  "duration": 30
}
```

## 🔧 Comandos Serial

Durante a execução, você pode usar comandos via Serial Monitor:

```bash
# Controlar relés
relay 0 on 30      # Liga relé 0 por 30 segundos
relay 1 off        # Desliga relé 1
relay 2 on         # Liga relé 2 indefinidamente

# Status do sistema
status             # Mostra status completo
sensors            # Lê sensores agora
wifi               # Reconfigura WiFi

# Ajuda
help               # Lista todos os comandos
```

## 💡 LED de Status

O LED interno indica o estado do sistema:

- **Respiração suave** → Funcionando normalmente
- **Piscar lento** → Conectando ao WiFi
- **Piscar rápido** → Erro de conexão
- **Duplo piscar** → Enviando dados
- **Desligado** → Sistema inativo

## 📊 Monitoramento

### Serial Monitor
```
🌱 ===== ESP32 SISTEMA HIDROPÔNICO =====
🔧 Versão: 2.0 - Integração Web API
📅 Compilado: Dec 20 2024 10:30:00
=====================================

📊 Lendo sensores...
🌡️ Temperatura ambiente: 25.0°C
💧 Umidade ambiente: 65.5%
🌊 Temperatura água: 22.5°C
⚗️ pH: 6.2
🧪 TDS: 850 ppm
📏 Nível água: OK
✅ Dados enviados para a API
---

🔌 === STATUS DOS RELÉS ===
   Relé 0: ON (Timer: 25s restantes)
   Relé 1: OFF
   Relé 2: OFF
   ...
========================
```

## 🔧 Personalização

### Intervalos de Tempo
```cpp
// Em main.cpp
const unsigned long SENSOR_READ_INTERVAL = 30000;  // 30s
const unsigned long RELAY_CHECK_INTERVAL = 5000;   // 5s
const unsigned long STATUS_PRINT_INTERVAL = 60000; // 1min
```

### Device ID
```cpp
apiClient->setDeviceId("ESP32_HIDRO_001");
```

### Pinos dos Sensores
   ```cpp
const int DHT_PIN = 22;
const int SENSOR_NPN_PIN = 23;
const int SENSOR_PNP_PIN = 32;
const int STATUS_LED_PIN = 2;
```

## 🛠️ Troubleshooting

### WiFi não conecta
1. Verifique SSID e senha
2. Use comando `wifi` no Serial Monitor
3. Acesse modo AP para reconfigurar

### API não responde
1. Verifique URL do servidor
2. Teste conectividade: `ping 192.168.1.100`
3. Verifique logs do servidor Next.js

### Relés não funcionam
1. Verifique conexões I2C
2. Teste endereços PCF8574: `i2cdetect`
3. Verifique alimentação dos módulos

### Sensores com valores incorretos
1. Verifique conexões
2. Calibre sensores pH/TDS
3. Substitua sensores defeituosos

## 📚 Estrutura do Código

```
src/
├── main.cpp              # Loop principal
├── WiFiManager.cpp       # Gerenciamento WiFi
├── APIClient.cpp         # Comunicação HTTP
├── RelayController.cpp   # Controle de relés
├── StatusLED.cpp         # LED de status
├── HydroControl.cpp      # Sistema hidropônico
├── DHTSensor.cpp         # Sensor DHT22
└── ...

include/
├── WiFiManager.h
├── APIClient.h
├── RelayController.h
├── StatusLED.h
└── ...
```

## 🔄 Atualizações

### Over-The-Air (OTA)
Para implementar atualizações remotas, adicione:
```cpp
#include <ArduinoOTA.h>
// Configurar OTA no setup()
```

### Backup de Configurações
   ```bash
# Fazer backup do SPIFFS
pio run --target uploadfs
```

## 📞 Suporte

Para dúvidas e suporte:
1. Verifique logs no Serial Monitor
2. Consulte documentação da API
3. Abra issue no repositório

---

**Desenvolvido para integração com sistema web Next.js de cultivo hidropônico** 🌱

# HydroWave - Interface de Diagnóstico

Este é o servidor de diagnóstico local para o sistema HydroWave. Ele fornece uma interface web para monitoramento e configuração do sistema hidropônico.

## Funcionalidades

- 🌡️ Monitoramento em tempo real de sensores
- 🔌 Controle e visualização dos relés
- 📊 Status do sistema e conectividade
- ⚙️ Configuração de WiFi e API
- 📱 Interface responsiva e moderna

## Requisitos

- ESP32
- PlatformIO
- Bibliotecas (incluídas no platformio.ini):
  - ESPAsyncWebServer
  - AsyncTCP
  - ArduinoJson
  - SPIFFS

## Instalação

1. Clone este repositório
2. Abra o projeto no PlatformIO
3. Faça o upload do SPIFFS:
   ```bash
   pio run -t uploadfs
   ```
4. Compile e faça o upload do firmware:
   ```bash
   pio run -t upload
   ```

## Configuração

### WiFi

1. Na primeira inicialização, conecte-se ao AP "HydroWave-Setup"
2. Acesse http://192.168.4.1
3. Configure suas credenciais WiFi

### API Supabase

1. Acesse a interface web
2. Vá para a seção de Configuração
3. Insira a URL e chave da sua instância Supabase

## Uso

1. Conecte-se à mesma rede WiFi do dispositivo
2. Acesse http://[IP-DO-ESP32]
3. A interface mostrará:
   - Status do sistema
   - Leituras dos sensores
   - Estado dos relés
   - Opções de configuração

## Endpoints da API

- `/status` - Status do sistema
- `/sensors` - Dados dos sensores
- `/relays` - Estado dos relés
- `/config/wifi` - Configuração WiFi (POST)
- `/config/api` - Configuração Supabase (POST)

## Estrutura de Arquivos

```
.
├── src/
│   ├── main.cpp
│   └── DiagWebServer.cpp
├── include/
│   ├── Config.h
│   └── DiagWebServer.h
├── data/
│   └── index.html
└── platformio.ini
```

## Contribuindo

1. Fork o projeto
2. Crie sua Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit suas mudanças (`git commit -m 'Add some AmazingFeature'`)
4. Push para a Branch (`git push origin feature/AmazingFeature`)
5. Abra um Pull Request

## Licença

Este projeto está licenciado sob a Licença MIT - veja o arquivo [LICENSE](LICENSE) para detalhes.
