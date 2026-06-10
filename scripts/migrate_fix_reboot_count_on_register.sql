-- FIX: register_device_with_email não zerar reboot_count em todo boot
-- Executar no SQL Editor Supabase (CREATE OR REPLACE idempotente)
--
-- Antes: ON CONFLICT sempre reboot_count = 0 + reboot_count_reset sempre true
-- Depois: preserva reboot_count; zera só troca de dono ou INSERT novo

CREATE OR REPLACE FUNCTION public.register_device_with_email(
  p_device_id text,
  p_mac_address text,
  p_user_email text,
  p_device_name text DEFAULT NULL,
  p_location text DEFAULT NULL,
  p_ip_address text DEFAULT NULL
)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_count integer;
  v_final_name text;
  v_final_location text;
  v_email text;
  v_ip inet;
  v_stored_email text;
  v_previous_email text;
  v_mac_norm text;
  v_user_total_devices integer;
  v_prev_user_total_devices integer;
  v_user_ensured boolean;
  v_owner_changed boolean;
  v_is_new_device boolean;
BEGIN
  IF p_device_id IS NULL OR btrim(p_device_id) = '' THEN
    RETURN jsonb_build_object('success', false, 'message', 'device_id é obrigatório');
  END IF;

  IF p_mac_address IS NULL OR btrim(p_mac_address) = '' THEN
    RETURN jsonb_build_object('success', false, 'message', 'mac_address é obrigatório');
  END IF;

  IF p_user_email IS NULL OR btrim(p_user_email) = '' THEN
    RETURN jsonb_build_object('success', false, 'message', 'user_email é obrigatório');
  END IF;

  IF position('@' in p_user_email) = 0 THEN
    RETURN jsonb_build_object('success', false, 'message', 'user_email inválido');
  END IF;

  v_email := lower(btrim(p_user_email));
  v_mac_norm := upper(replace(btrim(p_mac_address), ':', ''));

  SELECT lower(btrim(user_email))
    INTO v_previous_email
    FROM public.device_status
   WHERE device_id = btrim(p_device_id);

  v_is_new_device := NOT FOUND;

  v_final_name := COALESCE(
    NULLIF(btrim(p_device_name), ''),
    'ESP32 - ' || right(v_mac_norm, 8)
  );
  v_final_location := COALESCE(
    NULLIF(btrim(p_location), ''),
    'Localização não especificada'
  );

  v_user_ensured := public.ensure_public_user(v_email, v_final_name);

  v_ip := NULL;
  IF p_ip_address IS NOT NULL AND btrim(p_ip_address) <> '' THEN
    BEGIN
      v_ip := btrim(p_ip_address)::inet;
    EXCEPTION
      WHEN others THEN
        v_ip := NULL;
    END;
  END IF;

  v_owner_changed := (
    NOT v_is_new_device
    AND v_previous_email IS NOT NULL
    AND btrim(v_previous_email) <> ''
    AND v_previous_email <> v_email
  );

  INSERT INTO public.device_status (
    device_id,
    mac_address,
    user_email,
    device_name,
    location,
    ip_address,
    device_type,
    reboot_count,
    last_seen,
    is_online,
    updated_at
  )
  VALUES (
    btrim(p_device_id),
    btrim(p_mac_address),
    v_email,
    v_final_name,
    v_final_location,
    v_ip,
    'ESP32_HYDROPONIC',
    0,
    now(),
    true,
    now()
  )
  ON CONFLICT (device_id) DO UPDATE SET
    mac_address = EXCLUDED.mac_address,
    device_name = COALESCE(EXCLUDED.device_name, device_status.device_name),
    location = COALESCE(EXCLUDED.location, device_status.location),
    ip_address = COALESCE(EXCLUDED.ip_address, device_status.ip_address),
    device_type = COALESCE(EXCLUDED.device_type, device_status.device_type),
    user_email = EXCLUDED.user_email,
    reboot_count = CASE
      WHEN v_owner_changed THEN 0
      ELSE device_status.reboot_count
    END,
    last_seen = now(),
    is_online = true,
    updated_at = now();

  SELECT lower(btrim(user_email))
    INTO v_stored_email
    FROM public.device_status
   WHERE device_id = btrim(p_device_id);

  SELECT count(*)::integer
    INTO v_count
    FROM public.device_status
   WHERE lower(user_email) = v_email;

  v_user_total_devices := public.sync_user_total_devices(v_email);

  IF v_previous_email IS NOT NULL
     AND btrim(v_previous_email) <> ''
     AND v_previous_email <> v_email THEN
    v_prev_user_total_devices := public.sync_user_total_devices(v_previous_email);
  END IF;

  RETURN jsonb_build_object(
    'success', true,
    'message', 'Dispositivo registrado com sucesso',
    'device_id', btrim(p_device_id),
    'user_email', v_email,
    'stored_user_email', v_stored_email,
    'email_applied', (v_stored_email = v_email),
    'owner_changed', v_owner_changed,
    'previous_owner_email', COALESCE(v_previous_email, ''),
    'device_count', v_count,
    'total_devices', v_user_total_devices,
    'previous_owner_total_devices', COALESCE(v_prev_user_total_devices, 0),
    'user_profile_ensured', v_user_ensured,
    'reboot_count_reset', (v_is_new_device OR v_owner_changed)
  );
EXCEPTION
  WHEN others THEN
    RETURN jsonb_build_object(
      'success', false,
      'message', SQLERRM
    );
END;
$$;

GRANT EXECUTE ON FUNCTION public.register_device_with_email(
  text, text, text, text, text, text
) TO anon, authenticated, service_role;
