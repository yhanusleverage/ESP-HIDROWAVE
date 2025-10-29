#ifndef RELAY_BRIDGE_H
#define RELAY_BRIDGE_H

#include <Arduino.h>
#include "SupabaseClient.h"    // RelayCommand (Supabase)
#include "ESPNowTypes.h"        // ESPNowRelayCommand (ESP-NOW)
#include "ESPNowTask.h"         // Para enviar comandos via ESP-NOW

/**
 * @brief Capa de traducción entre Supabase y ESP-NOW
 * 
 * Esta clase actúa como puente (bridge) entre:
 * - Supabase (RelayCommand) - Comandos del dashboard web
 * - ESP-NOW (ESPNowRelayCommand) - Comunicación con slaves
 * 
 * Responsabilidades:
 * 1. Polling de comandos pendientes en Supabase
 * 2. Conversión de formato Supabase → ESP-NOW
 * 3. Envío de comandos via ESP-NOW a slaves
 * 4. Actualización de estados en Supabase
 * 5. Sincronización de estados de relés
 */
class RelayBridge {
public:
    /**
     * @brief Constructor
     * @param supabase Puntero al cliente Supabase
     * @param espnowTask Puntero a la task ESP-NOW
     */
    RelayBridge(SupabaseClient* supabase, ESPNowTask* espnowTask);
    
    /**
     * @brief Inicializar el bridge
     * @return true si inicialización exitosa
     */
    bool begin();
    
    /**
     * @brief Actualizar - llamar en loop()
     * Procesa comandos pendientes de Supabase
     */
    void update();
    
    /**
     * @brief Procesar comandos pendientes del Supabase
     * Lee comandos con status "pending" y los procesa
     */
    void processSupabaseCommands();
    
    /**
     * @brief Enviar comando a slave específico
     * @param supaCmd Comando de Supabase
     * @param slaveMac MAC del slave destino
     * @return true si comando enviado exitosamente
     */
    bool sendCommandToSlave(const RelayCommand& supaCmd, const uint8_t* slaveMac);
    
    /**
     * @brief Enviar comando a slave por nombre
     * @param supaCmd Comando de Supabase
     * @param slaveName Nombre del slave
     * @return true si comando enviado exitosamente
     */
    bool sendCommandToSlaveByName(const RelayCommand& supaCmd, const String& slaveName);
    
    /**
     * @brief Actualizar estado del comando en Supabase
     * @param commandId ID del comando
     * @param status Nuevo estado ("sent", "completed", "failed")
     * @return true si actualización exitosa
     */
    bool updateCommandStatus(int commandId, const String& status);
    
    /**
     * @brief Marcar comando como enviado
     * @param commandId ID del comando
     * @return true si actualización exitosa
     */
    bool markCommandSent(int commandId);
    
    /**
     * @brief Marcar comando como completado
     * @param commandId ID del comando
     * @return true si actualización exitosa
     */
    bool markCommandCompleted(int commandId);
    
    /**
     * @brief Marcar comando como fallido
     * @param commandId ID del comando
     * @param errorMessage Mensaje de error
     * @return true si actualización exitosa
     */
    bool markCommandFailed(int commandId, const String& errorMessage);
    
    /**
     * @brief Habilitar/deshabilitar procesamiento automático
     * @param enabled true para habilitar
     */
    void setAutoProcessing(bool enabled);
    
    /**
     * @brief Verificar si está habilitado
     * @return true si está habilitado
     */
    bool isEnabled() const { return enabled; }
    
    /**
     * @brief Obtener estadísticas
     * @return String JSON con estadísticas
     */
    String getStatsJSON();
    
    /**
     * @brief Imprimir estadísticas
     */
    void printStats();

private:
    // Punteros a componentes
    SupabaseClient* supabase;
    ESPNowTask* espnowTask;
    
    // Estado
    bool enabled;
    unsigned long lastCheck;
    unsigned long checkInterval;
    
    // Estadísticas
    uint32_t commandsProcessed;
    uint32_t commandsSent;
    uint32_t commandsFailed;
    uint32_t commandsCompleted;
    
    /**
     * @brief Convertir RelayCommand (Supabase) a ESPNowRelayCommand (ESP-NOW)
     * @param supaCmd Comando de Supabase
     * @return Comando ESP-NOW
     */
    ESPNowRelayCommand toESPNowCommand(const RelayCommand& supaCmd);
    
    /**
     * @brief Calcular checksum para comando ESP-NOW
     * @param cmd Puntero al comando
     * @return Checksum calculado
     */
    uint8_t calculateChecksum(const ESPNowRelayCommand* cmd);
    
    /**
     * @brief Validar comando de Supabase
     * @param supaCmd Comando a validar
     * @return true si comando es válido
     */
    bool validateSupabaseCommand(const RelayCommand& supaCmd);
    
    /**
     * @brief Log de comando procesado
     * @param supaCmd Comando procesado
     * @param success true si exitoso
     */
    void logCommand(const RelayCommand& supaCmd, bool success);
};

#endif // RELAY_BRIDGE_H

