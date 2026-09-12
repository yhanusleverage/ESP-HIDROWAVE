# Handoff — Procedure auto-disable / retained (puntero)

Ver documento completo:

`HIDROWAVE-main/docs/handoffs/HANDOFF_PROCEDURE_AUTO_DISABLE_12SEP2026.md`

**Estado:** ✅ éxito validado en banco; optimizaciones de retained/reconnect aún recomendadas.

Código local:

- `infra/mqtt/bridge/index.js` — auto-disable + disable retained ×3
- `src/HydroSystemCore.cpp` — local disable + anti-retain post-reconnect
- `include/HydroSystemCore.h` — cooldown members
