# Ecuaciones canónicas — Auto EC / Auto pH

Fuente de verdad: firmware (`Controller.cpp`, `HydroControl.cpp`, `EcDilutionController.cpp`).

`flow_rate` global y `max_dose` / `max_pulse` / `max_consecutive` **no forman parte** de estas leyes.

---

## Auto EC — déficit (`EC < SP − tolerance`)

```
u = (V / k) · e · Kp · A     [ml de receta]
```

| Símbolo | Qué es |
|---|---|
| `V` | Volumen del tanque (L), **una vez** |
| `k` | `base_dose / total_ml` (receta) o k aprendido |
| `e` | `SP − EC` (µS/cm), solo déficit |
| `Kp` | 1.0 salvo que se cambie |
| `A` | agresividad (0.05–1) |

Código: `ECController::calculateDosage` luego `dosageML *= ecAggressiveness`.

### Split por bomba (después de `u`)

```
ml_i = u · (r_i / R)
t_i  = ml_i / q_i
```

| Símbolo | Qué es |
|---|---|
| `r_i` | `nutrients[i].mlPerLiter` |
| `R` | suma de `r_i` activos |
| `q_i` | `nutrients[i].flowRate` (Calibragem, ml/s) |

Sin `q_i` calibrado esa bomba no dosa.

---

## Auto EC — exceso (`EC > SP + tolerance`)

```
V_dreno = V · (1 − SP / EC)     [L]
```

Drenar esa fracción y reponer agua (~0 µS). Techo hidráulico: `dilution_max_volume_l` (no es max_dose de pH).

---

## Auto pH

El ESP **no** usa `(V/k)·e` en pH. El lazo es en **H⁺** (no en unidades pH).

### Puerta (única)

```
si |pH − SP| ≤ ph_tolerance  → idle
```

Sin `max_dose`, `max_pulse`, `max_consecutive`.

### Ley (firmware)

```
H      = 10^(−pH)
ErroH  = H_medido − H_SP
u      = A · |ErroH| / K     [ml]
t      = u / q
```

| Símbolo | Qué es |
|---|---|
| `ErroH > 0` | pH bajo → bomba **base** (`flow_rate_ph_up`) |
| `ErroH < 0` | pH alto → bomba **ácido** (`flow_rate_ph_down`) |
| `K` | `k_acid` o `k_base` (seed de calibragem o aprendido) |
| `A` | agresividad (0.05–1); en commissioning ≤ 0.3 |
| `q` | vazão de **esa** bomba pH (ml/s) |

Código: `AdaptivePHController::planDose`.

Preview operador (UI, dominio pH, equivalente si K está bien):  
`u ≈ A · |pH−SP| · s` con `s` = ml por unidad pH.

No hay split de receta: una bomba por lado (ácido o base), no tres nutrientes.

---

## Persistencia

| Dato | Dónde |
|---|---|
| Receta + `flowRate` por bomba | `ec_config_view.nutrients[]` |
| `V` | `ec_config_view.volume` |
| pH vazão | `ph_config_view.flow_rate_ph_up` / `flow_rate_ph_down` |

SQL para quitar columnas viejas: `HIDROWAVE-main/scripts/DROP_EC_FLOW_RATE_AND_PH_MAX.sql`.
