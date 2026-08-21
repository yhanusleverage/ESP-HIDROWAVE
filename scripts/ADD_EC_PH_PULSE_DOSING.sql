-- =====================================================
-- Dosificación por pulsos EC + pH (paridad HMI)
--   pulse_ml       ↔ UART pulseMl      ("ml por pulso")
--   pulse_gap_sec  ↔ UART pulseGapSec  ("Gap pulsos (s)")
-- Mismo camino HTTPS que auto_enabled:
--   GET /rest/v1/ec_config_view?device_id=eq.{id}&select=*
--   GET /rest/v1/ph_config_view?device_id=eq.{id}&select=*
-- Executar no SQL Editor do Supabase
-- =====================================================

BEGIN;

ALTER TABLE public.ec_config_view
  ADD COLUMN IF NOT EXISTS pulse_ml double precision DEFAULT 2.0;

ALTER TABLE public.ec_config_view
  ADD COLUMN IF NOT EXISTS pulse_gap_sec double precision DEFAULT 2.0;

ALTER TABLE public.ph_config_view
  ADD COLUMN IF NOT EXISTS pulse_ml double precision DEFAULT 2.0;

ALTER TABLE public.ph_config_view
  ADD COLUMN IF NOT EXISTS pulse_gap_sec double precision DEFAULT 2.0;

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'ec_config_view_pulse_ml_check'
  ) THEN
    ALTER TABLE public.ec_config_view
      ADD CONSTRAINT ec_config_view_pulse_ml_check
      CHECK (pulse_ml > 0 AND pulse_ml <= 50);
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'ec_config_view_pulse_gap_sec_check'
  ) THEN
    ALTER TABLE public.ec_config_view
      ADD CONSTRAINT ec_config_view_pulse_gap_sec_check
      CHECK (pulse_gap_sec >= 0 AND pulse_gap_sec <= 120);
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'ph_config_view_pulse_ml_check'
  ) THEN
    ALTER TABLE public.ph_config_view
      ADD CONSTRAINT ph_config_view_pulse_ml_check
      CHECK (pulse_ml > 0 AND pulse_ml <= 50);
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_constraint WHERE conname = 'ph_config_view_pulse_gap_sec_check'
  ) THEN
    ALTER TABLE public.ph_config_view
      ADD CONSTRAINT ph_config_view_pulse_gap_sec_check
      CHECK (pulse_gap_sec >= 0 AND pulse_gap_sec <= 120);
  END IF;
END $$;

UPDATE public.ec_config_view
SET pulse_ml = 2.0
WHERE pulse_ml IS NULL;

UPDATE public.ec_config_view
SET pulse_gap_sec = 2.0
WHERE pulse_gap_sec IS NULL;

UPDATE public.ph_config_view
SET pulse_ml = 2.0
WHERE pulse_ml IS NULL;

UPDATE public.ph_config_view
SET pulse_gap_sec = 2.0
WHERE pulse_gap_sec IS NULL;

COMMENT ON COLUMN public.ec_config_view.pulse_ml IS
  'ml por pulso Auto EC (global). Último pulso = resto exacto. UART pulseMl.';

COMMENT ON COLUMN public.ec_config_view.pulse_gap_sec IS
  'Gap pulsos (s) entre inyecciones Auto EC. UART pulseGapSec. Recirc = tempo_recirculacao post-secuencia.';

COMMENT ON COLUMN public.ph_config_view.pulse_ml IS
  'ml por pulso Auto pH (global). Último pulso = resto exacto. UART pulseMl.';

COMMENT ON COLUMN public.ph_config_view.pulse_gap_sec IS
  'Gap pulsos (s) entre inyecciones Auto pH. UART pulseGapSec.';

COMMIT;
