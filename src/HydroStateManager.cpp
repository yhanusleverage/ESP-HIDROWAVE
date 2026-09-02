#include "HybridStateManager.h"
#include "Config.h"
#include <SPIFFS.h>
#include <esp_task_wdt.h>  // ✅ CRÍTICO: Para esp_task_wdt_reset()
#include "DeviceRegistration.h" // ✅ NOVO: Sistema de registro por email
// ✅ ESPNowBridge eliminado - usar MasterSlaveManager y ESPNowController
#ifdef MASTER_MODE
    #include "MasterSlaveManager.h"
    #include "ESPNowController.h"
#endif

// ===== CONSTRUTOR E DESTRUTOR =====
HydroStateManager::HydroStateManager() : 
    currentState(WIFI_CONFIG_MODE),
    stateStartTime(0),
    wifiServer(nullptr),
    hydroCore(nullptr),
    adminServer(nullptr),
    webServerTask(nullptr),
    espNowController(nullptr),
    masterManager(nullptr),
    wifiReconnectPhase(WIFI_RECONNECT_IDLE),
    wifiReconnectStartedMs(0),
    wifiLastAttemptMs(0),
    wifiReconnectAttempts(0) {
    
    deviceID = "ESP32_HIDRO_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.println("🏗️ HydroStateManager inicializado");
}

HydroStateManager::~HydroStateManager() {
    cleanup();
}

void HydroStateManager::setMasterManager(MasterSlaveManager* masterMgr) {
    masterManager = masterMgr;
    if (hydroCore) {
        hydroCore->setMasterManager(masterMgr);
        Serial.println("✅ MasterSlaveManager re-injectado no HydroSystemCore (late bind)");
    }
}

// ===== MÉTODOS PRINCIPAIS =====
void HydroStateManager::begin() {
    Serial.println("🏗️ Inicializando HydroStateManager...");
    
    // ===== DEBUG DETALHADO DE INICIALIZAÇÃO =====
    Serial.println("\n🔍 === DEBUG INICIALIZAÇÃO ===");
    Serial.println("💾 Heap disponível: " + String(ESP.getFreeHeap()) + " bytes");
    
    // Verificar se SPIFFS está funcionando
    if (!SPIFFS.begin(true)) {
        Serial.println("❌ ERRO: Falha ao montar SPIFFS!");
    } else {
        Serial.println("✅ SPIFFS montado com sucesso");
    }
    
    // ===== INICIALIZAR PREFERENCES COM DEBUG =====
    Serial.println("\n🔑 === PREFERENCES DEBUG ===");
    Serial.println("🔑 Abrindo namespace 'hydro_system'...");
    
    if (!preferences.begin("hydro_system", false)) {
        Serial.println("❌ ERRO CRÍTICO: Falha ao abrir Preferences!");
        Serial.println("🔄 Tentando reiniciar o sistema...");
        delay(3000);
        ESP.restart();
    }
    
    Serial.println("✅ Namespace 'hydro_system' aberto com sucesso");
    
    // ===== VERIFICAÇÃO DETALHADA DAS CREDENCIAIS =====
    Serial.println("\n📋 === VERIFICAÇÃO DE CREDENCIAIS ===");
    
    // ✅ NOVO: Limpar estado residual de WiFi antes de tentar conectar
    // Isso resolve problemas de reconexão após reinício
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("🧹 Limpando estado residual de WiFi...");
        WiFi.disconnect(true);  // true = apagar credenciais da memória
        delay(500);
        WiFi.mode(WIFI_OFF);
        delay(500);
        Serial.println("✅ Estado residual de WiFi limpo");
    }
    
    // Ler todas as credenciais salvas
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");
    String deviceName = preferences.getString("device_name", "");
    
    Serial.println("🔍 SSID lido: '" + ssid + "' (" + String(ssid.length()) + " chars)");
    Serial.println("🔍 Password lido: " + String(password.length() > 0 ? "[PRESENTE - " + String(password.length()) + " chars]" : "[AUSENTE]"));
    Serial.println("🔍 Device Name lido: '" + deviceName + "' (" + String(deviceName.length()) + " chars)");
    
    // Verificar se há credenciais válidas
    bool hasCredentials = hasWiFiCredentials();
    Serial.println("🎯 hasWiFiCredentials(): " + String(hasCredentials ? "true" : "false"));
    
    // Decidir estado inicial baseado nas credenciais WiFi
    if (hasCredentials) {
        Serial.println("\n✅ === CREDENCIAIS ENCONTRADAS ===");
        Serial.println("📝 Tentando conectar à rede: " + ssid);
        
        // ✅ NOVO: Verificar memória antes de conectar WiFi
        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 50000) {
            Serial.printf("⚠️ [CRÍTICO] Heap muito baixo (%u bytes) antes de conectar WiFi!\n", freeHeap);
            Serial.println("   Tentando continuar mesmo assim...");
        }
        
        // Configurar WiFi em modo Station (mesma sequência que runtime reconnect)
        armWifiStation(ssid, password);
        
        Serial.println("🔄 Conectando ao WiFi...");
        
        unsigned long startTime = millis();
        int dotCount = 0;
        
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_RECONNECT_TIMEOUT_MS) {
            delay(500);
            esp_task_wdt_reset();
            
            Serial.print(".");
            dotCount++;
            
            if ((millis() - startTime) > 15000 && WiFi.status() != WL_CONNECTED && dotCount % 10 == 0) {
                Serial.println("\n⚠️ Timeout parcial - resetando WiFi...");
                WiFi.disconnect();
                delay(1000);
                armWifiStation(ssid, password);
                esp_task_wdt_reset();
            }
            
            if (dotCount % 5 == 0) {
                Serial.printf(" [%s] ", wifiStatusString(WiFi.status()));
            }
        }
        
        Serial.println(); // Nova linha após os pontos
        
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(WIFI_PS_NONE);
            Serial.println("\n🎉 === CONEXÃO WiFi ESTABELECIDA ===");
            Serial.println("✅ WiFi conectado com sucesso!");
            Serial.println("🌐 SSID: " + WiFi.SSID());
            Serial.println("🌐 IP: " + WiFi.localIP().toString());
            Serial.println("📶 RSSI: " + String(WiFi.RSSI()) + " dBm");
            Serial.println("🔗 Gateway: " + WiFi.gatewayIP().toString());
            Serial.println("🔗 DNS: " + WiFi.dnsIP().toString());
            Serial.println("📱 MAC: " + WiFi.macAddress());
            
            switchToHydroActive();
        } else {
            Serial.println("\n❌ === FALHA NA CONEXÃO WiFi ===");
            wl_status_t finalStatus = WiFi.status();
            Serial.print("❌ Status final: ");
            switch (finalStatus) {
                case WL_NO_SSID_AVAIL:
                    Serial.println("Rede '" + ssid + "' não encontrada");
                    break;
                case WL_CONNECT_FAILED:
                    Serial.println("Falha na autenticação (senha incorreta?)");
                    break;
                case WL_CONNECTION_LOST:
                    Serial.println("Conexão perdida durante o processo");
                    break;
                case WL_DISCONNECTED:
                    Serial.println("Desconectado (problema de sinal?)");
                    break;
                default:
                    Serial.println("Erro desconhecido (" + String(finalStatus) + ")");
                    break;
            }
            
            Serial.println("🔄 Ativando modo configuração para reconfigurar WiFi");
            switchToWiFiConfig();
        }
    } else {
        Serial.println("\n📝 === NENHUMA CREDENCIAL ENCONTRADA ===");
        Serial.println("🔧 Ativando modo configuração WiFi");
        switchToWiFiConfig();
    }
    
    Serial.println("\n✅ === INICIALIZAÇÃO CONCLUÍDA ===");
    Serial.println("🏗️ Estado inicial: " + getStateString());
    Serial.println("💾 Heap após inicialização: " + String(ESP.getFreeHeap()) + " bytes");
}

void HydroStateManager::loop() {
    unsigned long now = millis();
    
    // Verificar timeouts automáticos
    autoSwitchIfNeeded();
    
    // Executar loop do estado ativo
    switch (currentState) {
        case WIFI_CONFIG_MODE:
            if (wifiServer && wifiServer->isActive()) {
                wifiServer->loop();
            }
            break;
            
        case HYDRO_ACTIVE_MODE:
            if (hydroCore && hydroCore->isReady()) {
                hydroCore->loop();
            }
            break;
            
        case ADMIN_PANEL_MODE:
            // ✅ CORREÇÃO: Executar loop do HydroSystemCore para componentes de solução funcionarem
            if (hydroCore && hydroCore->isReady()) {
                hydroCore->loop();
            }
            // WebSocket opcional para debug adicional
            if (adminServer && adminServer->isActive()) {
                adminServer->loop();
            }
            break;
    }
}

// ===== TRANSIÇÕES DE ESTADO =====
void HydroStateManager::switchToWiFiConfig() {
    Serial.println("\n🌐 === MUDANDO PARA WIFI CONFIG MODE ===");
    
    cleanup();
    currentState = WIFI_CONFIG_MODE;
    stateStartTime = millis();
    
    // Criar e inicializar servidor WiFi
    wifiServer = new WiFiConfigServer();
    
    // Configurar callback para quando WiFi for configurado
    wifiServer->onWiFiConfigured([this]() {
        Serial.println("✅ WiFi configurado com sucesso!");
        delay(2000);
        ESP.restart(); // Reiniciar para aplicar configurações
    });
    
    // ✅ NOVO: Configurar callback para registro com email
    wifiServer->onEmailRegistered([this](String userEmail, String deviceName, String location) {
        Serial.println("📧 Email recebido para registro: " + userEmail);
        Serial.println("🏷️ Nome do dispositivo (portal): " + deviceName);
        Serial.println("📍 Localização (portal): " + location);
        
        if (registerDeviceWithEmail(userEmail, deviceName, location)) {
            Serial.println("🎉 Dispositivo registrado com sucesso no Supabase!");
        } else {
            Serial.println("❌ Erro ao registrar dispositivo no Supabase");
        }
    });
    
    if (wifiServer->begin()) {
        Serial.println("✅ WiFi Config Server iniciado");
        Serial.println("🌐 Acesse: http://192.168.4.1");
        Serial.println("⏰ Timeout: 5 minutos");
    } else {
        Serial.println("❌ Erro ao iniciar WiFi Config Server");
    }
}

void HydroStateManager::switchToHydroActive() {
    Serial.println("\n🌱 === MUDANDO PARA HYDRO ACTIVE MODE ===");
    
    cleanup();
    currentState = HYDRO_ACTIVE_MODE;
    stateStartTime = millis();
    resetWifiReconnectState();
    
    // Verificar conexão WiFi
    if (!WiFi.isConnected()) {
        Serial.println("❌ WiFi não conectado - Não é possível ativar modo hidropônico");
        switchToWiFiConfig();
        return;
    }
    
    // ✅ CORREÇÃO: Usar injeção de dependências em vez de extern (anti-patrón)
    // Criar e inicializar sistema hidropônico com injeção de dependências
    #ifdef MASTER_MODE
        hydroCore = new HydroSystemCore(webServerTask, espNowController, masterManager);
    #else
        hydroCore = new HydroSystemCore(webServerTask, espNowController, nullptr);
    #endif
    
    if (hydroCore->begin()) {
        Serial.println("✅ Sistema Hidropônico ativo");
        Serial.println("🌐 IP: " + WiFi.localIP().toString());
        Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
        
        // Log das dependências injetadas
        if (webServerTask) {
            Serial.println("✅ WebServerTask integrado ao HydroSystemCore");
        }
        if (espNowController) {
            Serial.println("✅ ESPNowController integrado ao HydroSystemCore");
        }
    } else {
        Serial.println("❌ Erro ao inicializar sistema hidropônico");
    }
}

void HydroStateManager::switchToAdminPanel() {
    Serial.println("\n🔧 === MUDANDO PARA ADMIN PANEL MODE ===");
    
    cleanup();
    currentState = ADMIN_PANEL_MODE;
    stateStartTime = millis();
    
    // Verificar conexão WiFi
    if (!WiFi.isConnected()) {
        Serial.println("❌ WiFi não conectado - Admin Panel requer conexão");
        switchToWiFiConfig();
        return;
    }
    
    // ✅ CORREÇÃO CRÍTICA: Inicializar HydroSystemCore para que os componentes de solução funcionem
    // ✅ CORREÇÃO: Usar injeção de dependências em vez de extern (anti-patrón)
    // Criar e inicializar sistema hidropônico com injeção de dependências
    #ifdef MASTER_MODE
        hydroCore = new HydroSystemCore(webServerTask, espNowController, masterManager);
    #else
        hydroCore = new HydroSystemCore(webServerTask, espNowController, nullptr);
    #endif
    
    if (!hydroCore->begin()) {
        Serial.println("❌ Erro ao inicializar HydroSystemCore no Admin Panel");
        // Voltar para modo hidropônico se falhar
        switchToHydroActive();
        return;
    }
    Serial.println("✅ HydroSystemCore inicializado - Componentes de solução ativos");
    
    // Criar e inicializar servidor WebSocket (opcional, para debug adicional)
    adminServer = new AdminWebSocketServer();
    
    if (adminServer->begin()) {
        Serial.println("✅ Admin Panel WebSocket ativo");
        Serial.println("🌐 Acesse: http://" + WiFi.localIP().toString());
        Serial.println("🔌 WebSocket: ws://" + WiFi.localIP().toString() + ":81/ws");
        Serial.println("⏰ Auto-desliga em 5 minutos");
        Serial.println("💾 Heap livre: " + String(ESP.getFreeHeap()) + " bytes");
        Serial.println("🌱 Componentes de solução: ✅ ATIVOS (sensores e relés funcionando)");
    } else {
        Serial.println("⚠️ AdminWebSocketServer não inicializado, mas HydroSystemCore está ativo");
        Serial.println("💡 Componentes de solução continuam funcionando via WebServerManager");
    }
}

// ===== UTILITIES =====
String HydroStateManager::getStateString() const {
    switch (currentState) {
        case WIFI_CONFIG_MODE: return "WiFi Config Mode";
        case HYDRO_ACTIVE_MODE: return "Hydro Active Mode";
        case ADMIN_PANEL_MODE: return "Admin Panel Mode";
        default: return "Unknown";
    }
}

void HydroStateManager::handleSerialCommand(const String& command) {
    if (command == "sensors" && currentState == HYDRO_ACTIVE_MODE) {
        if (hydroCore) hydroCore->printSensorReadings();
    }
    else if (command == "supabase" && currentState == HYDRO_ACTIVE_MODE) {
        if (hydroCore) hydroCore->testSupabaseConnection();
    }
    else if (command == "hydro_status" && currentState == HYDRO_ACTIVE_MODE) {
        if (hydroCore) hydroCore->printSystemStatus();
    }
    else if (command == "wifi_status" && currentState == WIFI_CONFIG_MODE) {
        if (wifiServer) {
            Serial.println("\n📊 WiFi Config Status:");
            Serial.println("⏰ Uptime: " + String(wifiServer->getUptime()/1000) + "s");
            Serial.println("🌐 AP IP: " + wifiServer->getAPIP());
            Serial.println("👥 Conexões: " + String(wifiServer->getActiveConnections()));
        }
    }
    else if (command == "admin_status" && currentState == ADMIN_PANEL_MODE) {
        if (adminServer) {
            Serial.println("\n📊 Admin Panel Status:");
            Serial.println("⏰ Uptime: " + String(adminServer->getUptime()/1000) + "s");
            Serial.println("🔌 Clientes WS: " + String(adminServer->getConnectedClients()));
            Serial.println("⏰ Auto-shutdown em: " + String((300000 - adminServer->getUptime())/1000) + "s");
        }
    }
    else if (command == "espnow_status") {
        // Comando ESP-NOW disponível em todos os estados
        printESPNowStatus();
    }
    else if (hydroCore && (command.startsWith("EC ") || command == "EC CAL 1413")) {
        if (currentState == HYDRO_ACTIVE_MODE || currentState == ADMIN_PANEL_MODE) {
            if (hydroCore->getHydroControl().processEcSerialCommand(command)) {
                return;
            }
        }
        Serial.println("❓ Comando EC no disponible en estado: " + getStateString());
    }
    else {
        // ✅ CORREÇÃO: Ignorar comandos vazios silenciosamente
        if (command.length() == 0) {
            return;
        }
        Serial.println("❓ Comando '" + command + "' não reconhecido no estado atual: " + getStateString());
        Serial.println("💡 Comandos disponíveis por estado:");
        Serial.println("   WiFi Config: wifi_status, espnow_status");
        Serial.println("   Hydro Active: sensors, supabase, hydro_status, espnow_status");
        Serial.println("   Admin Panel: admin_status, espnow_status");
    }
}

// ===== MÉTODOS PRIVADOS =====
void HydroStateManager::cleanup() {
    Serial.println("🧹 Limpando estado anterior...");
    
    // Liberar recursos do estado anterior
    if (wifiServer) {
        wifiServer->end();
        delete wifiServer;
        wifiServer = nullptr;
        Serial.println("✅ WiFi Config Server limpo");
    }
    
    if (hydroCore) {
        hydroCore->end();
        delete hydroCore;
        hydroCore = nullptr;
        Serial.println("✅ Hydro System Core limpo");
    }
    
    if (adminServer) {
        adminServer->end();
        delete adminServer;
        adminServer = nullptr;
        Serial.println("✅ Admin WebSocket Server limpo");
    }
    
    // Force garbage collection
    delay(100);
    
    Serial.println("💾 Heap após limpeza: " + String(ESP.getFreeHeap()) + " bytes");
}

void HydroStateManager::printESPNowStatus() {
    Serial.println("\n📡 === STATUS ESP-NOW ===");
    
#ifdef MASTER_MODE
    Serial.println("🎯 Modo: MASTER");
    // ✅ CORREÇÃO: Usar injeção de dependências em vez de extern (anti-patrón)
    if (masterManager) {
        Serial.println("✅ MasterSlaveManager ativo");
        if (espNowController) {
            Serial.println("📡 MAC: " + espNowController->getLocalMacString());
        }
        // TODO: Agregar método printStatus() a MasterSlaveManager si es necesario
    } else if (espNowController) {
        Serial.println("✅ ESPNowController ativo");
        Serial.println("📡 MAC: " + espNowController->getLocalMacString());
    } else {
        Serial.println("⚠️ ESP-NOW não inicializado");
    }
#endif

#ifdef SLAVE_MODE
    Serial.println("🤖 Modo: SLAVE");
    // ✅ espNowBridge eliminado - usar implementación actual de Slave
    Serial.println("✅ Modo Slave ativo");
    // TODO: Agregar status específico de Slave si es necesario
#endif

    Serial.println("📶 WiFi Status: " + String(WiFi.isConnected() ? "✅ Conectado" : "❌ Desconectado"));
    if (WiFi.isConnected()) {
        Serial.println("🌐 IP: " + WiFi.localIP().toString());
        Serial.println("📡 SSID: " + WiFi.SSID());
        Serial.println("📶 Canal: " + String(WiFi.channel()));
    }
    Serial.println("🆔 MAC: " + WiFi.macAddress());
    Serial.println("========================\n");
}

bool HydroStateManager::hasWiFiCredentials() {
    Serial.println("\n🔍 === hasWiFiCredentials() DEBUG ===");
    
    // Verificar se preferences está aberto
    if (!preferences.isKey("ssid")) {
        Serial.println("❌ Chave 'ssid' não existe no namespace");
        return false;
    }
    
    String ssid = preferences.getString("ssid", "");
    Serial.println("🔍 SSID lido em hasWiFiCredentials(): '" + ssid + "' (" + String(ssid.length()) + " chars)");
    
    bool hasCredentials = ssid.length() > 0;
    Serial.println("🎯 Resultado: " + String(hasCredentials ? "HAS CREDENTIALS" : "NO CREDENTIALS"));
    
    return hasCredentials;
}

void HydroStateManager::autoSwitchIfNeeded() {
    unsigned long now = millis();
    unsigned long stateUptime = now - stateStartTime;
    
    switch (currentState) {
        case WIFI_CONFIG_MODE:
            // Timeout de 5 minutos no modo WiFi Config
            if (stateUptime > WIFI_CONFIG_TIMEOUT) {
                Serial.println("⏰ Timeout WiFi Config Mode - Verificando credenciais...");
                if (hasWiFiCredentials()) {
                    Serial.println("📝 Credenciais encontradas - Tentando modo hidropônico");
                    switchToHydroActive();
                } else {
                    Serial.println("📝 Sem credenciais - Reiniciando WiFi Config");
                    switchToWiFiConfig();
                }
            }
            break;
            
        case ADMIN_PANEL_MODE:
            // Timeout de 5 minutos no modo Admin Panel
            if (adminServer && adminServer->shouldShutdown()) {
                Serial.println("⏰ Timeout Admin Panel Mode - Voltando para modo hidropônico");
                switchToHydroActive();
            }
            break;
            
        case HYDRO_ACTIVE_MODE:
            handleWifiReconnectRuntime(now);
            break;
    }
}

const char* HydroStateManager::wifiStatusString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS: return "IDLE";
        case WL_NO_SSID_AVAIL: return "NO_SSID";
        case WL_SCAN_COMPLETED: return "SCAN_DONE";
        case WL_CONNECTED: return "CONNECTED";
        case WL_CONNECT_FAILED: return "FAILED";
        case WL_CONNECTION_LOST: return "LOST";
        case WL_DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
    }
}

void HydroStateManager::armWifiStation(const String& ssid, const String& password) {
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
}

void HydroStateManager::resetWifiReconnectState() {
    wifiReconnectPhase = WIFI_RECONNECT_IDLE;
    wifiReconnectStartedMs = 0;
    wifiLastAttemptMs = 0;
    wifiReconnectAttempts = 0;
}

void HydroStateManager::startWifiReconnectAttempt(unsigned long now) {
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");
    if (ssid.length() == 0) {
        return;
    }

    WiFi.disconnect(false);
    armWifiStation(ssid, password);

    wifiReconnectPhase = WIFI_RECONNECT_IN_PROGRESS;
    wifiReconnectStartedMs = now;

    Serial.printf("[WiFi] Reconnect intento %u/%u SSID=%s\n",
                  static_cast<unsigned>(wifiReconnectAttempts + 1),
                  static_cast<unsigned>(WIFI_RECONNECT_MAX_ATTEMPTS),
                  ssid.c_str());
}

void HydroStateManager::handleWifiReconnectRuntime(unsigned long now) {
    if (WiFi.isConnected()) {
        if (wifiReconnectPhase != WIFI_RECONNECT_IDLE) {
            WiFi.setSleep(WIFI_PS_NONE);
            Serial.println("[WiFi] Reconectado: " + WiFi.localIP().toString() +
                           " RSSI=" + String(WiFi.RSSI()) + " dBm");
        }
        resetWifiReconnectState();
        return;
    }

    switch (wifiReconnectPhase) {
        case WIFI_RECONNECT_IDLE:
            if (wifiLastAttemptMs == 0 ||
                (now - wifiLastAttemptMs) >= WIFI_RETRY_INTERVAL_MS) {
                if (!hasWiFiCredentials()) {
                    Serial.println("❌ Sem credenciais para reconexão");
                    switchToWiFiConfig();
                    return;
                }
                startWifiReconnectAttempt(now);
            }
            break;

        case WIFI_RECONNECT_IN_PROGRESS: {
            wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED) {
                WiFi.setSleep(WIFI_PS_NONE);
                Serial.println("[WiFi] Reconectado: " + WiFi.localIP().toString() +
                               " RSSI=" + String(WiFi.RSSI()) + " dBm");
                resetWifiReconnectState();
                return;
            }

            const bool authFailed = (st == WL_CONNECT_FAILED);
            const bool timedOut =
                (now - wifiReconnectStartedMs) >= WIFI_RECONNECT_TIMEOUT_MS;

            if (authFailed || timedOut) {
                wifiReconnectAttempts++;
                Serial.printf("[WiFi] Intento %u/%u falló (%s)\n",
                              static_cast<unsigned>(wifiReconnectAttempts),
                              static_cast<unsigned>(WIFI_RECONNECT_MAX_ATTEMPTS),
                              wifiStatusString(st));
                wifiLastAttemptMs = now;

                if (wifiReconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
                    Serial.println("[WiFi] Max intentos — reiniciando...");
                    delay(500);
                    ESP.restart();
                }

                wifiReconnectPhase = WIFI_RECONNECT_COOLDOWN;
            }
            break;
        }

        case WIFI_RECONNECT_COOLDOWN:
            if ((now - wifiLastAttemptMs) >= WIFI_RETRY_INTERVAL_MS) {
                wifiReconnectPhase = WIFI_RECONNECT_IDLE;
            }
            break;
    }
}

void HydroStateManager::dumpWifiReconnectStatus(Stream& out) const {
    const char* phaseStr = "unknown";
    switch (wifiReconnectPhase) {
        case WIFI_RECONNECT_IDLE: phaseStr = "idle"; break;
        case WIFI_RECONNECT_IN_PROGRESS: phaseStr = "in_progress"; break;
        case WIFI_RECONNECT_COOLDOWN: phaseStr = "cooldown"; break;
    }

    out.printf("[WiFi STATUS] connected=%d wl=%s phase=%s attempts=%u/%u\n",
               WiFi.isConnected() ? 1 : 0,
               wifiStatusString(WiFi.status()),
               phaseStr,
               static_cast<unsigned>(wifiReconnectAttempts),
               static_cast<unsigned>(WIFI_RECONNECT_MAX_ATTEMPTS));

    if (WiFi.isConnected()) {
        out.printf("  IP=%s SSID=%s RSSI=%d ch=%d\n",
                   WiFi.localIP().toString().c_str(),
                   WiFi.SSID().c_str(),
                   WiFi.RSSI(),
                   WiFi.channel());
    } else if (wifiReconnectPhase == WIFI_RECONNECT_IN_PROGRESS) {
        unsigned long elapsed = millis() - wifiReconnectStartedMs;
        out.printf("  in_progress_ms=%lu timeout_ms=%u\n",
                   static_cast<unsigned long>(elapsed),
                   static_cast<unsigned>(WIFI_RECONNECT_TIMEOUT_MS));
    }
}

String HydroStateManager::getDeviceID() {
    return deviceID;
} 