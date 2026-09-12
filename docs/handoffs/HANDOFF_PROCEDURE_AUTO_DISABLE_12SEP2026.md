# Handoff — Procedure auto-disable / retained (puntero)

Ver documento completo:

`HIDROWAVE-main/docs/handoffs/HANDOFF_PROCEDURE_AUTO_DISABLE_12SEP2026.md`

**Estado:** ✅ éxito validado en banco; optimizaciones de retained/reconnect aún recomendadas.

**2026-09-12 (circ noise):** sin pausar `fn_recirculacao` — UI filtra ACK ±12s de `procedure_finished`; firmware preserva runtime en upsert retained de fn_recirc. Ver `HIDROWAVE-main/docs/handoffs/HANDOFF_FN_CIRC_HISTORY_NOISE_12SEP2026.md`.

Código local:

- `infra/mqtt/bridge/index.js` — auto-disable + disable retained ×3
- `src/HydroSystemCore.cpp` — local disable + anti-retain post-reconnect
- `include/HydroSystemCore.h` — cooldown members
