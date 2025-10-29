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
        _analogBuffer[_analogBufferIndex] = analogRead(_pin);
        _analogBufferIndex++;
        if (_analogBufferIndex == _sampleCount) {
            _analogBufferIndex = 0;
        }
    }

    static unsigned long printTimepoint = millis();
    if (millis() - printTimepoint > 800U) {
        printTimepoint = millis();

        for (int i = 0; i < _sampleCount; i++) {
            _analogBufferTemp[i] = _analogBuffer[i];
        }

        _averageVoltage = calculateMedian(_analogBufferTemp, _sampleCount) * (_vref / 4095.0);

        float compensationCoefficient = 1.0 + 0.02 * (_temperature - 25.0);
        float compensationVoltage = _averageVoltage / compensationCoefficient;

        float rawTDS = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage 
                    - 255.86 * compensationVoltage * compensationVoltage 
                    + 857.39 * compensationVoltage) * 0.5 * _calibrationFactor;

        if (rawTDS < 0 || rawTDS > 1000) {
            _tdsValue = 0;
        } else {
            _tdsValue = rawTDS;
        }

        Serial.print("Voltage: ");
        Serial.print(_averageVoltage, 3);
        Serial.print("V | TDS Raw: ");
        Serial.print(rawTDS);
        Serial.print(" | TDS Final: ");
        Serial.print(_tdsValue, 0);
        Serial.print(" ppm | EC: ");
        Serial.print(getECValue(), 0);
        Serial.println(" uS/cm");
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
