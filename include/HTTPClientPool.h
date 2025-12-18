/**
 * @file HTTPClientPool.h
 * @brief Pool de HTTPClient para reutilização
 * 
 * ✅ Reutilizar clientes HTTP evita alocações constantes
 */

#ifndef HTTP_CLIENT_POOL_H
#define HTTP_CLIENT_POOL_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class HTTPClientPool {
private:
    static const int POOL_SIZE = 3;  // ✅ 3 clientes HTTP
    HTTPClient* pool[POOL_SIZE];
    bool inUse[POOL_SIZE];
    SemaphoreHandle_t mutex;
    
public:
    /**
     * @brief Construtor - cria pool de clientes HTTP
     */
    HTTPClientPool() {
        mutex = xSemaphoreCreateMutex();
        if (!mutex) {
            Serial.println("❌ [HTTPPool] Falha ao criar mutex!");
            return;
        }
        
        // ✅ CRIAR UMA VEZ: Alocar todos os clientes no início
        for (int i = 0; i < POOL_SIZE; i++) {
            pool[i] = new HTTPClient();
            if (pool[i]) {
                inUse[i] = false;
            }
        }
        Serial.printf("✅ [HTTPPool] Pool criado: %d clientes HTTP\n", POOL_SIZE);
    }
    
    /**
     * @brief Destrutor - libera todos os clientes
     */
    ~HTTPClientPool() {
        // ✅ DESTRUIR APENAS NO FINAL
        if (mutex) {
            xSemaphoreTake(mutex, portMAX_DELAY);
        }
        
        for (int i = 0; i < POOL_SIZE; i++) {
            if (pool[i]) {
                pool[i]->end();  // ✅ Fechar conexão antes de destruir
                delete pool[i];
                pool[i] = nullptr;
            }
        }
        
        if (mutex) {
            xSemaphoreGive(mutex);
            vSemaphoreDelete(mutex);
        }
        Serial.println("✅ [HTTPPool] Pool destruído");
    }
    
    /**
     * @brief Obter cliente HTTP disponível do pool
     * @param ssl Cliente SSL opcional (se fornecido, configura o HTTPClient)
     * @return Ponteiro para cliente ou nullptr se todos estão em uso
     */
    HTTPClient* acquire(WiFiClientSecure* ssl = nullptr) {
        if (!mutex) return nullptr;
        
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("⚠️ [HTTPPool] Timeout ao adquirir mutex");
            return nullptr;
        }
        
        // ✅ REUTILIZAR: Procurar cliente disponível
        HTTPClient* client = nullptr;
        for (int i = 0; i < POOL_SIZE; i++) {
            if (!inUse[i] && pool[i]) {
                inUse[i] = true;
                client = pool[i];
                
                // ✅ LIMPAR: Fechar conexão anterior se houver
                if (client->connected()) {
                    client->end();
                }
                
                // ✅ CONFIGURAR: Se SSL fornecido, já configurar
                // (mas não iniciar conexão ainda - isso será feito pelo caller)
                
                break;
            }
        }
        
        xSemaphoreGive(mutex);
        
        if (!client) {
            Serial.println("⚠️ [HTTPPool] Todos os clientes HTTP estão em uso!");
        }
        
        return client;
    }
    
    /**
     * @brief Liberar cliente HTTP de volta ao pool
     * @param client Cliente a ser liberado
     */
    void release(HTTPClient* client) {
        if (!client || !mutex) return;
        
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("⚠️ [HTTPPool] Timeout ao liberar mutex");
            return;
        }
        
        // ✅ DEVOLVER AO POOL: Marcar como disponível
        for (int i = 0; i < POOL_SIZE; i++) {
            if (pool[i] == client) {
                inUse[i] = false;
                
                // ✅ LIMPAR: Fechar conexão para reutilização
                if (client->connected()) {
                    client->end();
                }
                
                xSemaphoreGive(mutex);
                return;
            }
        }
        
        xSemaphoreGive(mutex);
        Serial.println("⚠️ [HTTPPool] Tentativa de liberar cliente que não pertence ao pool!");
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
                pool[i]->end();
            }
        }
        
        xSemaphoreGive(mutex);
        Serial.println("⚠️ [HTTPPool] Todos os clientes foram forçados a liberar");
    }
};

#endif // HTTP_CLIENT_POOL_H

