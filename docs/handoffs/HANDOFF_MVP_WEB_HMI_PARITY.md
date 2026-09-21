# HANDOFF — MVP paridad Web ↔ HMI (alcance acotado)

**Fecha:** 2026-09-20  
**Repos:** ESP-HIDROWAVE-main (Master) + RX-TX/HMI-INTERFACE-main (HMI) + HIDROWAVE-main (web)  
**Estado:** implementación en curso · Oleada 2/3 **fuera**

## A0 — Modelo (congelado)

| Actor | Rol |
|-------|-----|
| Master | Única verdad sensores + actúa (Auto EC/pH, dose, relés) |
| Web | Misma operación + **historial/cloud** |
| HMI | Misma operación offline vía UART · **solo live** · **sin historial** |

```
Sensores → Master → { MQTT→Web , UART→HMI }
Web/HMI → comandos → Master
```

## Schema anti-loop

| Paso | Qué | Estado |
|------|-----|--------|
| A0 | Modelo | Acordado |
| A1 | pH/temp Modbus | **Enchufar sensores** (DI=GPIO26) |
| A2 | telemetry ph+ec+temp+`*_valid` | Tras enchufe |
| A3 | HMI consume `*_valid` → `--` | Código |
| A4 | Oleada 0+1 | Código |
| A5 | Este handoff | Hecho |

## Fuera de alcance (ahora)

- Historial / charts en HMI (**WEB-ONLY** definitivo)
- Oleada 2: badges Recirc/Dosando UX, tipagem UART, dilución
- Oleada 3: procedure Start/Abort, **editor** Rules, interlock UART
- Editar Rules en HMI (sigue web)

## Dentro — Oleada 0+1

| Ítem | Notas |
|------|-------|
| Live EC/pH/temp honestos | `*_valid` + `--` si false |
| Alvo / Auto / nutrientes / dose / cebar / calib caudal | Ya UART; DoD bancada |
| Atlas ON/OFF/timer/ciclo + estados | Ya |
| **Atlas candado regra/Auto** | Master emite `locked` en `slaves`; HMI bloquea UI; Master reject `relay_slave` Manual |
| **Relé Master local** | UI → `relay_local` (mismo camino que dose/cebar) |
| WiFi | SoftAP **o** UART `wifi_config` (mismo NVS); HMI precarga vía `sys_info` |
| Claim / reboot | WEB-ONLY |

## Atlas rule-lock (= web)

Referencia web: `HIDROWAVE-main/src/lib/manual-slave-relay-lock.ts`.

Contrato Master→HMI en `t:slaves` por relé (array `relays[]` objetos o campos paralelos):

```json
{"t":"slaves","slaves":[{
  "mac":"AA:BB:…","online":true,"numRelays":8,
  "relays":[{"on":0,"locked":true,"lock_reason":"rule","lock_label":"Recirc"}]
}]}
```

`lock_reason`: `rule` | `auto_ec` | `auto_ph`.

## Relé Master local

Mismo camino: HMI → UART → Master actúa.  
Dose usa `dose*` (R1–R6); genérico ON/OFF usa `relay_local`.

## Docs relacionados

- [`../HMI_BRIDGE.md`](../HMI_BRIDGE.md)
- [`../UART_AUDIT_MATRIX.md`](../UART_AUDIT_MATRIX.md)
- [`../../../HIDROWAVE-main/docs/handoffs/HMI_WEB_PARITY_F0.md`](../../../HIDROWAVE-main/docs/handoffs/HMI_WEB_PARITY_F0.md)
- HMI `docs/HANDOFF.md` (mapa pantallas; parcialmente obsoleto en Auto pH)

## DoD bancada

1. Enchufar Modbus → Serial `ph=`/`temp=` valid=1  
2. `[HMI UART TX] telemetry` con ph/ec/temp + `*_valid`  
3. HMI Central: `--` si `*_valid=false`  
4. `uart_status` / `hmi_last` en Master  
5. Atlas: relé con regra enabled → UI locked + `cmd_ack ok=false` si fuerza  
6. Master local: ON/OFF vía `relay_local` desde HMI  
