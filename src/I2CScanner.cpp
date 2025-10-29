#include "I2CScanner.h"

// Incluir configurações se disponível
#ifndef CONFIG_H
    #include "Config.h"
#endif

// Definir macros de debug se não estiverem definidas
#ifndef DEBUG_PRINTLN
    #define DEBUG_PRINTLN(x) Serial.println(x)
#endif

#ifndef DEBUG_PRINTF
    #define DEBUG_PRINTF(x, ...) Serial.printf(x, __VA_ARGS__)
#endif

// Inicializar variável estática
std::vector<I2CScanner::I2CDevice> I2CScanner::lastScanResults;

void I2CScanner::begin(int sda_pin, int scl_pin, uint32_t frequency) {
    DEBUG_PRINTLN("🔍 Inicializando I2C Scanner...");
    DEBUG_PRINTF("📍 SDA: GPIO%d | SCL: GPIO%d | Freq: %dHz\n", sda_pin, scl_pin, frequency);
    
    Wire.begin(sda_pin, scl_pin);
    Wire.setClock(frequency);
    
    delay(100); // Aguardar estabilização
    DEBUG_PRINTLN("✅ I2C Scanner inicializado");
}

std::vector<I2CScanner::I2CDevice> I2CScanner::scanAll() {
    Serial.println("\n🔍 === ESCANEANDO DISPOSITIVOS I2C ===");
    
    lastScanResults.clear();
    int devicesFound = 0;
    
    for (uint8_t address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        uint8_t error = Wire.endTransmission();
        
        if (error == 0) {
            I2CDevice device;
            device.address = address;
            device.deviceType = getDeviceType(address);
            device.responding = true;
            
            lastScanResults.push_back(device);
            devicesFound++;
            
            Serial.printf("✅ Dispositivo encontrado: 0x%02X (%s)\n", 
                         address, device.deviceType.c_str());
        }
        
        delay(10); // Pequeno delay entre tentativas
    }
    
    if (devicesFound == 0) {
        Serial.println("❌ Nenhum dispositivo I2C encontrado!");
        Serial.println("🔧 Verifique:");
        Serial.println("   - Conexões SDA/SCL");
        Serial.println("   - Alimentação dos dispositivos");
        Serial.println("   - Resistores pull-up (4.7kΩ)");
    } else {
        Serial.printf("✅ Total de dispositivos encontrados: %d\n", devicesFound);
    }
    
    Serial.println("=====================================\n");
    return lastScanResults;
}

uint8_t I2CScanner::findPCF8574() {
    Serial.println("🔍 Procurando PCF8574...");
    
    // Endereços possíveis do PCF8574: 0x20 a 0x27
    for (uint8_t addr = 0x20; addr <= 0x27; addr++) {
        if (testAddress(addr)) {
            Serial.printf("✅ PCF8574 encontrado no endereço: 0x%02X\n", addr);
            return addr;
        }
    }
    
    Serial.println("❌ PCF8574 não encontrado!");
    Serial.println("🔧 Verifique:");
    Serial.println("   - Conexões A0, A1, A2 do PCF8574");
    Serial.println("   - Alimentação (VCC/GND)");
    Serial.println("   - Conexões I2C (SDA/SCL)");
    
    return 0; // Não encontrado
}

std::vector<uint8_t> I2CScanner::findAllPCF8574() {
    Serial.println("🔍 Procurando todos os PCF8574...");
    
    std::vector<uint8_t> pcfAddresses;
    
    for (uint8_t addr = 0x20; addr <= 0x27; addr++) {
        if (testAddress(addr)) {
            pcfAddresses.push_back(addr);
            Serial.printf("✅ PCF8574 encontrado: 0x%02X\n", addr);
        }
    }
    
    if (pcfAddresses.empty()) {
        Serial.println("❌ Nenhum PCF8574 encontrado!");
    } else {
        Serial.printf("✅ Total de PCF8574 encontrados: %d\n", pcfAddresses.size());
    }
    
    return pcfAddresses;
}

bool I2CScanner::testAddress(uint8_t address) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    return (error == 0);
}

void I2CScanner::printScanResults() {
    if (lastScanResults.empty()) {
        scanAll(); // Fazer scan se não foi feito ainda
    }
    
    Serial.println("\n📋 === RESULTADOS DO SCAN I2C ===");
    
    if (lastScanResults.empty()) {
        Serial.println("❌ Nenhum dispositivo encontrado");
        return;
    }
    
    Serial.println("Endereço | Tipo Provável");
    Serial.println("---------|------------------");
    
    for (const auto& device : lastScanResults) {
        Serial.printf("  0x%02X   | %s\n", device.address, device.deviceType.c_str());
    }
    
    // Mostrar PCF8574 especificamente
    std::vector<uint8_t> pcfList = findAllPCF8574();
    if (!pcfList.empty()) {
        Serial.println("\n🔌 === PCF8574 DETECTADOS ===");
        for (uint8_t addr : pcfList) {
            Serial.printf("PCF8574 em 0x%02X (A2=%d, A1=%d, A0=%d)\n", 
                         addr, 
                         (addr >> 2) & 1,  // A2
                         (addr >> 1) & 1,  // A1
                         addr & 1);        // A0
        }
    }
    
    Serial.println("================================\n");
}

String I2CScanner::getDeviceType(uint8_t address) {
    // Mapeamento de endereços conhecidos
    switch (address) {
        // PCF8574 (Expansor I/O)
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26:
            return "PCF8574 (Expansor I/O)";
            
        // LCD com I2C
        case 0x3C: case 0x3D:
            return "Display OLED";
        case 0x27: case 0x3F:
            return "LCD I2C";
            
        // Sensores comuns
        case 0x48: case 0x49: case 0x4A: case 0x4B:
            return "ADS1115 (ADC) ou TMP102";
        case 0x68: case 0x69:
            return "DS1307/DS3231 (RTC) ou MPU6050";
        case 0x76: case 0x77:
            return "BMP280/BME280 (Pressão/Temp)";
        case 0x5A:
            return "MLX90614 (Temp IR)";
        case 0x1E:
            return "HMC5883L (Magnetômetro)";
        case 0x53:
            return "ADXL345 (Acelerômetro)";
            
        default:
            return "Dispositivo Desconhecido";
    }
}

bool I2CScanner::isPCF8574Address(uint8_t address) {
    return (address >= 0x20 && address <= 0x27);
}
