# 🔍 Diferencia Real Entre `intervalo_auto_ec` y `tempo_recirculacao`

## 📊 Análisis del Código

### Flujo en `checkAutoEC()`:

```cpp
void checkAutoEC() {
    // 1️⃣ PRIMERO: Verificar tempo_recirculacao (línea 567)
    if (lastDosageCompleteTime > 0 && tempoRecirculacao > 0) {
        if (elapsedSeconds < tempoRecirculacao) {
            return;  // ⛔ BLOQUEO ABSOLUTO - NO verifica nada más
        }
    }
    
    // 2️⃣ DESPUÉS: Verificar intervalo_auto_ec (línea 600)
    if (currentMillis - lastECCheck < checkInterval) {
        return;  // ⏸️ Aún no es hora de verificar
    }
    
    // 3️⃣ Si pasa ambos checks → Verificar EC y dosificar si es necesario
}
```

---

## 🎯 Diferencia Real

### `tempo_recirculacao` (Tiempo Muerto)
- **Cuándo se aplica**: DESPUÉS de una dosagem completa
- **Qué hace**: BLOQUEO ABSOLUTO - no verifica EC durante este tiempo
- **Por qué existe**: Dar tiempo para que EC se estabilice después de dosificar
- **Ejemplo**: Dosagem completa → esperar 10 min → luego verificar

### `intervalo_auto_ec` (Intervalo de Verificación)
- **Cuándo se aplica**: Cuando NO hay dosagem reciente (fuera del tiempo muerto)
- **Qué hace**: Controla frecuencia de verificación cuando el sistema está "normal"
- **Por qué existe**: Evitar verificar demasiado frecuentemente cuando no hay dosagem
- **Ejemplo**: Sin dosagem reciente → verificar cada 3 segundos

---

## 📈 Ejemplos Concretos

### Escenario 1: `tempo_recirculacao = 600`, `intervalo_auto_ec = 3`

```
T=0s:    Dosagem completa → Inicia tempo_recirculacao (600s)
T=1s:    checkAutoEC() → ⛔ En tiempo muerto → return (ignora intervalo_auto_ec)
T=2s:    checkAutoEC() → ⛔ En tiempo muerto → return
...
T=600s:  Tempo morto termina
T=601s:  checkAutoEC() → ✅ Fuera de tiempo muerto → Verifica intervalo_auto_ec
         → Si pasaron 3s desde lastECCheck → Verifica EC
T=604s:  checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
T=607s:  checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
...
```

**Resultado**: 
- Durante 10 min después de dosagem: NO verifica (bloqueo)
- Después de 10 min: Verifica cada 3 segundos

---

### Escenario 2: `tempo_recirculacao = 600`, `intervalo_auto_ec = 300`

```
T=0s:    Dosagem completa → Inicia tempo_recirculacao (600s)
T=1s:    checkAutoEC() → ⛔ En tiempo muerto → return
...
T=600s:  Tempo morto termina
T=601s:  checkAutoEC() → ✅ Fuera de tiempo muerto → Verifica intervalo_auto_ec
         → Si pasaron 300s desde lastECCheck → Verifica EC
T=901s:  checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
T=1201s: checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
...
```

**Resultado**:
- Durante 10 min después de dosagem: NO verifica (bloqueo)
- Después de 10 min: Verifica cada 5 minutos

---

### Escenario 3: `tempo_recirculacao = 60`, `intervalo_auto_ec = 3`

```
T=0s:    Dosagem completa → Inicia tempo_recirculacao (60s)
T=1s:    checkAutoEC() → ⛔ En tiempo muerto → return
...
T=60s:   Tempo morto termina
T=61s:   checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
T=64s:   checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
T=67s:   checkAutoEC() → ✅ Verifica intervalo_auto_ec → Verifica EC
...
```

**Resultado**:
- Durante 1 min después de dosagem: NO verifica (bloqueo)
- Después de 1 min: Verifica cada 3 segundos

---

## 🔍 ¿Cuándo Importa Cada Uno?

### `tempo_recirculacao` importa cuando:
- ✅ Acabas de dosificar (bloqueo absoluto)
- ✅ EC necesita estabilizarse después de dosagem
- ✅ Quieres evitar overdosing

### `intervalo_auto_ec` importa cuando:
- ✅ NO hay dosagem reciente (fuera del tiempo muerto)
- ✅ Sistema está en modo "monitoreo normal"
- ✅ Quieres controlar frecuencia de verificación

---

## 💡 Conclusión

**Son diferentes pero complementarios**:

1. **`tempo_recirculacao`**: Bloqueo DESPUÉS de dosagem (previene overdosing)
2. **`intervalo_auto_ec`**: Frecuencia de verificación cuando NO hay dosagem reciente

**En la práctica**:
- Si `tempo_recirculacao` es alto (600s), `intervalo_auto_ec` solo importa después del tiempo muerto
- Si `tempo_recirculacao` es bajo (60s), `intervalo_auto_ec` importa más frecuentemente

**Recomendación**:
- `tempo_recirculacao = 600` (10 min) - Tiempo suficiente para estabilización
- `intervalo_auto_ec = 300` (5 min) - Balance entre responsividad y carga

