# 🔧 ESTRUTURA HYDROCONTROL E SISTEMA DE MUTEX

## 📋 VISÃO GERAL

Este documento explica profundamente a estrutura do `HydroControl`, como funciona o sistema de mutex no `SupabaseClient`, e **quem deve ter controle** sobre o mutex quando se integra com `ec_config` e `decision_rules`.

---

## 🏗️ 1. ESTRUTURA DO HYDROCONTROL

### 1.1. O que é HydroControl?

`HydroControl` é o **controlador principal do hardware** do sistema hidropônico. Ele gerencia:

- ✅ **Relés físicos** (via PCF8574 - expansores I/O I2C)
- ✅ **Sensores** (pH, TDS, temperatura, nível de água)
- ✅ **ECController** (controlador PID para dosagem proporcional)
- ✅ **Sistema sequencial de dosagem** (dosagem de múltiplos nutrientes)
- ✅ **LCD** (display I2C para status)

**Arquivo:** `include/HydroControl.h` | `src/HydroControl.cpp`

---

### 1.2. Componentes Principais

#### A. Hardware (Relés e Sensores)

```cpp
class HydroControl {
private:
    // Expansores I/O (PCF8574) - Controlam relés físicos
    PCF8574 pcf1;  // Relés 0-6 (endereço 0x20)
    PCF8574 pcf2;  // Relé 7+ (endereço 0x24)
    
    // Sensores
    phSensor* pHSensor;           // Sensor de pH
    TDSReaderSerial* tdsSensor;   // Sensor TDS/EC
    LevelSensor* tankSensor;      // Sensor de nível
    DallasTemperature sensors;    // Sensores de temperatura (DS18B20)
    
    // Display
    LiquidCrystal_I2C lcd;        // LCD I2C (16x2)
};
```

**O que faz:**
- Controla **16 relés** via PCF8574 (expansores I2C)
- Lê sensores (pH, TDS, temperatura, nível)
- Exibe status no LCD

---

#### B. Estados dos Relés

```cpp
class HydroControl {
private:
    // Estado dos relés (16 relés)
    bool relayStates[NUM_RELAYS];      // Estado atual (true = ligado)
    unsigned long startTimes[NUM_RELAYS]; // Quando foi ligado
    int timerSeconds[NUM_RELAYS];      // Timer em segundos (0 = permanente)
};
```

**O que faz:**
- Armazena estado de cada relé (ligado/desligado)
- Controla timers (desliga automaticamente após X segundos)
- Rastreia quando cada relé foi ligado

---

#### C. ECController (Controlador PID)

```cpp
class HydroControl {
private:
    ECController ecController;        // Controlador PID para EC
    float ecSetpoint;                  // Setpoint desejado de EC
    bool autoECEnabled;                // Modo automático ativo?
    unsigned long lastECCheck;         // Última verificação de EC
    int autoECIntervalSeconds;        // Intervalo entre verificações
};
```

**O que faz:**
- Calcula dosagem proporcional baseada em EC atual vs setpoint
- Controla modo automático (verifica EC a cada X segundos)
- Integra com sistema sequencial de dosagem

---

#### D. Sistema Sequencial de Dosagem

```cpp
class HydroControl {
private:
    SequentialState currentState;     // IDLE, DOSING, WAITING
    SimpleNutrient nutrients[8];      // Array de nutrientes
    int totalNutrients;                // Quantos nutrientes
    int currentNutrientIndex;          // Qual nutriente está dosando
    unsigned long stateStartTime;      // Quando começou o estado atual
    int intervalSeconds;               // Intervalo entre nutrientes
};
```

**O que faz:**
- Executa dosagem sequencial de múltiplos nutrientes
- Controla intervalo entre nutrientes
- Máquina de estados (IDLE → DOSING → WAITING → DOSING...)

---

### 1.3. Métodos Principais

#### Controle de Relés

```cpp
// Ligar/desligar relé com timer opcional
void toggleRelay(int relay, int seconds = 0);
void setRelay(int relay, bool state, int seconds = 0);

// Obter estados dos relés
bool* getRelayStates();
```

**Uso:**
```cpp
hydroControl.toggleRelay(0, 30);  // Liga relé 0 por 30 segundos
hydroControl.setRelay(1, true, 0);  // Liga relé 1 permanentemente
bool* states = hydroControl.getRelayStates();  // Array de estados
```

---

#### Leitura de Sensores

```cpp
// Getters para leituras
float& getTemperature();
float& getpH();
float& getTDS();
float& getEC();
bool isWaterLevelOk();
```

**Uso:**
```cpp
float ph = hydroControl.getpH();
float tds = hydroControl.getTDS();
bool levelOk = hydroControl.isWaterLevelOk();
```

---

#### ECController

```cpp
// Configuração do controlador EC
void setECSetpoint(float setpoint);
float getECSetpoint() const;
void setAutoECEnabled(bool enabled);
bool isAutoECEnabled() const;
ECController& getECController();
```

**Uso:**
```cpp
hydroControl.setECSetpoint(1.8);
hydroControl.setAutoECEnabled(true);
ECController& ec = hydroControl.getECController();
```

---

#### Sistema Sequencial

```cpp
// Iniciar dosagem sequencial
void startSimpleSequentialDosage(float totalML, float ecSetpoint, float ecActual);
void executeWebDosage(JsonArray distribution, int intervalo);
bool isDosageActive() const;
void cancelCurrentDosage();
```

**Uso:**
```cpp
hydroControl.startSimpleSequentialDosage(15.5, 1.8, 1.5);
if (hydroControl.isDosageActive()) {
    // Dosagem em andamento
}
```

---

## 🔒 2. SISTEMA DE MUTEX NO SUPABASECLIENT

### 2.1. O que o Mutex Protege?

**IMPORTANTE:** O mutex em `SupabaseClient` **NÃO protege o HydroControl**. Ele protege apenas as **operações HTTP** (chamadas ao Supabase).

```cpp
class SupabaseClient {
private:
    SemaphoreHandle_t requestMutex;        // Protege makeRequest() - HTTP
    SemaphoreHandle_t commandCheckMutex;   // Protege checkForCommands() - HTTP
};
```

**O que protege:**
- ✅ `makeRequest()` - Evita múltiplas requisições HTTP simultâneas
- ✅ `checkForCommands()` - Evita buscar comandos simultaneamente
- ✅ Operações RPC (POST para Supabase)

**O que NÃO protege:**
- ❌ Acesso a `HydroControl` (relés, sensores)
- ❌ Operações de hardware (ligar/desligar relés)
- ❌ Leitura de sensores

---

### 2.2. Como Funciona o Mutex

```cpp
bool SupabaseClient::makeRequest(...) {
    // 1. Adquirir mutex (timeout de 5 segundos)
    if (xSemaphoreTake(requestMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.println("❌ Timeout ao adquirir requestMutex");
        return false;
    }
    
    // 2. Fazer requisição HTTP (protegida)
    int httpCode = httpClient->POST(payload);
    
    // 3. Liberar mutex ANTES de retornar
    xSemaphoreGive(requestMutex);
    
    return (httpCode >= 200 && httpCode < 300);
}
```

**Fluxo:**
1. Thread tenta adquirir mutex (`xSemaphoreTake`)
2. Se adquirir, faz requisição HTTP
3. Libera mutex (`xSemaphoreGive`)
4. Se timeout (5s), retorna erro

---

## 🎯 3. QUEM DEVE TER CONTROLE DO MUTEX?

### 3.1. Para Operações HTTP (SupabaseClient)

**Resposta:** O próprio `SupabaseClient` gerencia o mutex internamente.

```cpp
// ✅ CORRETO: SupabaseClient gerencia mutex internamente
supabase.updateDeviceStatus(status);  // Mutex interno
supabase.checkForMasterCommands(...); // Mutex interno
```

**Não precisa:**
- ❌ Criar mutex externo
- ❌ Adquirir mutex antes de chamar
- ❌ Liberar mutex depois

---

### 3.2. Para Operações de Hardware (HydroControl)

**Resposta:** **NÃO há mutex atualmente** para proteger `HydroControl`. Isso pode causar **race conditions** se múltiplas threads acessarem simultaneamente.

**Problema atual:**
```cpp
// Thread 1 (SupabaseClient)
hydroControl.toggleRelay(0, 30);  // Liga relé 0

// Thread 2 (DecisionEngine) - SIMULTÂNEO
hydroControl.toggleRelay(0, 60);  // Liga relé 0 novamente (conflito!)
```

**Solução proposta:** Adicionar mutex ao `HydroControl`:

```cpp
class HydroControl {
private:
    SemaphoreHandle_t relayMutex;  // ✅ NOVO: Protege acesso a relés
    
public:
    bool initMutexes() {
        relayMutex = xSemaphoreCreateMutex();
        return (relayMutex != nullptr);
    }
    
    void toggleRelay(int relay, int seconds = 0) {
        // ✅ Adquirir mutex antes de modificar relé
        if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Modificar relé (protegido)
            relayStates[relay] = true;
            startTimes[relay] = millis();
            timerSeconds[relay] = seconds;
            
            // Controlar hardware
            if (relay < 7) {
                pcf1.digitalWrite(relay, LOW);  // Liga relé
            } else {
                pcf2.digitalWrite(relay - 7, LOW);
            }
            
            xSemaphoreGive(relayMutex);  // Liberar mutex
        }
    }
};
```

---

### 3.3. Para DecisionEngine e ECController

**Resposta:** Ambos devem **adquirir mutex do HydroControl** antes de modificar relés.

```cpp
// DecisionEngine executando ação
void DecisionEngine::executeRelayAction(const RuleAction& action, const String& rule_id) {
    // ✅ Adquirir mutex do HydroControl antes de modificar relé
    if (hydroControl->acquireRelayMutex(1000)) {  // Timeout 1s
        hydroControl->toggleRelay(action.target_relay, action.duration_ms / 1000);
        hydroControl->releaseRelayMutex();
    }
}

// ECController executando dosagem
void ECController::executeDosage(const Distribution& dist) {
    // ✅ Adquirir mutex do HydroControl antes de modificar relés
    if (hydroControl->acquireRelayMutex(1000)) {
        for (auto& nutrient : dist.distribution) {
            hydroControl->toggleRelay(nutrient.relay, nutrient.tempoDosagem);
            delay(nutrient.intervalo * 1000);  // Intervalo entre nutrientes
        }
        hydroControl->releaseRelayMutex();
    }
}
```

---

## 🔄 4. FLUXO COMPLETO COM MUTEX

### 4.1. DecisionEngine → HydroControl

```
DecisionEngine avalia regra
  ↓
Condição atendida
  ↓
DecisionEngine adquire mutex do HydroControl (1s timeout)
  ↓
HydroControl.toggleRelay() (protegido por mutex)
  ↓
Relé modificado no hardware
  ↓
DecisionEngine libera mutex
  ↓
Cria comando em relay_commands_master (SupabaseClient - mutex interno)
```

**Código:**
```cpp
// DecisionEngine.cpp
void DecisionEngine::executeRelayAction(const RuleAction& action, const String& rule_id) {
    // 1. Adquirir mutex do HydroControl
    if (!hydroControl->acquireRelayMutex(1000)) {
        Serial.println("❌ Timeout ao adquirir mutex do HydroControl");
        return;
    }
    
    // 2. Executar ação (protegida)
    if (action.target_device_id.isEmpty()) {
        // Relé local
        hydroControl->toggleRelay(action.target_relay, action.duration_ms / 1000);
    } else {
        // Relé remoto (ESP-NOW) - não precisa mutex do HydroControl
        masterManager->controlRelay(action.target_device_id, action.target_relay, 
                                    action.type == RELAY_ON ? "on" : "off", 
                                    action.duration_ms / 1000);
    }
    
    // 3. Liberar mutex
    hydroControl->releaseRelayMutex();
    
    // 4. Criar comando no Supabase (mutex interno do SupabaseClient)
    supabase->createRelayCommand(...);
}
```

---

### 4.2. ECController → HydroControl

```
ECController verifica EC
  ↓
EC fora do setpoint
  ↓
ECController calcula dosagem proporcional
  ↓
ECController adquire mutex do HydroControl (1s timeout)
  ↓
HydroControl.startSimpleSequentialDosage() (protegido por mutex)
  ↓
Dosagem sequencial executada (protegida)
  ↓
ECController libera mutex
  ↓
Atualiza ec_controller_config no Supabase (SupabaseClient - mutex interno)
```

**Código:**
```cpp
// ECController.cpp (ou HydroControl.cpp)
void HydroControl::checkAutoEC() {
    if (!autoECEnabled) return;
    
    unsigned long now = millis();
    if (now - lastECCheck < (autoECIntervalSeconds * 1000)) return;
    
    lastECCheck = now;
    
    // 1. Adquirir mutex (protege acesso a relés durante dosagem)
    if (!acquireRelayMutex(1000)) {
        Serial.println("❌ Timeout ao adquirir mutex para EC");
        return;
    }
    
    // 2. Calcular dosagem (protegida)
    float currentEC = getEC();
    float totalML = ecController.calculateDosage(currentEC, ecSetpoint);
    
    if (totalML > 0.1) {  // Se precisa dosar
        Distribution dist = ecController.calculateDistribution(totalML);
        startSimpleSequentialDosage(totalML, ecSetpoint, currentEC);
    }
    
    // 3. Liberar mutex
    releaseRelayMutex();
}
```

---

## ✅ 5. IMPLEMENTAÇÃO RECOMENDADA

### 5.1. Adicionar Mutex ao HydroControl

```cpp
// HydroControl.h
class HydroControl {
private:
    SemaphoreHandle_t relayMutex;  // ✅ NOVO: Protege acesso a relés
    
public:
    // ✅ NOVO: Métodos para gerenciar mutex
    bool acquireRelayMutex(unsigned long timeoutMs = 1000);
    void releaseRelayMutex();
    bool initMutexes();
    void cleanupMutexes();
};
```

```cpp
// HydroControl.cpp
bool HydroControl::initMutexes() {
    relayMutex = xSemaphoreCreateMutex();
    if (!relayMutex) {
        Serial.println("❌ [HYDRO] Falha ao criar relayMutex");
        return false;
    }
    Serial.println("✅ [HYDRO] relayMutex criado");
    return true;
}

bool HydroControl::acquireRelayMutex(unsigned long timeoutMs) {
    if (relayMutex == nullptr) return false;
    return (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
}

void HydroControl::releaseRelayMutex() {
    if (relayMutex != nullptr) {
        xSemaphoreGive(relayMutex);
    }
}
```

---

### 5.2. Proteger Métodos Críticos

```cpp
// HydroControl.cpp
void HydroControl::toggleRelay(int relay, int seconds) {
    // ✅ Adquirir mutex antes de modificar
    if (!acquireRelayMutex(1000)) {
        Serial.printf("❌ [HYDRO] Timeout ao adquirir mutex para relé %d\n", relay);
        return;
    }
    
    // Modificar relé (protegido)
    relayStates[relay] = !relayStates[relay];
    if (relayStates[relay]) {
        startTimes[relay] = millis();
        timerSeconds[relay] = seconds;
        
        // Controlar hardware
        if (relay < 7) {
            pcf1.digitalWrite(relay, LOW);
        } else {
            pcf2.digitalWrite(relay - 7, LOW);
        }
    } else {
        // Desligar relé
        if (relay < 7) {
            pcf1.digitalWrite(relay, HIGH);
        } else {
            pcf2.digitalWrite(relay - 7, HIGH);
        }
    }
    
    // ✅ Liberar mutex
    releaseRelayMutex();
}
```

---

## 📊 6. RESUMO

### 6.1. Mutex no SupabaseClient

- **Protege:** Operações HTTP (makeRequest, checkForCommands)
- **Gerenciado por:** SupabaseClient internamente
- **Não precisa:** Adquirir/liberar manualmente

### 6.2. Mutex no HydroControl (PROPOSTO)

- **Protege:** Acesso a relés físicos (toggleRelay, setRelay)
- **Gerenciado por:** HydroControl (métodos acquire/release)
- **Precisa:** Adquirir antes de modificar relés, liberar depois

### 6.3. Quem Usa o Mutex?

| Componente | Mutex Usado | Quando |
|------------|-------------|--------|
| **SupabaseClient** | `requestMutex` (interno) | Sempre que faz HTTP |
| **DecisionEngine** | `HydroControl::relayMutex` | Antes de modificar relés |
| **ECController** | `HydroControl::relayMutex` | Antes de executar dosagem |
| **HydroSystemCore** | `HydroControl::relayMutex` | Antes de processar comandos |

---

## ✅ 7. CHECKLIST DE IMPLEMENTAÇÃO

- [ ] Adicionar `relayMutex` ao `HydroControl`
- [ ] Implementar `initMutexes()` e `cleanupMutexes()` no `HydroControl`
- [ ] Implementar `acquireRelayMutex()` e `releaseRelayMutex()`
- [ ] Proteger `toggleRelay()` e `setRelay()` com mutex
- [ ] Proteger `startSimpleSequentialDosage()` com mutex
- [ ] Atualizar `DecisionEngine` para usar mutex do `HydroControl`
- [ ] Atualizar `ECController` para usar mutex do `HydroControl`
- [ ] Testar concorrência (múltiplas threads modificando relés)

---

**Versão:** 1.0 | **Última atualização:** 2025-01-12
