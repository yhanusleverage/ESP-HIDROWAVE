#include "Config.h"
#include "TDSReaderSerial.h"

TDSReaderSerial::TDSReaderSerial(uint8_t pin, float vref, float calibrationFactor)
    : _pin(pin)
    , _vref(vref)
    , _calibrationFactor(calibrationFactor)
    , _temperature(25.0)  // Temperatura padrão
    , _tdsValue(0)
    , _averageVoltage(0)
    , _analogBufferIndex(0)
    , _bufferStartTime(0)
    , _bufferInitialized(false)
{
    // Inicializar buffers
    for (int i = 0; i < _sampleCount; i++) {
        _analogBuffer[i] = 0;
        _analogBufferTemp[i] = 0;
    }
}

void TDSReaderSerial::begin() {
    pinMode(_pin, INPUT);
    Serial.begin(115200);
    Serial.println("TDS/EC Serial Monitor");
}

void TDSReaderSerial::readTDS() {
    static unsigned long analogSampleTimepoint = millis();

    if (millis() - analogSampleTimepoint > 40U) {
        analogSampleTimepoint = millis();
        
        // ✅ Marcar início do buffer na primeira amostra
        if (!_bufferInitialized && _analogBufferIndex == 0) {
            _bufferStartTime = millis();
            _bufferInitialized = true;
        }
        
        _analogBuffer[_analogBufferIndex] = analogRead(_pin);
        _analogBufferIndex++;
        if (_analogBufferIndex == _sampleCount) {
            _analogBufferIndex = 0;
            // ✅ Buffer completo - resetar flag se necessário
        }
    }

    static unsigned long printTimepoint = millis();
    if (millis() - printTimepoint > 800U) {
        printTimepoint = millis();

        for (int i = 0; i < _sampleCount; i++) {
            _analogBufferTemp[i] = _analogBuffer[i];
        }

        // ✅ Calcular mediana uma única vez (otimização)
        int medianReading = calculateMedian(_analogBufferTemp, _sampleCount);
        _averageVoltage = medianReading * (_vref / 4095.0);

        // 🔍 DIAGNÓSTICO: Log detalhado para investigar problema de leitura 1/3
        static unsigned long lastDiagnosticLog = 0;
        bool shouldLogDiagnostic = (millis() - lastDiagnosticLog > 10000);  // A cada 10 segundos
        if (shouldLogDiagnostic) {
            Serial.println("\n🔍 === DIAGNÓSTICO TDS/EC ===");
            Serial.printf("   📊 Leitura ADC mediana: %d\n", medianReading);
            Serial.printf("   ⚡ Voltage médio: %.4fV\n", _averageVoltage);
            Serial.printf("   🌡️  Temperatura usada: %.2f°C\n", _temperature);
            lastDiagnosticLog = millis();
        }

        float compensationCoefficient = 1.0 + 0.02 * (_temperature - 25.0);
        float compensationVoltage = _averageVoltage / compensationCoefficient;

        if (shouldLogDiagnostic) {
            Serial.printf("   🔧 Coeficiente compensação: %.4f\n", compensationCoefficient);
            Serial.printf("   ⚡ Voltage compensado: %.4fV\n", compensationVoltage);
        }

        float rawTDS = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage 
                    - 255.86 * compensationVoltage * compensationVoltage 
                    + 857.39 * compensationVoltage) * 0.5 * _calibrationFactor;

        if (shouldLogDiagnostic) {
            Serial.printf("   📈 TDS Raw (antes validação): %.2f ppm\n", rawTDS);
            Serial.printf("   🎯 Fator calibração: %.2f\n", _calibrationFactor);
            Serial.printf("   📏 VREF: %.2fV\n", _vref);
            Serial.println("================================\n");
        }

        // ✅ CORREÇÃO: Usar MAX_TDS de Config.h (5000) em vez de 1000 fixo
        // ✅ Validação: Se a leitura analógica é muito baixa (< 10), o sensor pode estar desconectado
        if (medianReading < 10) {
            static unsigned long lastSensorError = 0;
            if (millis() - lastSensorError > 5000) {  // Avisar a cada 5 segundos
                Serial.println("⚠️ [TDS/EC] Leitura analógica muito baixa - verifique conexão do sensor!");
                Serial.printf("   📊 Leitura mediana: %d (esperado: > 10)\n", medianReading);
                Serial.printf("   💡 Verifique: Pino %d, conexão do sensor, alimentação\n", _pin);
                lastSensorError = millis();
            }
            _tdsValue = 0;
        } else if (rawTDS < 0 || rawTDS > MAX_TDS) {
            // ✅ CORREÇÃO: Usar MAX_TDS de Config.h
            static unsigned long lastRangeError = 0;
            if (millis() - lastRangeError > 5000) {  // Avisar a cada 5 segundos
                Serial.printf("⚠️ [TDS/EC] Valor fora do intervalo: %.2f ppm (esperado: 0-%.0f)\n", rawTDS, MAX_TDS);
                Serial.printf("   📊 Voltage: %.3fV, Compensado: %.3fV\n", _averageVoltage, compensationVoltage);
                lastRangeError = millis();
            }
            _tdsValue = 0;
        } else {
            _tdsValue = rawTDS;
        }

        // ✅ Melhorar diagnóstico: mostrar leitura analógica bruta também
        Serial.print("ADC: ");
        Serial.print(medianReading);
        Serial.print(" | Voltage: ");
        Serial.print(_averageVoltage, 3);
        Serial.print("V | TDS Raw: ");
        Serial.print(rawTDS, 2);
        Serial.print(" | TDS Final: ");
        Serial.print(_tdsValue, 0);
        Serial.print(" ppm | EC: ");
        Serial.print(getECValue(), 0);
        Serial.print(" uS/cm | Buffer: ");
        Serial.print(isBufferReady() ? "OK" : "Aguardando");
        Serial.println();
    }
}

void TDSReaderSerial::updateTemperature(float temp) {
    _temperature = temp;
}

float TDSReaderSerial::getTDSValue() {
    return _tdsValue;
}

float TDSReaderSerial::getECValue() {
    return _tdsValue * 2;
}

// ✅ Verificar se o buffer está cheio (EC confiável)
// Buffer precisa de 30 amostras × 40ms = 1200ms (1.2 segundos) para encher
// Adicionamos margem de segurança: 1500ms (1.5 segundos)
bool TDSReaderSerial::isBufferReady() const {
    if (!_bufferInitialized) {
        return false;  // Buffer nunca foi inicializado
    }
    
    // Verificar se passou tempo suficiente para encher o buffer
    // 30 amostras × 40ms = 1200ms, mas adicionamos margem de 300ms = 1500ms total
    unsigned long bufferFillTime = 1500;  // 1.5 segundos
    unsigned long elapsed = millis() - _bufferStartTime;
    
    return elapsed >= bufferFillTime;
}

float TDSReaderSerial::calculateMedian(int *bArray, int iFilterLen) {
    int bTab[iFilterLen];
    for (int i = 0; i < iFilterLen; i++) {
        bTab[i] = bArray[i];
    }

    for (int j = 0; j < iFilterLen - 1; j++) {
        for (int i = 0; i < iFilterLen - j - 1; i++) {
            if (bTab[i] > bTab[i + 1]) {
                int bTemp = bTab[i];
                bTab[i] = bTab[i + 1];
                bTab[i + 1] = bTemp;
            }
        }
    }

    if ((iFilterLen & 1) > 0) {
        return bTab[(iFilterLen - 1) / 2];
    } else {
        return (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2.0;
    }
}

// ✅ Métodos de calibração
bool TDSReaderSerial::calibrate(float standardValue, float measuredValue) {
    if (measuredValue <= 0.0 || standardValue <= 0.0) {
        Serial.println("❌ [TDS Calibração] Valores inválidos para calibração!");
        Serial.printf("   📊 Valor padrão: %.2f, Valor medido: %.2f\n", standardValue, measuredValue);
        return false;
    }
    
    // Calcular KValue (fator de calibração)
    // KValue = Standard Value / Measured Value
    float newCalibrationFactor = standardValue / measuredValue;
    
    Serial.println("\n🔧 === CALIBRAÇÃO TDS ===");
    Serial.printf("   📊 Valor padrão (solução): %.2f ppm\n", standardValue);
    Serial.printf("   📈 Valor medido pelo sensor: %.2f ppm\n", measuredValue);
    Serial.printf("   🎯 Fator antigo: %.4f\n", _calibrationFactor);
    Serial.printf("   ✅ Fator novo: %.4f\n", newCalibrationFactor);
    Serial.printf("   📏 VREF: %.2fV\n", _vref);
    Serial.println("==========================\n");
    
    _calibrationFactor = newCalibrationFactor;
    return true;
}

bool TDSReaderSerial::calibrateWithSolution1413() {
    // Solução padrão 1413 µS/cm = 707 ppm TDS (aproximadamente)
    // Esperar buffer estar pronto e fazer várias leituras para estabilizar
    if (!isBufferReady()) {
        Serial.println("⏳ [TDS Calibração] Aguardando buffer estar pronto...");
        return false;
    }
    
    // Fazer várias leituras e calcular média
    const int calibrationSamples = 10;
    float sumTDS = 0.0;
    int validSamples = 0;
    
    Serial.println("🔧 [TDS Calibração] Iniciando calibração com solução 1413 µS/cm...");
    Serial.println("   💡 Aguarde estabilização das leituras...");
    
    for (int i = 0; i < calibrationSamples; i++) {
        readTDS();
        float currentTDS = getTDSValue();
        if (currentTDS > 0 && currentTDS < 2000) {  // Valores válidos
            sumTDS += currentTDS;
            validSamples++;
        }
        delay(500);  // Aguardar entre leituras
    }
    
    if (validSamples < 5) {
        Serial.println("❌ [TDS Calibração] Leituras insuficientes para calibração!");
        return false;
    }
    
    float averageMeasured = sumTDS / validSamples;
    float standardValue = 707.0;  // 1413 µS/cm = 707 ppm TDS
    
    Serial.printf("   📊 Média das leituras: %.2f ppm (%d amostras válidas)\n", averageMeasured, validSamples);
    
    return calibrate(standardValue, averageMeasured);
}

void TDSReaderSerial::setCalibrationFactor(float factor) {
    if (factor > 0.0 && factor <= 10.0) {  // Validação: fator entre 0.1 e 10.0
        Serial.printf("🔧 [TDS] Fator de calibração atualizado: %.4f -> %.4f\n", _calibrationFactor, factor);
        _calibrationFactor = factor;
    } else {
        Serial.printf("❌ [TDS] Fator de calibração inválido: %.4f (deve estar entre 0.1 e 10.0)\n", factor);
    }
}

void TDSReaderSerial::setVRef(float vref) {
    if (vref > 0.0 && vref <= 5.0) {  // Validação: VREF entre 0.1V e 5.0V
        Serial.printf("🔧 [TDS] VREF atualizado: %.2fV -> %.2fV\n", _vref, vref);
        _vref = vref;
    } else {
        Serial.printf("❌ [TDS] VREF inválido: %.2fV (deve estar entre 0.1V e 5.0V)\n", vref);
    }
}

float TDSReaderSerial::getCalibrationFactor() const {
    return _calibrationFactor;
}

float TDSReaderSerial::getVRef() const {
    return _vref;
}
