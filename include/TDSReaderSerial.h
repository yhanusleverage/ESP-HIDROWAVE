#ifndef TDSREADERSERIAL_H  //se não está definido, define
#define TDSREADERSERIAL_H  //define

#include <Arduino.h> //biblioteca do Arduino

class TDSReaderSerial {
public:
    TDSReaderSerial(uint8_t pin, float vref, float calibrationFactor); //construtor
    void begin();
    void readTDS();
    void updateTemperature(float temp); // Atualiza a temperatura lida externamente
    float getTDSValue(); //obter o valor do TDS
    float getECValue(); //obter o valor do EC
    bool isBufferReady() const; // ✅ Verificar se o buffer está cheio (EC confiável)
    
    // ✅ Métodos de calibração
    bool calibrate(float standardValue, float measuredValue); // Calibrar com solução padrão (ex: 1413 µS/cm)
    bool calibrateWithSolution1413(); // Calibrar automaticamente com solução 1413 µS/cm (707 ppm)
    void setCalibrationFactor(float factor); // Definir fator de calibração manualmente
    void setVRef(float vref); // Atualizar tensão de referência
    float getCalibrationFactor() const; // Obter fator de calibração atual
    float getVRef() const; // Obter tensão de referência atual

private:
    uint8_t _pin; //pino do TDS
    float _vref; //referência de tensão
    float _calibrationFactor; //fator de calibração
    float _temperature; // Temperatura dinâmica fornecida externamente
    float _tdsValue; //valor do TDS
    float _averageVoltage; //tensão média
    static const int _sampleCount = 30; //número de amostras
    int _analogBuffer[_sampleCount];
    int _analogBufferTemp[_sampleCount]; //buffer temporário    
    int _analogBufferIndex; //índice do buffer
    unsigned long _bufferStartTime; // ✅ Tempo de início do buffer (para detectar quando está cheio)
    bool _bufferInitialized; // ✅ Flag para rastrear se o buffer foi inicializado

    float calculateMedian(int *bArray, int iFilterLen); //calcular a mediana
};

#endif  
