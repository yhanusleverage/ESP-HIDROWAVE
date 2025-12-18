# 🔧 Correção do Cálculo do Fator de Calibração

## ❌ Erro Identificado

**Cálculo ERRADO**:
```
Fator necessário: 1040 / 3354 = 0.031
```

**Cálculo CORRETO**:
```
Fator necessário: 3354 / 1040 = 3.23
```

---

## 🔍 Análise da Lógica de Calibração

### Fórmula no Código (TDSReaderSerial.cpp linha 192):
```cpp
float newCalibrationFactor = standardValue / measuredValue;
```

**Onde**:
- `standardValue` = Valor real/correto (do medidor profissional)
- `measuredValue` = Valor medido pelo sensor embarcado

### Aplicação do Fator (linha 81):
```cpp
rawTDS = (133.42 * V³ - 255.86 * V² + 857.39 * V) * 0.5 * _calibrationFactor;
```

**O fator é MULTIPLICADO**, então:
- Se sensor lê **BAIXO** → precisa de fator **ALTO** para aumentar
- Se sensor lê **ALTO** → precisa de fator **BAIXO** para reduzir

---

## 📊 Cálculo Correto para Seu Caso

### Situação 1: Água de Canilla
```
Sensor mede: 40 µS/cm
Real: 150 µS/cm
Fator = 150 / 40 = 3.75
Mas você usou 3.0 (funcionou aproximadamente)
```

### Situação 2: Após Dosagem
```
Sensor mede: 3354 µS/cm
Real (medidor profissional): 1040 µS/cm
Fator = 1040 / 3354 = 0.31 ✅ (CORRETO!)
```

**Mas espera!** Se o fator é `standardValue / measuredValue`:
- Fator = 1040 / 3354 = **0.31** (para REDUZIR o valor)

**Porém**, você disse que o fator 3.0 funciona para água de canilla. Isso significa:
- Sensor lê baixo (40) → aplica fator alto (3.0) → aumenta para 120 ✅

**Então, para após dosagem**:
- Sensor lê alto (3354) → precisa de fator baixo (0.31) → reduz para 1040 ✅

---

## 💡 Conclusão

**O cálculo CORRETO é**:
```
Fator = Valor_Real / Valor_Medido
Fator = 1040 / 3354 = 0.31
```

**NÃO é**:
```
Fator = 3354 / 1040 = 3.23 ❌
```

**Razão**: O fator é aplicado como **multiplicação**, então:
- Se sensor lê ALTO (3354), precisa DIVIDIR por 3.23, ou seja, MULTIPLICAR por (1/3.23) = 0.31

---

## 🎯 Solução

**Para corrigir o erro após dosagem**:
1. Calcular fator: `1040 / 3354 = 0.31`
2. Aplicar: `tdsSensor->setCalibrationFactor(0.31)`

**Mas isso vai quebrar a calibração da água de canilla!**

**Solução**: Calibração multi-ponto ou usar solução padrão 1413 µS/cm.


