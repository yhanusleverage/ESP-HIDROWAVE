#include "WebServerTask.h"
#include <WiFi.h>
#include <esp_task_wdt.h>

// Instancia estática para acceso desde callbacks
static WebServerTask* instance = nullptr;

WebServerTask::WebServerTask() : 
    server(nullptr),
    initialized(false),
    taskRunning(false),
    taskHandle(nullptr) {
    instance = this;
}

WebServerTask::~WebServerTask() {
    end();
}

void WebServerTask::taskFunction(void* parameter) {
    WebServerTask* task = static_cast<WebServerTask*>(parameter);
    if (task) {
        task->run();
    }
    vTaskDelete(NULL);
}

void WebServerTask::run() {
    Serial.println("🌐 [Core 1] WebServerTask iniciada");
    Serial.printf("   ✓ Core: %d\n", xPortGetCoreID());
    Serial.printf("   ✓ Prioridade: %d\n", uxTaskPriorityGet(NULL));
    Serial.printf("   ✓ Stack: %d bytes\n", uxTaskGetStackHighWaterMark(NULL));
    
    // Configurar endpoints REST API
    setupAPIEndpoints();
    
    // Configurar archivos estáticos
    setupStaticFiles();
    
    // Iniciar servidor
    if (server) {
        server->begin();
        Serial.println("✅ [Core 1] AsyncWebServer iniciado en puerto 80");
        
        // Notificar que servidor está listo
        if (onServerReady) {
            onServerReady();
        }
    }
    
    // Loop principal de la Task (Core 1)
    while (taskRunning) {
        // ✅ CRÍTICO: Resetar watchdog para evitar timeout
        esp_task_wdt_reset();
        
        // El AsyncWebServer maneja las requests automáticamente
        // Solo necesitamos mantener la Task viva
        vTaskDelay(pdMS_TO_TICKS(1000)); // Delay de 1 segundo
        
        // Verificar heap periódicamente
        static unsigned long lastHeapCheck = 0;
        if (millis() - lastHeapCheck > 30000) { // Cada 30 segundos
            Serial.printf("🌐 [Core 1] Heap libre: %d bytes\n", ESP.getFreeHeap());
            lastHeapCheck = millis();
        }
    }
    
    Serial.println("⚠️ [Core 1] WebServerTask finalizando...");
}

bool WebServerTask::begin() {
    if (initialized) {
        Serial.println("⚠️ WebServerTask ya está inicializado");
        return true;
    }
    
    Serial.println("\n🚀 ==========================================");
    Serial.println("🚀 INICIALIZANDO WEBSERVER TASK (CORE 1)");
    Serial.println("🚀 ==========================================");
    
    // Inicializar SPIFFS si no está montado
    if (!SPIFFS.begin(true)) {
        Serial.println("❌ Error al inicializar SPIFFS");
        return false;
    }
    Serial.println("✅ SPIFFS montado");
    
    // Crear AsyncWebServer
    server = new AsyncWebServer(80);
    if (!server) {
        Serial.println("❌ Error al crear AsyncWebServer");
        return false;
    }
    Serial.println("✅ AsyncWebServer creado");
    
    // Crear Task en Core 1
    taskRunning = true;
    BaseType_t result = xTaskCreatePinnedToCore(
        taskFunction,           // Función de la Task
        "WebServerTask",        // Nombre de la Task
        8192,                   // Stack size (8KB)
        this,                   // Parámetro
        1,                      // Prioridad (menor que ESP-NOW)
        &taskHandle,            // Handle de la Task
        1                      // Core 1 (dedicado para Web Server)
    );
    
    if (result != pdPASS) {
        Serial.println("❌ Error al crear WebServerTask");
        delete server;
        server = nullptr;
        return false;
    }
    
    initialized = true;
    Serial.println("✅ WebServerTask creada en Core 1");
    Serial.println("   ✓ Stack: 8KB");
    Serial.println("   ✓ Prioridad: 1");
    Serial.println("   ✓ Core: 1");
    Serial.println("==========================================\n");
    
    // Esperar un poco para que la Task inicie
    delay(500);
    
    return true;
}

void WebServerTask::end() {
    if (!initialized) return;
    
    taskRunning = false;
    
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = nullptr;
    }
    
    if (server) {
        server->end();
        delete server;
        server = nullptr;
    }
    
    initialized = false;
    Serial.println("✅ WebServerTask finalizada");
}

void WebServerTask::setupAPIEndpoints() {
    if (!server) return;
    
    Serial.println("🔧 [Core 1] Configurando endpoints REST API...");
    
    // ===== ENDPOINT: GET /api/status =====
    server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["status"] = "ok";
        doc["uptime"] = millis() / 1000;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["core"] = xPortGetCoreID();
        doc["task"] = "WebServerTask";
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // ===== ENDPOINT: POST /api/command =====
    // Este endpoint será configurado externamente por WebServerManager
    // (No configuramos aquí para permitir que WebServerManager lo haga)
    
    Serial.println("✅ [Core 1] Endpoints REST API configurados");
}

void WebServerTask::setupStaticFiles() {
    if (!server) return;
    
    Serial.println("🔧 [Core 1] Configurando archivos estáticos...");
    
    // Servir archivos estáticos desde SPIFFS
    server->serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
    server->serveStatic("/style.css", SPIFFS, "/style.css");
    server->serveStatic("/script.js", SPIFFS, "/script.js");
    
    Serial.println("✅ [Core 1] Archivos estáticos configurados");
}

void WebServerTask::addEndpoint(const char* uri, WebRequestMethod method, 
                               std::function<void(AsyncWebServerRequest*)> handler) {
    if (!server || !initialized) {
        Serial.printf("❌ [WebServerTask] NO INICIALIZADO - No se puede agregar endpoint: %s\n", uri);
        return;
    }
    
    Serial.printf("🔧 [WebServerTask] Agregando endpoint: %s (método: %d)\n", uri, method);
    Serial.printf("   Server: %s\n", server ? "✅ Disponível" : "❌ nullptr");
    Serial.printf("   Initialized: %s\n", initialized ? "✅ SIM" : "❌ NÃO");
    
    server->on(uri, method, [handler, uri](AsyncWebServerRequest *request) {
        Serial.printf("🎯 [WebServerTask] Handler ejecutado para: %s\n", uri);
        
        // ✅ WRAPPER DE SEGURANÇA: Garantir que sempre retorne resposta
        if (!request) {
            Serial.println("❌ [WebServerTask] Request é nullptr!");
            return;
        }
        
        // ✅ Verificar se handler é válido
        if (!handler) {
            Serial.println("❌ [WebServerTask] Handler é nullptr!");
            request->send(500, "application/json", "{\"error\":\"Handler not configured\"}");
            return;
        }
        
        // ✅ Executar handler com proteção
        handler(request);
        
        // ✅ Verificar se resposta foi enviada (se não, enviar fallback)
        // Nota: AsyncWebServer não permite verificar se já foi enviado,
        // mas o handler deve sempre chamar request->send()
    });
    
    Serial.printf("✅ [Core 1] Endpoint REGISTRADO: %s (método: %d)\n", uri, method);
}

void WebServerTask::addPostEndpoint(const char* uri,
                                    std::function<void(AsyncWebServerRequest*)> onRequest,
                                    std::function<void(AsyncWebServerRequest*, String, size_t, size_t, size_t)> onUpload,
                                    std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)> onBody) {
    if (!server || !initialized) {
        Serial.println("⚠️ WebServerTask no inicializado - no se puede agregar endpoint POST");
        return;
    }
    
    // Configurar endpoint POST con handlers opcionales
    if (onBody) {
        // POST con body handler (puede tener o no onUpload)
        if (onUpload) {
            // POST con upload y body handlers
            server->on(uri, HTTP_POST, 
                [onRequest](AsyncWebServerRequest *request) {
                    if (onRequest) onRequest(request);
                },
                [onUpload](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
                    if (onUpload) onUpload(request, filename, index, len, final ? index + len : 0);
                },
                [onBody](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                    if (onBody) onBody(request, data, len, index, total);
                }
            );
        } else {
            // POST con body handler pero sin upload handler
            server->on(uri, HTTP_POST, 
                [onRequest](AsyncWebServerRequest *request) {
                    if (onRequest) onRequest(request);
                },
                nullptr,  // onUpload = nullptr
                [onBody](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                    if (onBody) onBody(request, data, len, index, total);
                }
            );
        }
    } else {
        // POST simple (solo onRequest, sin body handler)
        server->on(uri, HTTP_POST, 
            [onRequest](AsyncWebServerRequest *request) {
                if (onRequest) onRequest(request);
            }
        );
    }
    
    Serial.printf("✅ [Core 1] Endpoint POST agregado: %s\n", uri);
}

