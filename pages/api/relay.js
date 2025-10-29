// pages/api/relay.js

// Armazenamento em memória para comandos pendentes
const pendingCommands = new Map();

export default async function handler(req, res) {
  if (req.method !== 'POST' && req.method !== 'GET') {
    return res.status(405).json({ error: 'Método não permitido' })
  }

  try {
    const { device_id } = req.method === 'GET' ? req.query : req.body;

    if (!device_id) {
      return res.status(400).json({ error: 'device_id obrigatório' })
    }

    // GET: Obter comandos pendentes
    if (req.method === 'GET') {
      const commands = pendingCommands.get(device_id) || [];
      return res.status(200).json({ commands });
    }

    // POST: Enviar comando para relé
    const { relay_number, action, duration } = req.body;

    if (relay_number === undefined || !action) {
      return res.status(400).json({ error: 'relay_number e action são obrigatórios' })
    }

    // Validar parâmetros
    if (relay_number < 0 || relay_number > 7) {
      return res.status(400).json({ error: 'relay_number deve estar entre 0 e 7' })
    }

    if (!['on', 'off'].includes(action.toLowerCase())) {
      return res.status(400).json({ error: 'action deve ser "on" ou "off"' })
    }

    // Criar comando
    const command = {
      id: Date.now().toString(),
      relay_number,
      action: action.toLowerCase(),
      duration: duration || 0,
      timestamp: new Date().toISOString(),
      status: 'pending'
    }

    // Armazenar comando
    if (!pendingCommands.has(device_id)) {
      pendingCommands.set(device_id, []);
    }
    pendingCommands.get(device_id).push(command);

    // Log do comando
    console.log('🔌 ===== COMANDO RELÉ =====')
    console.log('📅 Timestamp:', command.timestamp)
    console.log('🔧 Device:', device_id)
    console.log('🔢 Relé:', relay_number)
    console.log('⚡ Ação:', action)
    if (duration) {
      console.log('⏱️ Duração:', duration, 'segundos')
    }
    console.log('========================\n')

    res.status(200).json({
      success: true,
      message: 'Comando enviado com sucesso',
      command
    })

  } catch (error) {
    console.error('❌ Erro no endpoint relay:', error)
    res.status(500).json({ error: 'Erro interno do servidor' })
  }
} 