# Contrato SET_RELAY_MASK (Master alinhado ao SLAVE)

**Opcode:** `0x0F` — **não** `0x10`.  
**Slave repo:** `ESPNOW-SLAVE-TASK-main` (já alinhado).

```
0x01  RELAY_COMMAND      Master → slave   um relé
0x0E  ALL_RELAYS_STATUS  slave → Master    actual 8 bits
0x0F  SET_RELAY_MASK     Master → slave   ALL_ON/OFF atómico
0x11  PERSISTENT_STATE  Master interno   NVS (não é máscara)
```

Payload (8 bytes packed): `mask`, `pad`, `durationSec`, `commandId`.  
Lógica: bit i = ON. O slave faz `write8(~mask)` (PCF activo LOW). O Master **não** inverte.

Serial: `on_all` / `relay on_all` → um RF `mask=0xFF`. `off_all` → `0x00`.  
ACK máscara: `relayNumber=0xFF`. MQTT `relay/state` = último 0x0E (actual).

Flashear **Master** (este opcode) e **Slave** (já 0x0F).
