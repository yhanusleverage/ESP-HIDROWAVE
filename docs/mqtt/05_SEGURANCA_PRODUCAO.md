# 05 — Segurança e viabilidade em produção

## Nível de ameaça aceito (MVP hidropônico)

| Dado | Sensibilidade | Canal MVP |
|------|---------------|-----------|
| pH, TDS, temperatura | Média-baixa | MQTT plain + TLS WiFi local |
| Estado relés | Média | MQTT plain + ACL |
| Credencial Supabase anon no ESP | Média | HTTPS TLS (já existe) |
| Senha usuário | Alta | **Nunca** MQTT — só Auth HTTPS |
| `service_role` | Crítica | **Só** no bridge (servidor) |

Plain MQTT na internet é **aceitável** para este produto se auth + ACL + IP fixo + senhas fortes. Não é aceitável para dados médicos/financeiros.

---

## Controles obrigatórios

### No broker (Mosquitto)

| Controle | Configuração |
|----------|--------------|
| Anônimo desligado | `allow_anonymous false` |
| Senhas | `password_file /var/lib/mosquitto/passwd` |
| ACL | `acl_file /var/lib/mosquitto/acl` |
| Listener | `listener 1883 0.0.0.0` apenas se firewall restringe |
| Arquivos em `/var/lib/mosquitto/` | Evita `Unable to open pwfile` (AppArmor) |

### Por dispositivo (produção alvo)

- Um usuário MQTT por ESP: `mqtt_ESP32_HIDRO_XXXXXX`
- Senha única gerada (16+ chars)
- ACL: read só `.../command`, write só `.../#` do próprio id

### No ESP

- `mqtt_pass` apenas em `secrets.ini` (gitignored)
- Não embutir `service_role`
- `ENABLE_MQTT` flag para desligar em campo

### No bridge

- `.env` com permissão `600`, usuário dedicado
- Rate limit de inserts Supabase
- Validar `device_id` do tópico contra regex `^ESP32_HIDRO_[0-9A-F]{6}$`

---

## Plain vs MQTTS (TLS)

| | Plain :1883 | MQTTS :8883 |
|---|-------------|-------------|
| RAM ESP | Menor | +10–20 KB (segundo contexto SSL) |
| Config | Simples | Certificado servidor + possível pin |
| Sniffing WAN | Possível | Mitigado |
| Recomendação MVP | ✅ | Fase futura |

Se migrar para TLS: usar Let's Encrypt na Lightsail + listener 8883; ESP com `WiFiClientSecure` + cert ou fingerprint.

---

## Firewall e rede

1. Lightsail: só **1883** (e depois 8883) para IPs necessários; não abrir 22 para `0.0.0.0/0` sem chave SSH forte.
2. IP estático: firmware não deve depender de IP efêmero.
3. IPv6: regra separada se ESP usar IPv6 (muitos usam só IPv4).

---

## Rotação e incidentes

| Evento | Ação |
|--------|------|
| Senha MQTT vazou no chat | `mosquitto_passwd` + atualizar ESP `secrets.ini` |
| Device roubado | Revogar user MQTT + desassociar Supabase |
| Broker comprometido | Rotacionar todas senhas; auditar ACL |

---

## Produção: o que **não** colocar em produção

| Item | Papel |
|------|-------|
| MQTTX / MQTT Explorer | Debug local |
| `allow_anonymous true` | Nunca |
| Broker sem ACL | Nunca |
| Frontend com credencial MQTT | Nunca |
| Admin HTTP :80 em campo | `ENABLE_LOCAL_ADMIN_HTTP=0` |

---

## Viabilidade em produção (resposta direta)

| Pergunta | Resposta |
|----------|----------|
| Mosquitto Lightsail plain em produção? | **Sim**, para escala atual, com controles acima |
| Substituir Supabase? | **Não** |
| MQTT sozinho para UI? | **Não** — bridge + Supabase |
| Quando subir para MQTTS? | Quando houver requisito formal ou incidente de sniffing |

---

## Monitoramento mínimo

- `systemctl status mosquitto hidrowave-bridge`
- Logrotate em `/var/log/mosquitto/`
- Alerta se nenhum heartbeat de device crítico em 10 min (cron ou Uptime Kuma)
