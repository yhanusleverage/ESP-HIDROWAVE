import { useState } from 'react'

export default function TestRelays() {
  const [loading, setLoading] = useState(false)
  const [logs, setLogs] = useState([])

  const addLog = (message) => {
    setLogs(prev => [...prev, `${new Date().toLocaleTimeString()}: ${message}`])
  }

  const controlRelay = async (relayNumber, action, duration = null) => {
    setLoading(true)
    try {
      const response = await fetch('/api/relay', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ relayNumber, action, duration })
      })

      const result = await response.json()

      if (response.ok) {
        addLog(`✅ Relé ${relayNumber} ${action} ${duration ? `por ${duration}s` : ''}`)
      } else {
        addLog(`❌ Erro: ${result.error}`)
      }
    } catch (error) {
      addLog(`❌ Erro de conexão: ${error.message}`)
    } finally {
      setLoading(false)
    }
  }

  const relayNames = Array.from({ length: 16 }, (_, i) => `Relé ${i}`)

  return (
    <div className="p-8 max-w-6xl mx-auto">
      <h1 className="text-3xl font-bold mb-8">🌱 Controle ESP32 Hidropônico</h1>
      
      {/* Grid de Relés */}
      <div className="grid grid-cols-4 gap-4 mb-8">
        {Array.from({length: 16}, (_, i) => (
          <div key={i} className="border p-4 rounded-lg bg-gray-50 shadow">
            <h3 className="font-bold mb-2 text-sm">Relé {i}</h3>
            <p className="text-xs text-gray-600 mb-3">{relayNames[i]}</p>
            <div className="space-y-2">
              <button 
                onClick={() => controlRelay(i, 'on')}
                disabled={loading}
                className="w-full bg-green-500 text-white px-3 py-2 rounded text-sm hover:bg-green-600 disabled:opacity-50"
              >
                ON
              </button>
              <button 
                onClick={() => controlRelay(i, 'off')}
                disabled={loading}
                className="w-full bg-red-500 text-white px-3 py-2 rounded text-sm hover:bg-red-600 disabled:opacity-50"
              >
                OFF
              </button>
              <button 
                onClick={() => controlRelay(i, 'on', 10)}
                disabled={loading}
                className="w-full bg-blue-500 text-white px-3 py-2 rounded text-sm hover:bg-blue-600 disabled:opacity-50"
              >
                10s
              </button>
              <button 
                onClick={() => controlRelay(i, 'on', 60)}
                disabled={loading}
                className="w-full bg-purple-500 text-white px-3 py-2 rounded text-sm hover:bg-purple-600 disabled:opacity-50"
              >
                60s
              </button>
            </div>
          </div>
        ))}
      </div>

      {/* Controles Gerais */}
      <div className="mb-8 p-4 bg-blue-50 rounded-lg">
        <h3 className="font-bold mb-4">🎛️ Controles Gerais</h3>
        <div className="flex gap-4">
          <button 
            onClick={() => {
              for(let i = 0; i < 16; i++) {
                setTimeout(() => controlRelay(i, 'off'), i * 100)
              }
            }}
            disabled={loading}
            className="bg-red-600 text-white px-6 py-2 rounded hover:bg-red-700 disabled:opacity-50"
          >
            🚫 DESLIGAR TODOS
          </button>
          <button 
            onClick={() => {
              addLog('🔄 Testando comunicação com ESP32...')
              fetch('/api/relay/check?device_id=ESP32_HIDRO_001')
                .then(res => res.json())
                .then(data => {
                  addLog(`📡 Resposta: ${data.commands?.length || 0} comandos pendentes`)
                })
                .catch(err => addLog(`❌ Erro de comunicação: ${err.message}`))
            }}
            className="bg-blue-600 text-white px-6 py-2 rounded hover:bg-blue-700"
          >
            📡 TESTAR COMUNICAÇÃO
          </button>
        </div>
      </div>

      {/* Logs */}
      <div className="bg-black text-green-400 p-4 rounded-lg h-64 overflow-y-auto font-mono text-sm">
        <h3 className="text-white mb-2">📋 Logs de Atividade:</h3>
        {logs.length === 0 && (
          <div className="text-gray-500">Aguardando comandos...</div>
        )}
        {logs.map((log, index) => (
          <div key={index}>{log}</div>
        ))}
      </div>

      {/* Botões de Controle */}
      <div className="mt-4 flex gap-4">
        <button 
          onClick={() => setLogs([])}
          className="bg-gray-500 text-white px-4 py-2 rounded hover:bg-gray-600"
        >
          🗑️ Limpar Logs
        </button>
        <button 
          onClick={() => addLog('ℹ️ Sistema pronto para receber comandos ESP32')}
          className="bg-green-600 text-white px-4 py-2 rounded hover:bg-green-700"
        >
          ✅ Sistema OK
        </button>
      </div>

      {/* Informações */}
      <div className="mt-8 p-4 bg-yellow-50 rounded-lg">
        <h3 className="font-bold mb-2">ℹ️ Informações do Sistema</h3>
        <ul className="text-sm space-y-1">
          <li>• <strong>Device ID:</strong> ESP32_HIDRO_001</li>
          <li>• <strong>Relés:</strong> 16 relés via PCF8574 (0x20 e 0x24)</li>
          <li>• <strong>Comunicação:</strong> HTTP POST/GET a cada 5s</li>
          <li>• <strong>Logs:</strong> Verifique o console do navegador e logs do Vercel</li>
        </ul>
      </div>
    </div>
  )
} 