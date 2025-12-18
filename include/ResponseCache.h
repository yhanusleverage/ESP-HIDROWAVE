#ifndef RESPONSE_CACHE_H
#define RESPONSE_CACHE_H

#include <Arduino.h>

/**
 * @brief Cache simples para respostas HTTP frequentes
 * Reduz chamadas ao Supabase para dados que não mudam frequentemente
 */
class ResponseCache {
private:
    struct CacheEntry {
        String key;
        String value;
        unsigned long timestamp;
        unsigned long ttl; // Time to live em ms
        bool valid;
    };
    
    static const int MAX_CACHE_ENTRIES = 5;
    CacheEntry entries[MAX_CACHE_ENTRIES];
    int entryCount;
    
public:
    ResponseCache() : entryCount(0) {
        for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
            entries[i].valid = false;
        }
    }
    
    /**
     * @brief Obter valor do cache
     * @param key Chave do cache
     * @param value String para armazenar o valor (se encontrado)
     * @return true se encontrado e válido, false caso contrário
     */
    bool get(const String& key, String& value) {
        unsigned long now = millis();
        
        for (int i = 0; i < entryCount; i++) {
            if (entries[i].valid && entries[i].key == key) {
                // Verificar se expirou
                if (now - entries[i].timestamp < entries[i].ttl) {
                    value = entries[i].value;
                    return true; // Cache hit
                } else {
                    // Expirou, invalidar
                    entries[i].valid = false;
                }
            }
        }
        
        return false; // Cache miss
    }
    
    /**
     * @brief Armazenar valor no cache
     * @param key Chave do cache
     * @param value Valor a armazenar
     * @param ttl Time to live em milissegundos (padrão: 30s)
     */
    void put(const String& key, const String& value, unsigned long ttl = 30000) {
        unsigned long now = millis();
        
        // Procurar entrada existente
        for (int i = 0; i < entryCount; i++) {
            if (entries[i].key == key) {
                entries[i].value = value;
                entries[i].timestamp = now;
                entries[i].ttl = ttl;
                entries[i].valid = true;
                return;
            }
        }
        
        // Procurar slot vazio
        for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
            if (!entries[i].valid) {
                entries[i].key = key;
                entries[i].value = value;
                entries[i].timestamp = now;
                entries[i].ttl = ttl;
                entries[i].valid = true;
                if (i >= entryCount) {
                    entryCount = i + 1;
                }
                return;
            }
        }
        
        // Cache cheio, substituir entrada mais antiga (FIFO simples)
        if (entryCount > 0) {
            entries[0].key = key;
            entries[0].value = value;
            entries[0].timestamp = now;
            entries[0].ttl = ttl;
            entries[0].valid = true;
        }
    }
    
    /**
     * @brief Invalidar entrada do cache
     * @param key Chave a invalidar
     */
    void invalidate(const String& key) {
        for (int i = 0; i < entryCount; i++) {
            if (entries[i].key == key) {
                entries[i].valid = false;
                return;
            }
        }
    }
    
    /**
     * @brief Limpar todo o cache
     */
    void clear() {
        for (int i = 0; i < entryCount; i++) {
            entries[i].valid = false;
        }
        entryCount = 0;
    }
    
    /**
     * @brief Obter estatísticas do cache
     * @param hits Referência para armazenar número de hits
     * @param misses Referência para armazenar número de misses
     */
    void getStats(int& hits, int& misses) {
        // Implementação futura se necessário
        hits = 0;
        misses = 0;
    }
};

#endif // RESPONSE_CACHE_H

