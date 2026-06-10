#include "SupabaseClient.h"
#include "DeviceID.h"
#include <Preferences.h>
#include "DeviceRegistration.h"
#include "Config.h"  // ✅ Para acessar MIN_TEMP, MAX_TEMP, etc.
#include "ObjectPoolManager.h"  // ✅ Object Pool Pattern
#include "NetworkWatchdog.h"    // ✅ Watchdog para operaciones de red
#include <cmath>     // ✅ Para std::isnan() e std::isinf()
#include <math.h>    // ✅ Para isnan() e isinf() no ESP32
#include <freertos/FreeRTOS.h>  // ✅ Para vTaskDelay
#include <freertos/task.h>      // ✅ Para vTaskDelay

SupabaseClient::SupabaseClient() : 
    secureClient(nullptr),
    isConnected(false),
    lastCommandCheck(0),
    commandPollIntervalMs(COMMAND_POLL_INTERVAL_MS),
    commandPollQuiet(true),
    requestMutex(nullptr),        // ✅ NOVO: Mutex inicializado como nullptr
    commandCheckMutex(nullptr) {   // ✅ NOVO: Mutex inicializado como nullptr
}

SupabaseClient::~SupabaseClient() {
    http.end();
    // ✅ Liberar cliente SSL para evitar memory leak
    if (secureClient != nullptr) {
        delete secureClient;
        secureClient = nullptr;
    }
    // ✅ NOVO: Limpar mutexes
    cleanupMutexes();
}

bool SupabaseClient::begin(const String& url, const String& key) {
    baseUrl = url;
    apiKey = key;
    
    // ✅ NOVO: Inicializar mutexes primeiro
    if (!initMutexes()) {
        Serial.println("❌ [SUPABASE] Falha ao inicializar mutexes");
        setError("Falha ao inicializar mutexes");
        return false;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        setError("WiFi não conectado");
        cleanupMutexes();  // ✅ Limpar mutexes se falhar
        return false;
    }
    
    // ✅ NOVO: Verificar se Object Pool Manager está inicializado
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    if (!poolMgr || !poolMgr->isInitialized()) {
        Serial.println("⚠️ [SUPABASE] Object Pool Manager não inicializado");
        Serial.println("   Tentando usar modo legacy (pode causar problemas de memória)");
        // ✅ FALLBACK: Usar modo antigo se pool não disponível
        if (secureClient != nullptr) {
            delete secureClient;
        }
        secureClient = new WiFiClientSecure();
        secureClient->setInsecure();
        http.begin(*secureClient, baseUrl + "/rest/v1/");
    } else {
        // ✅ CORREÇÃO: Manter secureClient de fallback incluso quando pool está disponível
        // Isso permite que métodos como testConnection(), checkForCommands(), etc. funcionem
        if (secureClient == nullptr) {
            secureClient = new WiFiClientSecure();
            secureClient->setInsecure();
        }
        Serial.println("✅ [SUPABASE] Usando Object Pool para conexões SSL (com fallback)");
    }
    
    http.setUserAgent("ESP32-Hydro/2.1.0");
    http.setConnectTimeout(15000); // 15 segundos timeout de conexão
    http.setTimeout(20000); // 20 segundos timeout total
    
    Serial.println("🔐 Configurando conexão SSL para Supabase...");
    Serial.println("🔓 Certificados auto-assinados: ACEITOS (desenvolvimento)");
    
    // Testar conexão DNS primeiro
    Serial.printf("🌐 Testando DNS para: %s\n", baseUrl.c_str());
    
    // Testar conexão
    if (testConnection()) {
        isConnected = true;
        Serial.println("✅ Supabase conectado com sucesso");
        return true;
    } else {
        setError("Falha ao conectar com Supabase - verifique DNS/SSL");
        return false;
    }
}

String SupabaseClient::buildAuthHeader() {
    return "Bearer " + apiKey;
}

bool SupabaseClient::makeRequest(const String& method, const String& endpoint, const String& payload) {
    // ✅ NOVO: Proteger com mutex para thread-safety
    if (requestMutex == nullptr) {
        Serial.println("⚠️ [SUPABASE] requestMutex não inicializado - operação não protegida");
        // Continuar sem mutex se não estiver inicializado (fallback)
    } else {
        // Adquirir mutex (timeout de 5 segundos)
        if (xSemaphoreTake(requestMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            Serial.println("❌ [SUPABASE] Timeout ao adquirir requestMutex");
            setError("Timeout ao adquirir mutex de requisição");
            return false;
        }
    }
    
    // ✅ PASSO 1: Verificar estado do cliente (ordem procedural)
    if (!isReady()) {
        setError("Cliente não está pronto");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex antes de retornar
        }
        return false;
    }
    
    String url = baseUrl + "/rest/v1/" + endpoint;
    
    // ✅ NOVO: Usar Object Pool se disponível
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        // ✅ POOL: Adquirir do pool
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            Serial.println("⚠️ [SUPABASE] Pool esgotado - tentando modo legacy");
            // ✅ FALLBACK: Usar modo antigo se pool esgotado
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure(); // ✅ CRÍTICO: Configurar SSL insecure
        }
    }
    
    // ✅ FALLBACK: Usar modo antigo se pool não disponível
    if (!usePool) {
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado");
            if (requestMutex != nullptr) {
                xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
            }
            return false;
        }
        sslClient = secureClient;
        httpClient = &http;
    }
    
    // ✅ PASSO 2: Verificar heap ANTES de criar conexão (ordem procedural)
    uint32_t freeHeapBefore = ESP.getFreeHeap();
    uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
    
    if (freeHeapBefore < 40000 || maxAllocHeap < 35000) {
        static unsigned long lastWarning = 0;
        if (millis() - lastWarning > 30000) {
            Serial.printf("⚠️ [SUPABASE] Memoria insuficiente: libre=%d, max_contiguo=%d\n", 
                         freeHeapBefore, maxAllocHeap);
            lastWarning = millis();
        }
        setError("Memoria insuficiente o muy fragmentada para SSL");
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ NOVO: Iniciar watchdog de red para proteger operación HTTP
    if (!networkWatchdog.beginOperation(httpClient, sslClient)) {
        Serial.println("❌ [SUPABASE] NetworkWatchdog rechazó iniciar operación (memoria insuficiente)");
        setError("Watchdog rechazó operación - memoria insuficiente");
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 3: Fechar conexão previa (ordem procedural)
    if (httpClient->connected()) {
        httpClient->end();
        vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        // ✅ Alimentar watchdog después de operación
        if (!networkWatchdog.feed()) {
            networkWatchdog.endOperation(false);
            setError("Watchdog timeout durante cierre de conexión previa");
            if (usingPool && poolMgr) {
                poolMgr->releaseHTTPClient(httpClient);
                poolMgr->releaseSSLClient(sslClient);
            }
            if (requestMutex != nullptr) {
                xSemaphoreGive(requestMutex);
            }
            return false;
        }
    }
    
    // ✅ PASSO 4: Iniciar conexão SSL COM VERIFICAÇÃO (ordem procedural)
    if (!httpClient->begin(*sslClient, url)) {
        Serial.printf("❌ [SUPABASE REQUEST] Falha ao iniciar conexão SSL\n");
        Serial.printf("   URL: %s\n", url.c_str());
        Serial.printf("   Heap: %d bytes\n", ESP.getFreeHeap());
        setError("Falha ao iniciar conexão SSL");
        networkWatchdog.endOperation(false);
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ Alimentar watchdog después de begin()
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante inicio de conexión");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    // ✅ PASSO 5: Configurar headers e timeout (ordem procedural)
    httpClient->addHeader("Authorization", buildAuthHeader());
    httpClient->addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    httpClient->addHeader("Prefer", SUPABASE_PREFER);
    httpClient->addHeader("apikey", apiKey);
    // ✅ REDUCIDO: Timeouts más cortos para evitar bloqueos largos
    httpClient->setConnectTimeout(8000);   // 8s conexão (reducido de 10s)
    httpClient->setTimeout(12000);          // 12s total (reducido de 15s)
    
    // ✅ PASSO 6: Executar método HTTP COM VERIFICAÇÃO (ordem procedural)
    // ⚠️ NOTA: Las operaciones POST/GET/PATCH son bloqueantes, pero el watchdog
    // verificará después si se excedió el timeout
    int httpCode = -1;
    bool httpSuccess = false;
    
    if (method == "POST") {
        httpCode = httpClient->POST(payload);
        httpSuccess = true;
    } else if (method == "GET") {
        httpCode = httpClient->GET();
        httpSuccess = true;
    } else if (method == "PATCH") {
        httpCode = httpClient->PATCH(payload);
        httpSuccess = true;
    } else {
        setError("Método HTTP não suportado: " + method);
        httpClient->end();  // ✅ FECHAR conexão
        networkWatchdog.endOperation(false);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ CRÍTICO: Verificar watchdog después de operación HTTP bloqueante
    if (!networkWatchdog.feed()) {
        Serial.println("⏰ [SUPABASE] Watchdog timeout después de operación HTTP - forzando cierre");
        httpClient->end();
        if (sslClient) {
            sslClient->stop();
        }
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante operación HTTP");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    // ✅ PASSO 7: Verificar código HTTP ANTES de ler resposta (ordem procedural)
    if (httpCode <= 0) {
        Serial.printf("❌ [SUPABASE REQUEST] Erro HTTP: %d\n", httpCode);
        Serial.printf("   Método: %s\n", method.c_str());
        Serial.printf("   Endpoint: %s\n", endpoint.c_str());
        Serial.printf("   Erro: %s\n", httpClient->errorToString(httpCode).c_str());
        Serial.printf("   Heap: %d bytes\n", ESP.getFreeHeap());
        setError("Erro HTTP: " + String(httpCode) + " - " + httpClient->errorToString(httpCode));
        httpClient->end();  // ✅ FECHAR conexão sempre
        networkWatchdog.endOperation(false);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 8: Verificar se resposta é sucesso (ordem procedural)
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ [SUPABASE REQUEST] %s %s: HTTP %d\n", method.c_str(), endpoint.c_str(), httpCode);
        httpClient->end();  // ✅ FECHAR conexão IMEDIATAMENTE
        networkWatchdog.endOperation(true);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
        }
        return true;
    }
    
    // ✅ PASSO 9: Tratar erro HTTP (ordem procedural)
    // Verificar tamanho antes de ler
    int contentLength = httpClient->getSize();
    String response = "";
    
    if (contentLength > 0 && contentLength < 500) {  // Limitar tamanho da resposta de erro
        response = httpClient->getString();
    } else if (contentLength > 0) {
        response = httpClient->getString().substring(0, 200);  // Limitar a 200 chars
    }
    
    httpClient->end();  // ✅ FECHAR conexão sempre
    networkWatchdog.endOperation(false);
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // ✅ Liberar pools se estavam em uso
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    setError("HTTP " + String(httpCode) + ": " + response);
    Serial.printf("❌ [SUPABASE REQUEST] %s %s: HTTP %d\n", method.c_str(), endpoint.c_str(), httpCode);
    if (response.length() > 0) {
        Serial.printf("   Resposta: %s\n", response.c_str());
    }
    
    // ✅ PASSO 10: Verificar heap após requisição (ordem procedural)
    uint32_t freeHeapAfter = ESP.getFreeHeap();
    uint32_t heapUsed = freeHeapBefore - freeHeapAfter;
    if (heapUsed > 20000) {
        Serial.printf("⚠️ [SUPABASE REQUEST] Muito heap usado: %d bytes\n", heapUsed);
    }
    
    // ✅ Liberar mutex antes de retornar
    if (requestMutex != nullptr) {
        xSemaphoreGive(requestMutex);  // ✅ Liberar mutex
    }
    
    return false;
}

bool SupabaseClient::sendEnvironmentData(const EnvironmentReading& reading) {
    // ✅ VALIDAÇÃO ANTES DE ENVIAR: Verificar valores válidos
    // Verificar NaN e infinito
    if (std::isnan(reading.temperature) || std::isinf(reading.temperature) ||
        reading.temperature > 1000.0) {
        Serial.printf("❌ [ENV] Temperatura inválida (NaN/Inf): %.2f (ignorando envio)\n", reading.temperature);
        return false;
    }
    if (reading.temperature < MIN_TEMP || reading.temperature > MAX_TEMP) {
        Serial.printf("❌ [ENV] Temperatura fora do intervalo: %.2f (restrição [%.1f, %.1f] — environment_data_temperature_check)\n",
            reading.temperature, MIN_TEMP, MAX_TEMP);
        return false;
    }
    if (std::isnan(reading.humidity) || std::isinf(reading.humidity)) {
        Serial.printf("❌ [ENV] Umidade inválida (NaN/Inf): %.2f (ignorando envio)\n", reading.humidity);
        return false;
    }
    if (reading.humidity < MIN_HUMIDITY || reading.humidity > MAX_HUMIDITY) {
        Serial.printf("❌ [ENV] Umidade fora do intervalo: %.2f (restrição [%.1f, %.1f] — environment_data_humidity_check)\n",
            reading.humidity, MIN_HUMIDITY, MAX_HUMIDITY);
        return false;
    }
    
    String payload = buildEnvironmentPayload(reading);
    return makeRequest("POST", SUPABASE_ENVIRONMENT_TABLE, payload);
}

bool SupabaseClient::sendHydroData(const HydroReading& reading) {
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   📤 ENVIANDO DADOS HIDROPÔNICOS AO SUPABASE      ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.printf("🔍 [HYDRO] Device ID: %s\n", getDeviceID().c_str());
    Serial.printf("   Temp: %.2f°C (min: %.1f, max: %.1f)\n", reading.temperature, MIN_TEMP, MAX_TEMP);
    Serial.printf("   pH: %.2f (min: %.1f, max: %.1f)\n", reading.ph, MIN_PH, MAX_PH);
    Serial.printf("   TDS: %.2f ppm (min: %.1f, max: %.1f)\n", reading.tds, MIN_TDS, MAX_TDS);
    Serial.printf("   Water Level OK: %s\n", reading.waterLevelOk ? "SIM" : "NÃO");
    
    // ✅ VALIDAÇÃO ANTES DE ENVIAR: Verificar valores válidos
    // Verificar NaN e infinito
    if (std::isnan(reading.temperature) || std::isinf(reading.temperature)) {
        Serial.printf("❌ [HYDRO] Temperatura é NaN ou Inf: %.2f (ignorando envio)\n", reading.temperature);
        return false;
    }
    if (reading.temperature < MIN_TEMP || reading.temperature > MAX_TEMP) {
        Serial.printf("❌ [HYDRO] Temperatura fora do intervalo: %.2f (min: %.1f, max: %.1f)\n", 
            reading.temperature, MIN_TEMP, MAX_TEMP);
        return false;
    }
    if (reading.temperature > 1000.0) {  // Valor máximo razoável
        Serial.printf("❌ [HYDRO] Temperatura muito alta: %.2f (ignorando envio)\n", reading.temperature);
        return false;
    }
    
    if (std::isnan(reading.ph) || std::isinf(reading.ph)) {
        Serial.printf("❌ [HYDRO] pH é NaN ou Inf: %.2f (ignorando envio)\n", reading.ph);
        return false;
    }
    if (reading.ph < MIN_PH || reading.ph > MAX_PH) {
        Serial.printf("❌ [HYDRO] pH fora do intervalo: %.2f (min: %.1f, max: %.1f)\n", 
            reading.ph, MIN_PH, MAX_PH);
        return false;
    }
    
    if (std::isnan(reading.tds) || std::isinf(reading.tds)) {
        Serial.printf("❌ [HYDRO] TDS é NaN ou Inf: %.2f (ignorando envio)\n", reading.tds);
        return false;
    }
    if (reading.tds < MIN_TDS || reading.tds > MAX_TDS) {
        Serial.printf("❌ [HYDRO] TDS fora do intervalo: %.2f (min: %.1f, max: %.1f)\n", 
            reading.tds, MIN_TDS, MAX_TDS);
        return false;
    }
    
    Serial.println("✅ [HYDRO] Validações passaram - construindo payload...");
    String payload = buildHydroPayload(reading);
    Serial.printf("📦 [HYDRO] Payload: %s\n", payload.c_str());
    Serial.printf("📤 [HYDRO] Enviando para tabela: %s\n", SUPABASE_HYDRO_TABLE);
    Serial.printf("🔗 [HYDRO] URL completa: %s/rest/v1/%s\n", baseUrl.c_str(), SUPABASE_HYDRO_TABLE);
    
    bool result = makeRequest("POST", SUPABASE_HYDRO_TABLE, payload);
    
    if (result) {
        Serial.println("✅ [HYDRO] Dados hidropônicos enviados com sucesso!");
        Serial.printf("   ✅ Device ID: %s\n", getDeviceID().c_str());
        Serial.printf("   ✅ TDS: %.2f ppm\n", reading.tds);
        Serial.println("╚════════════════════════════════════════════════════╝\n");
    } else {
        Serial.println("❌ [HYDRO] Falha ao enviar dados hidropônicos");
        Serial.printf("   ❌ Device ID: %s\n", getDeviceID().c_str());
        Serial.printf("   ❌ TDS: %.2f ppm\n", reading.tds);
        Serial.printf("   ❌ Erro: %s\n", getLastError().c_str());
        Serial.println("╚════════════════════════════════════════════════════╝\n");
    }
    
    return result;
}

bool SupabaseClient::updateDeviceStatus(const DeviceStatusData& status) {
    String payload = buildDeviceStatusPayload(status);
    
    // Usar UPSERT para atualizar ou inserir
    String endpoint = String(SUPABASE_STATUS_TABLE) + "?device_id=eq." + status.deviceId;
    
    // ✅ CORREÇÃO: Usar makeRequest() que já usa Object Pool
    // Primeiro tentar UPDATE via PATCH
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    
    // ✅ Usar Object Pool si está disponible
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure();
        }
    }
    
    if (!usePool) {
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado e pool não disponível");
            return false;
        }
        sslClient = secureClient;
        httpClient = &http;
    }
    
    // ✅ Verificar heap antes de conectar
    if (ESP.getFreeHeap() < 30000) {
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        return makeRequest("POST", SUPABASE_STATUS_TABLE, payload); // Fallback
    }
    
    // ✅ Cerrar cualquier conexión previa
    if (!usingPool && http.connected()) {
        http.end();
        vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
    }
    
    if (!httpClient->begin(*sslClient, fullUrl)) {
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        return makeRequest("POST", SUPABASE_STATUS_TABLE, payload); // Fallback
    }
    
    httpClient->addHeader("Authorization", buildAuthHeader());
    httpClient->addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    httpClient->addHeader("Prefer", "resolution=merge-duplicates");
    httpClient->addHeader("apikey", apiKey);
    httpClient->setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = httpClient->PATCH(payload);
    
    bool success = (httpCode >= 200 && httpCode < 300);
    
    if (success) {
        Serial.printf("✅ Device status atualizado: %d\n", httpCode);
    }
    
    httpClient->end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // ✅ Liberar pools si estavam en uso
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    if (success) {
        return true;
    } else {
        // Se falhou, tentar INSERT via makeRequest (que usa Object Pool)
        return makeRequest("POST", SUPABASE_STATUS_TABLE, payload);
    }
}

String SupabaseClient::buildEnvironmentPayload(const EnvironmentReading& reading) {
    DynamicJsonDocument doc(256);
    
    doc["device_id"] = getDeviceID();
    doc["temperature"] = reading.temperature;
    doc["humidity"] = reading.humidity;
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

String SupabaseClient::buildHydroPayload(const HydroReading& reading) {
    DynamicJsonDocument doc(256);
    
    String deviceId = getDeviceID();
    doc["device_id"] = deviceId;
    doc["temperature"] = reading.temperature;
    doc["ph"] = reading.ph;
    doc["tds"] = reading.tds;
    doc["water_level_ok"] = reading.waterLevelOk;
    
    String payload;
    serializeJson(doc, payload);
    
    // ✅ DEBUG: Mostrar device_id que está sendo enviado
    Serial.printf("🔍 [HYDRO] Device ID no payload: %s\n", deviceId.c_str());
    
    return payload;
}

String SupabaseClient::buildDeviceStatusPayload(const DeviceStatusData& status) {
    DynamicJsonDocument doc(1024);
    
    doc["device_id"] = status.deviceId;
    doc["last_seen"] = "now()";
    doc["wifi_rssi"] = status.wifiRssi;
    doc["free_heap"] = status.freeHeap;
    doc["uptime_seconds"] = status.uptimeSeconds;
    doc["is_online"] = status.isOnline;
    doc["firmware_version"] = status.firmwareVersion;
    doc["ip_address"] = status.ipAddress;
    doc["updated_at"] = "now()";
    // Sempre enviar — NVS é fonte; dashboard sincroniza (inclui 0 após portal)
    doc["reboot_count"] = status.rebootCount;
    
    // ✅ CORRIGIDO: NÃO enviar relay_states aqui
    // relay_states foi removido de device_status (migrado para relay_master e relay_slaves)
    // Os estados dos relés são atualizados separadamente via updateRelayMaster()
    
    String payload;
    serializeJson(doc, payload);
    return payload;
}

bool SupabaseClient::checkForCommands(RelayCommand* commands, int maxCommands, int& commandCount) {
    // ✅ NOVO: Proteger com mutex para thread-safety
    if (commandCheckMutex == nullptr) {
        Serial.println("⚠️ [SUPABASE] commandCheckMutex não inicializado - operação não protegida");
        // Continuar sem mutex se não estiver inicializado (fallback)
    } else {
        // Adquirir mutex (timeout de 5 segundos)
        if (xSemaphoreTake(commandCheckMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            Serial.println("❌ [SUPABASE] Timeout ao adquirir commandCheckMutex");
            return false;
        }
    }
    
    if (!isReady()) {
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // Verificar apenas a cada COMMAND_POLL_INTERVAL_MS
    unsigned long now = millis();
    if (now - lastCommandCheck < commandPollIntervalMs) {
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    lastCommandCheck = now;  // ✅ Protegido pelo mutex
    
    // ✅ BUSCAR COMANDOS PENDENTES com priorização correta
    // Ordenar por: command_type (peristaltic > rule > manual), priority DESC, created_at ASC
    // Nota: Supabase não suporta CASE em ORDER BY direto, então ordenamos por created_at
    // e depois ordenamos no código (ou usar função SQL get_pending_commands)
    String endpoint = String(SUPABASE_RELAY_TABLE) 
      + "?device_id=eq." + getDeviceID() 
      + "&status=eq.pending"
      + "&order=priority.desc,created_at.asc"  // ✅ Ordenar por priority primeiro
      + "&limit=" + maxCommands;
    
    Serial.printf("🔍 Verificando comandos: %s\n", (baseUrl + "/rest/v1/" + endpoint).c_str());
    
    // ✅ CORREÇÃO CRÍTICA: Usar Object Pool si está disponible (ordem procedural)
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        // ✅ POOL: Adquirir do pool
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            Serial.println("⚠️ [SUPABASE CHECK] Pool esgotado - tentando modo legacy");
            // ✅ FALLBACK: Usar modo antigo se pool esgotado
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure();
        }
    }
    
    // ✅ FALLBACK: Usar modo antigo se pool não disponível
    if (!usePool) {
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado e pool não disponível");
            return false;
        }
        sslClient = secureClient;
        httpClient = &http;
    }
    
    // ✅ Verificar memoria disponible antes de intentar conexión SSL
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 30000) { // Mínimo 30KB libres para SSL
        Serial.printf("⚠️ [SUPABASE CHECK] Memoria insuficiente para SSL: %d bytes libres (mínimo: 30000)\n", freeHeap);
        setError("Memoria insuficiente para conexión SSL");
        // ✅ Liberar pools si estavam en uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 3: Fechar conexão previa (ordem procedural)
    if (usingPool) {
        // Pool gerencia conexões, não precisa fechar manualmente
    } else {
        if (http.connected()) {
            http.end();
        }
        // Delay para liberação de memória SSL
        if (freeHeap < 40000) {
            vTaskDelay(pdMS_TO_TICKS(100)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        } else {
            vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        }
    }
    
    // ✅ PASSO 5: Iniciar conexão SSL COM VERIFICAÇÃO (ordem procedural)
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    if (!httpClient->begin(*sslClient, fullUrl)) {
        Serial.printf("❌ [SUPABASE CHECK] Falha ao iniciar conexão SSL\n");
        Serial.printf("   URL: %s\n", fullUrl.c_str());
        Serial.printf("   Heap: %d bytes\n", ESP.getFreeHeap());
        setError("Falha ao iniciar conexão SSL");
        // ✅ Liberar pools si estavam en uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 6: Configurar headers e timeout (ordem procedural)
    httpClient->setConnectTimeout(10000); // 10s timeout conexão
    httpClient->setTimeout(15000); // 15s timeout total
    httpClient->setUserAgent("ESP32-Hydro/2.1.0");
    httpClient->addHeader("Authorization", buildAuthHeader());
    httpClient->addHeader("apikey", apiKey);
    httpClient->addHeader("Accept", "application/json");
    
    // ✅ PASSO 7: Fazer GET request COM VERIFICAÇÃO (ordem procedural)
    Serial.println("📡 [SUPABASE CHECK] Enviando requisição GET para comandos...");
    int httpCode = httpClient->GET();
    
    // ✅ PASSO 8: Verificar código HTTP ANTES de ler resposta (ordem procedural)
    if (httpCode <= 0) {
        Serial.printf("❌ [SUPABASE CHECK] Erro HTTP: %d\n", httpCode);
        Serial.printf("   Erro: %s\n", httpClient->errorToString(httpCode).c_str());
        Serial.printf("   Heap: %d bytes\n", ESP.getFreeHeap());
        httpClient->end();  // ✅ FECHAR conexão sempre
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Liberar pools si estavam en uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    if (httpCode != 200) {
        Serial.printf("❌ [SUPABASE CHECK] HTTP %d\n", httpCode);
        if (httpCode > 0) {
            int contentLength = httpClient->getSize();
            if (contentLength > 0 && contentLength < 500) {
                String errorResponse = httpClient->getString();
                Serial.printf("   Resposta: %s\n", errorResponse.c_str());
            }
        }
        httpClient->end();  // ✅ FECHAR conexão siempre
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Liberar pools si estavam en uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 9: Verificar tamanho da resposta ANTES de ler (ordem procedural)
    int contentLength = httpClient->getSize();
    if (contentLength <= 0) {
        Serial.println("⚠️ [SUPABASE CHECK] Resposta vazia ou tamanho desconhecido");
        httpClient->end();  // ✅ FECHAR conexão
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Liberar pools si estavam en uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    Serial.printf("📥 [SUPABASE CHECK] Resposta recebida: %d bytes\n", contentLength);
    
    // ✅ PASSO 10: Ler resposta (ordem procedural)
    String response = httpClient->getString();
    
    // ✅ PASSO 11: FECHAR conexão IMEDIATAMENTE após ler (ordem procedural)
    httpClient->end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // ✅ Liberar pools si estavam en uso
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    // ✅ PASSO 12: Verificar se resposta não está vazia (ordem procedural)
    if (response.length() == 0) {
        Serial.println("❌ [SUPABASE CHECK] Resposta vazia após getString()");
        Serial.printf("   Content-Length esperado: %d bytes\n", contentLength);
        // ✅ Liberar mutex antes de retornar
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ PASSO 13: Verificar formato JSON ANTES de parsear (ordem procedural)
    if (response.charAt(0) != '[' && response.charAt(0) != '{') {
        Serial.println("❌ [SUPABASE CHECK] Resposta não é JSON válido");
        Serial.printf("   Primeiro caractere: '%c' (esperado: '[' ou '{')\n", response.charAt(0));
        Serial.printf("   Primeiros 100 chars: %s\n", response.substring(0, 100).c_str());
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 14: Parsear JSON COM TRATAMENTO DE ERRO (ordem procedural)
    // ✅ OTIMIZAÇÃO: Buffer dinâmico baseado no tamanho da resposta (mínimo 2KB, máximo 16KB)
    int jsonSize = max(2048, min((int)(response.length() * 1.3), 16384)); // 30% de margem, max 16KB
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
        Serial.printf("❌ [SUPABASE CHECK] Erro ao parsear JSON: %s\n", error.c_str());
        Serial.printf("   Tamanho da resposta: %d bytes | Buffer usado: %d bytes\n", response.length(), jsonSize);
        Serial.printf("   Primeiros 200 chars: %s\n", response.substring(0, 200).c_str());
        
        // ✅ Log de heap após erro
        uint32_t freeHeapAfter = ESP.getFreeHeap();
        Serial.printf("   Heap: %d → %d (usado: %d bytes)\n", 
                      freeHeap, freeHeapAfter, freeHeap - freeHeapAfter);
        setError("Erro ao parsear comandos JSON: " + String(error.c_str()));
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
        }
        return false;
    }
    
    // ✅ PASSO 15: Processar comandos (ordem procedural)
    Serial.printf("✅ [SUPABASE CHECK] Resposta parseada: %d bytes\n", response.length());
    
    JsonArray commandsArray = doc.as<JsonArray>();
    commandCount = min((int)commandsArray.size(), maxCommands);
    
    // ✅ Processar comandos recebidos
    
    for (int i = 0; i < commandCount; i++) {
        JsonObject cmd = commandsArray[i];
        commands[i].id = cmd["id"];
        commands[i].relayNumber = cmd["relay_number"];
        commands[i].action = cmd["action"].as<String>();
        commands[i].durationSeconds = cmd["duration_seconds"] | 0;
        commands[i].status = cmd["status"].as<String>();
        commands[i].timestamp = millis(); // ✅ Timestamp de recebimento
        
        // ✅ INTEGRAÇÃO ESP-NOW: Parsear target_device_id (opcional)
        if (cmd.containsKey("target_device_id")) {
            commands[i].target_device_id = cmd["target_device_id"].as<String>();
        } else {
            commands[i].target_device_id = ""; // Default: relés locais
        }
        
        // ✅ FORK: Parsear command_type e campos relacionados
        if (cmd.containsKey("command_type")) {
            commands[i].command_type = cmd["command_type"].as<String>();
        } else {
            // ✅ Fallback: Determinar tipo baseado em triggered_by (compatibilidade)
            if (cmd.containsKey("triggered_by")) {
                String triggered = cmd["triggered_by"].as<String>();
                if (triggered == "automation" || triggered == "rule") {
                    commands[i].command_type = "rule";
                } else if (triggered == "peristaltic") {
                    commands[i].command_type = "peristaltic";
                } else {
                    commands[i].command_type = "manual";
                }
            } else {
                commands[i].command_type = "manual"; // Default
            }
        }
        
        // ✅ PRIORIDADE: Parsear priority (0-100)
        if (cmd.containsKey("priority")) {
            commands[i].priority = cmd["priority"] | 50; // Default: 50 se não tiver
        } else {
            // ✅ Default baseado em command_type (mesma lógica do frontend)
            if (commands[i].command_type == "peristaltic") {
                commands[i].priority = 80; // Alta prioridade
            } else if (commands[i].command_type == "rule") {
                commands[i].priority = 50; // Média prioridade
            } else {
                commands[i].priority = 10; // Baixa prioridade (manual)
            }
        }
        
        // ✅ Campos relacionados a regras
        if (cmd.containsKey("triggered_by")) {
            commands[i].triggered_by = cmd["triggered_by"].as<String>();
        } else {
            commands[i].triggered_by = "manual";
        }
        
        if (cmd.containsKey("rule_id")) {
            commands[i].rule_id = cmd["rule_id"].as<String>();
        } else {
            commands[i].rule_id = "";
        }
        
        if (cmd.containsKey("rule_name")) {
            commands[i].rule_name = cmd["rule_name"].as<String>();
        } else {
            commands[i].rule_name = "";
        }
        
        // ✅ DEBUG: Log de prioridade
        Serial.printf("   📊 Comando #%d: type=%s, priority=%d\n", 
                     commands[i].id, 
                     commands[i].command_type.c_str(), 
                     commands[i].priority);
    }
    
    if (commandCount > 0) {
        Serial.printf("📥 [SUPABASE CHECK] Recebidos %d comandos de relé pendentes\n", commandCount);
        Serial.println("📊 Ordem de processamento (priorizada):");
        for (int i = 0; i < commandCount; i++) {
            Serial.printf("   %d. ID=%d | type=%s | priority=%d | relay=%d | action=%s\n",
                         i + 1,
                         commands[i].id,
                         commands[i].command_type.c_str(),
                         commands[i].priority,
                         commands[i].relayNumber,
                         commands[i].action.c_str());
        }
    }
    
    // ✅ PASSO 16: Verificar heap após processar (ordem procedural)
    uint32_t freeHeapAfter = ESP.getFreeHeap();
    uint32_t heapUsed = freeHeap - freeHeapAfter;
    if (heapUsed > 20000) {
        Serial.printf("⚠️ [SUPABASE CHECK] Muito heap usado: %d bytes\n", heapUsed);
    }
    
    // ✅ Liberar mutex antes de retornar
    if (commandCheckMutex != nullptr) {
        xSemaphoreGive(commandCheckMutex);  // ✅ Liberar mutex
    }
    
    return true;
}

// ✅ NOVO: Buscar comandos Master usando RPC atômica
bool SupabaseClient::checkForMasterCommands(RelayCommand* commands, int maxCommands, int& commandCount) {
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        return false;
    }
    
    // ✅ Proteger com mutex
    if (commandCheckMutex != nullptr) {
        if (xSemaphoreTake(commandCheckMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            setError("Timeout ao obter mutex para checkForMasterCommands");
            return false;
        }
    }
    
    // Verificar apenas a cada COMMAND_POLL_INTERVAL_MS
    unsigned long now = millis();
    if (now - lastCommandCheck < commandPollIntervalMs) {
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    lastCommandCheck = now;
    
    // ✅ NOVO: Usar RPC atômica get_and_lock_master_commands
    // ✅ CORREÇÃO: Usar POST com payload JSON (GET é read-only, RPC precisa fazer UPDATE)
    String endpoint = "rpc/get_and_lock_master_commands";
    
    // ✅ Construir payload JSON para POST
    DynamicJsonDocument payloadDoc(256);
    payloadDoc["p_device_id"] = getDeviceID();
    payloadDoc["p_limit"] = maxCommands;
    payloadDoc["p_timeout_seconds"] = 30;
    
    String payload;
    serializeJson(payloadDoc, payload);
    
    if (!commandPollQuiet) {
        Serial.printf("🔍 [RPC MASTER] Verificando comandos: %s\n", (baseUrl + "/rest/v1/" + endpoint).c_str());
        Serial.printf("📦 [RPC MASTER] Payload: %s\n", payload.c_str());
    }
    
    // ✅ Usar Object Pool se disponível
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            Serial.println("⚠️ [RPC MASTER] Pool esgotado - tentando modo legacy");
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure();
        }
    }
    
    if (!usePool) {
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado e pool não disponível");
            if (commandCheckMutex != nullptr) {
                xSemaphoreGive(commandCheckMutex);
            }
            return false;
        }
        sslClient = secureClient;
        httpClient = &http;
    }
    
    // Verificar memória
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 30000) {
        Serial.printf("⚠️ [RPC MASTER] Memoria insuficiente: %d bytes\n", freeHeap);
        setError("Memoria insuficiente para SSL");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ NOVO: Iniciar watchdog de red para proteger operación RPC
    if (!networkWatchdog.beginOperation(httpClient, sslClient)) {
        Serial.println("❌ [RPC MASTER] NetworkWatchdog rechazó iniciar operación");
        setError("Watchdog rechazó operación - memoria insuficiente");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Fechar conexão prévia
    if (!usingPool && http.connected()) {
        http.end();
        vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        if (!networkWatchdog.feed()) {
            networkWatchdog.endOperation(false);
            setError("Watchdog timeout durante cierre de conexión previa");
            if (usingPool && poolMgr) {
                poolMgr->releaseHTTPClient(httpClient);
                poolMgr->releaseSSLClient(sslClient);
            }
            if (commandCheckMutex != nullptr) {
                xSemaphoreGive(commandCheckMutex);
            }
            return false;
        }
    }
    
    // Iniciar conexão SSL
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    if (!httpClient->begin(*sslClient, fullUrl)) {
        Serial.printf("❌ [RPC MASTER] Falha ao iniciar conexão SSL\n");
        setError("Falha ao iniciar conexão SSL");
        networkWatchdog.endOperation(false);
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Alimentar watchdog después de begin()
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante inicio de conexión");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Configurar headers
    // ✅ REDUCIDO: Timeouts más cortos para evitar bloqueos largos
    httpClient->setConnectTimeout(8000);   // 8s conexão (reducido de 10s)
    httpClient->setTimeout(12000);          // 12s total (reducido de 15s)
    httpClient->setUserAgent("ESP32-Hydro/2.1.0");
    httpClient->addHeader("Authorization", buildAuthHeader());
    httpClient->addHeader("apikey", apiKey);
    httpClient->addHeader("Accept", "application/json");
    httpClient->addHeader("Content-Type", "application/json");  // ✅ POST precisa de Content-Type
    
    // ✅ Fazer POST request (GET é read-only, RPC precisa fazer UPDATE)
    Serial.println("📡 [RPC MASTER] Enviando requisição POST...");
    int httpCode = httpClient->POST(payload);
    
    // ✅ CRÍTICO: Verificar watchdog después de operación HTTP bloqueante
    if (!networkWatchdog.feed()) {
        Serial.println("⏰ [RPC MASTER] Watchdog timeout después de POST - forzando cierre");
        httpClient->end();
        if (sslClient) {
            sslClient->stop();
        }
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante operación POST");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (httpCode <= 0) {
        Serial.printf("❌ [RPC MASTER] Erro HTTP: %d\n", httpCode);
        httpClient->end();
        networkWatchdog.endOperation(false);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (httpCode != 200) {
        Serial.printf("❌ [RPC MASTER] HTTP %d\n", httpCode);
        
        // ✅ DEBUG MEJORADO: Ler resposta de erro completa
        if (httpCode > 0) {
            int errorContentLength = httpClient->getSize();
            Serial.printf("📏 [RPC MASTER] Tamanho da resposta: %d bytes\n", errorContentLength);
            
            if (errorContentLength > 0) {
                // Aumentar limite para capturar respuestas más largas
                String errorResponse = httpClient->getString();
                Serial.printf("📄 [RPC MASTER] Resposta completa:\n%s\n", errorResponse.c_str());
                
                // Tentar parsear JSON para extrair mensaje de erro
                if (errorResponse.length() > 0) {
                    DynamicJsonDocument errorDoc(1024);
                    DeserializationError jsonError = deserializeJson(errorDoc, errorResponse);
                    if (!jsonError) {
                        if (errorDoc.containsKey("message")) {
                            Serial.printf("💬 [RPC MASTER] Mensagem: %s\n", errorDoc["message"].as<const char*>());
                        }
                        if (errorDoc.containsKey("hint")) {
                            Serial.printf("💡 [RPC MASTER] Dica: %s\n", errorDoc["hint"].as<const char*>());
                        }
                        if (errorDoc.containsKey("code")) {
                            Serial.printf("🔢 [RPC MASTER] Código: %s\n", errorDoc["code"].as<const char*>());
                        }
                    }
                }
            } else {
                Serial.println("⚠️ [RPC MASTER] Resposta vazia (contentLength = 0)");
            }
        }
        
        // ✅ DEBUG: Mostrar URL completa que foi chamada
        Serial.printf("🔍 [RPC MASTER] URL chamada: %s\n", fullUrl.c_str());
        Serial.printf("🔍 [RPC MASTER] Endpoint: %s\n", endpoint.c_str());
        Serial.printf("🔍 [RPC MASTER] Device ID: %s\n", getDeviceID().c_str());
        
        httpClient->end();
        networkWatchdog.endOperation(false, httpCode);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Ler resposta
    int contentLength = httpClient->getSize();
    Serial.printf("📏 [RPC MASTER] Content-Length: %d bytes\n", contentLength);
    
    // ✅ CRÍTICO: Tentar ler resposta mesmo se getSize() retorna 0 ou -1
    // Supabase pode retornar array vazio [] que tem tamanho pequeno
    String response = httpClient->getString();
    Serial.printf("📄 [RPC MASTER] Resposta recebida: %d bytes\n", response.length());
    
    // ✅ Alimentar watchdog después de leer respuesta
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante lectura de respuesta");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (response.length() > 0) {
        Serial.printf("📄 [RPC MASTER] Primeiros 200 chars: %s\n", 
                     response.substring(0, min(200, (int)response.length())).c_str());
    }
    
    if (contentLength <= 0 && response.length() == 0) {
        Serial.println("⚠️ [RPC MASTER] Resposta realmente vazia (sem dados)");
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Se getSize() retornou 0 mas temos resposta, continuar
    if (response.length() == 0) {
        Serial.println("⚠️ [RPC MASTER] Resposta vazia após getString()");
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    httpClient->end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    // Parsear JSON
    // ✅ OTIMIZAÇÃO: Buffer dinâmico baseado no tamanho da resposta (mínimo 2KB, máximo 16KB)
    int jsonSize = max(2048, min((int)(response.length() * 1.3), 16384)); // 30% de margem, max 16KB
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
        Serial.printf("❌ [RPC MASTER] Erro ao parsear JSON: %s\n", error.c_str());
        Serial.printf("   Tamanho resposta: %d bytes | Buffer usado: %d bytes\n", response.length(), jsonSize);
        setError("Erro ao parsear comandos JSON: " + String(error.c_str()));
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Processar comandos (RPC retorna array)
    JsonArray commandsArray = doc.as<JsonArray>();
    commandCount = min((int)commandsArray.size(), maxCommands);
    
    Serial.printf("📊 [RPC MASTER] Array recebido: %d comandos\n", commandsArray.size());
    
    // ✅ Se array vazio, não é erro - apenas não há comandos pendentes
    if (commandCount == 0) {
        if (!commandPollQuiet) {
            Serial.println("ℹ️ [RPC MASTER] Nenhum comando pendente (array vazio [])");
        }
        networkWatchdog.endOperation(true);
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return true;  // ✅ Retornar true mesmo sem comandos (não é erro)
    }
    
    // ✅ Processar comandos recebidos
    
    for (int i = 0; i < commandCount; i++) {
        JsonObject cmd = commandsArray[i];
        commands[i].id = cmd["id"];
        
        // ✅ Timestamp de recebimento
        commands[i].timestamp = millis();
        
        // ✅ NOVO: Parsear arrays relay_numbers, actions, duration_seconds
        // Por enquanto, pegar o primeiro elemento do array
        if (cmd.containsKey("relay_numbers") && cmd["relay_numbers"].is<JsonArray>()) {
            JsonArray relayNumbers = cmd["relay_numbers"];
            if (relayNumbers.size() > 0) {
                commands[i].relayNumber = relayNumbers[0];
            } else {
                commands[i].relayNumber = 0;
            }
        } else {
            commands[i].relayNumber = cmd["relay_number"] | 0;
        }
        
        if (cmd.containsKey("actions") && cmd["actions"].is<JsonArray>()) {
            JsonArray actions = cmd["actions"];
            if (actions.size() > 0) {
                commands[i].action = actions[0].as<String>();
            } else {
                commands[i].action = "off";
            }
        } else {
            commands[i].action = cmd["action"].as<String>();
        }
        
        if (cmd.containsKey("duration_seconds") && cmd["duration_seconds"].is<JsonArray>()) {
            JsonArray durations = cmd["duration_seconds"];
            if (durations.size() > 0) {
                commands[i].durationSeconds = durations[0];
            } else {
                commands[i].durationSeconds = 0;
            }
        } else {
            commands[i].durationSeconds = cmd["duration_seconds"] | 0;
        }
        
        commands[i].status = cmd["status"].as<String>();
        commands[i].timestamp = now;
        
        // Campos opcionais
        if (cmd.containsKey("command_type")) {
            commands[i].command_type = cmd["command_type"].as<String>();
        } else {
            commands[i].command_type = "manual";
        }
        
        if (cmd.containsKey("priority")) {
            commands[i].priority = cmd["priority"] | 50;
        } else {
            commands[i].priority = 10;
        }
        
        if (cmd.containsKey("triggered_by")) {
            commands[i].triggered_by = cmd["triggered_by"].as<String>();
        } else {
            commands[i].triggered_by = "manual";
        }
        
        if (cmd.containsKey("rule_id")) {
            commands[i].rule_id = cmd["rule_id"].as<String>();
        } else {
            commands[i].rule_id = "";
        }
        
        if (cmd.containsKey("rule_name")) {
            commands[i].rule_name = cmd["rule_name"].as<String>();
        } else {
            commands[i].rule_name = "";
        }
        
        // Master não tem slave_mac_address
        commands[i].target_device_id = "";
    }
    
    if (commandCount > 0) {
        Serial.printf("📥 [RPC MASTER] Recebidos %d comandos (status: processing)\n", commandCount);
    }

    networkWatchdog.endOperation(true);
    
    if (commandCheckMutex != nullptr) {
        xSemaphoreGive(commandCheckMutex);
    }
    
    return true;
}

// ✅ NOVO: Buscar comandos Slave usando RPC atômica
bool SupabaseClient::checkForSlaveCommands(RelayCommand* commands, int maxCommands, int& commandCount) {
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        return false;
    }
    
    // ✅ Proteger com mutex
    if (commandCheckMutex != nullptr) {
        if (xSemaphoreTake(commandCheckMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            setError("Timeout ao obter mutex para checkForSlaveCommands");
            return false;
        }
    }
    
    // Verificar apenas a cada COMMAND_POLL_INTERVAL_MS
    unsigned long now = millis();
    if (now - lastCommandCheck < commandPollIntervalMs) {
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    lastCommandCheck = now;
    
    // ✅ NOVO: Usar RPC atômica get_and_lock_slave_commands
    // ✅ CORREÇÃO: Usar POST com payload JSON (GET é read-only, RPC precisa fazer UPDATE)
    String endpoint = "rpc/get_and_lock_slave_commands";
    
    // ✅ Construir payload JSON para POST
    DynamicJsonDocument payloadDoc(256);
    payloadDoc["p_master_device_id"] = getDeviceID();
    payloadDoc["p_limit"] = maxCommands;
    payloadDoc["p_timeout_seconds"] = 30;
    
    String payload;
    serializeJson(payloadDoc, payload);
    
    Serial.printf("🔍 [RPC SLAVE] Verificando comandos: %s\n", (baseUrl + "/rest/v1/" + endpoint).c_str());
    Serial.printf("📦 [RPC SLAVE] Payload: %s\n", payload.c_str());
    
    // ✅ Usar Object Pool se disponível (mesmo código que checkForMasterCommands)
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            Serial.println("⚠️ [RPC SLAVE] Pool esgotado - tentando modo legacy");
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure();
        }
    }
    
    if (!usePool) {
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado e pool não disponível");
            if (commandCheckMutex != nullptr) {
                xSemaphoreGive(commandCheckMutex);
            }
            return false;
        }
        sslClient = secureClient;
        httpClient = &http;
    }
    
    // Verificar memória
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 30000) {
        Serial.printf("⚠️ [RPC SLAVE] Memoria insuficiente: %d bytes\n", freeHeap);
        setError("Memoria insuficiente para SSL");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ NOVO: Iniciar watchdog de red para proteger operación RPC
    if (!networkWatchdog.beginOperation(httpClient, sslClient)) {
        Serial.println("❌ [RPC SLAVE] NetworkWatchdog rechazó iniciar operación");
        setError("Watchdog rechazó operación - memoria insuficiente");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Fechar conexão prévia
    if (!usingPool && http.connected()) {
        http.end();
        vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        if (!networkWatchdog.feed()) {
            networkWatchdog.endOperation(false);
            setError("Watchdog timeout durante cierre de conexión previa");
            if (usingPool && poolMgr) {
                poolMgr->releaseHTTPClient(httpClient);
                poolMgr->releaseSSLClient(sslClient);
            }
            if (commandCheckMutex != nullptr) {
                xSemaphoreGive(commandCheckMutex);
            }
            return false;
        }
    }
    
    // Iniciar conexão SSL
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    if (!httpClient->begin(*sslClient, fullUrl)) {
        Serial.printf("❌ [RPC SLAVE] Falha ao iniciar conexão SSL\n");
        setError("Falha ao iniciar conexão SSL");
        networkWatchdog.endOperation(false);
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Alimentar watchdog después de begin()
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante inicio de conexión");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Configurar headers
    // ✅ REDUCIDO: Timeouts más cortos para evitar bloqueos largos
    httpClient->setConnectTimeout(8000);   // 8s conexão (reducido de 10s)
    httpClient->setTimeout(12000);          // 12s total (reducido de 15s)
    httpClient->setUserAgent("ESP32-Hydro/2.1.0");
    httpClient->addHeader("Authorization", buildAuthHeader());
    httpClient->addHeader("apikey", apiKey);
    httpClient->addHeader("Accept", "application/json");
    httpClient->addHeader("Content-Type", "application/json");  // ✅ POST precisa de Content-Type
    
    // ✅ DEBUG: Mostrar URL completa antes de chamar
    Serial.printf("🔍 [RPC SLAVE] URL completa: %s\n", fullUrl.c_str());
    Serial.printf("🔍 [RPC SLAVE] Endpoint: %s\n", endpoint.c_str());
    Serial.printf("🔍 [RPC SLAVE] Master Device ID: %s\n", getDeviceID().c_str());
    Serial.printf("🔍 [RPC SLAVE] Max Commands: %d\n", maxCommands);
    
    // ✅ Fazer POST request (GET é read-only, RPC precisa fazer UPDATE)
    Serial.println("📡 [RPC SLAVE] Enviando requisição POST...");
    int httpCode = httpClient->POST(payload);
    
    // ✅ CRÍTICO: Verificar watchdog después de operación HTTP bloqueante
    if (!networkWatchdog.feed()) {
        Serial.println("⏰ [RPC SLAVE] Watchdog timeout después de POST - forzando cierre");
        httpClient->end();
        if (sslClient) {
            sslClient->stop();
        }
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante operación POST");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (httpCode <= 0) {
        Serial.printf("❌ [RPC SLAVE] Erro HTTP: %d\n", httpCode);
        httpClient->end();
        networkWatchdog.endOperation(false);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (httpCode != 200) {
        Serial.printf("❌ [RPC SLAVE] HTTP %d\n", httpCode);
        
        // ✅ DEBUG MEJORADO: Ler resposta de erro completa
        if (httpCode > 0) {
            int errorContentLength = httpClient->getSize();
            Serial.printf("📏 [RPC SLAVE] Tamanho da resposta: %d bytes\n", errorContentLength);
            
            if (errorContentLength > 0) {
                // Aumentar limite para capturar respuestas más largas
                String errorResponse = httpClient->getString();
                Serial.printf("📄 [RPC SLAVE] Resposta completa:\n%s\n", errorResponse.c_str());
                
                // Tentar parsear JSON para extrair mensaje de erro
                if (errorResponse.length() > 0) {
                    DynamicJsonDocument errorDoc(1024);
                    DeserializationError jsonError = deserializeJson(errorDoc, errorResponse);
                    if (!jsonError) {
                        if (errorDoc.containsKey("message")) {
                            Serial.printf("💬 [RPC SLAVE] Mensagem: %s\n", errorDoc["message"].as<const char*>());
                        }
                        if (errorDoc.containsKey("hint")) {
                            Serial.printf("💡 [RPC SLAVE] Dica: %s\n", errorDoc["hint"].as<const char*>());
                        }
                        if (errorDoc.containsKey("code")) {
                            Serial.printf("🔢 [RPC SLAVE] Código: %s\n", errorDoc["code"].as<const char*>());
                        }
                    }
                }
            } else {
                Serial.println("⚠️ [RPC SLAVE] Resposta vazia (contentLength = 0)");
            }
        }
        
        // ✅ DEBUG: Mostrar URL completa que foi chamada
        Serial.printf("🔍 [RPC SLAVE] URL chamada: %s\n", fullUrl.c_str());
        Serial.printf("🔍 [RPC SLAVE] Endpoint: %s\n", endpoint.c_str());
        Serial.printf("🔍 [RPC SLAVE] Device ID: %s\n", getDeviceID().c_str());
        
        httpClient->end();
        networkWatchdog.endOperation(false, httpCode);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Ler resposta
    int contentLength = httpClient->getSize();
    Serial.printf("📏 [RPC SLAVE] Content-Length: %d bytes\n", contentLength);
    
    // ✅ CRÍTICO: Tentar ler resposta mesmo se getSize() retorna 0 ou -1
    // Supabase pode retornar array vazio [] que tem tamanho pequeno
    String response = httpClient->getString();
    Serial.printf("📄 [RPC SLAVE] Resposta recebida: %d bytes\n", response.length());
    
    // ✅ Alimentar watchdog después de leer respuesta
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante lectura de respuesta");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (response.length() > 0) {
        Serial.printf("📄 [RPC SLAVE] Primeiros 200 chars: %s\n", 
                     response.substring(0, min(200, (int)response.length())).c_str());
    }
    
    if (contentLength <= 0 && response.length() == 0) {
        Serial.println("⚠️ [RPC SLAVE] Resposta realmente vazia (sem dados)");
        httpClient->end();
        networkWatchdog.endOperation(false);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Se getSize() retornou 0 mas temos resposta, continuar
    if (response.length() == 0) {
        Serial.println("⚠️ [RPC SLAVE] Resposta vazia após getString()");
        httpClient->end();
        networkWatchdog.endOperation(false);
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    httpClient->end();
    networkWatchdog.endOperation(true);
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    // Parsear JSON
    // ✅ OTIMIZAÇÃO: Buffer dinâmico baseado no tamanho da resposta (mínimo 2KB, máximo 16KB)
    int jsonSize = max(2048, min((int)(response.length() * 1.3), 16384)); // 30% de margem, max 16KB
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
        Serial.printf("❌ [RPC SLAVE] Erro ao parsear JSON: %s\n", error.c_str());
        Serial.printf("   Tamanho resposta: %d bytes | Buffer usado: %d bytes\n", response.length(), jsonSize);
        setError("Erro ao parsear comandos JSON: " + String(error.c_str()));
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Processar comandos (RPC retorna array)
    JsonArray commandsArray = doc.as<JsonArray>();
    commandCount = min((int)commandsArray.size(), maxCommands);
    
    Serial.printf("📊 [RPC SLAVE] Array recebido: %d comandos\n", commandsArray.size());
    
    // ✅ Se array vazio, não é erro - apenas não há comandos pendentes
    if (commandCount == 0) {
        Serial.println("ℹ️ [RPC SLAVE] Nenhum comando pendente (array vazio [])");
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return true;  // ✅ Retornar true mesmo sem comandos (não é erro)
    }
    
    // ✅ Processar comandos recebidos
    
    for (int i = 0; i < commandCount; i++) {
        JsonObject cmd = commandsArray[i];
        commands[i].id = cmd["id"];
        
        // ✅ Timestamp de recebimento
        commands[i].timestamp = millis();
        
        // ✅ NOVO: Parsear arrays relay_numbers, actions, duration_seconds
        if (cmd.containsKey("relay_numbers") && cmd["relay_numbers"].is<JsonArray>()) {
            JsonArray relayNumbers = cmd["relay_numbers"];
            if (relayNumbers.size() > 0) {
                commands[i].relayNumber = relayNumbers[0];
            } else {
                commands[i].relayNumber = 0;
            }
        } else {
            commands[i].relayNumber = cmd["relay_number"] | 0;
        }
        
        if (cmd.containsKey("actions") && cmd["actions"].is<JsonArray>()) {
            JsonArray actions = cmd["actions"];
            if (actions.size() > 0) {
                commands[i].action = actions[0].as<String>();
            } else {
                commands[i].action = "off";
            }
        } else {
            commands[i].action = cmd["action"].as<String>();
        }
        
        if (cmd.containsKey("duration_seconds") && cmd["duration_seconds"].is<JsonArray>()) {
            JsonArray durations = cmd["duration_seconds"];
            if (durations.size() > 0) {
                commands[i].durationSeconds = durations[0];
            } else {
                commands[i].durationSeconds = 0;
            }
        } else {
            commands[i].durationSeconds = cmd["duration_seconds"] | 0;
        }
        
        commands[i].status = cmd["status"].as<String>();
        commands[i].timestamp = now;
        
        // ✅ NOVO: Parsear slave_mac_address para target_device_id
        if (cmd.containsKey("slave_mac_address")) {
            String slaveMac = cmd["slave_mac_address"].as<String>();
            commands[i].target_device_id = slaveMac;
        } else {
            commands[i].target_device_id = "";
        }
        
        // Campos opcionais
        if (cmd.containsKey("command_type")) {
            commands[i].command_type = cmd["command_type"].as<String>();
        } else {
            commands[i].command_type = "manual";
        }
        
        if (cmd.containsKey("priority")) {
            commands[i].priority = cmd["priority"] | 50;
        } else {
            commands[i].priority = 10;
        }
        
        if (cmd.containsKey("triggered_by")) {
            commands[i].triggered_by = cmd["triggered_by"].as<String>();
        } else {
            commands[i].triggered_by = "manual";
        }
        
        if (cmd.containsKey("rule_id")) {
            commands[i].rule_id = cmd["rule_id"].as<String>();
        } else {
            commands[i].rule_id = "";
        }
        
        if (cmd.containsKey("rule_name")) {
            commands[i].rule_name = cmd["rule_name"].as<String>();
        } else {
            commands[i].rule_name = "";
        }
    }
    
    if (commandCount > 0) {
        Serial.printf("📥 [RPC SLAVE] Recebidos %d comandos (status: processing)\n", commandCount);
    }
    
    if (commandCheckMutex != nullptr) {
        xSemaphoreGive(commandCheckMutex);
    }
    
    return true;
}

bool SupabaseClient::markCommandSent(int commandId, bool isSlave) {
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        return false;
    }
    
    // ✅ NOVO: Usar tabela correta (master ou slave)
    String table = isSlave ? "relay_commands_slave" : "relay_commands_master";
    String endpoint = table + "?id=eq." + String(commandId);
    String payload = "{\"status\": \"sent\", \"sent_at\": \"now()\"}";
    
    // ✅ Cerrar cualquier conexión previa antes de reutilizar http
    http.end();
    // ✅ Usar secureClient para mantener conexión SSL
    http.begin(*secureClient, baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    http.end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    return (httpCode >= 200 && httpCode < 300);
}

bool SupabaseClient::markCommandCompleted(int commandId, bool currentState, bool isSlave) {
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        return false;
    }
    
    // ✅ NOVO: Usar tabela correta (master ou slave)
    String table = isSlave ? "relay_commands_slave" : "relay_commands_master";
    String endpoint = table + "?id=eq." + String(commandId);
    
    // ✅ NOVO: Incluir current_state no payload
    DynamicJsonDocument doc(256);
    doc["status"] = "completed";
    doc["completed_at"] = "now()";
    doc["current_state"] = currentState;
    
    String payload;
    serializeJson(doc, payload);
    
    // ✅ Cerrar cualquier conexión previa antes de reutilizar http
    http.end();
    // ✅ Usar secureClient para mantener conexión SSL
    http.begin(*secureClient, baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    http.end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    return (httpCode >= 200 && httpCode < 300);
}

bool SupabaseClient::markCommandFailed(int commandId, const String& errorMessage, bool isSlave) {
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        return false;
    }
    
    // ✅ NOVO: Usar tabela correta (master ou slave)
    String table = isSlave ? "relay_commands_slave" : "relay_commands_master";
    String endpoint = table + "?id=eq." + String(commandId);
    
    DynamicJsonDocument doc(256);
    doc["status"] = "failed";
    doc["error_message"] = errorMessage;
    doc["completed_at"] = "now()";
    
    String payload;
    serializeJson(doc, payload);
    
    // ✅ Cerrar cualquier conexión previa antes de reutilizar http
    http.end();
    // ✅ Usar secureClient para mantener conexión SSL
    http.begin(*secureClient, baseUrl + "/rest/v1/" + endpoint);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    http.end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    return (httpCode >= 200 && httpCode < 300);
}

bool SupabaseClient::testConnection() {
    Serial.println("🧪 Testando conexão com Supabase...");
    
    // Verificar WiFi primeiro
    if (WiFi.status() != WL_CONNECTED) {
        setError("WiFi não conectado durante teste");
        return false;
    }
    
    Serial.printf("📡 WiFi OK - IP: %s\n", WiFi.localIP().toString().c_str());
    
    // ✅ CORREÇÃO: Usar Object Pool si está disponible, sino usar secureClient de fallback
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        sslClient = poolMgr->acquireSSLClient();
        if (sslClient) {
            sslClient->setInsecure();
            usingPool = true;
        } else {
            // Pool esgotado, usar fallback
            if (secureClient == nullptr) {
                setError("Cliente SSL não inicializado e pool esgotado");
                return false;
            }
            sslClient = secureClient;
        }
    } else {
        // Modo legacy
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado");
            return false;
        }
        sslClient = secureClient;
    }
    
    HTTPClient testHttp;
    // GET /rest/v1/ devolve 401 mesmo com anon válida; testar tabela real
    String testUrl = baseUrl + "/rest/v1/" + String(SUPABASE_STATUS_TABLE) + "?select=device_id&limit=1";
    Serial.printf("🌐 Testando URL: %s\n", testUrl.c_str());
    
    testHttp.begin(*sslClient, testUrl);
    testHttp.setConnectTimeout(15000); // 15s timeout conexão
    testHttp.setTimeout(20000); // 20s timeout total
    testHttp.setUserAgent("ESP32-Hydro/2.1.0");
    
    testHttp.addHeader("apikey", apiKey);
    testHttp.addHeader("Authorization", buildAuthHeader());
    testHttp.addHeader("Accept", "application/json");
    
    int httpCode = testHttp.GET();
    
    // ✅ Liberar cliente SSL del pool si se usó
    if (usingPool && poolMgr && sslClient) {
        poolMgr->releaseSSLClient(sslClient);
    }
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ Teste de conexão OK: HTTP %d\n", httpCode);
        testHttp.end();
        return true;
    } else if (httpCode > 0) {
        String response = testHttp.getString();
        Serial.printf("❌ Teste falhou: HTTP %d - %s\n", httpCode, response.c_str());
        setError("Teste de conexão falhou: HTTP " + String(httpCode));
        testHttp.end();
        return false;
    } else {
        // Erro de conexão
        String errorMsg = "Erro de conexão durante teste: HTTP " + String(httpCode);
        
        switch (httpCode) {
            case -7:
                errorMsg += " - Servidor não encontrado (verifique DNS)";
                Serial.println("🔍 Dica: Verifique se o DNS está funcionando");
                Serial.println("🔍 Tente ping google.com ou 8.8.8.8");
                break;
            case -4:
                errorMsg += " - Sem conexão de rede";
                break;
            case -11:
                errorMsg += " - Timeout (conexão muito lenta)";
                break;
        }
        
        setError(errorMsg);
        testHttp.end();
        return false;
    }
}

void SupabaseClient::setError(const String& error) {
    lastError = error;
    Serial.println("❌ SupabaseClient: " + error);
}

// ===== FUNCIÓN DE AUTO-REGISTRO =====
bool SupabaseClient::autoRegisterDevice(const String& deviceName, const String& location) {
    if (!isConnected) {
        setError("Supabase não conectado para auto-registro");
        return false;
    }
    
    Serial.println("🆔 Iniciando auto-registro do dispositivo...");
    
    // ✅ NOVO: Ler email de Preferences se existir
    Preferences preferences;
    String userEmail = "";
    if (preferences.begin("hydro_system", true)) {
        userEmail = preferences.getString("user_email", "");
        Serial.println("🔍 DEBUG: Email lido de Preferences: '" + userEmail + "' (length: " + String(userEmail.length()) + ")");
        preferences.end();
    } else {
        Serial.println("⚠️ DEBUG: Não foi possível abrir Preferences 'hydro_system'");
    }
    
    if (userEmail.length() == 0) {
        Serial.println("⚠️ DEBUG: Email NÃO encontrado em Preferences. Dispositivo será registrado sem email.");
    }
    
    // ✅ Se tem email, usar função register_device_with_email (melhor método)
    if (userEmail.length() > 0) {
        Serial.println("📧 Email encontrado em Preferences, usando register_device_with_email...");
        extern bool registerDeviceWithEmail(const String& userEmail, const String& deviceName, const String& location);

        String savedDeviceName = "";
        String savedLocation = "";
        if (preferences.begin("hydro_system", true)) {
            savedDeviceName = preferences.getString("device_name", "");
            savedLocation = preferences.getString("location", "");
            preferences.end();
        }
        String finalDeviceName = savedDeviceName.length() > 0
            ? savedDeviceName
            : (deviceName.isEmpty() ? String("ESP32 Hidropônico") : deviceName);
        String finalLocation = savedLocation.length() > 0
            ? savedLocation
            : (location.isEmpty() ? String("Estufa") : location);
        Serial.println("🏷️ Nome usado no registro: " + finalDeviceName);
        Serial.println("📍 Localização usada no registro: " + finalLocation);
        
        if (registerDeviceWithEmail(userEmail, finalDeviceName, finalLocation)) {
            Serial.println("✅ Dispositivo registrado com email via register_device_with_email");
            return true;
        } else {
            Serial.println("⚠️ Falha no registro com email, tentando método direto...");
            // Continuar com método direto como fallback
        }
    }
    
    // Preparar dados do dispositivo (método direto - sem email ou fallback)
    DynamicJsonDocument doc(512);
    doc["device_id"] = getDeviceID();
    doc["mac_address"] = getFullMAC();
    doc["ip_address"] = WiFi.localIP().toString();
    doc["device_name"] = deviceName.isEmpty() ? ("ESP32 - " + getMACsuffix()) : deviceName;
    doc["location"] = location.isEmpty() ? "Ubicación no especificada" : location;
    doc["device_type"] = "ESP32_HYDROPONIC";
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["is_online"] = true;
    
    // ✅ CRÍTICO: Verificar email atual no Supabase ANTES de atualizar
    String deviceId = getDeviceID();
    String existingEmail = "";
    
    // Buscar email atual no Supabase
    String checkUrl = baseUrl + "/rest/v1/" + String(SUPABASE_STATUS_TABLE) + "?device_id=eq." + deviceId + "&select=user_email";
    HTTPClient checkHttp;
    if (checkHttp.begin(*secureClient, checkUrl)) {
        checkHttp.addHeader("apikey", apiKey);
        checkHttp.addHeader("Authorization", "Bearer " + apiKey);
        checkHttp.addHeader("Accept", "application/json");
        
        int checkCode = checkHttp.GET();
        if (checkCode == 200) {
            String checkResponse = checkHttp.getString();
            DynamicJsonDocument checkDoc(256);
            if (deserializeJson(checkDoc, checkResponse) == DeserializationError::Ok) {
                if (checkDoc.is<JsonArray>() && checkDoc.size() > 0) {
                    JsonObject device = checkDoc[0];
                    if (device.containsKey("user_email") && !device["user_email"].isNull()) {
                        existingEmail = device["user_email"].as<String>();
                        Serial.println("🔒 Email existente no Supabase: " + existingEmail);
                    }
                }
            }
        }
        checkHttp.end();
    }
    
    // ✅ PROTEÇÃO: Só usar email de Preferences se dispositivo NÃO tem email no Supabase
    String emailToUse = "";
    if (existingEmail.length() > 0) {
        // Dispositivo já tem email - NÃO alterar
        emailToUse = existingEmail;
        Serial.println("🔒 Protegendo email existente: " + emailToUse);
        Serial.println("⚠️ Email de Preferences será ignorado para proteger email atual");
    } else if (userEmail.length() > 0) {
        // Dispositivo não tem email - usar email de Preferences
        emailToUse = userEmail;
        Serial.println("📧 Usando email de Preferences: " + emailToUse);
    } else {
        Serial.println("⚠️ Nenhum email disponível - dispositivo será registrado sem email");
    }
    
    // ✅ Adicionar email ao payload APENAS se não existe no Supabase
    if (emailToUse.length() > 0 && existingEmail.length() == 0) {
        doc["user_email"] = emailToUse;
        Serial.println("📧 Adicionando email ao payload: " + emailToUse);
    }
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("📤 Payload auto-registro: %s\n", payload.c_str());
    
    // ✅ CORRIGIDO: Usar estratégia PATCH + INSERT para UPSERT
    // Primeiro, tentar PATCH (atualizar se existir)
    String patchUrl = baseUrl + "/rest/v1/" + String(SUPABASE_STATUS_TABLE) + "?device_id=eq." + deviceId;
    
    // Preparar payload para PATCH (sem device_id, pois está na URL)
    DynamicJsonDocument patchDoc(512);
    patchDoc["mac_address"] = getFullMAC();
    patchDoc["ip_address"] = WiFi.localIP().toString();
    patchDoc["device_name"] = deviceName.isEmpty() ? ("ESP32 - " + getMACsuffix()) : deviceName;
    patchDoc["location"] = location.isEmpty() ? "Ubicación no especificada" : location;
    patchDoc["device_type"] = "ESP32_HYDROPONIC";
    patchDoc["firmware_version"] = FIRMWARE_VERSION;
    patchDoc["is_online"] = true;
    patchDoc["last_seen"] = "now()"; // Supabase aceita "now()" como string
    
    // ✅ CRÍTICO: NÃO atualizar email se já existe no Supabase
    if (emailToUse.length() > 0 && existingEmail.length() == 0) {
        patchDoc["user_email"] = emailToUse;
        Serial.println("📧 Adicionando email ao PATCH: " + emailToUse);
    } else if (existingEmail.length() > 0) {
        Serial.println("🔒 Email NÃO será atualizado no PATCH (proteção)");
    }
    
    // ✅ Adicionar email ao PATCH se existir
    if (userEmail.length() > 0) {
        patchDoc["user_email"] = userEmail;
    }
    
    String patchPayload;
    serializeJson(patchDoc, patchPayload);
    
    // ✅ Usar secureClient para mantener conexión SSL
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        return false;
    }
    // ✅ Cerrar cualquier conexión previa antes de reutilizar http
    http.end();
    http.begin(*secureClient, patchUrl);
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.addHeader("Prefer", "return=representation");
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(patchPayload);
    String response = http.getString();
    http.end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Se PATCH não funcionou (não encontrou ou erro), fazer INSERT
    // PATCH retorna 200 mesmo se não encontrar, então verificamos se a resposta está vazia
    bool patchSuccess = (httpCode >= 200 && httpCode < 300);
    bool needsInsert = !patchSuccess || response.length() < 10; // Resposta muito curta = não encontrou
    
    if (needsInsert) {
        Serial.println("📝 Dispositivo não encontrado ou PATCH falhou, fazendo INSERT...");
        
        // Fazer INSERT normal
        // ✅ Cerrar cualquier conexión previa antes de reutilizar http
        http.end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        // ✅ Usar secureClient para mantener conexión SSL
        String insertUrl = baseUrl + "/rest/v1/" + String(SUPABASE_STATUS_TABLE);
        http.begin(*secureClient, insertUrl);
        http.addHeader("Authorization", buildAuthHeader());
        http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
        http.addHeader("apikey", apiKey);
        http.addHeader("Prefer", "return=representation");
        http.setTimeout(SUPABASE_TIMEOUT_MS);
        
        // Payload completo para INSERT
        httpCode = http.POST(payload);
        response = http.getString();
        http.end();
        
        // Se INSERT também falhou com 409 (já existe), tentar PATCH novamente
        if (httpCode == 409) {
            Serial.println("⚠️ Dispositivo já existe, tentando PATCH novamente...");
            // ✅ Cerrar cualquier conexión previa antes de reutilizar http
            http.end();
            // ✅ Usar secureClient para mantener conexión SSL
            http.begin(*secureClient, patchUrl);
            http.addHeader("Authorization", buildAuthHeader());
            http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
            http.addHeader("apikey", apiKey);
            http.addHeader("Prefer", "return=representation");
            http.setTimeout(SUPABASE_TIMEOUT_MS);
            
            httpCode = http.PATCH(patchPayload);
            response = http.getString();
            http.end();
            // ✅ Dar tempo para SSL liberar memória
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ Dispositivo auto-registrado: %s\n", getDeviceID().c_str());
        Serial.printf("📍 Nome: %s | Localização: %s\n", 
                     deviceName.c_str(), location.c_str());
        return true;
    } else {
        Serial.printf("❌ Erro no auto-registro - HTTP %d: %s\n", httpCode, response.c_str());
        setError("Auto-registro falhou: " + String(httpCode));
        return false;
    }
}

// ===== MÉTODO GENÉRICO PARA INSERIR DADOS =====
bool SupabaseClient::insert(const String& table, const String& jsonData) {
    if (!isReady()) {
        setError("Supabase não está pronto");
        return false;
    }
    
    String endpoint = table;
    return makeRequest("POST", endpoint, jsonData);
}

// ===== ATUALIZAR ESTADO DE RELÉ DE SLAVE NO SUPABASE =====
// ✅ CORRIGIDO: Este método atualiza relay_slaves (não mais relay_states que foi removida)
// Atualiza um relé individual do array em relay_slaves
bool SupabaseClient::updateSlaveRelayState(const String& masterDeviceId, const String& slaveMacAddress, 
                                          const String& slaveDeviceId, int relayNumber, 
                                          bool state, bool hasTimer, int remainingTime) {
    Serial.printf("🔍 [RELAY_SLAVES] Atualizando relé %d do slave %s\n", relayNumber, slaveMacAddress.c_str());
    
    if (!isReady()) {
        Serial.println("❌ [RELAY_SLAVES] Supabase não está pronto");
        setError("Supabase não está pronto");
        return false;
    }
    
    if (secureClient == nullptr) {
        Serial.println("❌ [RELAY_SLAVES] Cliente SSL não inicializado");
        setError("Cliente SSL não inicializado");
        return false;
    }
    
    if (relayNumber < 0 || relayNumber >= 8) {
        Serial.printf("❌ [RELAY_SLAVES] Relé %d inválido (deve ser 0-7)\n", relayNumber);
        return false;
    }
    
    // ✅ PASSO 1: Buscar estado atual do slave em relay_slaves
    String getUrl = baseUrl + "/rest/v1/relay_slaves?device_id=eq." + slaveDeviceId + "&select=relay_states,relay_has_timers,relay_remaining_times";
    
    HTTPClient getHttp;
    bool hasExistingData = false;
    bool currentStates[8] = {false};
    bool currentHasTimers[8] = {false};
    int currentRemainingTimes[8] = {0};
    
    if (getHttp.begin(*secureClient, getUrl)) {
        getHttp.addHeader("apikey", apiKey);
        getHttp.addHeader("Authorization", buildAuthHeader());
        getHttp.addHeader("Accept", "application/json");
        getHttp.setTimeout(5000);
        
        int getCode = getHttp.GET();
        if (getCode == 200) {
            String getResponse = getHttp.getString();
            DynamicJsonDocument getDoc(1024);
            if (deserializeJson(getDoc, getResponse) == DeserializationError::Ok) {
                if (getDoc.is<JsonArray>() && getDoc.size() > 0) {
                    JsonObject slave = getDoc[0];
                    hasExistingData = true;
                    
                    // Ler arrays existentes
                    if (slave.containsKey("relay_states") && slave["relay_states"].is<JsonArray>()) {
                        JsonArray states = slave["relay_states"];
                        for (int i = 0; i < 8 && i < states.size(); i++) {
                            currentStates[i] = states[i].as<bool>();
        }
    }
                    if (slave.containsKey("relay_has_timers") && slave["relay_has_timers"].is<JsonArray>()) {
                        JsonArray timers = slave["relay_has_timers"];
                        for (int i = 0; i < 8 && i < timers.size(); i++) {
                            currentHasTimers[i] = timers[i].as<bool>();
                        }
                    }
                    if (slave.containsKey("relay_remaining_times") && slave["relay_remaining_times"].is<JsonArray>()) {
                        JsonArray times = slave["relay_remaining_times"];
                        for (int i = 0; i < 8 && i < times.size(); i++) {
                            currentRemainingTimes[i] = times[i].as<int>();
    }
                    }
                }
            }
        }
        getHttp.end();
    }
    
    // ✅ PASSO 2: Atualizar apenas o relé específico no array
    currentStates[relayNumber] = state;
    currentHasTimers[relayNumber] = hasTimer;
    currentRemainingTimes[relayNumber] = remainingTime;
    
    Serial.printf("📊 [RELAY_SLAVES] Estado atualizado: relay[%d] = %s (timer: %s, time: %d)\n", 
        relayNumber, state ? "ON" : "OFF", hasTimer ? "SIM" : "NÃO", remainingTime);
    
    // ✅ PASSO 3: Usar updateRelaySlaves para fazer PATCH/POST completo
    return updateRelaySlaves(slaveDeviceId, masterDeviceId, slaveMacAddress, 
                             currentStates, currentHasTimers, currentRemainingTimes, nullptr);
}

// ===== MÉTODO LEGACY: Fallback para slave_relay_states =====
// Mantido para compatibilidade durante migração
bool SupabaseClient::updateSlaveRelayStateLegacy(const String& masterDeviceId, const String& slaveMacAddress, 
                                                 const String& slaveDeviceId, int relayNumber, 
                                                 bool state, bool hasTimer, int remainingTime) {
    if (!isReady() || secureClient == nullptr) {
        return false;
    }
    
    DynamicJsonDocument doc(512);
    doc["master_device_id"] = masterDeviceId;
    doc["slave_mac_address"] = slaveMacAddress;
    doc["slave_device_id"] = slaveDeviceId;
    doc["relay_number"] = relayNumber;
    doc["state"] = state;
    doc["has_timer"] = hasTimer;
    doc["remaining_time"] = remainingTime;
    doc["last_update"] = "now()";
    doc["updated_at"] = "now()";
    
    String payload;
    serializeJson(doc, payload);
    
    String endpoint = "slave_relay_states";
    http.end();
    vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
    
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    if (!http.begin(*secureClient, fullUrl)) {
        return false;
    }
    
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("Prefer", "resolution=merge-duplicates");
    http.addHeader("apikey", apiKey);
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.POST(payload);
    http.end();
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ [SUPABASE] Estado atualizado via fallback (slave_relay_states)\n");
        return true;
    }
    
    return false;
}

// ===== NOVO: ATUALIZAR ESTADOS DOS RELÉS MASTER (relay_master) =====
// Escreve em relay_master com arrays segregados (dosadores, níveis, reservados)
bool SupabaseClient::updateRelayMaster(const String& deviceId, bool* relayStates, 
                                       bool* hasTimers, int* remainingTimes, 
                                       const String* relayNames) {
    // ✅ CRÍTICO: Proteger com mutex para evitar conflitos de escritura sequencial
    if (requestMutex == nullptr) {
        Serial.println("⚠️ [SUPABASE] requestMutex não inicializado - operação não protegida");
    } else {
        if (xSemaphoreTake(requestMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            Serial.println("❌ [SUPABASE] Timeout ao adquirir requestMutex em updateRelayMaster");
            setError("Timeout ao adquirir mutex de requisição");
            return false;
        }
    }
    
    if (!isReady()) {
        setError("Supabase não está pronto");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    // Obter user_email e master_mac_address
    String userEmail = "";
    String masterMacAddress = WiFi.macAddress();
    
    String checkUrl = baseUrl + "/rest/v1/" + String(SUPABASE_STATUS_TABLE) + "?device_id=eq." + deviceId + "&select=user_email,mac_address";
    HTTPClient checkHttp;
    if (checkHttp.begin(*secureClient, checkUrl)) {
        checkHttp.addHeader("apikey", apiKey);
        checkHttp.addHeader("Authorization", buildAuthHeader());
        checkHttp.addHeader("Accept", "application/json");
        checkHttp.setTimeout(5000);
        
        int checkCode = checkHttp.GET();
        if (checkCode == 200) {
            String checkResponse = checkHttp.getString();
            DynamicJsonDocument checkDoc(512);
            if (deserializeJson(checkDoc, checkResponse) == DeserializationError::Ok) {
                if (checkDoc.is<JsonArray>() && checkDoc.size() > 0) {
                    JsonObject device = checkDoc[0];
                    if (device.containsKey("user_email") && !device["user_email"].isNull()) {
                        userEmail = device["user_email"].as<String>();
                    }
                    if (device.containsKey("mac_address") && !device["mac_address"].isNull()) {
                        masterMacAddress = device["mac_address"].as<String>();
                    }
                }
            }
        }
        checkHttp.end();
    }
    
    // Fallback: tentar obter email de Preferences
    if (userEmail.length() == 0) {
        Preferences preferences;
        if (preferences.begin("hydro_system", true)) {
            userEmail = preferences.getString("user_email", "");
            preferences.end();
        }
    }
    
    // ✅ CRÍTICO: user_email é NOT NULL no schema - se não tiver, não pode fazer UPSERT
    if (userEmail.length() == 0) {
        Serial.println("⚠️ [SUPABASE] user_email não encontrado - não é possível atualizar relay_master");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    // ✅ CRÍTICO: PATCH atualiza apenas campos enviados (preserva dados existentes)
    // POST sobrescreve tudo (usar apenas se registro não existe)
    
    // Construir payload JSON para relay_master
    // ✅ PATCH: Enviar apenas estados/timers (não sobrescreve nomes se não enviarmos)
    DynamicJsonDocument doc(2048);
    doc["device_id"] = deviceId;
    doc["user_email"] = userEmail;  // ✅ OBRIGATÓRIO
    doc["master_mac_address"] = masterMacAddress;  // ✅ OBRIGATÓRIO
    
    // ✅ PCF8574 #1: Dosadores (0-7) - SEMPRE atualizar estados/timers
    JsonArray doserStates = doc.createNestedArray("doser_relay_states");
    JsonArray doserTimers = doc.createNestedArray("doser_relay_has_timers");
    JsonArray doserTimes = doc.createNestedArray("doser_relay_remaining_times");
    
    // ✅ Nomes: Só enviar se temos nomes novos (não sobrescrever existentes)
    bool hasNewNames = false;
    JsonArray doserNames;
    if (relayNames) {
        for (int i = 0; i < 8; i++) {
            if (relayNames[i].length() > 0) {
                hasNewNames = true;
                break;
            }
        }
        if (hasNewNames) {
            doserNames = doc.createNestedArray("doser_relay_names");
        }
    }
    
    for (int i = 0; i < 8; i++) {
        doserStates.add(relayStates ? relayStates[i] : false);
        doserTimers.add(hasTimers ? hasTimers[i] : false);
        doserTimes.add(remainingTimes ? remainingTimes[i] : 0);
        if (hasNewNames && relayNames && relayNames[i].length() > 0) {
            doserNames.add(relayNames[i]);
        } else if (hasNewNames) {
            doserNames.add(nullptr);  // Só adiciona null se estamos enviando o array
        }
    }
    
    // ✅ PCF8574 #2: Níveis (8-11)
    JsonArray levelStates = doc.createNestedArray("level_relay_states");
    JsonArray levelTimers = doc.createNestedArray("level_relay_has_timers");
    JsonArray levelTimes = doc.createNestedArray("level_relay_remaining_times");
    
    bool hasNewLevelNames = false;
    JsonArray levelNames;
    if (relayNames) {
        for (int i = 8; i < 12; i++) {
            if (relayNames[i].length() > 0) {
                hasNewLevelNames = true;
                break;
            }
        }
        if (hasNewLevelNames) {
            levelNames = doc.createNestedArray("level_relay_names");
        }
    }
    
    for (int i = 8; i < 12; i++) {
        levelStates.add(relayStates ? relayStates[i] : false);
        levelTimers.add(hasTimers ? hasTimers[i] : false);
        levelTimes.add(remainingTimes ? remainingTimes[i] : 0);
        if (hasNewLevelNames && relayNames && relayNames[i].length() > 0) {
            levelNames.add(relayNames[i]);
        } else if (hasNewLevelNames) {
            levelNames.add(nullptr);
        }
    }
    
    // ✅ Reservados (12-15)
    JsonArray reservedStates = doc.createNestedArray("reserved_relay_states");
    JsonArray reservedTimers = doc.createNestedArray("reserved_relay_has_timers");
    JsonArray reservedTimes = doc.createNestedArray("reserved_relay_remaining_times");
    
    bool hasNewReservedNames = false;
    JsonArray reservedNames;
    if (relayNames) {
        for (int i = 12; i < 16; i++) {
            if (relayNames[i].length() > 0) {
                hasNewReservedNames = true;
                break;
            }
        }
        if (hasNewReservedNames) {
            reservedNames = doc.createNestedArray("reserved_relay_names");
        }
    }
    
    for (int i = 12; i < 16; i++) {
        reservedStates.add(relayStates ? relayStates[i] : false);
        reservedTimers.add(hasTimers ? hasTimers[i] : false);
        reservedTimes.add(remainingTimes ? remainingTimes[i] : 0);
        if (hasNewReservedNames && relayNames && relayNames[i].length() > 0) {
            reservedNames.add(relayNames[i]);
        } else if (hasNewReservedNames) {
            reservedNames.add(nullptr);
        }
    }
    
    doc["last_update"] = "now()";
    doc["updated_at"] = "now()";
    
    String payload;
    serializeJson(doc, payload);
    
    // ✅ CORRIGIDO: PATCH primeiro (atualiza apenas campos enviados - PRESERVA dados existentes)
    // Se não encontrou, POST (INSERT novo registro)
    String patchUrl = baseUrl + "/rest/v1/relay_master?device_id=eq." + deviceId;
    
    http.end();
    vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
    
    if (!http.begin(*secureClient, patchUrl)) {
        setError("Falha ao iniciar conexão SSL para updateRelayMaster");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.addHeader("Prefer", "return=representation");
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    int httpCode = http.PATCH(payload);
    String response = http.getString();
    http.end();
    
    // Se PATCH não encontrou (404) ou falhou, tentar POST (INSERT)
    bool patchSuccess = (httpCode >= 200 && httpCode < 300);
    bool needsInsert = !patchSuccess || response.length() < 10;
    
    if (needsInsert) {
        Serial.println("📝 [SUPABASE] relay_master não encontrado, fazendo INSERT...");
        
        // ✅ POST: Enviar payload completo (incluindo arrays vazios para nomes se necessário)
        // Para INSERT, precisamos de arrays completos mesmo que vazios
        DynamicJsonDocument postDoc(2048);
        postDoc["device_id"] = deviceId;
        postDoc["user_email"] = userEmail;
        postDoc["master_mac_address"] = masterMacAddress;
        
        // Arrays completos para INSERT
        JsonArray postDoserStates = postDoc.createNestedArray("doser_relay_states");
        JsonArray postDoserTimers = postDoc.createNestedArray("doser_relay_has_timers");
        JsonArray postDoserTimes = postDoc.createNestedArray("doser_relay_remaining_times");
        JsonArray postDoserNames = postDoc.createNestedArray("doser_relay_names");
        
        for (int i = 0; i < 8; i++) {
            postDoserStates.add(relayStates ? relayStates[i] : false);
            postDoserTimers.add(hasTimers ? hasTimers[i] : false);
            postDoserTimes.add(remainingTimes ? remainingTimes[i] : 0);
            postDoserNames.add(nullptr);  // Array completo para INSERT
        }
        
        JsonArray postLevelStates = postDoc.createNestedArray("level_relay_states");
        JsonArray postLevelTimers = postDoc.createNestedArray("level_relay_has_timers");
        JsonArray postLevelTimes = postDoc.createNestedArray("level_relay_remaining_times");
        JsonArray postLevelNames = postDoc.createNestedArray("level_relay_names");
        
        for (int i = 8; i < 12; i++) {
            postLevelStates.add(relayStates ? relayStates[i] : false);
            postLevelTimers.add(hasTimers ? hasTimers[i] : false);
            postLevelTimes.add(remainingTimes ? remainingTimes[i] : 0);
            postLevelNames.add(nullptr);
        }
        
        JsonArray postReservedStates = postDoc.createNestedArray("reserved_relay_states");
        JsonArray postReservedTimers = postDoc.createNestedArray("reserved_relay_has_timers");
        JsonArray postReservedTimes = postDoc.createNestedArray("reserved_relay_remaining_times");
        JsonArray postReservedNames = postDoc.createNestedArray("reserved_relay_names");
        
        for (int i = 12; i < 16; i++) {
            postReservedStates.add(relayStates ? relayStates[i] : false);
            postReservedTimers.add(hasTimers ? hasTimers[i] : false);
            postReservedTimes.add(remainingTimes ? remainingTimes[i] : 0);
            postReservedNames.add(nullptr);
        }
        
        postDoc["last_update"] = "now()";
        postDoc["updated_at"] = "now()";
        
        String postPayload;
        serializeJson(postDoc, postPayload);
        
        String postUrl = baseUrl + "/rest/v1/relay_master";
        
        http.end();
        vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        
        if (!http.begin(*secureClient, postUrl)) {
            if (requestMutex != nullptr) {
                xSemaphoreGive(requestMutex);
            }
            return false;
        }
        
        http.addHeader("Authorization", buildAuthHeader());
        http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
        http.addHeader("apikey", apiKey);
        http.addHeader("Prefer", "return=representation");
        http.setTimeout(SUPABASE_TIMEOUT_MS);
        
        httpCode = http.POST(postPayload);
        response = http.getString();
        http.end();
    }
    
    // ✅ Liberar mutex antes de retornar
    if (requestMutex != nullptr) {
        xSemaphoreGive(requestMutex);
    }
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ [SUPABASE] Estados dos relés master atualizados em relay_master: device=%s\n", deviceId.c_str());
        return true;
    } else {
        Serial.printf("❌ [SUPABASE] Erro ao atualizar relay_master: HTTP %d\n", httpCode);
        Serial.printf("   Resposta: %s\n", response.c_str());
        setError("Erro HTTP " + String(httpCode));
        return false;
    }
}

// ===== NOVO: ATUALIZAR ESTADOS DOS RELÉS DE SLAVE (relay_slaves) =====
// Escreve em relay_slaves com arrays (8 relés)
bool SupabaseClient::updateRelaySlaves(const String& slaveDeviceId, const String& masterDeviceId,
                                       const String& slaveMacAddress, bool* relayStates,
                                       bool* hasTimers, int* remainingTimes, 
                                       const String* relayNames) {
    // ✅ CRÍTICO: Proteger com mutex para evitar conflitos de escritura sequencial
    if (requestMutex == nullptr) {
        Serial.println("⚠️ [SUPABASE] requestMutex não inicializado - operação não protegida");
    } else {
        if (xSemaphoreTake(requestMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            Serial.println("❌ [SUPABASE] Timeout ao adquirir requestMutex em updateRelaySlaves");
            setError("Timeout ao adquirir mutex de requisição");
            return false;
        }
    }
    
    if (!isReady()) {
        setError("Supabase não está pronto");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    if (secureClient == nullptr) {
        setError("Cliente SSL não inicializado");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    // Obter user_email e master_mac_address
    String userEmail = "";
    String masterMacAddress = WiFi.macAddress();
    
    Serial.printf("🔍 [RELAY_SLAVES] Buscando user_email para master: %s\n", masterDeviceId.c_str());
    
    String checkUrl = baseUrl + "/rest/v1/" + String(SUPABASE_STATUS_TABLE) + "?device_id=eq." + masterDeviceId + "&select=user_email,mac_address";
    Serial.printf("🔍 [RELAY_SLAVES] URL de verificação: %s\n", checkUrl.c_str());
    
    HTTPClient checkHttp;
    if (checkHttp.begin(*secureClient, checkUrl)) {
        checkHttp.addHeader("apikey", apiKey);
        checkHttp.addHeader("Authorization", buildAuthHeader());
        checkHttp.addHeader("Accept", "application/json");
        checkHttp.setTimeout(5000);
        
        int checkCode = checkHttp.GET();
        Serial.printf("📡 [RELAY_SLAVES] GET user_email: HTTP %d\n", checkCode);
        
        if (checkCode == 200) {
            String checkResponse = checkHttp.getString();
            Serial.printf("📄 [RELAY_SLAVES] Resposta: %s\n", checkResponse.c_str());
            
            DynamicJsonDocument checkDoc(512);
            if (deserializeJson(checkDoc, checkResponse) == DeserializationError::Ok) {
                if (checkDoc.is<JsonArray>() && checkDoc.size() > 0) {
                    JsonObject device = checkDoc[0];
                    if (device.containsKey("user_email") && !device["user_email"].isNull()) {
                        userEmail = device["user_email"].as<String>();
                        Serial.printf("✅ [RELAY_SLAVES] user_email encontrado: %s\n", userEmail.c_str());
                    } else {
                        Serial.println("⚠️ [RELAY_SLAVES] user_email não encontrado na resposta JSON");
                    }
                    if (device.containsKey("mac_address") && !device["mac_address"].isNull()) {
                        masterMacAddress = device["mac_address"].as<String>();
                        Serial.printf("✅ [RELAY_SLAVES] master_mac_address: %s\n", masterMacAddress.c_str());
                    }
                } else {
                    Serial.printf("⚠️ [RELAY_SLAVES] Array vazio ou inválido (size: %d)\n", checkDoc.size());
                }
            } else {
                Serial.println("❌ [RELAY_SLAVES] Erro ao parsear JSON da resposta");
            }
        } else {
            Serial.printf("❌ [RELAY_SLAVES] Erro HTTP ao buscar user_email: %d\n", checkCode);
            String errorResponse = checkHttp.getString();
            if (errorResponse.length() > 0) {
                Serial.printf("📄 [RELAY_SLAVES] Resposta de erro: %s\n", errorResponse.c_str());
            }
        }
        checkHttp.end();
    } else {
        Serial.println("❌ [RELAY_SLAVES] Falha ao iniciar conexão SSL para buscar user_email");
    }
    
    // Fallback: tentar obter email de Preferences
    if (userEmail.length() == 0) {
        Serial.println("🔍 [RELAY_SLAVES] Tentando obter user_email de Preferences...");
        Preferences preferences;
        if (preferences.begin("hydro_system", true)) {
            userEmail = preferences.getString("user_email", "");
            preferences.end();
            if (userEmail.length() > 0) {
                Serial.printf("✅ [RELAY_SLAVES] user_email encontrado em Preferences: %s\n", userEmail.c_str());
            } else {
                Serial.println("⚠️ [RELAY_SLAVES] user_email não encontrado em Preferences");
            }
        } else {
            Serial.println("❌ [RELAY_SLAVES] Falha ao abrir Preferences");
        }
    }
    
    // ✅ CRÍTICO: user_email é NOT NULL no schema - se não tiver, não pode fazer UPSERT
    if (userEmail.length() == 0) {
        Serial.println("❌ [RELAY_SLAVES] user_email não encontrado - não é possível atualizar relay_slaves");
        Serial.println("💡 [RELAY_SLAVES] Dica: Verifique se device_status tem user_email para este device_id");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    // ✅ CRÍTICO: PATCH atualiza apenas campos enviados (preserva dados existentes)
    // POST sobrescreve tudo (usar apenas se registro não existe)
    
    // Construir payload JSON para relay_slaves
    // ✅ PATCH: Enviar apenas estados/timers (não sobrescreve nomes se não enviarmos)
    DynamicJsonDocument doc(1536);
    doc["device_id"] = slaveDeviceId;
    doc["user_email"] = userEmail;  // ✅ OBRIGATÓRIO
    doc["master_device_id"] = masterDeviceId;  // ✅ OBRIGATÓRIO
    doc["master_mac_address"] = masterMacAddress;  // ✅ OBRIGATÓRIO
    doc["slave_mac_address"] = slaveMacAddress;  // ✅ OBRIGATÓRIO
    
    // Arrays para 8 relés - SEMPRE atualizar estados/timers
    JsonArray states = doc.createNestedArray("relay_states");
    JsonArray timers = doc.createNestedArray("relay_has_timers");
    JsonArray times = doc.createNestedArray("relay_remaining_times");
    
    // ✅ Nomes: Só enviar se temos nomes novos (não sobrescrever existentes)
    bool hasNewSlaveNames = false;
    JsonArray names;
    if (relayNames) {
        for (int i = 0; i < 8; i++) {
            if (relayNames[i].length() > 0) {
                hasNewSlaveNames = true;
                break;
            }
        }
        if (hasNewSlaveNames) {
            names = doc.createNestedArray("relay_names");
        }
    }
    
    for (int i = 0; i < 8; i++) {
        states.add(relayStates ? relayStates[i] : false);
        timers.add(hasTimers ? hasTimers[i] : false);
        times.add(remainingTimes ? remainingTimes[i] : 0);
        if (hasNewSlaveNames && relayNames && relayNames[i].length() > 0) {
            names.add(relayNames[i]);
        } else if (hasNewSlaveNames) {
            names.add(nullptr);  // Só adiciona null se estamos enviando o array
        }
    }
    
    doc["last_update"] = "now()";
    doc["updated_at"] = "now()";
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("📦 [RELAY_SLAVES] Payload PATCH: %s\n", payload.c_str());
    Serial.printf("📊 [RELAY_SLAVES] Estados no payload: [");
    for (int i = 0; i < 8; i++) {
        Serial.printf("%s", relayStates ? (relayStates[i] ? "true" : "false") : "false");
        if (i < 7) Serial.printf(", ");
    }
    Serial.println("]");
    
    // ✅ CORRIGIDO: PATCH primeiro (atualiza apenas campos enviados - PRESERVA dados existentes)
    // Se não encontrou, POST (INSERT novo registro)
    String patchUrl = baseUrl + "/rest/v1/relay_slaves?device_id=eq." + slaveDeviceId;
    Serial.printf("🔍 [RELAY_SLAVES] Tentando PATCH: %s\n", patchUrl.c_str());
    
    http.end();
    vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
    
    if (!http.begin(*secureClient, patchUrl)) {
        Serial.println("❌ [RELAY_SLAVES] Falha ao iniciar conexão SSL para PATCH");
        setError("Falha ao iniciar conexão SSL para updateRelaySlaves");
        if (requestMutex != nullptr) {
            xSemaphoreGive(requestMutex);
        }
        return false;
    }
    
    http.addHeader("Authorization", buildAuthHeader());
    http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
    http.addHeader("apikey", apiKey);
    http.addHeader("Prefer", "return=representation");
    http.setTimeout(SUPABASE_TIMEOUT_MS);
    
    Serial.println("📡 [RELAY_SLAVES] Enviando requisição PATCH...");
    int httpCode = http.PATCH(payload);
    String response = http.getString();
    Serial.printf("📥 [RELAY_SLAVES] PATCH HTTP %d\n", httpCode);
    Serial.printf("📄 [RELAY_SLAVES] Resposta: %s\n", response.c_str());
    http.end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Se PATCH não encontrou (404) ou falhou, tentar POST (INSERT)
    bool patchSuccess = (httpCode >= 200 && httpCode < 300);
    bool needsInsert = !patchSuccess || response.length() < 10;
    
    if (needsInsert) {
        Serial.printf("⚠️ [RELAY_SLAVES] PATCH falhou ou registro não existe (HTTP %d, response length: %d)\n", httpCode, response.length());
    }
    
    if (needsInsert) {
        Serial.println("📝 [SUPABASE] relay_slaves não encontrado, fazendo INSERT...");
        
        // ✅ POST: Enviar payload completo (incluindo arrays vazios para nomes)
        // Para INSERT, precisamos de arrays completos mesmo que vazios
        DynamicJsonDocument postDoc(1536);
        postDoc["device_id"] = slaveDeviceId;
        postDoc["user_email"] = userEmail;
        postDoc["master_device_id"] = masterDeviceId;
        postDoc["master_mac_address"] = masterMacAddress;
        postDoc["slave_mac_address"] = slaveMacAddress;
        
        // Arrays completos para INSERT
        JsonArray postStates = postDoc.createNestedArray("relay_states");
        JsonArray postTimers = postDoc.createNestedArray("relay_has_timers");
        JsonArray postTimes = postDoc.createNestedArray("relay_remaining_times");
        JsonArray postNames = postDoc.createNestedArray("relay_names");
        
        for (int i = 0; i < 8; i++) {
            postStates.add(relayStates ? relayStates[i] : false);
            postTimers.add(hasTimers ? hasTimers[i] : false);
            postTimes.add(remainingTimes ? remainingTimes[i] : 0);
            postNames.add(nullptr);  // Array completo para INSERT
        }
        
        postDoc["last_update"] = "now()";
        postDoc["updated_at"] = "now()";
        
        String postPayload;
        serializeJson(postDoc, postPayload);
        
        String postUrl = baseUrl + "/rest/v1/relay_slaves";
        
        http.end();
        vTaskDelay(pdMS_TO_TICKS(50)); // ✅ Otimizado: vTaskDelay não bloqueia outros tasks
        
        if (!http.begin(*secureClient, postUrl)) {
            if (requestMutex != nullptr) {
                xSemaphoreGive(requestMutex);
            }
            return false;
        }
        
        http.addHeader("Authorization", buildAuthHeader());
        http.addHeader("Content-Type", SUPABASE_CONTENT_TYPE);
        http.addHeader("apikey", apiKey);
        http.addHeader("Prefer", "return=representation");
        http.setTimeout(SUPABASE_TIMEOUT_MS);
        
        Serial.printf("📦 [RELAY_SLAVES] Payload POST: %s\n", postPayload.c_str());
        Serial.println("📡 [RELAY_SLAVES] Enviando requisição POST...");
        httpCode = http.POST(postPayload);
        response = http.getString();
        Serial.printf("📥 [RELAY_SLAVES] POST HTTP %d\n", httpCode);
        Serial.printf("📄 [RELAY_SLAVES] Resposta POST: %s\n", response.c_str());
        http.end();
    }
    
    // ✅ Liberar mutex antes de retornar
    if (requestMutex != nullptr) {
        xSemaphoreGive(requestMutex);
    }
    
    if (httpCode >= 200 && httpCode < 300) {
        Serial.printf("✅ [SUPABASE] Estados dos relés slave atualizados em relay_slaves: slave=%s\n", slaveMacAddress.c_str());
        Serial.printf("✅ [RELAY_SLAVES] PATCH/POST sucesso! Resposta: %s\n", response.c_str());
        return true;
    } else {
        Serial.printf("❌ [SUPABASE] Erro ao atualizar relay_slaves: HTTP %d\n", httpCode);
        Serial.printf("❌ [RELAY_SLAVES] Resposta completa: %s\n", response.c_str());
        
        // Tentar parsear erro JSON
        if (response.length() > 0) {
            DynamicJsonDocument errorDoc(512);
            DeserializationError jsonError = deserializeJson(errorDoc, response);
            if (!jsonError) {
                if (errorDoc.containsKey("message")) {
                    Serial.printf("💬 [RELAY_SLAVES] Mensagem de erro: %s\n", errorDoc["message"].as<const char*>());
                }
                if (errorDoc.containsKey("hint")) {
                    Serial.printf("💡 [RELAY_SLAVES] Dica: %s\n", errorDoc["hint"].as<const char*>());
                }
                if (errorDoc.containsKey("code")) {
                    Serial.printf("🔢 [RELAY_SLAVES] Código: %s\n", errorDoc["code"].as<const char*>());
                }
            }
        }
        
        setError("Erro HTTP " + String(httpCode));
        return false;
    }
}

// ===== INICIALIZAÇÃO E LIMPEZA DE MUTEXES =====
// ✅ NOVO: Mutex para thread-safety (proteção contra race conditions)

bool SupabaseClient::initMutexes() {
    Serial.println("🔒 [SUPABASE] Inicializando mutexes para thread-safety...");
    
    // Criar mutex para proteger makeRequest()
    requestMutex = xSemaphoreCreateMutex();
    if (!requestMutex) {
        Serial.println("❌ [SUPABASE] Falha ao criar requestMutex");
        return false;
    }
    Serial.println("✅ [SUPABASE] requestMutex criado");
    
    // Criar mutex para proteger checkForCommands()
    commandCheckMutex = xSemaphoreCreateMutex();
    if (!commandCheckMutex) {
        Serial.println("❌ [SUPABASE] Falha ao criar commandCheckMutex");
        vSemaphoreDelete(requestMutex);
        requestMutex = nullptr;
        return false;
    }
    Serial.println("✅ [SUPABASE] commandCheckMutex criado");
    
    Serial.println("✅ [SUPABASE] Mutexes inicializados com sucesso");
    return true;
}

// ✅ NOVO: Buscar EC Config do Supabase via RPC activate_auto_ec
bool SupabaseClient::getECConfigFromSupabase(ECConfig& config) {
    // Inicializar config como inválida
    config.isValid = false;
    
    if (!isConnected) {
        Serial.println("❌ [RPC EC_CONFIG] Supabase não conectado");
        setError("Supabase não conectado");
        return false;
    }
    
    // ✅ Usar mutex para thread-safety
    if (commandCheckMutex != nullptr) {
        if (xSemaphoreTake(commandCheckMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            Serial.println("❌ [RPC EC_CONFIG] Timeout ao adquirir mutex");
            setError("Timeout ao adquirir mutex");
            return false;
        }
    }
    
    // ✅ RPC activate_auto_ec
    String endpoint = "rpc/activate_auto_ec";
    
    // ✅ Construir payload JSON para POST
    DynamicJsonDocument payloadDoc(256);
    payloadDoc["p_device_id"] = getDeviceID();
    
    String payload;
    serializeJson(payloadDoc, payload);
    
    Serial.printf("🔍 [RPC EC_CONFIG] Verificando config: %s\n", (baseUrl + "/rest/v1/" + endpoint).c_str());
    Serial.printf("📦 [RPC EC_CONFIG] Payload: %s\n", payload.c_str());
    Serial.printf("🔍 [RPC EC_CONFIG] Device ID: %s\n", getDeviceID().c_str());
    
    // ✅ Usar Object Pool se disponível
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            Serial.println("⚠️ [RPC EC_CONFIG] Pool esgotado - tentando modo legacy");
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure();
        }
    }
    
    if (!usePool) {
        if (secureClient == nullptr) {
            setError("Cliente SSL não inicializado e pool não disponível");
            if (commandCheckMutex != nullptr) {
                xSemaphoreGive(commandCheckMutex);
            }
            return false;
        }
        sslClient = secureClient;
        httpClient = &http;
    }
    
    // ✅ NOVO: Iniciar watchdog de red para proteger operação RPC (DESPUÉS de obtener clientes)
    extern NetworkWatchdog networkWatchdog;
    if (!networkWatchdog.beginOperation(httpClient, sslClient)) {
        Serial.println("❌ [RPC EC_CONFIG] NetworkWatchdog rechazó iniciar operación");
        setError("Watchdog rechazó operación - memoria insuficiente");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Fechar conexão prévia
    if (!usingPool && http.connected()) {
        http.end();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!networkWatchdog.feed()) {
            networkWatchdog.endOperation(false);
            setError("Watchdog timeout durante cierre de conexión previa");
            if (usingPool && poolMgr) {
                poolMgr->releaseHTTPClient(httpClient);
                poolMgr->releaseSSLClient(sslClient);
            }
            if (commandCheckMutex != nullptr) {
                xSemaphoreGive(commandCheckMutex);
            }
            return false;
        }
    }
    
    // ✅ Verificar saúde SSL: memória e estado da conexão
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    
    Serial.printf("🔍 [RPC EC_CONFIG] Verificando saúde SSL...\n");
    Serial.printf("   💾 Heap livre: %u bytes\n", freeHeap);
    Serial.printf("   📊 Max alloc: %u bytes\n", maxAlloc);
    
    if (freeHeap < 40000) {  // ✅ Aumentado de 30KB para 40KB
        Serial.printf("❌ [RPC EC_CONFIG] Memoria insuficiente: %u bytes (mínimo: 40KB)\n", freeHeap);
        setError("Memoria insuficiente para SSL");
        networkWatchdog.endOperation(false);
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ BLOQUEAR chamada se fragmentação detectada (maxAlloc muito baixo)
    if (maxAlloc < 30000) {  // ✅ Aumentado threshold: 30KB mínimo
        Serial.printf("❌ [RPC EC_CONFIG] Fragmentação detectada: maxAlloc = %u bytes (mínimo: 30KB)\n", maxAlloc);
        Serial.println("   💡 Adiando chamada SSL para evitar fragmentação excessiva");
        setError("Fragmentação de memória detectada");
        networkWatchdog.endOperation(false);
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;  // ✅ BLOQUEAR chamada
    } else if (maxAlloc < 50000) {
        Serial.printf("⚠️ [RPC EC_CONFIG] Max alloc moderado: %u bytes - monitorando fragmentação\n", maxAlloc);
    }
    
    // Iniciar conexão SSL
    String fullUrl = baseUrl + "/rest/v1/" + endpoint;
    if (!httpClient->begin(*sslClient, fullUrl)) {
        Serial.printf("❌ [RPC EC_CONFIG] Falha ao iniciar conexão SSL\n");
        setError("Falha ao iniciar conexão SSL");
        networkWatchdog.endOperation(false);
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Alimentar watchdog después de begin()
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante inicio de conexión");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // Configurar headers
    httpClient->setConnectTimeout(8000);   // 8s conexão
    httpClient->setTimeout(12000);          // 12s total
    httpClient->setUserAgent("ESP32-Hydro/2.1.0");
    httpClient->addHeader("Authorization", buildAuthHeader());
    httpClient->addHeader("apikey", apiKey);
    httpClient->addHeader("Accept", "application/json");
    httpClient->addHeader("Content-Type", "application/json");
    httpClient->addHeader("Prefer", "return=representation");
    
    // ✅ Fazer POST request
    Serial.println("📡 [RPC EC_CONFIG] Enviando requisição POST...");
    int httpCode = httpClient->POST(payload);
    
    // ✅ Verificar watchdog después de operación HTTP bloqueante
    if (!networkWatchdog.feed()) {
        Serial.println("⏰ [RPC EC_CONFIG] Watchdog timeout después de POST - forzando cierre");
        httpClient->end();
        if (sslClient) {
            sslClient->stop();
        }
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante operación POST");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (httpCode <= 0) {
        Serial.printf("❌ [RPC EC_CONFIG] Erro HTTP: %d\n", httpCode);
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Erro HTTP: " + String(httpCode));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (httpCode != 200) {
        Serial.printf("❌ [RPC EC_CONFIG] HTTP Code: %d (esperado 200)\n", httpCode);
        int contentLength = httpClient->getSize();
        Serial.printf("📏 [RPC EC_CONFIG] Content-Length: %d bytes\n", contentLength);
        
        if (contentLength > 0 && contentLength < 500) {
            String errorResponse = httpClient->getString();
            Serial.printf("📄 [RPC EC_CONFIG] Resposta de erro: %s\n", errorResponse.c_str());
        }
        
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false, httpCode);
        setError("HTTP Code: " + String(httpCode));
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Ler resposta
    int contentLength = httpClient->getSize();
    Serial.printf("📏 [RPC EC_CONFIG] Content-Length: %d bytes\n", contentLength);
    
    String response = httpClient->getString();
    Serial.printf("📄 [RPC EC_CONFIG] Resposta recebida: %d bytes\n", response.length());
    
    // ✅ Alimentar watchdog después de leer respuesta
    if (!networkWatchdog.feed()) {
        httpClient->end();
        // ✅ Delay para liberação de memória SSL (saúde operacional)
        vTaskDelay(pdMS_TO_TICKS(200));
        networkWatchdog.endOperation(false);
        setError("Watchdog timeout durante lectura de respuesta");
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    if (response.length() > 0) {
        Serial.printf("📄 [RPC EC_CONFIG] Primeiros 200 chars: %s\n", 
                     response.substring(0, min(200, (int)response.length())).c_str());
    }
    
    httpClient->end();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    if (response.length() == 0) {
        Serial.println("⚠️ [RPC EC_CONFIG] Resposta vazia");
        networkWatchdog.endOperation(false);
        setError("Resposta vazia");
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Parsear JSON (array com 1 elemento)
    int jsonSize = max(2048, min((int)(response.length() * 1.3), 16384));
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
        Serial.printf("❌ [RPC EC_CONFIG] Erro ao parsear JSON: %s\n", error.c_str());
        Serial.printf("   Tamanho resposta: %d bytes | Buffer usado: %d bytes\n", response.length(), jsonSize);
        setError("Erro ao parsear JSON: " + String(error.c_str()));
        networkWatchdog.endOperation(false);
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return false;
    }
    
    // ✅ Processar resposta (RPC retorna array)
    JsonArray configArray = doc.as<JsonArray>();
    
    Serial.printf("📊 [RPC EC_CONFIG] Array recebido: %d elemento(s)\n", configArray.size());
    
    if (configArray.size() == 0) {
        Serial.println("ℹ️ [RPC EC_CONFIG] Nenhuma config encontrada (array vazio [])");
        Serial.println("   💡 Auto EC desativado no Supabase ou execute 'Salvar Parâmetros' primeiro");
        config.isValid = false;
        networkWatchdog.endOperation(true);
        if (commandCheckMutex != nullptr) {
            xSemaphoreGive(commandCheckMutex);
        }
        return true;
    }
    
    // ✅ Extrair config do primeiro elemento
    JsonObject configObj = configArray[0];
    
    config.base_dose = configObj["base_dose"] | 0.0;
    config.flow_rate = configObj["flow_rate"] | 1.0;
    config.volume = configObj["volume"] | 10.0;
    config.total_ml = configObj["total_ml"] | 0.0;
    config.kp = configObj["kp"] | 1.0;
    config.ec_setpoint = configObj["ec_setpoint"] | 0.0;
    config.auto_enabled = configObj["auto_enabled"] | false;
    config.intervalo_auto_ec = configObj["intervalo_auto_ec"] | 300;
    config.tempo_recirculacao = configObj["tempo_recirculacao"] | 60;
    
    // ✅ Nutrients array (não salvo em NVS, mas usado para cálculo local)
    if (configObj.containsKey("nutrients") && configObj["nutrients"].is<JsonArray>()) {
        JsonArray nutrientsArray = configObj["nutrients"].as<JsonArray>();
        serializeJson(nutrientsArray, config.nutrientsJson);
        Serial.printf("📊 [RPC EC_CONFIG] Nutrients recebidos: %d nutriente(s)\n", nutrientsArray.size());
    } else {
        config.nutrientsJson = "[]";
    }
    
    config.isValid = true;
    
    // ✅ Mostrar valores recebidos
    Serial.println("✅ [RPC EC_CONFIG] Config recebida com sucesso:");
    Serial.printf("   • base_dose:        %.2f µS/cm\n", config.base_dose);
    Serial.printf("   • flow_rate:        %.3f ml/s\n", config.flow_rate);
    Serial.printf("   • volume:           %.2f L\n", config.volume);
    Serial.printf("   • total_ml:         %.2f ml/L\n", config.total_ml);
    Serial.printf("   • kp:               %.2f\n", config.kp);
    Serial.printf("   • ec_setpoint:      %.0f µS/cm\n", config.ec_setpoint);
    Serial.printf("   • auto_enabled:     %s\n", config.auto_enabled ? "true" : "false");
    Serial.printf("   • intervalo_auto_ec: %d segundos\n", config.intervalo_auto_ec);
    Serial.printf("   • tempo_recirculacao: %lu segundos\n", config.tempo_recirculacao);
    
    // ✅ Fechar conexão SSL e liberar recursos
    httpClient->end();
    if (sslClient && !usingPool) {
        sslClient->stop();
    }
    
    // ✅ Delay para liberação de memória SSL (saúde operacional)
    vTaskDelay(pdMS_TO_TICKS(200));  // 200ms para liberar memória SSL
    
    // ✅ Verificar saúde SSL após operação
    uint32_t freeHeapAfter = ESP.getFreeHeap();
    Serial.printf("💚 [RPC EC_CONFIG] Saúde SSL: Heap após operação: %u bytes\n", freeHeapAfter);
    if (freeHeapAfter < freeHeap - 5000) {
        Serial.printf("⚠️ [RPC EC_CONFIG] Possível vazamento de memória SSL: %u bytes liberados\n", 
            freeHeap - freeHeapAfter);
    }
    
    networkWatchdog.endOperation(true);
    
    // ✅ Liberar recursos do pool se estavam em uso
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    if (commandCheckMutex != nullptr) {
        xSemaphoreGive(commandCheckMutex);
    }
    
    return true;
}

void SupabaseClient::cleanupMutexes() {
    if (requestMutex != nullptr) {
        vSemaphoreDelete(requestMutex);
        requestMutex = nullptr;
        Serial.println("✅ [SUPABASE] requestMutex eliminado");
    }
    
    if (commandCheckMutex != nullptr) {
        vSemaphoreDelete(commandCheckMutex);
        commandCheckMutex = nullptr;
        Serial.println("✅ [SUPABASE] commandCheckMutex eliminado");
    }
}

bool SupabaseClient::insertNutrientDosage(const String& deviceId, const String& sequenceId,
                                          const String& nutrientName, int relayNumber,
                                          float dosageMl, float dosageTimeSeconds,
                                          float ecBefore, float ecSetpoint,
                                          const String& source) {
    if (!isReady()) {
        setError("Supabase não está pronto para insertNutrientDosage");
        return false;
    }

    DynamicJsonDocument doc(512);
    doc["device_id"] = deviceId;
    doc["sequence_id"] = sequenceId;
    doc["nutrient_name"] = nutrientName;
    doc["relay_number"] = relayNumber;
    doc["dosage_ml"] = round(dosageMl * 1000.0) / 1000.0;
    doc["dosage_time_seconds"] = round(dosageTimeSeconds * 100.0) / 100.0;
    doc["ec_before"] = round(ecBefore * 100.0) / 100.0;
    doc["ec_setpoint"] = round(ecSetpoint * 100.0) / 100.0;
    doc["source"] = source.length() > 0 ? source : "auto_ec";

    String payload;
    serializeJson(doc, payload);

    Serial.printf("💾 [DOSAGEM] INSERT nutrient_dosages: %s %.2f ml relé %d\n",
        nutrientName.c_str(), dosageMl, relayNumber + 1);

    return insert("nutrient_dosages", payload);
}

bool SupabaseClient::updateEcOperationState(const String& deviceId, const String& state,
                                            int operationRemainingSec, int nextCheckInSec) {
    if (!isReady()) {
        return false;
    }

    if (requestMutex != nullptr) {
        if (xSemaphoreTake(requestMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
            return false;
        }
    }

    DynamicJsonDocument doc(256);
    doc["ec_operation_state"] = state;
    doc["ec_operation_remaining_sec"] = operationRemainingSec > 0 ? operationRemainingSec : 0;
    doc["ec_next_check_in_sec"] = nextCheckInSec > 0 ? nextCheckInSec : 0;

    String payload;
    serializeJson(doc, payload);

    String patchUrl = baseUrl + "/rest/v1/relay_master?device_id=eq." + deviceId;
    bool ok = false;

    if (secureClient != nullptr && http.begin(*secureClient, patchUrl)) {
        http.addHeader("apikey", apiKey);
        http.addHeader("Authorization", buildAuthHeader());
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Prefer", "return=minimal");
        http.setTimeout(8000);
        int code = http.PATCH(payload);
        ok = (code >= 200 && code < 300);
        if (!ok) {
            Serial.printf("⚠️ [EC OP] PATCH relay_master ec_operation falhou HTTP %d\n", code);
        }
        http.end();
    }

    if (requestMutex != nullptr) {
        xSemaphoreGive(requestMutex);
    }
    return ok;
} 