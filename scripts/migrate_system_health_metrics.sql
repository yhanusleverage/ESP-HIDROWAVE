-- system_health_metrics — VIEW read-only (NÃO criar tabela)
--
-- Pipeline MQTT heartbeat:
--   ESP → hidrowave/{id}/heartbeat → bridge → UPDATE device_status
--   Frontend → SELECT * FROM system_health_metrics
--
-- Erro CREATE OR REPLACE com colunas em ordem diferente da view antiga:
--   "cannot change name of view column is_online to device_name"
-- Solução: DROP VIEW + CREATE (não usar REPLACE quando muda ordem/nomes de colunas)

-- Garantir colunas de saúde em device_status (idempotente)
ALTER TABLE public.device_status
  ADD COLUMN IF NOT EXISTS wifi_rssi integer,
  ADD COLUMN IF NOT EXISTS free_heap integer,
  ADD COLUMN IF NOT EXISTS uptime_seconds integer,
  ADD COLUMN IF NOT EXISTS reboot_count integer DEFAULT 0,
  ADD COLUMN IF NOT EXISTS firmware_version text DEFAULT '2.1.0',
  ADD COLUMN IF NOT EXISTS ip_address inet,
  ADD COLUMN IF NOT EXISTS is_online boolean DEFAULT false,
  ADD COLUMN IF NOT EXISTS last_seen timestamptz DEFAULT now();

-- Recriar view (DROP obrigatório se a versão antiga tinha outra ordem de colunas)
DROP VIEW IF EXISTS public.system_health_metrics CASCADE;

CREATE VIEW public.system_health_metrics AS
SELECT
  ds.device_id,
  ds.device_name,
  ds.location,
  ds.is_online,
  ds.last_seen,
  EXTRACT(EPOCH FROM (NOW() - ds.last_seen)) / 60 AS minutes_offline,
  ds.wifi_rssi,
  ds.free_heap,
  ds.uptime_seconds,
  ds.reboot_count,
  ds.firmware_version,
  ds.ip_address,
  CASE
    WHEN ds.free_heap > 180000 THEN 'EXCELLENT'
    WHEN ds.free_heap > 100000 THEN 'GOOD'
    WHEN ds.free_heap > 50000 THEN 'FAIR'
    WHEN ds.free_heap > 20000 THEN 'WARNING'
    ELSE 'CRITICAL'
  END AS memory_status,
  COALESCE(cmd_stats.total_commands, 0) AS commands_24h,
  COALESCE(cmd_stats.success_rate, 100) AS success_rate_percent,
  COALESCE(cmd_stats.avg_completion_time, 0) AS avg_completion_seconds,
  (SELECT COUNT(*) FROM unnest(ds.relay_states) AS state WHERE state = true) AS active_relays,
  CASE
    WHEN NOT ds.is_online THEN 0
    WHEN ds.last_seen < NOW() - INTERVAL '10 minutes' THEN 25
    WHEN ds.free_heap < 20000 THEN 30
    WHEN ds.wifi_rssi < -80 THEN 40
    WHEN COALESCE(cmd_stats.success_rate, 100) < 80 THEN 60
    WHEN ds.free_heap < 50000 THEN 80
    ELSE 100
  END AS health_score
FROM public.device_status ds
LEFT JOIN (
  SELECT
    device_id,
    COUNT(*) AS total_commands,
    ROUND(
      (COUNT(CASE WHEN status = 'completed' THEN 1 END) * 100.0 / COUNT(*)),
      1
    ) AS success_rate,
    AVG(EXTRACT(EPOCH FROM (completed_at - sent_at))) AS avg_completion_time
  FROM public.relay_commands
  WHERE created_at > NOW() - INTERVAL '24 hours'
    AND status IN ('completed', 'failed')
  GROUP BY device_id
) cmd_stats ON ds.device_id = cmd_stats.device_id;

COMMENT ON VIEW public.system_health_metrics IS
  'Métricas de saúde (read-only). Fonte: device_status via HTTPS e/ou MQTT heartbeat.';
