#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

/**
 * @brief Clase para manejar comunicación ESP-NOW en una tarea de FreeRTOS
 * 
 * Esta clase proporciona una interfaz para enviar comandos de relé
 * y buscar dispositivos esclavos mediante ESP-NOW.
 */
class ESPNowTask {
public:
    ESPNowTask();
    ~ESPNowTask();
    
    /**
     * @brief Inicializa la tarea ESP-NOW
     * @return true si se inicializó correctamente
     */
    bool begin();
    
    /**
     * @brief Finaliza la tarea ESP-NOW
     */
    void end();
    
    /**
     * @brief Busca la dirección MAC de un esclavo por su ID
     * @param device_id ID del dispositivo esclavo
     * @return Puntero a la dirección MAC (6 bytes) o nullptr si no se encuentra
     */
    uint8_t* findSlaveMac(const String& device_id);
    
    /**
     * @brief Envía un comando de relé a un dispositivo
     * @param targetMac Dirección MAC del dispositivo destino
     * @param relayNumber Número del relé (0-15)
     * @param action Acción a ejecutar ("on", "off", "toggle")
     * @param duration_ms Duración en milisegundos (0 = permanente)
     * @return true si el comando se envió correctamente
     */
    bool sendRelayCommand(const uint8_t* targetMac, int relayNumber, const char* action, unsigned long duration_ms);
    
    /**
     * @brief Envía un comando de relé (sobrecarga con String)
     * @param targetMac Dirección MAC del dispositivo destino
     * @param relayNumber Número del relé (0-15)
     * @param action Acción a ejecutar ("on", "off", "toggle")
     * @param duration_ms Duración en milisegundos (0 = permanente)
     * @return true si el comando se envió correctamente
     */
    bool sendRelayCommand(const uint8_t* targetMac, int relayNumber, const String& action, unsigned long duration_ms);
    
    /**
     * @brief Verifica si la tarea está lista
     * @return true si está lista
     */
    bool isReady() const { return ready; }
    
    /**
     * @brief Verifica si la tarea está inicializada (alias de isReady)
     * @return true si está inicializada
     */
    bool isInitialized() const { return ready; }
    
    /**
     * @brief Obtiene el MAC local como string
     * @return String con el MAC en formato XX:XX:XX:XX:XX:XX
     */
    String getLocalMacString();
    
private:
    bool ready;
    // Aquí se pueden agregar más miembros privados según la implementación
};

#endif // ESPNOW_TASK_H

