-- =============================================================================
-- ADD level_interlock_mode — Normal (≠vazio) | Carrera (solo alto)
-- =============================================================================
-- Ejecutar en Supabase SQL Editor (idempotente).
-- Firmware: NVS lvl_ilock + MQTT set_level_interlock + telemetry interlock_mode
-- Bridge: PATCH device_status.level_interlock_mode desde telemetry/levels
-- =============================================================================

ALTER TABLE public.device_status
  ADD COLUMN IF NOT EXISTS level_interlock_mode text DEFAULT 'normal';

COMMENT ON COLUMN public.device_status.level_interlock_mode IS
  'Modo interlock Auto EC/pH: normal (=≠vazio) | carrera (=solo alto 4/4)';

-- Normalizar filas existentes
UPDATE public.device_status
SET level_interlock_mode = 'normal'
WHERE level_interlock_mode IS NULL
   OR level_interlock_mode NOT IN ('normal', 'carrera');
