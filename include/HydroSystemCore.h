#ifndef HYDRO_SYSTEM_CORE_H
#define HYDRO_SYSTEM_CORE_H

#include <Arduino.h>
#include <WiFi.h>
#include "HydroControl.h"
#include "SupabaseClient.h"
#include "HydroSupaManager.h"
#include "RelayCommandBox.h"
#include "Config.h"

class HydroSystemCore {
private:
    HydroControl hydroControl;
    RelayCommandBox relayController;  // ✅ Controlador de relés (8 relés)
    SupabaseClient supabase;
    HydroSupaManager hybridSupabase;  // ✅ Manager híbrido
    
    // Estados do sistema
    bool systemReady;
    bool supabaseConnected;
    unsigned long startTime;
    
    // Controle de timing para operações
    unsigned long lastSensorSend;
    unsigned long lastStatusSend;
    unsigned long lastStatusPrint;
    unsigned long lastSupabaseCheck;
    unsigned long lastMemoryProtection;
    
    // Intervalos otimizados
    static const unsigned long SENSOR_SEND_INTERVAL = 30000;      // 30s
    static const unsigned long STATUS_SEND_INTERVAL = 60000;      // 1 min
    static const unsigned long STATUS_PRINT_INTERVAL = 30000;     // 30s
    static const unsigned long SUPABASE_CHECK_INTERVAL = 30000;   // 30s
    static const unsigned long MEMORY_CHECK_INTERVAL = 10000;     // 10s
    
    // Proteção de memória específica para HTTPS
    static const uint32_t MIN_HEAP_FOR_HTTPS = 30000;  // 30KB mínimo para SSL
    
public:
    HydroSystemCore();
    ~HydroSystemCore();
    
    // Controle do sistema
    bool begin();
    void loop();
    void end();
    
    // Status
    bool isReady() const { return systemReady; }
    bool isSupabaseConnected() const { return supabaseConnected; }
    unsigned long getUptime() const { return millis() - startTime; }
    
    // Acesso aos módulos (para comandos seriais)
    HydroControl& getHydroControl() { return hydroControl; }
    SupabaseClient& getSupabase() { return supabase; }
    
    // Debug e comandos
    void printSystemStatus();
    void printSensorReadings();
    void testSupabaseConnection();
    
private:
    // Operações principais
    void checkSupabaseCommands();
    void processRelayCommand(const RelayCommand& cmd);
    void sendSensorDataToSupabase();
    void sendDeviceStatusToSupabase();
    void performMemoryProtection();
    
    // Utilities
    bool hasEnoughMemoryForHTTPS();
    void printPeriodicStatus();
};

#endif // HYDRO_SYSTEM_CORE_H

