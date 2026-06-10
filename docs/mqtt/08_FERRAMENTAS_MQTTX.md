# 08 — MQTTX e ferramentas de teste

## O que é MQTTX

[MQTTX](https://mqttx.app/) é um **cliente desktop** (Windows, macOS, Linux) para conectar a brokers MQTT, publicar e subscrever mensagens, inspecionar JSON e salvar conexões.

**Não é:**

- Broker
- Parte do runtime de produção
- Substituto do HIDROWAVE ou do bridge

**É:**

- Ferramenta de **desenvolvimento e suporte** equivalente a `mosquitto_pub` / `mosquitto_sub` com UI.

Alternativas: **MQTT Explorer**, linha de comando no SSH da Lightsail.

---

## Viabilidade

| Uso | Viável? |
|-----|---------|
| Testar broker Lightsail antes do ESP | ✅ Muito recomendado |
| Validar ACL (tentar tópico de outro device) | ✅ |
| Debug em campo com notebook | ✅ |
| App para usuário final | ❌ |
| Produção 24/7 | ❌ |

---

## Configuração de conexão (exemplo)

| Campo | Valor |
|-------|-------|
| Name | HIDROWAVE Lightsail |
| Host | IP estático Lightsail (ex. configurado na Fase 0) |
| Port | `1883` |
| Username | user Mosquitto (dev ou por device) |
| Password | de `passwd` |
| SSL/TLS | Desligado no MVP |

### Subscribe (ver tudo de um ESP)

```
hidrowave/ESP32_HIDRO_269844/#
```

### Publish teste heartbeat

- **Topic:** `hidrowave/ESP32_HIDRO_269844/heartbeat`
- **Payload:**
  ```json
  {"v":1,"device_id":"ESP32_HIDRO_269844","ts":0,"online":true}
  ```

Se o bridge estiver ativo, `last_seen` deve mudar no Supabase.

---

## Fluxo de teste recomendado

1. MQTTX → publish heartbeat manual → Supabase OK (Fase 1).
2. ESP firmware Fase 2 → MQTTX só observa subscribe.
3. Comando: publicar em `.../command` → ver relé (Fase 3) **somente em bancada**.

---

## Produção vs MQTTX

```
PRODUÇÃO                          DESENVOLVIMENTO
────────                          ───────────────
ESP ──MQTT──► Mosquitto           MQTTX ──MQTT──► Mosquitto
Bridge ──HTTPS──► Supabase            (mesmo broker)
HIDROWAVE ──HTTPS──► Supabase
```

O usuário da estufa **nunca** instala MQTTX. Ele usa o site; técnicos usam MQTTX quando necessário.

---

## Windows sem `mosquitto_pub`

No PC Windows muitas vezes não há cliente CLI. Opções:

1. MQTTX (recomendado)
2. SSH na Lightsail + `mosquitto_pub` local
3. WSL com `mosquitto-clients`

---

## Segurança ao usar MQTTX

- Não salvar conexão em repositório compartilhado com senha.
- Usar user de teste com ACL limitada, não `bridge_internal`.
- Rotacionar senha se exportar screenshot com credencial.
