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
      envTemp,
      envHumidity,
      waterTemp,
      ph,
      tds,
      waterLevelOk,
      timestamp
    } = req.body

    if (!device_id) {
      return res.status(400).json({ error: 'device_id obrigatório' })
    }

    // Preparar dados para o Supabase
    const sensorData = {
      device_id,
      environment_temperature: envTemp,
      environment_humidity: envHumidity,
      water_temperature: waterTemp,
      ph_level: ph,
      tds_level: tds,
      water_level_ok: waterLevelOk,
      timestamp: timestamp || new Date().toISOString(),
      created_at: new Date().toISOString()
    }

    // Inserir dados no Supabase
    const { data, error } = await supabase
      .from('sensor_readings')
      .insert(sensorData)
      .select()

    if (error) throw error

    // Log dos dados
    console.log('📊 ===== DADOS DOS SENSORES =====')
    console.log('📅 Timestamp:', sensorData.timestamp)
    console.log('🔧 Device:', device_id)
    console.log('🌡️ Temp. Ambiente:', envTemp, '°C')
    console.log('💧 Umidade:', envHumidity, '%')
    console.log('🌊 Temp. Água:', waterTemp, '°C')
    console.log('⚗️ pH:', ph)
    console.log('🧪 TDS:', tds, 'ppm')
    console.log('📏 Nível água:', waterLevelOk ? 'OK' : 'BAIXO')
    console.log('==============================\n')

    res.status(200).json({
      success: true,
      message: 'Dados dos sensores salvos com sucesso',
      data: data[0]
    })

  } catch (error) {
    console.error('❌ Erro no endpoint sensor-data:', error)
    res.status(500).json({ error: 'Erro interno do servidor' })
  }
} 