/**
 * @file ObjectPool.h
 * @brief Object Pool Pattern para ESP32 - Reutilização de objetos
 * 
 * ✅ CONCEITO: Criar uma vez, reutilizar muitas vezes
 * 
 * PROBLEMA:
 * - Criar/destruir objetos constantemente causa fragmentação de heap
 * - ESP32 tem memória limitada (poucos KB de RAM)
 * - Alocação/desalocação frequente = fragmentação = crash
 * 
 * SOLUÇÃO:
 * - Criar objetos uma vez no início
 * - Reutilizar os mesmos objetos
 * - Limpar/resetar ao invés de destruir
 * - Destruir apenas no final (ou nunca)
 * 
 * BENEFÍCIOS:
 * - Menos fragmentação de heap
 * - Mais memória disponível
 * - Sistema mais estável
 * - Melhor performance
 */

#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

/**
 * @brief Pool de DynamicJsonDocument para reutilização
 * 
 * ✅ Cria documentos uma vez e reutiliza
 * ✅ Evita alocação/desalocação constante
 */
class JsonDocumentPool {
private:
    static const int POOL_SIZE = 3;  // ✅ 3 documentos no pool
    DynamicJsonDocument* pool[POOL_SIZE];
    bool inUse[POOL_SIZE];
    size_t documentSize;
    
public:
    /**
     * @brief Construtor - cria pool de documentos
     * @param size Tamanho de cada documento (bytes)
     */
    JsonDocumentPool(size_t size = 4096) : documentSize(size) {
        // ✅ CRIAR UMA VEZ: Alocar todos os documentos no início
        for (int i = 0; i < POOL_SIZE; i++) {
            pool[i] = new DynamicJsonDocument(documentSize);
            inUse[i] = false;
        }
        Serial.printf("✅ [JsonPool] Pool criado: %d documentos de %d bytes cada\n", 
                     POOL_SIZE, documentSize);
    }
    
    /**
     * @brief Destrutor - libera todos os documentos
     */
    ~JsonDocumentPool() {
        // ✅ DESTRUIR APENAS NO FINAL: Liberar tudo de uma vez
        for (int i = 0; i < POOL_SIZE; i++) {
            if (pool[i]) {
                delete pool[i];
                pool[i] = nullptr;
            }
        }
        Serial.println("✅ [JsonPool] Pool destruído");
    }
    
    /**
     * @brief Obter documento disponível do pool
     * @return Ponteiro para documento ou nullptr se todos estão em uso
     */
    DynamicJsonDocument* acquire() {
        // ✅ REUTILIZAR: Procurar documento disponível
        for (int i = 0; i < POOL_SIZE; i++) {
            if (!inUse[i] && pool[i]) {
                inUse[i] = true;
                pool[i]->clear();  // ✅ LIMPAR ao invés de destruir
                return pool[i];
            }
        }
        
        // ✅ Nenhum disponível - retornar nullptr
        Serial.println("⚠️ [JsonPool] Todos os documentos estão em uso!");
        return nullptr;
    }
    
    /**
     * @brief Liberar documento de volta ao pool
     * @param doc Documento a ser liberado
     */
    void release(DynamicJsonDocument* doc) {
        if (!doc) return;
        
        // ✅ DEVOLVER AO POOL: Marcar como disponível
        for (int i = 0; i < POOL_SIZE; i++) {
            if (pool[i] == doc) {
                inUse[i] = false;
                pool[i]->clear();  // ✅ LIMPAR para próxima uso
                return;
            }
        }
        
        Serial.println("⚠️ [JsonPool] Tentativa de liberar documento que não pertence ao pool!");
    }
    
    /**
     * @brief Verificar quantos documentos estão disponíveis
     */
    int getAvailableCount() {
        int count = 0;
        for (int i = 0; i < POOL_SIZE; i++) {
            if (!inUse[i]) count++;
        }
        return count;
    }
    
    /**
     * @brief Forçar liberação de todos os documentos (emergência)
     */
    void forceReleaseAll() {
        for (int i = 0; i < POOL_SIZE; i++) {
            inUse[i] = false;
            if (pool[i]) pool[i]->clear();
        }
        Serial.println("⚠️ [JsonPool] Todos os documentos foram forçados a liberar");
    }
};

/**
 * @brief RAII Wrapper para uso automático do pool
 * 
 * ✅ Adquire documento no construtor
 * ✅ Libera automaticamente no destrutor
 * ✅ Garante que documento sempre é devolvido ao pool
 */
class JsonDocumentGuard {
private:
    JsonDocumentPool* pool;
    DynamicJsonDocument* doc;
    
public:
    /**
     * @brief Construtor - adquire documento do pool
     */
    JsonDocumentGuard(JsonDocumentPool* p) : pool(p), doc(nullptr) {
        if (pool) {
            doc = pool->acquire();
        }
    }
    
    /**
     * @brief Destrutor - libera documento de volta ao pool
     */
    ~JsonDocumentGuard() {
        if (pool && doc) {
            pool->release(doc);
        }
    }
    
    /**
     * @brief Obter documento (nullptr se não disponível)
     */
    DynamicJsonDocument* get() {
        return doc;
    }
    
    /**
     * @brief Verificar se documento está disponível
     */
    bool isValid() {
        return doc != nullptr;
    }
    
    // ✅ Proibir cópia (evitar problemas)
    JsonDocumentGuard(const JsonDocumentGuard&) = delete;
    JsonDocumentGuard& operator=(const JsonDocumentGuard&) = delete;
};

#endif // OBJECT_POOL_H

