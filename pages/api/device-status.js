import { createClient } from '@supabase/supabase-js'

// Inicializar cliente Supabase
const supabase = createClient(
  process.env.NEXT_PUBLIC_SUPABASE_URL,
  process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY
)

export default async function handler(req, res) {
  if (req.method !== 'POST' && req.method !== 'GET') {
    return res.status(405).json({ error: 'Método não permitido' })
  }

  try {
    // GET: Obter status atual do dispositivo
    if (req.method === 'GET') {
      const { device_id } = req.query
      
      if (!device_id) {
        return res.status(400).json({ error: 'device_id obrigatório' })
      }

      const { data, error } = await supabase
        .from('device_status')
        .select('*')
        .eq('device_id', device_id)
        .single()

      if (error) throw error

      return res.status(200).json(data || { online: false })
    }

    // POST: Atualizar status do dispositivo
    const {
      device_id,
      wifi_status,
      relay_states,
      sensor_status,
      last_update,
      ip_address,
      rssi,
      uptime,
      free_heap,
      errors
    } = req.body

    if (!device_id) {
      return res.status(400).json({ error: 'device_id obrigatório' })
    }

    // Preparar dados para atualização
    const statusData = {
      device_id,
      online: true,
      wifi_status,
      relay_states: relay_states || [],
      sensor_status: sensor_status || {},
      last_update: last_update || new Date().toISOString(),
      ip_address,
      rssi,
      uptime,
      free_heap,
      errors: errors || [],
      updated_at: new Date().toISOString()
    }

    // Atualizar ou inserir status no Supabase
    const { data, error } = await supabase
      .from('device_status')
      .upsert(statusData)
      .select()

    if (error) throw error

    // Log do status atualizado
    console.log('🔄 ===== STATUS DISPOSITIVO ATUALIZADO =====')
    console.log('📅 Timestamp:', statusData.updated_at)
    console.log('🔧 Device:', device_id)
    console.log('📡 WiFi:', wifi_status)
    console.log('🔌 Relés:', relay_states?.length || 0)
    console.log('🌡️ Sensores:', Object.keys(sensor_status || {}).length)
    console.log('⚡ RSSI:', rssi, 'dBm')
    console.log('💾 Heap:', free_heap, 'bytes')
    console.log('⏰ Uptime:', uptime, 'ms')
    if (errors?.length) {
      console.log('❌ Erros:', errors.length)
    }
    console.log('=========================================\n')

    res.status(200).json({
      success: true,
      message: 'Status atualizado com sucesso',
      data: data[0]
    })

  } catch (error) {
    console.error('❌ Erro no endpoint device-status:', error)
    res.status(500).json({ error: 'Erro interno do servidor' })
  }
} 