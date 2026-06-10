# 01 — Arquitetura e decisão: por que MQTT **agora**

## Resumo executivo

O HIDROWAVE **continua** com Supabase como banco, autenticação de usuários e UI. O MQTT entra como **canal leve em tempo quase real** entre ESP32 e um broker (Mosquitto na Lightsail), com um **bridge** no servidor que persiste dados no Supabase. O navegador **não** fala MQTT.

```
┌─────────────┐     HTTPS (SSL)      ┌──────────────┐     HTTPS      ┌─────────────┐
│  ESP32      │ ────────────────────►│  Supabase    │◄───────────────│  HIDROWAVE  │
│  (Master)   │  comandos, registro, │  PostgreSQL  │  anon + auth   │  (Next.js)  │
│             │  histórico, relés    │              │                │             │
└──────┬──────┘                      └──────▲───────┘                └─────────────┘
       │ MQTT plain :1883                  │
       │ (telemetria, heartbeat, cmds)     │ service_role
       ▼                                   │
┌─────────────┐     subscribe/publish      │
│  Mosquitto  │◄───────────────────────────┤
│  Lightsail  │                            │
└──────┬──────┘                            │
       │                                   │
       └──────── Bridge Node.js ──────────┘
```

## Por que **não** ficamos só com HTTPS (situação até 2026)

| Problema observado | Causa no desenho atual |
|--------------------|-------------------------|
| Comandos de relé com latência de vários segundos | Poll `relay_commands` a cada 5 s + SSL handshake |
| Muitas requisições HTTPS no ESP | Sensores 30 s, status 60 s, sync relés 10 s, poll 5 s |
| Pressão de heap / watchdog | Várias conexões SSL concorrentes no mesmo chip |
| `last_seen` “lento” para UI | Atualização depende de ciclo HTTPS, não de push leve |

O documento `HIDROWAVE-main/ARQUITETURA_INTELIGENTE_SISTEMA.md` defende RPC + polling Supabase **sem** broker. Isso continua **válido para MVP simples**; a evolução para MQTT é uma **otimização operacional**, não uma troca de banco.

## Por que **não** MQTT no browser

| Motivo | Detalhe |
|--------|---------|
| Segurança | Credencial MQTT no front vazaria no bundle |
| Complexidade | Reconexão, ACL e QoS duplicariam o que Supabase já faz bem para UI |
| Produto | Usuário final usa dashboard; técnico usa MQTTX só em debug |

## Por que **não** AWS IoT Core / Amazon MQ (agora)

- Custo e complexidade (certificados X.509, políticas IAM) desproporcionais para dezenas de placas.
- **Lightsail + Mosquitto** já atende: IP fixo, firewall, ACL por `device_id`.

## O que **permanece** em Supabase HTTPS

| Função | Protocolo | Motivo |
|--------|-----------|--------|
| Registro de dispositivo / email | HTTPS RPC | Integração com `public.users` e auth |
| Portal WiFi (AP `192.168.4.1`) | HTTP local no AP | Configuração inicial; **não** é MQTT |
| Login / signup no setup WiFi | HTTPS Supabase Auth | Senha nunca vai no MQTT |
| Histórico `hydro_measurements` | HTTPS **ou** bridge | Bridge pode assumir inserts após fase estável |
| `relay_commands` (fila) | HTTPS + opcional espelho MQTT | Fila auditável; MQTT para entrega rápida |
| Decision engine / `decision_rules` | HTTPS poll 30 s | Lógica pesada; não migrar no MVP MQTT |

## O que **passa** para MQTT (planejado)

| Função | Tópico (ver doc 04) |
|--------|---------------------|
| Heartbeat / online | `hidrowave/{device_id}/heartbeat` |
| Telemetria sensores | `hidrowave/{device_id}/telemetry` |
| LWT offline | `hidrowave/{device_id}/status` (retain) |
| Estado relés (espelho) | `hidrowave/{device_id}/relay/state` |
| Comando rápido | `hidrowave/{device_id}/command` (subscribe ESP) |

## Princípios de desenho (não negociáveis)

1. **Supabase = fonte de verdade** para UI, usuários e histórico.
2. **MQTT = transporte**, não banco de dados.
3. **Um `device_id` = um prefixo de tópico** (`ESP32_HIDRO_XXXXXX` do MAC).
4. **ACL no broker** impede que um ESP publique no tópico de outro.
5. **HTTPS continua como backup** até MQTT + bridge estáveis por 24 h+.
6. **Sem credencial `service_role` no ESP** — só no bridge.

## Admin HTTP porta 80

`ENABLE_LOCAL_ADMIN_HTTP=0` em produção. Isso é **independente** de MQTT. O portal AP WiFi (`WiFiConfigServer`) permanece obrigatório.

## Referência de `device_id`

Gerado em `DeviceID.cpp`: prefixo `ESP32_HIDRO_` + últimos 6 caracteres hex do MAC.

Exemplo: MAC `E4:65:B8:26:98:44` → sufixo `269844` → `ESP32_HIDRO_269844`.

**Cuidado:** `HydroStateManager.cpp` usa `ESP.getEfuseMac()` em alguns caminhos; o ID canônico deve ser o de `getDeviceID()` após WiFi conectado para alinhar tópicos e Supabase.
