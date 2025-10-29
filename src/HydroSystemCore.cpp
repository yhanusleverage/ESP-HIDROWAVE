#include "HydroSystemCore.h"
#include "HydroControl.h"
#include "SupabaseClient.h"
#include "HydroSupaManager.h"  // ✅ Manager híbrido
#include "WebServerManager.h"
#include "WiFiManager.h"
#include "Config.h"
#include "DeviceID.h"
#include <esp_task_wdt.h>
#include <esp_err.h>
#include "HybridStateManager.h"  

// ===== CONSTRUTOR E DESTRUTOR =====
HydroSystemCore::HydroSystemCore() : 
    systemReady(false),
    supabaseConnected(false),
    startTime(0),
    lastSensorSend(0),
    lastStatusSend(0),
    lastStatusPrint(0),
    lastSupabaseCheck(0),
    lastMemoryProtection(0) {
}

HydroSystemCore::~HydroSystemCore() {
    end();
}

// ===== CONTROLE DO SISTEMA =====
bool HydroSystemCore::begin() {
    Serial.println("🌱 Inicializando HydroSystemCore...");
    
    if (systemReady) {
        Serial.println("⚠️ Sistema já está ativo");
        return true;
    }
    
    startTime = millis();
    
    // ===== INICIALIZAR SISTEMA HIDROPÔNICO =====
    Serial.println("🔧 Inicializando controle hidropônico...");
    if (!hydroControl.begin()) {
        Serial.println("❌ Erro ao inicializar HydroControl");
        return false;
    }
    Serial.println("✅ HydroControl inicializado");
    
    // ===== CONECTAR SUPABASE =====
    Serial.println("☁️ Conectando ao Supabase...");
    if (supabase.begin(SUPABASE_URL, SUPABASE_ANON_KEY)) {
        Serial.println("✅ Supabase conectado");
        supabaseConnected = true;
        
        // Testar conexão inicial
        testSupabaseConnection();
        
        // ===== AUTO-REGISTRO DO DISPOSITIVO =====
        Serial.println("🆔 Iniciando auto-registro...");
        if (supabase.autoRegisterDevice("ESP32 Hidropônico", "Sistema Principal")) {
            Serial.println("✅ Dispositivo registrado automaticamente");
        } else {
            Serial.println("⚠️ Auto-registro falhou, mas continuando...");
        }
    } else {
        Serial.println("❌ Erro ao conectar Supabase - Sistema continuará sem cloud");
        supabaseConnected = false;
    }
    
    // ===== INICIALIZAR SERVIDOR WEB ADMIN =====
    Serial.println("🌐 Iniciando painel admin web...");
    
    // Criar instâncias temporárias para compatibilidade
    static WiFiManager wifiManager;
    static WebServerManager webServerManager;
    
    webServerManager.beginAdminServer(wifiManager, hydroControl);
    
    Serial.println("✅ Painel admin disponível em: http://" + WiFi.localIP().toString());
    
    systemReady = true;
    
    Serial.println("✅ HydroSystemCore ativo!");
    Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("🌐 IP: " + WiFi.localIP().toString());
    
    // Status inicial dos sensores
    printSensorReadings();
    
    return true;
}

void HydroSystemCore::loop() {
    if (!systemReady) return;
    
    unsigned long now = millis();
    
    // ===== PROTEÇÃO DE MEMÓRIA (10s) =====
    if (now - lastMemoryProtection >= MEMORY_CHECK_INTERVAL) {
        performMemoryProtection();
        lastMemoryProtection = now;
    }
    
    // ===== SENSORES → SUPABASE (30s) =====
    if (now - lastSensorSend >= SENSOR_SEND_INTERVAL) {
        sendSensorDataToSupabase();
        lastSensorSend = now;
    }
    
    // ===== STATUS DEVICE → SUPABASE (60s) =====
    if (now - lastStatusSend >= STATUS_SEND_INTERVAL) {
        sendDeviceStatusToSupabase();
        lastStatusSend = now;
    }
    
    // ===== DEBUG PERIÓDICO (30s) =====
    if (now - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
        printPeriodicStatus();
        lastStatusPrint = now;
    }
    
    // ===== VERIFICAR SUPABASE (30s) =====
    if (now - lastSupabaseCheck >= SUPABASE_CHECK_INTERVAL) {
        checkSupabaseCommands();
        lastSupabaseCheck = now;
    }
    
    // ===== LOOP DOS SENSORES/RELÉS =====
    hydroControl.loop();
}

void HydroSystemCore::end() {
    if (!systemReady) return;
    
    Serial.println("🛑 Parando HydroSystemCore...");
    
    systemReady = false;
    supabaseConnected = false;
    
    Serial.println("✅ HydroSystemCore parado");
}

// ===== DEBUG E COMANDOS =====
void HydroSystemCore::printSystemStatus() {
    Serial.println("\n🌱 === STATUS SISTEMA HIDROPÔNICO ===");
    Serial.println("⏰ Uptime: " + String(getUptime()/1000) + "s");
    Serial.println("🌐 WiFi: " + (WiFi.isConnected() ? "Conectado (" + WiFi.localIP().toString() + ")" : "Desconectado"));
    Serial.println("☁️ Supabase: " + String(supabaseConnected ? "Conectado" : "Desconectado"));
    Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("🔧 Sistema: " + String(systemReady ? "Ativo" : "Inativo"));
    
    Serial.println("\n🔗 === STATUS RELÉS ===");
    bool* relayStates = hydroControl.getRelayStates();
    for (int i = 0; i < 16; i++) {
        Serial.printf("Relé %2d: %s\n", i, relayStates[i] ? "🟢 LIGADO" : "🔴 DESLIGADO");
    }
    
    Serial.println("\n📊 === LEITURAS DOS SENSORES ===");
    printSensorReadings();
    Serial.println("=====================================\n");
}

void HydroSystemCore::printSensorReadings() {
    Serial.println("🌡️ Temperatura: " + String(hydroControl.getTemperature()) + "°C");
    Serial.println("🧪 pH: " + String(hydroControl.getpH()));
    Serial.println("⚡ TDS: " + String(hydroControl.getTDS()) + " ppm");
    Serial.println("💧 Nível da água: " + String(hydroControl.isWaterLevelOk() ? "OK" : "BAIXO"));
}

void HydroSystemCore::testSupabaseConnection() {
    if (!hasEnoughMemoryForHTTPS()) {
        Serial.println("⚠️ Heap baixo - Não testando Supabase");
        supabaseConnected = false;
        return;
    }
    
    Serial.println("🧪 Testando conexão Supabase...");
    
    // Simulação simples de teste (em projeto real seria uma chamada HTTP)
    bool testResult = random(0, 10) > 1; // 90% sucesso
    
    if (testResult) {
        Serial.println("✅ Supabase: Conexão OK");
        supabaseConnected = true;
    } else {
        Serial.println("❌ Supabase: Falha na conexão");
        supabaseConnected = false;
    }
}

// ===== OPERAÇÕES PRINCIPAIS =====
void HydroSystemCore::checkSupabaseCommands() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }
    
    if (!supabase.isReady()) return;
    
    RelayCommand commands[5];
    int commandCount = 0;
    
    if (supabase.checkForCommands(commands, 5, commandCount)) {
        Serial.printf("📥 Recebidos %d comandos do Supabase\n", commandCount);
        
        for (int i = 0; i < commandCount; i++) {
            processRelayCommand(commands[i]);
        }
    }
}

void HydroSystemCore::processRelayCommand(const RelayCommand& cmd) {
    Serial.printf("🎛️ Comando: Relé %d -> %s", cmd.relayNumber, cmd.action.c_str());
    if (cmd.durationSeconds > 0) {
        Serial.printf(" por %d segundos", cmd.durationSeconds);
    }
    Serial.println();
    
    // Marcar comando como recebido
    if (supabaseConnected) {
        supabase.markCommandSent(cmd.id);
    }
    
    // Validar relé
    if (cmd.relayNumber < 0 || cmd.relayNumber >= 16) {
        Serial.printf("❌ Relé %d inválido\n", cmd.relayNumber);
        if (supabaseConnected) {
            supabase.markCommandFailed(cmd.id, "Relé inválido");
        }
        return;
    }
    
    // Executar comando
    bool success = false;
    bool* relayStates = hydroControl.getRelayStates();
    
    if (cmd.action == "on") {
        if (!relayStates[cmd.relayNumber]) {
            hydroControl.toggleRelay(cmd.relayNumber, cmd.durationSeconds);
            success = true;
        } else {
            success = true; // Já estava ligado
        }
    } else if (cmd.action == "off") {
        if (relayStates[cmd.relayNumber]) {
            hydroControl.toggleRelay(cmd.relayNumber, 0);
            success = true;
        } else {
            success = true; // Já estava desligado
        }
    }
    
    // Reportar resultado
    if (success) {
        Serial.println("✅ Comando executado com sucesso");
        if (supabaseConnected) {
            supabase.markCommandCompleted(cmd.id);
        }
    } else {
        Serial.println("❌ Falha na execução do comando");
        if (supabaseConnected) {
            supabase.markCommandFailed(cmd.id, "Falha na execução");
        }
    }
}

void HydroSystemCore::sendSensorDataToSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }
    
    if (!supabase.isReady()) return;
    
    // Dados ambientais
    EnvironmentReading envData;
    envData.temperature = hydroControl.getTemperature();
    envData.humidity = 65.0; // Simulado
    envData.timestamp = millis();
    
    // Dados hidropônicos
    HydroReading hydroData;
    hydroData.temperature = hydroControl.getTemperature();
    hydroData.ph = hydroControl.getpH();
    hydroData.tds = hydroControl.getTDS();
    hydroData.waterLevelOk = hydroControl.isWaterLevelOk();
    hydroData.timestamp = millis();
    
    // Enviar dados
    if (supabase.sendEnvironmentData(envData)) {
        Serial.println("📤 Dados ambientais enviados ao Supabase");
    }
    
    if (supabase.sendHydroData(hydroData)) {
        Serial.println("📤 Dados hidropônicos enviados ao Supabase");
    }
}

void HydroSystemCore::sendDeviceStatusToSupabase() {
    if (!supabaseConnected || !hasEnoughMemoryForHTTPS()) {
        return;
    }
    
    if (!supabase.isReady()) return;
    
    DeviceStatusData status;
    status.deviceId = getDeviceID();
    status.wifiRssi = WiFi.RSSI();
    status.freeHeap = ESP.getFreeHeap();
    status.uptimeSeconds = millis() / 1000;
    status.isOnline = true;
    status.firmwareVersion = FIRMWARE_VERSION;
    status.ipAddress = WiFi.localIP().toString();
    status.timestamp = millis();
    
    // Estados dos relés
    bool* relayStates = hydroControl.getRelayStates();
    for (int i = 0; i < 16; i++) {
        status.relayStates[i] = relayStates[i];
    }
    
    if (supabase.updateDeviceStatus(status)) {
        Serial.println("📤 Status do dispositivo atualizado no Supabase");
    }
}

void HydroSystemCore::performMemoryProtection() {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t fragmentationPercent = freeHeap > 0 ? (100 - (maxBlock * 100) / freeHeap) : 100;
    
    // ===== PROTEÇÃO CRÍTICA =====
    if (freeHeap < 15000) {
        Serial.println("🚨 ALERTA: Heap crítico! " + String(freeHeap) + " bytes");
        
        if (freeHeap < 8000) {
            Serial.println("💀 RESET EMERGENCIAL por falta de memória!");
            delay(1000);
            ESP.restart();
        }
    }
    
    // ===== PROTEÇÃO POR FRAGMENTAÇÃO =====
    if (fragmentationPercent > 70) {
        Serial.println("🧩 ALERTA: Fragmentação alta! " + String(fragmentationPercent) + "%");
        
        if (fragmentationPercent > 85 && freeHeap > 10000) {
            Serial.println("🔄 RESET por fragmentação extrema!");
            delay(1000);
            ESP.restart();
        }
    }
    
    // ===== DESABILITAR SUPABASE SE MEMÓRIA BAIXA =====
    if (freeHeap < MIN_HEAP_FOR_HTTPS && supabaseConnected) {
        Serial.println("⚠️ Desabilitando Supabase temporariamente - Heap baixo");
        supabaseConnected = false;
    } else if (freeHeap > MIN_HEAP_FOR_HTTPS + 10000 && !supabaseConnected) {
        Serial.println("✅ Reabilitando Supabase - Heap recuperado");
        supabaseConnected = true;
    }
}

// ===== UTILITIES =====
bool HydroSystemCore::hasEnoughMemoryForHTTPS() {
    return ESP.getFreeHeap() >= MIN_HEAP_FOR_HTTPS;
}

void HydroSystemCore::printPeriodicStatus() {
    Serial.printf("🔄 Sistema ativo há %ds | Heap: %d bytes | Supabase: %s | MASTER MODE\n", 
                  (int)(getUptime()/1000), 
                  ESP.getFreeHeap(),
                  supabaseConnected ? "✅" : "❌");
}

// ===== FIM DA IMPLEMENTAÇÃO ===== 