import { createClient } from '@supabase/supabase-js'

// Inicializar cliente Supabase
const supabase = createClient(
  process.env.NEXT_PUBLIC_SUPABASE_URL,
  process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY
)

export default async function handler(req, res) {
  const { method } = req

  try {
    switch (method) {
      case 'POST':
        return await updateDeviceStatus(req, res)
      case 'GET':
        return await getDeviceStatus(req, res)
      default:
        return res.status(405).json({ error: 'Método não permitido' })
    }
  } catch (error) {
    console.error('❌ Erro na API device-status-unified:', error)
    return res.status(500).json({ error: 'Erro interno do servidor' })
  }
}

// 📤 ATUALIZAR STATUS DO DISPOSITIVO (ESP32)
async function updateDeviceStatus(req, res) {
  const {
    device_id,
    wifi_rssi,
    free_heap,
    uptime_seconds,
    relay_states,
    firmware_version,
    ip_address,
    errors = []
  } = req.body

  if (!device_id) {
    return res.status(400).json({ error: 'device_id obrigatório' })
  }

  // Validar relay_states (deve ser array de 16 booleanos)
  let validatedRelayStates = relay_states
  if (relay_states && Array.isArray(relay_states)) {
    // Garantir que temos exatamente 16 estados
    validatedRelayStates = [...relay_states]
    while (validatedRelayStates.length < 16) {
      validatedRelayStates.push(false)
    }
    validatedRelayStates = validatedRelayStates.slice(0, 16)
  } else {
    // Estados padrão (todos desligados)
    validatedRelayStates = new Array(16).fill(false)
  }

  // Preparar dados para atualização
  const statusData = {
    device_id,
    last_seen: new Date().toISOString(),
    wifi_rssi,
    free_heap,
    uptime_seconds,
    relay_states: validatedRelayStates,
    is_online: true,
    firmware_version,
    ip_address,
    updated_at: new Date().toISOString()
  }

  // Atualizar ou inserir status no Supabase
  const { data, error } = await supabase
    .from('device_status')
    .upsert(statusData)
    .select()
    .single()

  if (error) {
    console.error('❌ Erro ao atualizar status:', error)
    return res.status(500).json({ error: 'Erro ao atualizar status' })
  }

  // Log do status atualizado
  console.log('🔄 ===== STATUS DISPOSITIVO ATUALIZADO =====')
  console.log('📅 Timestamp:', statusData.updated_at)
  console.log('🔧 Device:', device_id)
  console.log('📡 WiFi RSSI:', wifi_rssi, 'dBm')
  console.log('💾 Heap livre:', free_heap, 'bytes')
  console.log('⏰ Uptime:', uptime_seconds, 'segundos')
  console.log('🌐 IP:', ip_address)
  console.log('📦 Firmware:', firmware_version)
  
  // Log dos relés ativos
  const activeRelays = validatedRelayStates
    .map((state, index) => state ? index : null)
    .filter(index => index !== null)
  
  console.log('🔌 Relés ativos:', activeRelays.length > 0 ? activeRelays.join(', ') : 'Nenhum')
  
  if (errors.length > 0) {
    console.log('❌ Erros:', errors.length)
  }
  console.log('=========================================\n')

  return res.status(200).json({
    success: true,
    message: 'Status atualizado com sucesso',
    data
  })
}

// 📥 OBTER STATUS DO DISPOSITIVO
async function getDeviceStatus(req, res) {
  const { device_id } = req.query

  if (!device_id) {
    return res.status(400).json({ error: 'device_id obrigatório' })
  }

  // Buscar status atual
  const { data, error } = await supabase
    .from('device_status')
    .select('*')
    .eq('device_id', device_id)
    .single()

  if (error) {
    if (error.code === 'PGRST116') {
      // Dispositivo não encontrado - criar entrada padrão
      const defaultStatus = {
        device_id,
        is_online: false,
        relay_states: new Array(16).fill(false),
        created_at: new Date().toISOString(),
        updated_at: new Date().toISOString()
      }

      const { data: newData, error: insertError } = await supabase
        .from('device_status')
        .insert(defaultStatus)
        .select()
        .single()

      if (insertError) {
        console.error('❌ Erro ao criar status padrão:', insertError)
        return res.status(500).json({ error: 'Erro ao criar status do dispositivo' })
      }

      return res.status(200).json({
        success: true,
        data: newData,
        message: 'Status padrão criado'
      })
    }

    console.error('❌ Erro ao buscar status:', error)
    return res.status(500).json({ error: 'Erro ao buscar status' })
  }

  // Verificar se dispositivo está online (última atualização < 2 minutos)
  const lastSeen = new Date(data.last_seen)
  const now = new Date()
  const isOnline = (now - lastSeen) < (2 * 60 * 1000) // 2 minutos

  // Atualizar status online se necessário
  if (data.is_online !== isOnline) {
    await supabase
      .from('device_status')
      .update({ is_online: isOnline })
      .eq('device_id', device_id)
    
    data.is_online = isOnline
  }

  return res.status(200).json({
    success: true,
    data,
    online_status: {
      is_online: isOnline,
      last_seen: data.last_seen,
      minutes_since_last_seen: Math.floor((now - lastSeen) / (60 * 1000))
    }
  })
} 