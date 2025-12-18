/**
 * @file ObjectPoolManager.h
 * @brief Gerenciador central de Object Pools (Singleton)
 * 
 * ✅ GERENCIA: WiFiClientSecure, HTTPClient, DynamicJsonDocument
 * ✅ SINGLETON: Uma única instância global
 * ✅ THREAD-SAFE: Protegido com mutex
 * ✅ EVENT-DRIVEN: Não bloqueia sistema
 */

#ifndef OBJECT_POOL_MANAGER_H
#define OBJECT_POOL_MANAGER_H

#include "SSLClientPool.h"
#include "HTTPClientPool.h"
#include "ObjectPool.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class ObjectPoolManager {
private:
    static ObjectPoolManager* instance;
    static SemaphoreHandle_t instanceMutex;
    
    // ✅ POOLS: Criados uma vez
    WiFiClientSecurePool* sslPool;
    HTTPClientPool* httpPool;
    JsonDocumentPool* jsonPool;
    
    bool initialized;
    
    // ✅ Private constructor (Singleton)
    ObjectPoolManager() : sslPool(nullptr), httpPool(nullptr), 
                          jsonPool(nullptr), initialized(false) {}
    
    // ✅ Proibir cópia
    ObjectPoolManager(const ObjectPoolManager&) = delete;
    ObjectPoolManager& operator=(const ObjectPoolManager&) = delete;
    
public:
    /**
     * @brief Obter instância única (Singleton)
     */
    static ObjectPoolManager* getInstance() {
        if (!instanceMutex) {
            instanceMutex = xSemaphoreCreateMutex();
        }
        
        if (xSemaphoreTake(instanceMutex, portMAX_DELAY) != pdTRUE) {
            return nullptr;
        }
        
        if (!instance) {
            instance = new ObjectPoolManager();
        }
        
        xSemaphoreGive(instanceMutex);
        return instance;
    }
    
    /**
     * @brief Inicializar pools (chamar na ordem procedural)
     * ✅ DEVE SER CHAMADO: Após webServerTask, antes de HydroSystemCore
     */
    static bool initialize() {
        ObjectPoolManager* mgr = getInstance();
        if (!mgr) {
            Serial.println("❌ [PoolManager] Falha ao criar instância");
            return false;
        }
        
        if (mgr->initialized) {
            Serial.println("⚠️ [PoolManager] Já inicializado");
            return true;
        }
        
        // ✅ CRIAR POOLS: Uma vez no início
        Serial.println("🔧 [PoolManager] Inicializando pools...");
        
        mgr->sslPool = new WiFiClientSecurePool();
        mgr->httpPool = new HTTPClientPool();
        mgr->jsonPool = new JsonDocumentPool(4096);
        
        if (!mgr->sslPool || !mgr->httpPool || !mgr->jsonPool) {
            Serial.println("❌ [PoolManager] Falha ao criar pools");
            return false;
        }
        
        mgr->initialized = true;
        Serial.println("✅ [PoolManager] Pools inicializados com sucesso");
        return true;
    }
    
    /**
     * @brief Destruir pools (chamar no final, se necessário)
     */
    static void destroy() {
        if (!instanceMutex) return;
        
        if (xSemaphoreTake(instanceMutex, portMAX_DELAY) != pdTRUE) {
            return;
        }
        
        if (instance) {
            if (instance->sslPool) {
                delete instance->sslPool;
                instance->sslPool = nullptr;
            }
            if (instance->httpPool) {
                delete instance->httpPool;
                instance->httpPool = nullptr;
            }
            if (instance->jsonPool) {
                delete instance->jsonPool;
                instance->jsonPool = nullptr;
            }
            
            delete instance;
            instance = nullptr;
        }
        
        xSemaphoreGive(instanceMutex);
        vSemaphoreDelete(instanceMutex);
        instanceMutex = nullptr;
        
        Serial.println("✅ [PoolManager] Pools destruídos");
    }
    
    /**
     * @brief Verificar se está inicializado
     */
    bool isInitialized() const {
        return initialized;
    }
    
    // ===== SSL CLIENT POOL =====
    
    /**
     * @brief Adquirir cliente SSL do pool
     * @return Ponteiro para cliente ou nullptr se não disponível
     */
    WiFiClientSecure* acquireSSLClient() {
        if (!initialized || !sslPool) {
            Serial.println("⚠️ [PoolManager] SSL Pool não inicializado");
            return nullptr;
        }
        return sslPool->acquire();
    }
    
    /**
     * @brief Liberar cliente SSL de volta ao pool
     */
    void releaseSSLClient(WiFiClientSecure* client) {
        if (!initialized || !sslPool) return;
        sslPool->release(client);
    }
    
    // ===== HTTP CLIENT POOL =====
    
    /**
     * @brief Adquirir cliente HTTP do pool
     * @param ssl Cliente SSL opcional (para configurar)
     * @return Ponteiro para cliente ou nullptr se não disponível
     */
    HTTPClient* acquireHTTPClient(WiFiClientSecure* ssl = nullptr) {
        if (!initialized || !httpPool) {
            Serial.println("⚠️ [PoolManager] HTTP Pool não inicializado");
            return nullptr;
        }
        return httpPool->acquire(ssl);
    }
    
    /**
     * @brief Liberar cliente HTTP de volta ao pool
     */
    void releaseHTTPClient(HTTPClient* client) {
        if (!initialized || !httpPool) return;
        httpPool->release(client);
    }
    
    // ===== JSON DOCUMENT POOL =====
    
    /**
     * @brief Adquirir documento JSON do pool
     * @param size Tamanho do documento (padrão: 4096)
     * @return Ponteiro para documento ou nullptr se não disponível
     */
    DynamicJsonDocument* acquireJsonDocument(size_t size = 4096) {
        if (!initialized || !jsonPool) {
            Serial.println("⚠️ [PoolManager] JSON Pool não inicializado");
            return nullptr;
        }
        // ✅ NOTA: Por enquanto, tamanho fixo (4096)
        // TODO: Implementar pools com tamanhos diferentes se necessário
        return jsonPool->acquire();
    }
    
    /**
     * @brief Liberar documento JSON de volta ao pool
     */
    void releaseJsonDocument(DynamicJsonDocument* doc) {
        if (!initialized || !jsonPool) return;
        jsonPool->release(doc);
    }
    
    // ===== ESTATÍSTICAS =====
    
    /**
     * @brief Obter estatísticas dos pools
     */
    void printStats() {
        if (!initialized) {
            Serial.println("⚠️ [PoolManager] Não inicializado");
            return;
        }
        
        Serial.println("📊 [PoolManager] Estatísticas dos Pools:");
        if (sslPool) {
            Serial.printf("   SSL Pool: %d disponíveis\n", sslPool->getAvailableCount());
        }
        if (httpPool) {
            Serial.printf("   HTTP Pool: %d disponíveis\n", httpPool->getAvailableCount());
        }
        if (jsonPool) {
            Serial.printf("   JSON Pool: %d disponíveis\n", jsonPool->getAvailableCount());
        }
    }
};

// ✅ Static members - Definidos en ObjectPoolManager.cpp para evitar múltiples definiciones

#endif // OBJECT_POOL_MANAGER_H

