# 🌱 ESP-HIDROWAVE

<div align="center">

![Version](https://img.shields.io/badge/version-3.0.0-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)
![Framework](https://img.shields.io/badge/framework-Arduino-blue.svg)
![Frontend](https://img.shields.io/badge/frontend-Next.js-black.svg)
![Database](https://img.shields.io/badge/database-Supabase-green.svg)

**Sistema Completo de Automação Hidropônica com ESP32, Next.js y Supabase**

[Características](#-características) • [Instalação](#-instalação) • [Documentação](#-documentação) • [Arquitetura](#-arquitetura) • [Contribuir](#-contribuir)

</div>

---

## 📋 Índice

- [Sobre o Projeto](#-sobre-o-projeto)
- [Características](#-características)
- [Arquitetura](#-arquitetura)
- [Hardware](#-hardware)
- [Instalação](#-instalação)
- [Configuração](#-configuração)
- [Uso](#-uso)
- [API e Endpoints](#-api-e-endpoints)
- [Motor de Decisões](#-motor-de-decisões)
- [ESP-NOW e Comunicação](#-esp-now-e-comunicação)
- [Documentação](#-documentação)
- [Troubleshooting](#-troubleshooting)
- [Contribuir](#-contribuir)
- [Licença](#-licença)

---

## 🎯 Sobre o Projeto

**ESP-HIDROWAVE** é um sistema completo e avançado de automação hidropônica que combina hardware ESP32, firmware robusto e uma interface web moderna. O projeto oferece monitoramento em tempo real, controle automático inteligente, comunicação sem fio entre dispositivos e integração completa com Supabase para armazenamento e análise de dados.

### ✨ Destaques

- 🤖 **Automação Inteligente**: Motor de decisões baseado em regras JSON configuráveis
- 📡 **Comunicação Avançada**: ESP-NOW para comunicação master-slave sem necessidade de WiFi
- 🌐 **Interface Web Moderna**: Dashboard Next.js com React, TypeScript e Tailwind CSS
- ☁️ **Cloud Integration**: Integração completa com Supabase para telemetria e controle remoto
- 🔒 **Segurança**: Sistema de interlocks, modo emergência e validação rigorosa
- 🔄 **Reconexão Automática**: Sistema robusto de reconexão automática para WiFi e ESP-NOW
- 📊 **Telemetria em Tempo Real**: Monitoramento contínuo de sensores e estados do sistema

---

## 🚀 Características

### 🔧 Hardware e Sensores

- **ESP32 DevKit** - Microcontrolador principal
- **Módulo Relé 16 Canais** (PCF8574) - Controle de bombas, luzes, ventiladores
- **DHT22** - Sensor de temperatura e umidade ambiente
- **DS18B20** - Sensor de temperatura da água
- **Sensor pH** - Medição de acidez da solução nutritiva
- **Sensor TDS** - Condutividade elétrica/nutrientes
- **Sensor de Nível** - Monitoramento do reservatório
- **Display LCD I2C** - Interface local
- **LED de Status** - Indicação visual do sistema

### 🌐 Conectividade e Comunicação

- **WiFi** com reconexão automática e modo AP para configuração inicial
- **ESP-NOW** para comunicação master-slave sem necessidade de WiFi
- **HTTP/HTTPS Client** para comunicação com APIs
- **Servidor Web Local** para configuração e diagnóstico
- **WebSockets** para comunicação em tempo real
- **Supabase Integration** para armazenamento e sincronização de dados

### 🧠 Motor de Decisões

- **Sistema de Regras JSON** configuráveis via interface web
- **Avaliação Automática** de condições baseadas em sensores
- **Controle Inteligente** de relés com priorização
- **Interlocks de Segurança** para proteção do sistema
- **Modo Dry-Run** para testes sem acionamento físico
- **Cooldowns e Limites** para evitar execução excessiva
- **Logs Detalhados** e estatísticas em tempo real

### 📊 Funcionalidades do Sistema

#### Monitoramento de Sensores
- Leitura automática a cada 30 segundos
- Envio automático para Supabase
- Histórico completo de medições
- Alertas configuráveis

#### Controle de Relés
- Controle manual via interface web
- Controle automático via regras
- Controle remoto via Supabase
- Controle via ESP-NOW para slaves
- Timers e duração configurável
- Modo PWM para controle proporcional

#### Sistema Master-Slave
- Descoberta automática de slaves
- Reconexão automática
- Sincronização de estados
- Comunicação bidirecional
- RelayBridge para integração Supabase ↔ ESP-NOW

---

## 🏗️ Arquitetura

### Visão Geral

```
┌─────────────────────────────────────────────────────────────┐
│                    INTERFACE WEB (Next.js)                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  Dashboard   │  │ Editor Regras│  │  Controle     │       │
│  │  Monitoramento│  │  JSON       │  │  Manual       │       │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘       │
└─────────┼─────────────────┼─────────────────┼───────────────┘
          │                 │                 │
          └─────────────────┼─────────────────┘
                            │ HTTP/HTTPS
┌───────────────────────────▼─────────────────────────────────┐
│                    SUPABASE (Cloud)                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  Telemetria │  │  Regras JSON │  │  Comandos    │        │
│  │  Sensores   │  │  Decision    │  │  Relés       │        │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘       │
└─────────┼─────────────────┼─────────────────┼───────────────┘
          │                 │                 │
          └─────────────────┼─────────────────┘
                            │ Polling/Realtime
┌───────────────────────────▼─────────────────────────────────┐
│              ESP32 MASTER (HydroSystemCore)                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  Decision    │  │  RelayBridge │  │  HydroControl│       │
│  │  Engine      │  │  Supabase↔   │  │  Sensores    │       │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘       │
└─────────┼─────────────────┼─────────────────┼───────────────┘
          │                 │                 │
          └─────────────────┼─────────────────┘
                            │ ESP-NOW
┌───────────────────────────▼─────────────────────────────────┐
│              ESP32 SLAVES (RelayBox)                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  ESPNowTask  │  │  Relay       │  │  Hardware    │       │
│  │  Slave       │  │  Controller  │  │  Relés       │       │
│  └──────────────┘  └──────┬───────┘  └──────────────┘       │
└───────────────────────────┼─────────────────────────────────┘
                            │
                    ┌───────▼─────── ┐
                    │  Relés Físicos │
                    │  (Hardware)    │
                    └────────────────┘
```

### Componentes Principais

#### 1. **HydroSystemCore** (ESP32 Master)
- Gerenciamento central do sistema
- Integração de todos os módulos
- Gerenciamento de estados
- Watchdog de segurança

#### 2. **DecisionEngine** (Motor de Decisões)
- Avaliação de regras JSON
- Execução de ações automáticas
- Sistema de priorização
- Interlocks de segurança

#### 3. **RelayBridge**
- Ponte entre Supabase e ESP-NOW
- Conversão de formatos
- Sincronização de estados
- Processamento automático

#### 4. **ESPNowController**
- Comunicação master-slave
- Descoberta automática
- Reconexão automática
- Gerenciamento de conexões

#### 5. **SupabaseClient**
- Cliente HTTP para Supabase
- Envio de telemetria
- Leitura de comandos
- Atualização de estados

---

## 🔌 Hardware

### Componentes Necessários

| Componente | Quantidade | Descrição |
|------------|------------|-----------|
| ESP32 DevKit | 1+ | Master + Slaves |
| Módulo Relé 8/16 Canais | 1+ | Controle de dispositivos |
| PCF8574 | 2 | Expansor I/O I2C |
| DHT22 | 1 | Temp/Umidade ambiente |
| DS18B20 | 1 | Temp água |
| Sensor pH | 1 | Medição pH |
| Sensor TDS | 1 | Condutividade |
| Sensor Nível | 1 | Nível reservatório |
| LCD 16x2 I2C | 1 | Display local (opcional) |

### Conexões ESP32 Master

```
📍 SENSORES:
- DHT22 → GPIO 15
- DS18B20 → GPIO 4
- pH Sensor → GPIO 35 (ADC)
- TDS Sensor → GPIO 34 (ADC)
- Nível Água → GPIO 32/33

📍 I2C (SDA=21, SCL=22):
- LCD 16x2 → 0x27
- PCF8574 #1 → 0x20 (Relés 0-7)
- PCF8574 #2 → 0x24 (Relés 8-15)

📍 STATUS:
- LED Interno → GPIO 2
```

### Conexões ESP32 Slave (RelayBox)

```
📍 I2C (SDA=21, SCL=22):
- PCF8574 → 0x20 (Relés 0-7)

📍 STATUS:
- LED Status → GPIO 2
```

---

## 📦 Instalação

### Pré-requisitos

- **PlatformIO** (recomendado) ou Arduino IDE
- **Python 3.7+** (para PlatformIO)
- **Node.js 18+** (para frontend)
- **Conta Supabase** (gratuita)
- **Git**

### 1. Clonar o Repositório

```bash
git clone https://github.com/yhanusleverage/ESP-HIDROWAVE.git
cd ESP-HIDROWAVE
```

### 2. Configurar Firmware ESP32

```bash
# Instalar PlatformIO
pip install platformio

# Navegar para o diretório do firmware
cd ESP-HIDROWAVE-main

# Instalar dependências (automático com PlatformIO)
pio lib install

# Configurar variáveis de ambiente
cp env.example .env
# Editar .env com suas credenciais Supabase
```

### 3. Configurar Supabase

1. Criar projeto no [Supabase](https://supabase.com)
2. Executar scripts SQL:

```bash
# Script principal do banco
scripts/supabase-complete-setup.sql

# Motor de decisões (opcional)
scripts/DECISION_ENGINE_EXTENSION_COMPLETA.sql

# Correções de consistência
scripts/correcoes_schema_consistencia.sql
```

### 4. Configurar Frontend Next.js

```bash
# Navegar para diretório do frontend
cd ../HIDROWAVE-main  # ou seu diretório frontend

# Instalar dependências
npm install

# Configurar variáveis de ambiente
cp .env.example .env.local
# Editar .env.local com suas credenciais
```

### 5. Compilar e Upload

```bash
# Compilar firmware
pio run

# Upload para ESP32
pio run --target upload

# Upload filesystem (SPIFFS/LittleFS)
pio run --target uploadfs

# Monitor serial
pio device monitor
```

---

## ⚙️ Configuração

### Primeira Inicialização

#### Opção 1: Via Serial Monitor

```
SSID da rede WiFi: MinhaRede
Senha da rede WiFi: MinhaSenh@123
URL do servidor: https://seu-projeto.supabase.co
```

#### Opção 2: Via Access Point

1. Conecte ao WiFi: `ESP32-Hidro-Config`
2. Senha: `12345678`
3. Acesse: `http://192.168.4.1`
4. Configure rede e servidor

### Configuração Supabase

Edite `include/APIConfig.h` ou use variáveis de ambiente:

```cpp
#define SUPABASE_URL "https://seu-projeto.supabase.co"
#define SUPABASE_KEY "sua-chave-anon"
```

### Configuração do Frontend

Crie `.env.local`:

```env
NEXT_PUBLIC_SUPABASE_URL=https://seu-projeto.supabase.co
NEXT_PUBLIC_SUPABASE_ANON_KEY=sua-chave-anon
```

---

## 💻 Uso

### Comandos Seriais (ESP32)

```bash
# Status do sistema
status

# Listar sensores
sensors

# Controlar relés
relay 0 on 30      # Liga relé 0 por 30 segundos
relay 1 off        # Desliga relé 1
relay 2 toggle     # Alterna relé 2

# ESP-NOW (Master)
discover           # Descobrir slaves
list               # Listar slaves conhecidos
ping               # Ping em todos
ping <slave>       # Ping em slave específico
relay <slave> <n> <ação>  # Controlar relé remoto

# Motor de Decisões
rules              # Listar regras
engine_stats       # Estatísticas do motor
dry_run            # Ativar/desativar modo teste
emergency          # Modo emergência

# RelayBridge
bridge_stats       # Estatísticas da ponte
bridge_enable      # Ativar processamento
bridge_disable        # Desativar processamento

# WiFi
wifi               # Reconfigurar WiFi

# Ajuda
help               # Lista todos os comandos
```

### Interface Web

1. Iniciar servidor de desenvolvimento:

```bash
cd HIDROWAVE-main
npm run dev
```

2. Acessar: `http://localhost:3000`

3. Funcionalidades disponíveis:
   - 📊 Dashboard de monitoramento em tempo real
   - 🔌 Controle manual de relés
   - 🧠 Editor de regras do motor de decisões
   - 📈 Gráficos históricos
   - ⚙️ Configurações do sistema
   - 🔔 Alertas e notificações

---

## 🔌 API e Endpoints

### Supabase (Backend)

#### Envio de Dados dos Sensores

```http
POST /rest/v1/hydro_measurements
Content-Type: application/json
apikey: sua-chave

{
  "device_id": "ESP32_HIDRO_001",
  "ph": 6.2,
  "tds": 850,
  "temperature": 22.5,
  "water_level_ok": true
}
```

#### Verificação de Comandos

```http
GET /rest/v1/relay_commands?device_id=eq.ESP32_HIDRO_001&status=eq.pending
apikey: sua-chave
```

#### Criar Comando de Relé

```http
POST /rest/v1/relay_commands
Content-Type: application/json
apikey: sua-chave

{
  "device_id": "ESP32_HIDRO_001",
  "relay_number": 0,
  "action": "on",
  "duration_seconds": 30
}
```

### Next.js API Routes

```typescript
// Exemplos de endpoints
GET  /api/devices              // Listar dispositivos
GET  /api/devices/[id]/status  // Status do dispositivo
POST /api/devices/[id]/relay   // Controlar relé
GET  /api/rules                // Listar regras
POST /api/rules                // Criar regra
PUT  /api/rules/[id]           // Atualizar regra
DELETE /api/rules/[id]         // Deletar regra
```

---

## 🧠 Motor de Decisões

### Conceito

O Motor de Decisões permite criar regras automáticas baseadas em condições dos sensores. As regras são avaliadas periodicamente (padrão: 5 segundos) e executam ações quando as condições são atendidas.

### Estrutura de uma Regra

```json
{
  "id": "ph_correction_low",
  "name": "Correção de pH Baixo",
  "description": "Adiciona solução alcalina quando pH < 5.8",
  "enabled": true,
  "priority": 80,
  "condition": {
    "type": "sensor_compare",
    "sensor_name": "ph",
    "op": "<",
    "value_min": 5.8
  },
  "actions": [
    {
      "type": "relay_pulse",
      "target_relay": 2,
      "duration_ms": 5000,
      "message": "Dosagem pH por 5 segundos"
    }
  ],
  "safety_checks": [
    {
      "name": "Verificação nível mínimo",
      "condition": {
        "type": "system_status",
        "sensor_name": "water_level_ok",
        "op": "==",
        "value_min": 1
      },
      "error_message": "Nível de água insuficiente",
      "is_critical": false
    }
  ],
  "cooldown_ms": 60000,
  "max_executions_per_hour": 10
}
```

### Tipos de Condições

- **sensor_compare**: Compara valores de sensores (pH, TDS, temperatura, etc.)
- **system_status**: Verifica status do sistema (WiFi, memória, nível, etc.)
- **relay_state**: Verifica estado de relés
- **composite**: Combina múltiplas condições (AND/OR)

### Tipos de Ações

- **relay_on**: Liga relé
- **relay_off**: Desliga relé
- **relay_pulse**: Pulso com duração
- **relay_pwm**: Controle proporcional (PWM)
- **system_alert**: Envia alerta
- **log_event**: Registra evento

### Interlocks de Segurança

O sistema possui proteções automáticas:

- **Nível de Água**: Bloqueia bombas se nível baixo
- **Temperatura**: Limites de segurança (15-35°C água, 10-40°C ambiente)
- **pH Extremo**: Parada de emergência se pH < 3.0 ou > 11.0
- **Memória**: Modo emergência se heap < 15KB

### Documentação Completa

Consulte [DECISION_ENGINE_DOCUMENTATION.md](./DECISION_ENGINE_DOCUMENTATION.md) para documentação completa.

---

## 📡 ESP-NOW e Comunicação

### Arquitetura Master-Slave

O sistema utiliza ESP-NOW para comunicação sem fio entre o ESP32 Master e múltiplos ESP32 Slaves (RelayBoxes). Isso permite controlar relés remotos sem necessidade de WiFi nos slaves.

### Funcionalidades

- ✅ **Descoberta Automática**: Slaves são descobertos automaticamente no boot
- ✅ **Reconexão Automática**: Sistema robusto de reconexão
- ✅ **Sincronização**: Estados sincronizados entre master e slaves
- ✅ **Multi-Canal**: Suporte para múltiplos canais de comunicação
- ✅ **Bridge Supabase**: Integração transparente via RelayBridge

### Fluxo de Comunicação

```
Next.js → Supabase → RelayBridge → ESP-NOW → Slave → Relé Físico
```

### Configuração

O sistema detecta automaticamente o canal WiFi e sincroniza o ESP-NOW. Não é necessária configuração manual.

### Documentação Completa

Consulte [ESPNOW_SOLUTION_SUMMARY.md](./ESPNOW_SOLUTION_SUMMARY.md) para detalhes completos.

---

## 📚 Documentação

### Documentos Principais

- [DECISION_ENGINE_DOCUMENTATION.md](./DECISION_ENGINE_DOCUMENTATION.md) - Motor de Decisões completo
- [ESPNOW_SOLUTION_SUMMARY.md](./ESPNOW_SOLUTION_SUMMARY.md) - Sistema ESP-NOW
- [RELAY_BRIDGE_INTEGRATION.md](./RELAY_BRIDGE_INTEGRATION.md) - Integração RelayBridge
- [CONFIGURACAO_INICIAL_USUARIO.md](./CONFIGURACAO_INICIAL_USUARIO.md) - Guia de configuração inicial
- [DECISION_ENGINE_ROADMAP.md](./DECISION_ENGINE_ROADMAP.md) - Roadmap do motor de decisões

### Scripts SQL

- `scripts/supabase-complete-setup.sql` - Setup completo do banco
- `scripts/correcoes_schema_consistencia.sql` - Correções de consistência
- `scripts/DECISION_ENGINE_EXTENSION_COMPLETA.sql` - Extensão do motor de decisões

### Análises e Correções

- `ANALISE_CONSISTENCIA_SCHEMA.md` - Análise de consistência
- `CORRECAO_CONSISTENCIA_SCHEMA.md` - Correções aplicadas
- `CORRECAO_ESTADOS_RELES_SLAVES.md` - Correção de estados
- `CORRECAO_FINAL_DISCOVERY.md` - Correção final de descoberta

---

## 🔧 Troubleshooting

### WiFi não conecta

1. Verifique SSID e senha
2. Use comando `wifi` no Serial Monitor
3. Acesse modo AP para reconfigurar
4. Verifique força do sinal

### API não responde

1. Verifique URL do servidor Supabase
2. Teste conectividade: `ping seu-projeto.supabase.co`
3. Verifique chaves de API
4. Consulte logs do Serial Monitor

### Relés não funcionam

1. Verifique conexões I2C
2. Teste endereços PCF8574: use `i2c_scanner`
3. Verifique alimentação dos módulos
4. Teste relés individualmente via serial

### Sensores com valores incorretos

1. Verifique conexões dos sensores
2. Calibre sensores pH/TDS
3. Substitua sensores defeituosos
4. Verifique referência de tensão (3.3V)

### ESP-NOW não conecta

1. Verifique se master e slaves estão no mesmo canal
2. Execute `discover` manualmente
3. Verifique distância entre dispositivos
4. Consulte logs de descoberta

### Motor de Decisões não executa

1. Verifique se regras estão habilitadas
2. Execute `rules` para listar regras
3. Verifique condições das regras
4. Use `dry_run` para testar sem executar
5. Consulte `engine_stats` para estatísticas

### Memória baixa

1. Reduza número de regras ativas
2. Aumente intervalo de avaliação
3. Limpe logs antigos
4. Verifique vazamentos de memória

---

## 🤝 Contribuir

Contribuições são bem-vindas! Para contribuir:

1. **Fork** o projeto
2. Crie uma **branch** para sua feature (`git checkout -b feature/nova-feature`)
3. **Commit** suas alterações (`git commit -m 'Adiciona nova feature'`)
4. **Push** para a branch (`git push origin feature/nova-feature`)
5. Abra um **Pull Request**

### Diretrizes

- Siga o padrão de código existente
- Adicione comentários e documentação
- Teste suas alterações
- Atualize a documentação quando necessário

---

## 📄 Licença

Este projeto está licenciado sob a Licença MIT - veja o arquivo [LICENSE](LICENSE) para detalhes.

---

## 👥 Autores

- **yhanusleverage** - *Desenvolvimento inicial e manutenção*

---

## 🙏 Agradecimentos

- Comunidade ESP32
- Supabase pelo excelente serviço
- Todos os contribuidores do projeto

---

## 📞 Suporte

Para dúvidas e suporte:

1. Verifique a [Documentação](#-documentação)
2. Consulte os logs no Serial Monitor
3. Abra uma [Issue](https://github.com/yhanusleverage/ESP-HIDROWAVE/issues) no GitHub

---

<div align="center">

**Desenvolvido com ❤️ para automação hidropônica**

⭐ Se este projeto foi útil, considere dar uma estrela!

</div>
