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
        return await createCommand(req, res)
      case 'GET':
        return await getCommands(req, res)
      case 'PUT':
        return await updateCommandStatus(req, res)
      default:
        return res.status(405).json({ error: 'Método não permitido' })
    }
  } catch (error) {
    console.error('❌ Erro na API relay-commands:', error)
    return res.status(500).json({ error: 'Erro interno do servidor' })
  }
}

// �� CRIAR NOVO COMANDO (Interface Web → Supabase)
async function createCommand(req, res) {
  const { 
    device_id = process.env.DEFAULT_DEVICE_ID || 'ESP32_HIDRO_001',
    relay_number, 
    action, 
    duration_seconds, 
    created_by = 'web_interface' 
  } = req.body

  // Validações
  if (relay_number === undefined || !action) {
    return res.status(400).json({ 
      error: 'relay_number e action são obrigatórios' 
    })
  }

  if (relay_number < 0 || relay_number > 15) {
    return res.status(400).json({ 
      error: 'relay_number deve estar entre 0 e 15' 
    })
  }

  if (!['on', 'off'].includes(action.toLowerCase())) {
    return res.status(400).json({ 
      error: 'action deve ser "on" ou "off"' 
    })
  }

  // Inserir comando no Supabase
  const { data, error } = await supabase
    .from('relay_commands')
    .insert({
      device_id,
      relay_number,
      action: action.toLowerCase(),
      duration_seconds: duration_seconds || null,
      created_by,
      status: 'pending'
    })
    .select()
    .single()

  if (error) {
    console.error('❌ Erro ao criar comando:', error)
    return res.status(500).json({ error: 'Erro ao criar comando' })
  }

  // Log do comando criado
  console.log('🔌 ===== NOVO COMANDO CRIADO =====')
  console.log('📅 ID:', data.id)
  console.log('🔧 Device:', device_id)
  console.log('🔢 Relé:', relay_number)
  console.log('⚡ Ação:', action)
  console.log('⏱️ Duração:', duration_seconds || 'Indefinida')
  console.log('👤 Criado por:', created_by)
  console.log('==============================\n')

  return res.status(201).json({
    success: true,
    message: 'Comando criado com sucesso',
    command: data
  })
}

// 📥 BUSCAR COMANDOS PENDENTES (ESP32 → Supabase)
async function getCommands(req, res) {
  const { 
    device_id = process.env.DEFAULT_DEVICE_ID || 'ESP32_HIDRO_001',
    status = 'pending' 
  } = req.query

  // Buscar comandos pendentes (mesma query que o ESP32 usa)
  const { data, error } = await supabase
    .from('relay_commands')
    .select('*')
    .eq('device_id', device_id)
    .eq('status', status)
    .order('created_at', { ascending: true })

  if (error) {
    console.error('❌ Erro ao buscar comandos:', error)
    return res.status(500).json({ error: 'Erro ao buscar comandos' })
  }

  // Marcar comandos como 'sent' automaticamente
  if (data.length > 0) {
    const commandIds = data.map(cmd => cmd.id)
    
    const { error: updateError } = await supabase
      .from('relay_commands')
      .update({ 
        status: 'sent', 
        sent_at: new Date().toISOString() 
      })
      .in('id', commandIds)

    if (updateError) {
      console.error('❌ Erro ao marcar comandos como enviados:', updateError)
    } else {
      console.log(`📤 ${data.length} comando(s) enviado(s) para ${device_id}`)
    }
  }

  return res.status(200).json({
    success: true,
    commands: data
  })
}

// 🔄 ATUALIZAR STATUS DO COMANDO (ESP32 → Supabase)
async function updateCommandStatus(req, res) {
  const { command_id, status, error_message } = req.body

  if (!command_id || !status) {
    return res.status(400).json({ 
      error: 'command_id e status são obrigatórios' 
    })
  }

  const validStatuses = ['pending', 'processing', 'sent', 'completed', 'failed', 'expired']
  if (!validStatuses.includes(status)) {
    return res.status(400).json({ 
      error: `Status deve ser um de: ${validStatuses.join(', ')}` 
    })
  }

  // Preparar dados de atualização
  const updateData = { status }
  
  if (status === 'completed') {
    updateData.completed_at = new Date().toISOString()
  } else if (status === 'failed') {
    updateData.completed_at = new Date().toISOString()
    updateData.error_message = error_message || 'Erro não especificado'
  }

  // Atualizar comando
  const { data, error } = await supabase
    .from('relay_commands')
    .update(updateData)
    .eq('id', command_id)
    .select()
    .single()

  if (error) {
    console.error('❌ Erro ao atualizar comando:', error)
    return res.status(500).json({ error: 'Erro ao atualizar comando' })
  }

  // Log da atualização
  console.log('🔄 ===== COMANDO ATUALIZADO =====')
  console.log('📅 ID:', command_id)
  console.log('📊 Status:', status)
  if (error_message) {
    console.log('❌ Erro:', error_message)
  }
  console.log('==============================\n')

  return res.status(200).json({
    success: true,
    message: 'Status atualizado com sucesso',
    command: data
  })
} 