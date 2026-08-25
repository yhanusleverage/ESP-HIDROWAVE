# Ingeniería — puntero firmware

La gobernanza Master/Eng Manager vive en el repo de producto:

**[`HIDROWAVE-main/docs/engineering/00_INDICE.md`](../../../HIDROWAVE-main/docs/engineering/00_INDICE.md)**

Docs locales relevantes:

| Tema | Doc |
|------|-----|
| Ecuaciones Auto EC / pH | [`EQUACOES_AUTO_EC_PH.md`](EQUACOES_AUTO_EC_PH.md) |
| Tópicos / payloads v1 | [`docs/mqtt/04_MODELAGEM_TOPICOS_PAYLOADS.md`](../mqtt/04_MODELAGEM_TOPICOS_PAYLOADS.md) |
| Bounded contexts / desacople | ver engineering `03_FIRMWARE_BOUNDED_CONTEXTS.md` |
| DoD firmware | ver engineering `05_DEFINITION_OF_DONE.md` |

**Regla:** features nuevas de lazo en `HydroControl`; transporte en adapters/Core; mesh en ESP-NOW — no mezclar en un solo PR sin justificación P6.
