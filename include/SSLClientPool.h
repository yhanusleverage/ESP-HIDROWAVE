/**
 * @file SSLClientPool.h
 * @brief Pool de WiFiClientSecure para reutilização SSL
 * 
 * ✅ CRÍTICO: SSL precisa de blocos grandes contíguos (35KB+)
 * ✅ Reutilizar conexões SSL evita fragmentação
 */

#ifndef SSL_CLIENT_POOL_H
#define SSL_CLIENT_POOL_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class WiFiClientSecurePool {
private:
    static const int POOL_SIZE = 2;  // ✅ 2 clientes SSL (reutilizar conexões)
    WiFiClientSecure* pool[POOL_SIZE];
    bool inUse[POOL_SIZE];
    SemaphoreHandle_t mutex;  // ✅ Thread-safe
    
public:
    /**
     * @brief Construtor - cria pool de clientes SSL
     */
    WiFiClientSecurePool() {
        mutex = xSemaphoreCreateMutex();
        if (!mutex) {
            Serial.println("❌ [SSLPool] Falha ao criar mutex!");
            return;
        }
        
        // ✅ CRIAR UMA VEZ: Alocar todos os clientes no início
        for (int i = 0; i < POOL_SIZE; i++) {
            pool[i] = new WiFiClientSecure();
            if (pool[i]) {
                pool[i]->setInsecure();  // ✅ Configurar uma vez
                inUse[i] = false;
            }
        }
        Serial.printf("✅ [SSLPool] Pool criado: %d clientes SSL\n", POOL_SIZE);
    }
    
    /**
     * @brief Destrutor - libera todos os clientes
     */
    ~WiFiClientSecurePool() {
        // ✅ DESTRUIR APENAS NO FINAL
        if (mutex) {
            xSemaphoreTake(mutex, portMAX_DELAY);
        }
        
        for (int i = 0; i < POOL_SIZE; i++) {
            if (pool[i]) {
                pool[i]->stop();  // ✅ Fechar conexão antes de destruir
                delete pool[i];
                pool[i] = nullptr;
            }
        }
        
        if (mutex) {
            xSemaphoreGive(mutex);
            vSemaphoreDelete(mutex);
        }
        Serial.println("✅ [SSLPool] Pool destruído");
    }
    
    /**
     * @brief Obter cliente SSL disponível do pool
     * @return Ponteiro para cliente ou nullptr se todos estão em uso
     */
    WiFiClientSecure* acquire() {
        if (!mutex) return nullptr;
        
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("⚠️ [SSLPool] Timeout ao adquirir mutex");
            return nullptr;
        }
        
        // ✅ REUTILIZAR: Procurar cliente disponível
        WiFiClientSecure* client = nullptr;
        for (int i = 0; i < POOL_SIZE; i++) {
            if (!inUse[i] && pool[i]) {
                inUse[i] = true;
                client = pool[i];
                
                // ✅ LIMPAR: Fechar conexão anterior se houver
                if (client->connected()) {
                    client->stop();
                }
                
                break;
            }
        }
        
        xSemaphoreGive(mutex);
        
        if (!client) {
            Serial.println("⚠️ [SSLPool] Todos os clientes SSL estão em uso!");
        }
        
        return client;
    }
    
    /**
     * @brief Liberar cliente SSL de volta ao pool
     * @param client Cliente a ser liberado
     */
    void release(WiFiClientSecure* client) {
        if (!client || !mutex) return;
        
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("⚠️ [SSLPool] Timeout ao liberar mutex");
            return;
        }
        
        // ✅ DEVOLVER AO POOL: Marcar como disponível
        for (int i = 0; i < POOL_SIZE; i++) {
            if (pool[i] == client) {
                inUse[i] = false;
                
                // ✅ LIMPAR: Fechar conexão para reutilização
                if (client->connected()) {
                    client->stop();
                }
                
                xSemaphoreGive(mutex);
                return;
            }
        }
        
        xSemaphoreGive(mutex);
        Serial.println("⚠️ [SSLPool] Tentativa de liberar cliente que não pertence ao pool!");
    }
    
    /**
     * @brief Verificar quantos clientes estão disponíveis
     */
    int getAvailableCount() {
        if (!mutex) return 0;
        
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return 0;
        }
        
        int count = 0;
        for (int i = 0; i < POOL_SIZE; i++) {
            if (!inUse[i]) count++;
        }
        
        xSemaphoreGive(mutex);
        return count;
    }
    
    /**
     * @brief Forçar liberação de todos os clientes (emergência)
     */
    void forceReleaseAll() {
        if (!mutex) return;
        
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            return;
        }
        
        for (int i = 0; i < POOL_SIZE; i++) {
            inUse[i] = false;
            if (pool[i] && pool[i]->connected()) {
                pool[i]->stop();
            }
        }
        
        xSemaphoreGive(mutex);
        Serial.println("⚠️ [SSLPool] Todos os clientes foram forçados a liberar");
    }
};

#endif // SSL_CLIENT_POOL_H

