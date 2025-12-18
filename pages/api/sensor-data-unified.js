import { createClient } from '@supabase/supabase-js'

// Inicializar cliente Supabase
const supabase = createClient(
  process.env.NEXT_PUBLIC_SUPABASE_URL,
  process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY
)

export default async function handler(req, res) {
  if (req.method !== 'POST') {
    return res.status(405).json({ error: 'Método não permitido' })
  }

  try {
    const {
      device_id,
      // Dados ambientais
      environment_temperature,
      environment_humidity,
      // Dados hidropônicos
      water_temperature,
      ph,
      tds,
      water_level_ok,
      timestamp
    } = req.body

    if (!device_id) {
      return res.status(400).json({ error: 'device_id obrigatório' })
    }

    const results = {}

    // 🌡️ INSERIR DADOS AMBIENTAIS (se fornecidos)
    if (environment_temperature !== undefined && environment_humidity !== undefined) {
      const { data: envData, error: envError } = await supabase
        .from('environment_data')
        .insert({
          device_id,
          temperature: environment_temperature,
          humidity: environment_humidity,
          created_at: timestamp || new Date().toISOString()
        })
        .select()
        .single()

      if (envError) {
        console.error('❌ Erro ao inserir dados ambientais:', envError)
        return res.status(500).json({ error: 'Erro ao salvar dados ambientais' })
      }

      results.environment = envData
    }

    // 🌊 INSERIR DADOS HIDROPÔNICOS (se fornecidos)
    if (water_temperature !== undefined || ph !== undefined || tds !== undefined || water_level_ok !== undefined) {
      const { data: hydroData, error: hydroError } = await supabase
        .from('hydro_measurements')
        .insert({
          device_id,
          temperature: water_temperature,
          ph: ph,
          tds: tds,
          water_level_ok: water_level_ok,
          created_at: timestamp || new Date().toISOString()
        })
        .select()
        .single()

      if (hydroError) {
        console.error('❌ Erro ao inserir dados hidropônicos:', hydroError)
        return res.status(500).json({ error: 'Erro ao salvar dados hidropônicos' })
      }

      results.hydro = hydroData
    }

    // 📊 ATUALIZAR STATUS DO DISPOSITIVO
    await updateDeviceStatus(device_id)

    // Log dos dados salvos
    console.log('📊 ===== DADOS DOS SENSORES SALVOS =====')
    console.log('📅 Timestamp:', timestamp || new Date().toISOString())
    console.log('🔧 Device:', device_id)
    
    if (results.environment) {
      console.log('🌡️ Ambiente:', environment_temperature, '°C /', environment_humidity, '%')
    }
    
    if (results.hydro) {
      console.log('🌊 Água:', water_temperature, '°C')
      console.log('⚗️ pH:', ph)
      console.log('🧪 TDS:', tds, 'ppm')
      console.log('📏 Nível:', water_level_ok ? 'OK' : 'BAIXO')
    }
    console.log('====================================\n')

    return res.status(200).json({
      success: true,
      message: 'Dados dos sensores salvos com sucesso',
      data: results
    })

  } catch (error) {
    console.error('❌ Erro no endpoint sensor-data-unified:', error)
    return res.status(500).json({ error: 'Erro interno do servidor' })
  }
}

// 🔄 ATUALIZAR STATUS DO DISPOSITIVO
async function updateDeviceStatus(device_id) {
  try {
    const { error } = await supabase
      .from('device_status')
      .upsert({
        device_id,
        last_seen: new Date().toISOString(),
        is_online: true,
        updated_at: new Date().toISOString()
      })

    if (error) {
      console.error('❌ Erro ao atualizar status do dispositivo:', error)
    }
  } catch (error) {
    console.error('❌ Erro na função updateDeviceStatus:', error)
  }
} 