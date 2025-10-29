// pages/api/relay/check.js

// Importar o Map de comandos pendentes do endpoint principal
import { pendingCommands } from '../relay'

export default async function handler(req, res) {
  if (req.method !== 'POST' && req.method !== 'GET') {
    return res.status(405).json({ error: 'Método não permitido' })
  }

  try {
    const { device_id } = req.method === 'GET' ? req.query : req.body

    if (!device_id) {
      return res.status(400).json({ error: 'device_id obrigatório' })
    }

    // GET: Verificar comandos pendentes
    if (req.method === 'GET') {
      const commands = pendingCommands.get(device_id) || []
      const pendingCommand = commands.find(cmd => cmd.status === 'pending')

      return res.status(200).json({
        success: true,
        has_pending: !!pendingCommand,
        command: pendingCommand || null
      })
    }

    // POST: Atualizar status do comando
    const { command_id, status, error_message } = req.body

    if (!command_id || !status) {
      return res.status(400).json({ error: 'command_id e status são obrigatórios' })
    }

    const commands = pendingCommands.get(device_id) || []
    const commandIndex = commands.findIndex(cmd => cmd.id === command_id)

    if (commandIndex === -1) {
      return res.status(404).json({ error: 'Comando não encontrado' })
    }

    // Atualizar status do comando
    commands[commandIndex].status = status
    if (error_message) {
      commands[commandIndex].error = error_message
    }
    commands[commandIndex].updated_at = new Date().toISOString()

    // Se comando foi executado ou falhou, remover da lista após 5 minutos
    if (status === 'completed' || status === 'failed') {
      setTimeout(() => {
        const commands = pendingCommands.get(device_id) || []
        const filteredCommands = commands.filter(cmd => cmd.id !== command_id)
        pendingCommands.set(device_id, filteredCommands)
      }, 300000) // 5 minutos
    }

    // Log da atualização
    console.log('🔄 ===== ATUALIZAÇÃO COMANDO =====')
    console.log('📅 Timestamp:', new Date().toISOString())
    console.log('🔧 Device:', device_id)
    console.log('🔢 Comando ID:', command_id)
    console.log('📊 Status:', status)
    if (error_message) {
      console.log('❌ Erro:', error_message)
    }
    console.log('==============================\n')

    res.status(200).json({
      success: true,
      message: 'Status do comando atualizado',
      command: commands[commandIndex]
    })

  } catch (error) {
    console.error('❌ Erro no endpoint relay/check:', error)
    res.status(500).json({ error: 'Erro interno do servidor' })
  }
} 