#ifndef WEB_SERVER_TASK_H
#define WEB_SERVER_TASK_H

#include <Arduino.h>
#include <WiFi.h>  // ✅ Necesario antes de ESPAsyncWebServer
#include <ESPAsyncWebServer.h>  // ✅ Define HTTPMethod y AsyncWebServer
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include <functional>
#include <freertos/FreeRTOS.h>  // ✅ Para FreeRTOS Task
#include <freertos/task.h>      // ✅ Para TaskHandle_t

// ✅ HTTPMethod debería estar definido en ESPAsyncWebServer.h
// ❌ REMOVIDO: HTTPMethod (HTTP_GET, HTTP_POST, etc.) ya está definido en ESPAsyncWebServer.h
// como WebRequestMethod. No necesitamos redefinirlo aquí.
// ESPAsyncWebServer.h ya proporciona: HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, etc.

/**
 * @brief Task dedicada para AsyncWebServer y REST API en Core 1
 * 
 * Esta Task maneja:
 * - AsyncWebServer (puerto 80)
 * - REST API endpoints (GET, POST)
 * - WebSocket connections
 * - Servicio de archivos estáticos
 * 
 * Core: 1 (dedicado para Web Server)
 */
class WebServerTask {
private:
    AsyncWebServer* server;
    bool initialized;
    bool taskRunning;
    TaskHandle_t taskHandle;
    
    // Callbacks para integración con sistema principal
    std::function<void()> onServerReady;
    
    // Función estática para la Task
    static void taskFunction(void* parameter);
    
    // Función principal de la Task
    void run();
    
    // Configurar endpoints REST API
    void setupAPIEndpoints();
    
    // Configurar archivos estáticos
    void setupStaticFiles();
    
public:
    WebServerTask();
    ~WebServerTask();
    
    /**
     * @brief Inicializar WebServerTask en Core 1
     * @return true si inicialización fue exitosa
     */
    bool begin();
    
    /**
     * @brief Detener WebServerTask
     */
    void end();
    
    /**
     * @brief Verificar si está inicializado
     */
    bool isInitialized() const { return initialized; }
    
    /**
     * @brief Obtener instancia del servidor (para agregar endpoints externos)
     */
    AsyncWebServer* getServer() { return server; }
    
    /**
     * @brief Configurar callback cuando servidor esté listo
     */
    void setOnServerReadyCallback(std::function<void()> callback) {
        onServerReady = callback;
    }
    
    /**
     * @brief Agregar endpoint REST API personalizado (GET)
     * @param uri URI del endpoint
     * @param method Método HTTP (HTTP_GET, HTTP_POST, etc.)
     * @param handler Función handler
     */
    void addEndpoint(const char* uri, WebRequestMethod method, 
                    std::function<void(AsyncWebServerRequest*)> handler);
    
    /**
     * @brief Agregar endpoint REST API con POST body handler
     * @param uri URI del endpoint
     * @param onRequest Handler para request
     * @param onUpload Handler para upload (opcional)
     * @param onBody Handler para body POST (opcional)
     */
    void addPostEndpoint(const char* uri,
                        std::function<void(AsyncWebServerRequest*)> onRequest,
                        std::function<void(AsyncWebServerRequest*, String, size_t, size_t, size_t)> onUpload = nullptr,
                        std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)> onBody = nullptr);
};

#endif // WEB_SERVER_TASK_H

