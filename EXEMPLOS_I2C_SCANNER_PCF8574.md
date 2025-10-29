# EXEMPLOS PRÁTICOS - I2C SCANNER E PCF8574

## 1. EXEMPLO BÁSICO DE DETECÇÃO

### 1.1 Scanner I2C Simples
```cpp
#include <Arduino.h>
#include "I2CScanner.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🔍 I2C Scanner - Exemplo Básico");
    Serial.println("===============================");
    
    // Inicializar I2C Scanner
    I2CScanner::begin(21, 22, 100000); // SDA=21, SCL=22, 100kHz
    
    // Escanear todos os dispositivos
    std::vector<I2CScanner::I2CDevice> devices = I2CScanner::scanAll();
    
    // Imprimir resultados
    I2CScanner::printScanResults();
    
    // Procurar especificamente por PCF8574
    uint8_t pcfAddress = I2CScanner::findPCF8574();
    if (pcfAddress != 0) {
        Serial.println("✅ PCF8574 encontrado em: 0x" + String(pcfAddress, HEX));
    } else {
        Serial.println("❌ PCF8574 não encontrado");
    }
}

void loop() {
    // Nada no loop para este exemplo
    delay(1000);
}
```

### 1.2 Detecção de Múltiplos PCF8574
```cpp
#include <Arduino.h>
#include "I2CScanner.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🔍 I2C Scanner - Múltiplos PCF8574");
    Serial.println("===================================");
    
    // Inicializar I2C Scanner
    I2CScanner::begin();
    
    // Procurar todos os PCF8574
    std::vector<uint8_t> pcfList = I2CScanner::findAllPCF8574();
    
    Serial.println("📊 Resultados da busca:");
    Serial.println("Total de PCF8574 encontrados: " + String(pcfList.size()));
    
    for (int i = 0; i < pcfList.size(); i++) {
        uint8_t address = pcfList[i];
        Serial.printf("PCF8574 #%d: 0x%02X\n", i + 1, address);
        
        // Mostrar configuração dos pinos A0, A1, A2
        Serial.printf("  A2=%d, A1=%d, A0=%d\n", 
                     (address >> 2) & 1,
                     (address >> 1) & 1,
                     address & 1);
    }
}

void loop() {
    delay(1000);
}
```

## 2. EXEMPLO DE CONTROLE DE RELÉS

### 2.1 Controle Básico com RelayController
```cpp
#include <Arduino.h>
#include "RelayController.h"

RelayController relayController;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🔌 Relay Controller - Exemplo Básico");
    Serial.println("===================================");
    
    // Inicializar controlador de relés
    if (relayController.begin()) {
        Serial.println("✅ RelayController inicializado");
    } else {
        Serial.println("❌ Falha na inicialização");
        return;
    }
    
    // Configurar callback para mudanças de estado
    relayController.setStateChangeCallback([](int relayNumber, bool state, int remainingTime) {
        Serial.printf("🔌 Relé %d: %s", relayNumber, state ? "LIGADO" : "DESLIGADO");
        if (remainingTime > 0) {
            Serial.printf(" (Timer: %ds)", remainingTime);
        }
        Serial.println();
    });
    
    // Exemplo de controle de relés
    testRelayControl();
}

void loop() {
    relayController.update();
    delay(100);
}

void testRelayControl() {
    Serial.println("\n🧪 Testando controle de relés...");
    
    // Teste 1: Ligar relé 0
    Serial.println("Teste 1: Ligando relé 0");
    relayController.setRelay(0, true);
    delay(2000);
    
    // Teste 2: Desligar relé 0
    Serial.println("Teste 2: Desligando relé 0");
    relayController.setRelay(0, false);
    delay(1000);
    
    // Teste 3: Relé com timer
    Serial.println("Teste 3: Relé 1 com timer de 5 segundos");
    relayController.setRelayWithTimer(1, true, 5);
    
    // Teste 4: Múltiplos relés
    Serial.println("Teste 4: Ligando relés 2, 3, 4 simultaneamente");
    relayController.setRelay(2, true);
    relayController.setRelay(3, true);
    relayController.setRelay(4, true);
    
    delay(3000);
    
    // Desligar todos
    Serial.println("Desligando todos os relés");
    for (int i = 0; i < 16; i++) {
        relayController.setRelay(i, false);
    }
    
    Serial.println("✅ Testes concluídos");
}
```

### 2.2 Sistema de Irrigação Automática
```cpp
#include <Arduino.h>
#include "RelayController.h"

RelayController irrigationSystem;

// Mapeamento de relés para sistema de irrigação
#define RELAY_PUMP_MAIN 0      // Bomba principal
#define RELAY_PUMP_AUX 1       // Bomba auxiliar
#define RELAY_VALVE_1 2        // Válvula zona 1
#define RELAY_VALVE_2 3        // Válvula zona 2
#define RELAY_VALVE_3 4        // Válvula zona 3
#define RELAY_VALVE_4 5        // Válvula zona 4
#define RELAY_DRAIN 6          // Dreno
#define RELAY_RESERVE 7        // Reserva

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("💧 Sistema de Irrigação Automática");
    Serial.println("==================================");
    
    // Inicializar sistema
    if (!irrigationSystem.begin()) {
        Serial.println("❌ Falha na inicialização do sistema");
        return;
    }
    
    // Configurar callback
    irrigationSystem.setStateChangeCallback([](int relayNumber, bool state, int remainingTime) {
        String relayName = getRelayName(relayNumber);
        Serial.printf("💧 %s: %s", relayName.c_str(), state ? "ATIVADO" : "DESATIVADO");
        if (remainingTime > 0) {
            Serial.printf(" (Timer: %ds)", remainingTime);
        }
        Serial.println();
    });
    
    Serial.println("✅ Sistema de irrigação inicializado");
    Serial.println("🎯 Relés disponíveis:");
    for (int i = 0; i < 8; i++) {
        Serial.printf("   Relé %d: %s\n", i, getRelayName(i).c_str());
    }
}

void loop() {
    irrigationSystem.update();
    
    // Exemplo de sequência de irrigação
    static unsigned long lastIrrigation = 0;
    if (millis() - lastIrrigation > 300000) { // A cada 5 minutos
        runIrrigationSequence();
        lastIrrigation = millis();
    }
    
    delay(100);
}

void runIrrigationSequence() {
    Serial.println("\n💧 Iniciando sequência de irrigação...");
    
    // 1. Ligar bomba principal
    Serial.println("🔧 Ativando bomba principal");
    irrigationSystem.setRelay(RELAY_PUMP_MAIN, true);
    delay(2000); // Aguardar estabilização
    
    // 2. Irrigar cada zona sequencialmente
    int zones[] = {RELAY_VALVE_1, RELAY_VALVE_2, RELAY_VALVE_3, RELAY_VALVE_4};
    int zoneTimes[] = {30, 45, 30, 60}; // Tempos em segundos
    
    for (int i = 0; i < 4; i++) {
        Serial.printf("🌱 Irrigando zona %d por %d segundos\n", i + 1, zoneTimes[i]);
        
        // Abrir válvula da zona
        irrigationSystem.setRelay(zones[i], true);
        
        // Aguardar tempo de irrigação
        delay(zoneTimes[i] * 1000);
        
        // Fechar válvula da zona
        irrigationSystem.setRelay(zones[i], false);
        delay(1000); // Pequeno intervalo entre zonas
    }
    
    // 3. Desligar bomba principal
    Serial.println("🔧 Desativando bomba principal");
    irrigationSystem.setRelay(RELAY_PUMP_MAIN, false);
    
    // 4. Ativar dreno por 10 segundos
    Serial.println("🌊 Ativando dreno");
    irrigationSystem.setRelayWithTimer(RELAY_DRAIN, true, 10);
    
    Serial.println("✅ Sequência de irrigação concluída");
}

String getRelayName(int relayNumber) {
    const char* names[] = {
        "Bomba Principal", "Bomba Auxiliar", "Válvula Zona 1", "Válvula Zona 2",
        "Válvula Zona 3", "Válvula Zona 4", "Dreno", "Reserva"
    };
    
    if (relayNumber >= 0 && relayNumber < 8) {
        return String(names[relayNumber]);
    }
    return "Desconhecido";
}
```

## 3. EXEMPLO DE SISTEMA DE ILUMINAÇÃO

### 3.1 Controle de Luzes LED
```cpp
#include <Arduino.h>
#include "RelayController.h"

RelayController lightingSystem;

// Mapeamento de relés para sistema de iluminação
#define RELAY_LED_MAIN 8        // Luz principal
#define RELAY_LED_AUX 9         // Luz auxiliar
#define RELAY_LED_RED 10        // Luz vermelha (floração)
#define RELAY_LED_BLUE 11       // Luz azul (vegetativo)
#define RELAY_LED_WHITE 12      // Luz branca
#define RELAY_LED_UV 13         // Luz UV
#define RELAY_FAN 14            // Ventilador
#define RELAY_RESERVE 15        // Reserva

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("💡 Sistema de Iluminação LED");
    Serial.println("============================");
    
    // Inicializar sistema
    if (!lightingSystem.begin()) {
        Serial.println("❌ Falha na inicialização");
        return;
    }
    
    // Configurar callback
    lightingSystem.setStateChangeCallback([](int relayNumber, bool state, int remainingTime) {
        String relayName = getLightingName(relayNumber);
        Serial.printf("💡 %s: %s", relayName.c_str(), state ? "LIGADA" : "DESLIGADA");
        if (remainingTime > 0) {
            Serial.printf(" (Timer: %ds)", remainingTime);
        }
        Serial.println();
    });
    
    Serial.println("✅ Sistema de iluminação inicializado");
    
    // Configurar horários de iluminação
    setupLightingSchedule();
}

void loop() {
    lightingSystem.update();
    
    // Verificar horários de iluminação
    checkLightingSchedule();
    
    delay(100);
}

void setupLightingSchedule() {
    Serial.println("⏰ Configurando horários de iluminação...");
    
    // Exemplo: Ligar luzes às 6:00 e desligar às 18:00
    // (Implementação simplificada - em produção usar RTC)
    
    Serial.println("✅ Horários configurados:");
    Serial.println("   🌅 Luzes ligam às 06:00");
    Serial.println("   🌇 Luzes desligam às 18:00");
    Serial.println("   🌙 Modo noturno: luzes vermelhas apenas");
}

void checkLightingSchedule() {
    // Exemplo simplificado de controle por horário
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 60000) { // Verificar a cada minuto
        // Simular horário (em produção usar RTC)
        int currentHour = (millis() / 3600000) % 24;
        
        if (currentHour >= 6 && currentHour < 18) {
            // Horário diurno - luzes principais
            if (!lightingSystem.getRelayState(RELAY_LED_MAIN)) {
                Serial.println("🌅 Ativando iluminação diurna");
                activateDayLighting();
            }
        } else {
            // Horário noturno - luzes vermelhas apenas
            if (lightingSystem.getRelayState(RELAY_LED_MAIN)) {
                Serial.println("🌙 Ativando iluminação noturna");
                activateNightLighting();
            }
        }
        
        lastCheck = millis();
    }
}

void activateDayLighting() {
    // Ligar luzes principais
    lightingSystem.setRelay(RELAY_LED_MAIN, true);
    lightingSystem.setRelay(RELAY_LED_WHITE, true);
    lightingSystem.setRelay(RELAY_FAN, true);
    
    // Desligar luzes noturnas
    lightingSystem.setRelay(RELAY_LED_RED, false);
    lightingSystem.setRelay(RELAY_LED_BLUE, false);
}

void activateNightLighting() {
    // Desligar luzes principais
    lightingSystem.setRelay(RELAY_LED_MAIN, false);
    lightingSystem.setRelay(RELAY_LED_WHITE, false);
    lightingSystem.setRelay(RELAY_FAN, false);
    
    // Ligar luzes noturnas
    lightingSystem.setRelay(RELAY_LED_RED, true);
    lightingSystem.setRelay(RELAY_LED_BLUE, true);
}

String getLightingName(int relayNumber) {
    const char* names[] = {
        "Luz Principal", "Luz Auxiliar", "Luz Vermelha", "Luz Azul",
        "Luz Branca", "Luz UV", "Ventilador", "Reserva"
    };
    
    if (relayNumber >= 8 && relayNumber < 16) {
        return String(names[relayNumber - 8]);
    }
    return "Desconhecido";
}
```

## 4. EXEMPLO DE SISTEMA DE MONITORAMENTO

### 4.1 Monitoramento Completo
```cpp
#include <Arduino.h>
#include "I2CScanner.h"
#include "RelayController.h"

RelayController monitorSystem;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("📊 Sistema de Monitoramento");
    Serial.println("===========================");
    
    // Inicializar I2C Scanner
    I2CScanner::begin();
    
    // Inicializar RelayController
    if (!monitorSystem.begin()) {
        Serial.println("❌ Falha na inicialização");
        return;
    }
    
    // Executar diagnóstico completo
    performCompleteDiagnostics();
    
    // Configurar monitoramento periódico
    setupPeriodicMonitoring();
}

void loop() {
    monitorSystem.update();
    
    // Monitoramento periódico
    static unsigned long lastMonitor = 0;
    if (millis() - lastMonitor > 30000) { // A cada 30 segundos
        performPeriodicMonitoring();
        lastMonitor = millis();
    }
    
    delay(100);
}

void performCompleteDiagnostics() {
    Serial.println("\n🔍 === DIAGNÓSTICO COMPLETO ===");
    
    // 1. Escanear dispositivos I2C
    Serial.println("1. Escaneando dispositivos I2C...");
    std::vector<I2CScanner::I2CDevice> devices = I2CScanner::scanAll();
    Serial.println("   Dispositivos encontrados: " + String(devices.size()));
    
    // 2. Procurar PCF8574
    Serial.println("2. Procurando PCF8574...");
    std::vector<uint8_t> pcfList = I2CScanner::findAllPCF8574();
    Serial.println("   PCF8574 encontrados: " + String(pcfList.size()));
    
    // 3. Testar cada relé
    Serial.println("3. Testando relés...");
    testAllRelays();
    
    // 4. Verificar memória
    Serial.println("4. Verificando memória...");
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.println("   Memória livre: " + String(freeHeap) + " bytes");
    
    // 5. Status do sistema
    Serial.println("5. Status do sistema:");
    monitorSystem.printStatus();
    
    Serial.println("✅ Diagnóstico concluído\n");
}

void testAllRelays() {
    Serial.println("   Testando relés 0-15...");
    
    for (int i = 0; i < 16; i++) {
        // Ligar relé
        bool success1 = monitorSystem.setRelay(i, true);
        delay(100);
        
        // Desligar relé
        bool success2 = monitorSystem.setRelay(i, false);
        delay(100);
        
        if (success1 && success2) {
            Serial.printf("     Relé %2d: ✅ OK\n", i);
        } else {
            Serial.printf("     Relé %2d: ❌ FALHA\n", i);
        }
    }
}

void setupPeriodicMonitoring() {
    Serial.println("⏰ Configurando monitoramento periódico...");
    Serial.println("   - Verificação a cada 30 segundos");
    Serial.println("   - Status de relés");
    Serial.println("   - Status de PCF8574");
    Serial.println("   - Uso de memória");
}

void performPeriodicMonitoring() {
    Serial.println("\n📊 === MONITORAMENTO PERIÓDICO ===");
    
    // Status dos relés
    Serial.println("🔌 Status dos relés:");
    int activeRelays = 0;
    for (int i = 0; i < 16; i++) {
        if (monitorSystem.getRelayState(i)) {
            activeRelays++;
            int remaining = monitorSystem.getRemainingTime(i);
            Serial.printf("   Relé %2d: 🔴 LIGADO", i);
            if (remaining > 0) {
                Serial.printf(" (Timer: %ds)", remaining);
            }
            Serial.println();
        }
    }
    
    if (activeRelays == 0) {
        Serial.println("   Todos os relés desligados");
    }
    
    // Status de memória
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.printf("💾 Memória: %d bytes livres", freeHeap);
    if (freeHeap < 10000) {
        Serial.print(" ⚠️ BAIXA");
    } else {
        Serial.print(" ✅ OK");
    }
    Serial.println();
    
    // Uptime
    unsigned long uptime = millis() / 1000;
    Serial.printf("⏱️ Uptime: %lu segundos (%lu minutos)\n", uptime, uptime / 60);
    
    Serial.println("================================\n");
}
```

## 5. EXEMPLO DE SISTEMA DE BACKUP

### 5.1 Sistema com PCF8574 de Backup
```cpp
#include <Arduino.h>
#include "I2CScanner.h"
#include "RelayController.h"

RelayController primarySystem;
RelayController backupSystem;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🔄 Sistema de Backup PCF8574");
    Serial.println("============================");
    
    // Inicializar I2C Scanner
    I2CScanner::begin();
    
    // Procurar PCF8574 disponíveis
    std::vector<uint8_t> pcfList = I2CScanner::findAllPCF8574();
    
    if (pcfList.size() >= 2) {
        Serial.println("✅ Múltiplos PCF8574 encontrados");
        Serial.println("   Sistema de backup disponível");
        
        // Configurar sistema principal e backup
        setupBackupSystem(pcfList);
    } else {
        Serial.println("⚠️ Apenas " + String(pcfList.size()) + " PCF8574 encontrado(s)");
        Serial.println("   Sistema de backup não disponível");
    }
}

void loop() {
    // Atualizar sistemas
    primarySystem.update();
    backupSystem.update();
    
    // Verificar saúde dos sistemas
    static unsigned long lastHealthCheck = 0;
    if (millis() - lastHealthCheck > 60000) { // A cada 1 minuto
        performHealthCheck();
        lastHealthCheck = millis();
    }
    
    delay(100);
}

void setupBackupSystem(const std::vector<uint8_t>& pcfList) {
    Serial.println("🔧 Configurando sistema de backup...");
    
    // Sistema principal (primeiro PCF8574)
    Serial.println("   Sistema principal: PCF8574 em 0x" + String(pcfList[0], HEX));
    
    // Sistema de backup (segundo PCF8574)
    Serial.println("   Sistema backup: PCF8574 em 0x" + String(pcfList[1], HEX));
    
    // Configurar callbacks
    primarySystem.setStateChangeCallback([](int relayNumber, bool state, int remainingTime) {
        Serial.printf("🔴 Principal - Relé %d: %s\n", relayNumber, state ? "LIGADO" : "DESLIGADO");
    });
    
    backupSystem.setStateChangeCallback([](int relayNumber, bool state, int remainingTime) {
        Serial.printf("🟡 Backup - Relé %d: %s\n", relayNumber, state ? "LIGADO" : "DESLIGADO");
    });
    
    Serial.println("✅ Sistema de backup configurado");
}

void performHealthCheck() {
    Serial.println("\n🏥 === VERIFICAÇÃO DE SAÚDE ===");
    
    // Verificar sistema principal
    bool primaryOK = primarySystem.isOperational();
    Serial.println("🔴 Sistema Principal: " + String(primaryOK ? "✅ OK" : "❌ FALHA"));
    
    // Verificar sistema de backup
    bool backupOK = backupSystem.isOperational();
    Serial.println("🟡 Sistema Backup: " + String(backupOK ? "✅ OK" : "❌ FALHA"));
    
    // Determinar sistema ativo
    if (primaryOK) {
        Serial.println("🎯 Sistema ativo: Principal");
    } else if (backupOK) {
        Serial.println("🎯 Sistema ativo: Backup (Failover)");
    } else {
        Serial.println("❌ Ambos os sistemas falharam");
    }
    
    Serial.println("================================\n");
}
```

## 6. EXEMPLO DE INTEGRAÇÃO COM ESP-NOW

### 6.1 Master Controller com I2C Scanner
```cpp
#include <Arduino.h>
#include "I2CScanner.h"
#include "RelayController.h"
#include "ESPNowController.h"

RelayController localRelaySystem;
ESPNowController espNow("MasterWithI2C", 1);

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("🚀 Master com I2C Scanner");
    Serial.println("=========================");
    
    // 1. Inicializar I2C Scanner
    Serial.println("1. Inicializando I2C Scanner...");
    I2CScanner::begin();
    
    // 2. Escanear dispositivos
    Serial.println("2. Escaneando dispositivos I2C...");
    std::vector<I2CScanner::I2CDevice> devices = I2CScanner::scanAll();
    
    // 3. Procurar PCF8574
    Serial.println("3. Procurando PCF8574...");
    std::vector<uint8_t> pcfList = I2CScanner::findAllPCF8574();
    
    if (pcfList.size() > 0) {
        Serial.println("✅ PCF8574 encontrados: " + String(pcfList.size()));
        
        // 4. Inicializar RelayController
        Serial.println("4. Inicializando RelayController...");
        if (localRelaySystem.begin()) {
            Serial.println("✅ RelayController inicializado");
        } else {
            Serial.println("❌ Falha na inicialização do RelayController");
        }
    } else {
        Serial.println("⚠️ Nenhum PCF8574 encontrado");
        Serial.println("   Sistema funcionará apenas como Master ESP-NOW");
    }
    
    // 5. Inicializar ESP-NOW
    Serial.println("5. Inicializando ESP-NOW...");
    if (espNow.begin()) {
        Serial.println("✅ ESP-NOW inicializado");
    } else {
        Serial.println("❌ Falha na inicialização do ESP-NOW");
    }
    
    // 6. Configurar callbacks
    setupCallbacks();
    
    Serial.println("🎯 Sistema Master inicializado com sucesso!");
}

void loop() {
    // Atualizar sistemas
    if (localRelaySystem.isOperational()) {
        localRelaySystem.update();
    }
    espNow.update();
    
    // Exemplo de comandos automáticos
    static unsigned long lastCommand = 0;
    if (millis() - lastCommand > 60000) { // A cada 1 minuto
        runAutomatedCommands();
        lastCommand = millis();
    }
    
    delay(100);
}

void setupCallbacks() {
    // Callback para comandos ESP-NOW
    espNow.setRelayCommandCallback([](const uint8_t* senderMac, int relayNumber, const String& action, int duration) {
        Serial.printf("📨 Comando ESP-NOW: Relé %d, Ação: %s\n", relayNumber, action.c_str());
        
        // Executar comando no sistema local
        if (localRelaySystem.isOperational()) {
            localRelaySystem.processCommand(relayNumber, action, duration);
        }
    });
    
    // Callback para mudanças de estado local
    if (localRelaySystem.isOperational()) {
        localRelaySystem.setStateChangeCallback([](int relayNumber, bool state, int remainingTime) {
            Serial.printf("🔌 Local - Relé %d: %s\n", relayNumber, state ? "LIGADO" : "DESLIGADO");
        });
    }
}

void runAutomatedCommands() {
    Serial.println("🤖 Executando comandos automáticos...");
    
    if (localRelaySystem.isOperational()) {
        // Exemplo: Ligar relé 0 por 30 segundos
        localRelaySystem.setRelayWithTimer(0, true, 30);
        Serial.println("   Relé 0 ligado por 30 segundos");
    }
    
    // Exemplo: Enviar comando para dispositivos remotos
    // (implementar conforme necessário)
}
```

Estes exemplos fornecem implementações práticas e completas para diferentes cenários de uso do sistema I2C Scanner e PCF8574, desde detecção básica até sistemas complexos com backup e integração ESP-NOW.

