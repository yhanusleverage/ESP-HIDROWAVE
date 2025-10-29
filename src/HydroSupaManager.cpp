#include "HydroSupaManager.h"
#include "DeviceID.h"

HydroSupaManager::HydroSupaManager() :
    httpClient(),
    realtimeClient(),
    isInitialized(false),
    useWebSocket(true),
    lastHttpSend(0),
    lastStatusUpdate(0),
    lastCommandCheck(0),
    httpFailures(0),
    wsFailures(0) {
}

HydroSupaManager::~HydroSupaManager() {
    end();
}

bool HydroSupaManager::begin(const String& url, const String& key) {
    baseUrl = url;
    apiKey = key;
    deviceId = getDeviceID();
    
    Serial.println("🌊 Iniciando Hybrid Supabase Manager...");
    
    // Inicializar cliente HTTP
    if (!httpClient.begin(url, key)) {
        Serial.println("❌ Erro ao inicializar cliente HTTP");
        return false;
    }
    
    Serial.println("✅ Cliente HTTP inicializado");
    
    // Auto-registrar dispositivo via HTTP
    if (httpClient.autoRegisterDevice("ESP32 Hidropônico - Híbrido", "Sistema Principal")) {
        Serial.println("✅ Dispositivo auto-registrado via HTTP");
    }
    
    // Tentar inicializar WebSocket (não crítico)
    if (initializeWebSocket()) {
        Serial.println("✅ WebSocket Realtime inicializado");
        useWebSocket = true;
    } else {
        Serial.println("⚠️ WebSocket falhou - usando apenas HTTP");
        useWebSocket = false;
    }
    
    isInitialized = true;
    
    Serial.printf("🌊 Hybrid Manager ativo | HTTP: ✅ | WebSocket: %s\n", 
                  useWebSocket ? "✅" : "❌");
    
    return true;
}

bool HydroSupaManager::initializeWebSocket() {
    if (ESP.getFreeHeap() < 40000) {
        Serial.println("⚠️ Heap insuficiente para WebSocket - usando apenas HTTP");
        return false;
    }
    
    // Configurar callbacks
    realtimeClient.setCommandCallback([this](int relay, String action, int duration) {
        this->handleWebSocketCommand(relay, action, duration);
    });
    
    realtimeClient.setErrorCallback([this](String error) {
        this->handleWebSocketError(error);
    });
    
    return realtimeClient.begin(baseUrl, apiKey, deviceId);
}

void HydroSupaManager::loop() {
    if (!isInitialized) return;
    
    unsigned long now = millis();
    
    // ===== WEBSOCKET LOOP (se ativo) =====
    if (useWebSocket) {
        realtimeClient.loop();
        
        // Verificar se WebSocket está falhando muito
        if (!realtimeClient.isConnected() && wsFailures > MAX_WS_FAILURES) {
            Serial.println("⚠️ WebSocket falhando muito - desabilitando temporariamente");
            useWebSocket = false;
            wsFailures = 0;
            realtimeClient.end();
        }
    }
    
    // ===== HTTP HEARTBEAT + DADOS (30s) =====
    if (now - lastHttpSend >= HTTP_SEND_INTERVAL) {
        sendHttpData();
        lastHttpSend = now;
    }
    
    // ===== STATUS UPDATE (60s) =====
    if (now - lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
        updateDeviceStatus();
        lastStatusUpdate = now;
    }
    
    // ===== HTTP POLLING (apenas se WebSocket não estiver ativo) =====
    if (!useWebSocket && now - lastCommandCheck >= HTTP_POLL_INTERVAL) {
        checkHttpCommands();
        lastCommandCheck = now;
    }
    
    // ===== TENTAR REATIVAR WEBSOCKET (se desabilitado) =====
    if (!useWebSocket && now - lastWebSocketRetry >= WS_RETRY_INTERVAL) {
        if (ESP.getFreeHeap() > 50000) {
            Serial.println("🔄 Tentando reativar WebSocket...");
            if (initializeWebSocket()) {
                useWebSocket = true;
                wsFailures = 0;
                Serial.println("✅ WebSocket reativado");
            }
        }
        lastWebSocketRetry = now;
    }
}

void HydroSupaManager::end() {
    if (isInitialized) {
        realtimeClient.end();
        isInitialized = false;
        Serial.println("🌊 Hybrid Supabase Manager parado");
    }
}

void HydroSupaManager::sendHttpData() {
    if (!httpClient.isReady()) return;
    
    // Simular dados dos sensores (substituir pela leitura real)
    EnvironmentReading envData = {
        .temperature = 23.5,
        .humidity = 65.0,
        .timestamp = millis()
    };
    
    HydroReading hydroData = {
        .temperature = 22.8,
        .ph = 6.5,
        .tds = 850.0,
        .waterLevelOk = true,
        .timestamp = millis()
    };
    
    // Enviar dados
    bool envSuccess = httpClient.sendEnvironmentData(envData);
    bool hydroSuccess = httpClient.sendHydroData(hydroData);
    
    if (envSuccess && hydroSuccess) {
        Serial.println("📤 Dados HTTP enviados com sucesso");
        httpFailures = 0;
    } else {
        httpFailures++;
        Serial.printf("❌ Falha HTTP (%d/%d)\n", httpFailures, MAX_HTTP_FAILURES);
    }
}

void HydroSupaManager::updateDeviceStatus() {
    if (!httpClient.isReady()) return;
    
    DeviceStatusData status = {
        .deviceId = deviceId,
        .wifiRssi = WiFi.RSSI(),
        .freeHeap = ESP.getFreeHeap(),
        .uptimeSeconds = millis() / 1000,
        .relayStates = {false}, // Substituir pelos estados reais
        .isOnline = true,
        .firmwareVersion = FIRMWARE_VERSION,
        .ipAddress = WiFi.localIP().toString(),
        .timestamp = millis()
    };
    
    if (httpClient.updateDeviceStatus(status)) {
        Serial.println("📤 Status do dispositivo atualizado");
        
        // Se WebSocket ativo, enviar ping também
        if (useWebSocket && realtimeClient.isConnected()) {
            realtimeClient.sendHeartbeatPing();
        }
    }
}

void HydroSupaManager::checkHttpCommands() {
    if (!httpClient.isReady()) return;
    
    RelayCommand commands[5];
    int commandCount = 0;
    
    if (httpClient.checkForCommands(commands, 5, commandCount)) {
        Serial.printf("📥 HTTP: %d comandos recebidos\n", commandCount);
        
        for (int i = 0; i < commandCount; i++) {
            processCommand(commands[i], "HTTP");
        }
    }
}

void HydroSupaManager::handleWebSocketCommand(int relay, String action, int duration) {
    Serial.printf("⚡ WebSocket: Relé %d -> %s", relay, action.c_str());
    if (duration > 0) {
        Serial.printf(" por %ds", duration);
    }
    Serial.println();
    
    // Criar comando para processamento unificado
    RelayCommand cmd = {
        .id = 0, // WebSocket não tem ID específico
        .relayNumber = relay,
        .action = action,
        .durationSeconds = duration,
        .status = "received",
        .timestamp = millis()
    };
    
    processCommand(cmd, "WebSocket");
}

void HydroSupaManager::handleWebSocketError(String error) {
    Serial.println("❌ WebSocket Error: " + error);
    wsFailures++;
    
    if (wsFailures >= MAX_WS_FAILURES) {
        Serial.println("⚠️ Muitos erros WebSocket - desabilitando temporariamente");
        useWebSocket = false;
        realtimeClient.end();
    }
}

void HydroSupaManager::processCommand(const RelayCommand& cmd, const String& source) {
    Serial.printf("🎛️ [%s] Processando: Relé %d -> %s\n", 
                  source.c_str(), cmd.relayNumber, cmd.action.c_str());
    
    // Validar comando
    if (cmd.relayNumber < 0 || cmd.relayNumber >= 16) {
        Serial.printf("❌ Relé %d inválido\n", cmd.relayNumber);
        if (cmd.id > 0) {
            httpClient.markCommandFailed(cmd.id, "Relé inválido");
        }
        return;
    }
    
    // Executar comando (substituir pela implementação real)
    bool success = executeRelayCommand(cmd.relayNumber, cmd.action, cmd.durationSeconds);
    
    // Reportar resultado
    if (success) {
        Serial.println("✅ Comando executado com sucesso");
        if (cmd.id > 0) {
            httpClient.markCommandCompleted(cmd.id);
        }
    } else {
        Serial.println("❌ Falha na execução do comando");
        if (cmd.id > 0) {
            httpClient.markCommandFailed(cmd.id, "Falha na execução");
        }
    }
}

bool HydroSupaManager::executeRelayCommand(int relay, const String& action, int duration) {
    // IMPLEMENTAR: Lógica real de controle dos relés
    // Por enquanto, apenas simula sucesso
    
    Serial.printf("🔧 Executando: Relé %d -> %s", relay, action.c_str());
    if (duration > 0) {
        Serial.printf(" por %ds", duration);
    }
    Serial.println();
    
    // Simular sucesso (substituir pela implementação real)
    return true;
}

void HydroSupaManager::printStatus() {
    Serial.println("\n🌊 === HYBRID SUPABASE MANAGER ===");
    Serial.println("🆔 Device ID: " + deviceId);
    Serial.println("📡 HTTP Client: " + String(httpClient.isReady() ? "✅ Ativo" : "❌ Inativo"));
    Serial.println("⚡ WebSocket: " + String(useWebSocket ? "✅ Ativo" : "❌ Inativo"));
    
    if (useWebSocket) {
        Serial.println("📺 WS Estado: " + realtimeClient.getStateString());
        Serial.println("⏰ WS Uptime: " + String(realtimeClient.getUptime() / 1000) + "s");
    }
    
    Serial.println("❌ HTTP Failures: " + String(httpFailures) + "/" + String(MAX_HTTP_FAILURES));
    Serial.println("❌ WS Failures: " + String(wsFailures) + "/" + String(MAX_WS_FAILURES));
    Serial.println("💾 Heap Livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("=====================================\n");
}
