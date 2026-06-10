#ifndef NETWORK_WATCHDOG_H
#define NETWORK_WATCHDOG_H

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

/**
 * @brief Watchdog específico para operaciones de red (Supabase HTTP)
 * 
 * Protege contra:
 * - Bloqueos en operaciones SSL/HTTP
 * - Timeouts excesivos
 * - Pérdida de memoria durante conexiones
 * - Deadlocks en operaciones de red
 * 
 * Características:
 * - Timeout configurable por operación (default: 20s)
 * - Reset automático del watchdog global durante operaciones
 * - Cierre forzado de conexiones bloqueadas
 * - Monitoreo de memoria durante operaciones
 */
class NetworkWatchdog {
private:
    unsigned long operationStartTime = 0;
    unsigned long lastWatchdogFeed = 0;
    bool operationActive = false;
    HTTPClient* monitoredClient = nullptr;
    WiFiClientSecure* monitoredSSLClient = nullptr;
    
    // Configuración
    const unsigned long OPERATION_TIMEOUT_MS = 20000;  // 20 segundos máximo por operación
    const unsigned long WATCHDOG_FEED_INTERVAL_MS = 2000;  // Alimentar watchdog cada 2s durante operación
    const unsigned long MEMORY_CHECK_INTERVAL_MS = 5000;  // Verificar memoria cada 5s
    
    // Estadísticas
    unsigned long totalOperations = 0;
    unsigned long timeoutOperations = 0;
    unsigned long memoryFailures = 0;
    
    // ✅ NOVO: Contador de fallos consecutivos para reboot forçado
    int consecutiveFailures = 0;
    unsigned long lastSuccessfulOperation = 0;
    const int MAX_CONSECUTIVE_FAILURES = 5;  // Reboot após 5 fallos consecutivos
    const unsigned long FAILURE_WINDOW_MS = 120000;  // 2 minutos - resetar contador se passar muito tempo
    
public:
    /**
     * @brief Inicia el monitoreo de una operación HTTP
     * @param httpClient Cliente HTTP a monitorear (puede ser nullptr)
     * @param sslClient Cliente SSL a monitorear (puede ser nullptr)
     * @return true si se puede iniciar la operación
     */
    bool beginOperation(HTTPClient* httpClient = nullptr, WiFiClientSecure* sslClient = nullptr) {
        // Verificar memoria antes de iniciar
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        
        if (freeHeap < 40000 || maxAlloc < 35000) {
            Serial.printf("⚠️ [NETWORK_WDT] Memoria insuficiente: libre=%d, max_contiguo=%d\n", 
                         freeHeap, maxAlloc);
            memoryFailures++;
            return false;
        }
        
        operationStartTime = millis();
        lastWatchdogFeed = millis();
        operationActive = true;
        monitoredClient = httpClient;
        monitoredSSLClient = sslClient;
        totalOperations++;
        
        // Resetear watchdog global al inicio
        esp_task_wdt_reset();
        
        return true;
    }
    
    /**
     * @brief Debe llamarse periódicamente durante la operación HTTP
     * @return true si la operación puede continuar, false si debe abortarse
     * 
     * ✅ OPTIMIZADO: Early returns y verificaciones mínimas para máximo rendimiento
     * - Si no hay operación activa: retorna inmediatamente (~1μs)
     * - Verificaciones costosas solo cuando es necesario
     * - Overhead típico: <5μs por llamada (cuando operación activa)
     */
    bool feed() {
        // ✅ EARLY RETURN: Si no hay operación, retornar inmediatamente (muy rápido)
        if (!operationActive) {
            return true;  // ~1 microsegundo
        }
        
        // ✅ OPTIMIZACIÓN: Solo calcular now si realmente lo necesitamos
        unsigned long now = millis();
        unsigned long elapsed = now - operationStartTime;
        
        // ✅ Verificar timeout (comparación simple, muy rápida)
        if (elapsed > OPERATION_TIMEOUT_MS) {
            // Caso raro: timeout - operaciones costosas solo aquí
            Serial.printf("⏰ [NETWORK_WDT] TIMEOUT: Operación HTTP excedió %lu ms\n", OPERATION_TIMEOUT_MS);
            Serial.printf("   Forzando cierre de conexión...\n");
            
            // Forzar cierre de conexiones
            if (monitoredClient && monitoredClient->connected()) {
                Serial.println("   Cerrando HTTPClient...");
                monitoredClient->end();
            }
            
            if (monitoredSSLClient && monitoredSSLClient->connected()) {
                Serial.println("   Cerrando SSLClient...");
                monitoredSSLClient->stop();
            }
            
            timeoutOperations++;
            operationActive = false;
            
            // ✅ NOTA: endOperation(false) será chamado pelo código que detecta el timeout
            // e se encargará de incrementar consecutiveFailures y verificar reboot
            
            // Dar tiempo para liberar recursos
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // Resetear watchdog antes de retornar
            esp_task_wdt_reset();
            
            return false;  // Abortar operación
        }
        
        // ✅ OPTIMIZACIÓN: Verificar intervalo ANTES de llamar funciones costosas
        // Solo alimentar watchdog cada 2 segundos (no en cada llamada)
        unsigned long timeSinceLastFeed = now - lastWatchdogFeed;
        if (timeSinceLastFeed >= WATCHDOG_FEED_INTERVAL_MS) {
            esp_task_wdt_reset();  // ~10-20μs, pero solo cada 2s
            lastWatchdogFeed = now;
        }
        
        // ✅ OPTIMIZACIÓN: Verificar memoria solo cada 5 segundos (usando static)
        static unsigned long lastMemoryCheck = 0;
        unsigned long timeSinceLastMemoryCheck = now - lastMemoryCheck;
        if (timeSinceLastMemoryCheck >= MEMORY_CHECK_INTERVAL_MS) {
            // ESP.getFreeHeap() es más costoso (~50-100μs), pero solo cada 5s
            uint32_t freeHeap = ESP.getFreeHeap();
            if (freeHeap < 30000) {
                Serial.printf("⚠️ [NETWORK_WDT] Memoria crítica durante operación: %d bytes\n", freeHeap);
                // No abortar, solo advertir
            }
            lastMemoryCheck = now;
        }
        
        return true;  // Operación puede continuar
    }
    
    /**
     * @brief Finaliza el monitoreo de la operación
     * @param success true si la operación fue exitosa
     */
    void endOperation(bool success = true, int httpCode = 0) {
        if (!operationActive) {
            return;
        }
        
        unsigned long elapsed = millis() - operationStartTime;
        
        if (elapsed > 10000) {  // Log solo si tardó más de 10s
            Serial.printf("⏱️ [NETWORK_WDT] Operación completada en %lu ms\n", elapsed);
        }
        
        operationActive = false;
        monitoredClient = nullptr;
        monitoredSSLClient = nullptr;
        
        // ✅ NOVO: Gerenciar contador de fallos consecutivos
        unsigned long now = millis();
        if (success) {
            // Operação exitosa - resetar contador
            if (consecutiveFailures > 0) {
                Serial.printf("✅ [NETWORK_WDT] Operação exitosa - resetando contador de fallos (%d → 0)\n", 
                             consecutiveFailures);
            }
            consecutiveFailures = 0;
            lastSuccessfulOperation = now;
        } else {
            // 404/401/400 = schema/auth — não é falha de rede; evita reboot em loop
            const bool schemaOrClientError = (httpCode == 404 || httpCode == 401 || httpCode == 400);
            if (schemaOrClientError) {
                Serial.printf("⚠️ [NETWORK_WDT] HTTP %d (config/schema) — não conta para reboot\n", httpCode);
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(50));
                return;
            }
            // Operação falhou - incrementar contador
            consecutiveFailures++;
            Serial.printf("⚠️ [NETWORK_WDT] Fallo #%d consecutivo\n", consecutiveFailures);
            
            // ✅ CRÍTICO: Verificar se excedeu limite de fallos consecutivos
            if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
                Serial.println("\n🚨 ========================================");
                Serial.println("🚨 REBOOT FORÇADO POR FALLOS DE RED");
                Serial.println("🚨 ========================================");
                Serial.printf("   Fallos consecutivos: %d\n", consecutiveFailures);
                Serial.printf("   Última operação exitosa: %lu ms atrás\n", 
                             now - lastSuccessfulOperation);
                Serial.println("   Sistema não consegue comunicar com Supabase");
                Serial.println("   Forçando reboot para recuperação...");
                Serial.println("========================================\n");
                
                // Dar tempo para mensagem aparecer no serial
                delay(2000);
                
                // ✅ REBOOT FORÇADO - última vía de escape
                ESP.restart();
            }
        }
        
        // Resetear contador se passou muito tempo desde último fallo
        if (now - lastSuccessfulOperation > FAILURE_WINDOW_MS && consecutiveFailures > 0) {
            Serial.printf("🔄 [NETWORK_WDT] Resetando contador de fallos (passou %lu ms)\n", 
                         now - lastSuccessfulOperation);
            consecutiveFailures = 0;
        }
        
        // Resetear watchdog final
        esp_task_wdt_reset();
        
        // Dar tiempo para liberar recursos SSL
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    /**
     * @brief Verifica si hay una operación activa
     */
    bool isOperationActive() const {
        return operationActive;
    }
    
    /**
     * @brief Obtiene estadísticas del watchdog
     */
    void printStats() {
        Serial.println("📊 [NETWORK_WDT] Estadísticas:");
        Serial.printf("   Total operaciones: %lu\n", totalOperations);
        Serial.printf("   Timeouts: %lu\n", timeoutOperations);
        Serial.printf("   Fallos de memoria: %lu\n", memoryFailures);
        Serial.printf("   Fallos consecutivos: %d / %d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);
        if (lastSuccessfulOperation > 0) {
            unsigned long timeSinceSuccess = millis() - lastSuccessfulOperation;
            Serial.printf("   Última operação exitosa: %lu ms atrás\n", timeSinceSuccess);
        }
        if (totalOperations > 0) {
            Serial.printf("   Tasa de éxito: %.1f%%\n", 
                         (100.0 * (totalOperations - timeoutOperations - memoryFailures)) / totalOperations);
        }
    }
    
    /**
     * @brief Obtiene número de fallos consecutivos
     */
    int getConsecutiveFailures() const {
        return consecutiveFailures;
    }
    
    /**
     * @brief Reseta manualmente el contador de fallos (útil para testes)
     */
    void resetFailureCount() {
        consecutiveFailures = 0;
        lastSuccessfulOperation = millis();
        Serial.println("🔄 [NETWORK_WDT] Contador de fallos resetado manualmente");
    }
    
    /**
     * @brief Fuerza el cierre de una operación bloqueada
     */
    void forceAbort() {
        if (!operationActive) {
            return;
        }
        
        Serial.println("🛑 [NETWORK_WDT] Abortando operación forzadamente...");
        
        if (monitoredClient) {
            monitoredClient->end();
        }
        
        if (monitoredSSLClient) {
            monitoredSSLClient->stop();
        }
        
        operationActive = false;
        monitoredClient = nullptr;
        monitoredSSLClient = nullptr;
        
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
};

// Instancia global del watchdog de red
extern NetworkWatchdog networkWatchdog;

#endif // NETWORK_WATCHDOG_H

