import { useState, useEffect } from 'react'
import Head from 'next/head'

export default function ESP32Control() {
  const [relayStates, setRelayStates] = useState(Array(16).fill(false))
  const [loading, setLoading] = useState({})
  const [timers, setTimers] = useState({})
  const [deviceStatus, setDeviceStatus] = useState({
    is_online: false,
    last_seen: null,
    wifi_rssi: null,
    free_heap: null,
    uptime_seconds: null,
    firmware_version: null,
    ip_address: null
  })
  const [commands, setCommands] = useState([])
  
  const deviceId = 'ESP32_HIDRO_001'
  
  // Buscar status do dispositivo a cada 5 segundos
  useEffect(() => {
    const fetchDeviceStatus = async () => {
      try {
        const response = await fetch(`/api/device-status-unified?device_id=${deviceId}`)
        const data = await response.json()
        
        if (data.success) {
          setDeviceStatus(data.data)
          setRelayStates(data.data.relay_states || Array(16).fill(false))
        }
      } catch (error) {
        console.error('Erro ao buscar status:', error)
      }
    }
    
    fetchDeviceStatus()
    const interval = setInterval(fetchDeviceStatus, 5000)
    return () => clearInterval(interval)
  }, [])

  // Buscar histórico de comandos
  useEffect(() => {
    const fetchCommands = async () => {
      try {
        const response = await fetch(`/api/relay-commands?device_id=${deviceId}`)
        const data = await response.json()
        
        if (data.success) {
          setCommands(data.commands || [])
        }
      } catch (error) {
        console.error('Erro ao buscar comandos:', error)
      }
    }
    
    fetchCommands()
  }, [])
  
  // Controlar relé
  const controlRelay = async (relayNumber, action, duration = null) => {
    setLoading({ ...loading, [relayNumber]: true })
    
    try {
      const response = await fetch('/api/relay-commands', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device_id: deviceId,
          relay_number: relayNumber,
          action,
          duration_seconds: duration,
          created_by: 'web_interface'
        })
      })
      
      const data = await response.json()
      
      if (data.success) {
        alert(`✅ Comando enviado: Relé ${relayNumber} ${action.toUpperCase()}`)
        
        // Atualizar lista de comandos
        setCommands(prev => [data.command, ...prev.slice(0, 9)])
      } else {
        alert('❌ Erro: ' + data.error)
      }
    } catch (error) {
      alert('❌ Erro de comunicação: ' + error.message)
    } finally {
      setLoading({ ...loading, [relayNumber]: false })
    }
  }
  
  // Formatar tempo
  const formatUptime = (seconds) => {
    if (!seconds) return 'N/A'
    const hours = Math.floor(seconds / 3600)
    const minutes = Math.floor((seconds % 3600) / 60)
    return `${hours}h ${minutes}m`
  }
  
  // Formatar timestamp
  const formatTimestamp = (timestamp) => {
    if (!timestamp) return 'N/A'
    return new Date(timestamp).toLocaleString('pt-BR')
  }

  return (
    <>
      <Head>
        <title>ESP32 Controle de Relés - HydroWave</title>
        <meta name="description" content="Controle remoto dos relés ESP32" />
      </Head>

      <div className="min-h-screen bg-gray-50 py-8">
        <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8">
          
          {/* Header */}
          <div className="mb-8">
            <h1 className="text-3xl font-bold text-gray-900">
              🔌 Controle ESP32 - Relés
            </h1>
            <p className="mt-2 text-gray-600">
              Controle remoto dos relés do sistema hidropônico
            </p>
          </div>

          {/* Status do Dispositivo */}
          <div className="bg-white rounded-lg shadow-md p-6 mb-8">
            <h2 className="text-xl font-semibold mb-4 flex items-center">
              📊 Status do Dispositivo
              <span className={`ml-3 px-3 py-1 rounded-full text-sm font-medium ${
                deviceStatus.is_online 
                  ? 'bg-green-100 text-green-800' 
                  : 'bg-red-100 text-red-800'
              }`}>
                {deviceStatus.is_online ? '🟢 Online' : '🔴 Offline'}
              </span>
            </h2>
            
            <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
              <div>
                <p className="text-sm text-gray-500">Última Conexão</p>
                <p className="font-medium">{formatTimestamp(deviceStatus.last_seen)}</p>
              </div>
              <div>
                <p className="text-sm text-gray-500">WiFi RSSI</p>
                <p className="font-medium">{deviceStatus.wifi_rssi || 'N/A'} dBm</p>
              </div>
              <div>
                <p className="text-sm text-gray-500">Memória Livre</p>
                <p className="font-medium">
                  {deviceStatus.free_heap ? Math.round(deviceStatus.free_heap / 1024) + ' KB' : 'N/A'}
                </p>
              </div>
              <div>
                <p className="text-sm text-gray-500">Uptime</p>
                <p className="font-medium">{formatUptime(deviceStatus.uptime_seconds)}</p>
              </div>
              <div>
                <p className="text-sm text-gray-500">IP Address</p>
                <p className="font-medium">{deviceStatus.ip_address || 'N/A'}</p>
              </div>
              <div>
                <p className="text-sm text-gray-500">Firmware</p>
                <p className="font-medium">{deviceStatus.firmware_version || 'N/A'}</p>
              </div>
            </div>
          </div>

          {/* Grid de Relés */}
          <div className="bg-white rounded-lg shadow-md p-6 mb-8">
            <h2 className="text-xl font-semibold mb-6">🔌 Controle dos Relés</h2>
            
            <div className="grid grid-cols-2 md:grid-cols-4 lg:grid-cols-8 gap-4">
              {Array.from({ length: 16 }, (_, i) => (
                <div key={i} className="border rounded-lg p-4 bg-gray-50">
                  <div className="text-center mb-3">
                    <h3 className="font-bold text-sm">Relé {i}</h3>
                    <div className={`w-6 h-6 rounded-full mx-auto mt-2 ${
                      relayStates[i] ? 'bg-green-500' : 'bg-gray-300'
                    }`}></div>
                    <p className="text-xs mt-1 font-medium">
                      {relayStates[i] ? 'ON' : 'OFF'}
                    </p>
                  </div>
                  
                  <div className="space-y-2">
                    {/* Botões ON/OFF */}
                    <div className="flex gap-1">
                      <button
                        onClick={() => controlRelay(i, 'on')}
                        disabled={loading[i] || !deviceStatus.is_online}
                        className="flex-1 bg-green-500 hover:bg-green-600 text-white px-2 py-1 rounded text-xs font-medium disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
                      >
                        ON
                      </button>
                      <button
                        onClick={() => controlRelay(i, 'off')}
                        disabled={loading[i] || !deviceStatus.is_online}
                        className="flex-1 bg-red-500 hover:bg-red-600 text-white px-2 py-1 rounded text-xs font-medium disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
                      >
                        OFF
                      </button>
                    </div>
                    
                    {/* Timer */}
                    <div className="flex gap-1">
                      <input
                        type="number"
                        placeholder="Seg"
                        value={timers[i] || ''}
                        onChange={(e) => setTimers({ ...timers, [i]: e.target.value })}
                        className="flex-1 border rounded px-2 py-1 text-xs"
                        min="1"
                        max="86400"
                      />
                      <button
                        onClick={() => {
                          const duration = parseInt(timers[i] || '0')
                          if (duration > 0) {
                            controlRelay(i, 'on', duration)
                            setTimers({ ...timers, [i]: '' })
                          }
                        }}
                        disabled={loading[i] || !deviceStatus.is_online || !timers[i]}
                        className="bg-blue-500 hover:bg-blue-600 text-white px-2 py-1 rounded text-xs disabled:opacity-50 disabled:cursor-not-allowed transition-colors"
                        title="Ativar com timer"
                      >
                        ⏱️
                      </button>
                    </div>
                    
                    {loading[i] && (
                      <div className="text-center">
                        <div className="inline-block animate-spin rounded-full h-4 w-4 border-b-2 border-blue-500"></div>
                      </div>
                    )}
                  </div>
                </div>
              ))}
            </div>
          </div>

          {/* Histórico de Comandos */}
          <div className="bg-white rounded-lg shadow-md p-6">
            <h2 className="text-xl font-semibold mb-4">📋 Últimos Comandos</h2>
            
            {commands.length > 0 ? (
              <div className="overflow-x-auto">
                <table className="min-w-full divide-y divide-gray-200">
                  <thead className="bg-gray-50">
                    <tr>
                      <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">
                        Relé
                      </th>
                      <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">
                        Ação
                      </th>
                      <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">
                        Duração
                      </th>
                      <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">
                        Status
                      </th>
                      <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">
                        Criado em
                      </th>
                    </tr>
                  </thead>
                  <tbody className="bg-white divide-y divide-gray-200">
                    {commands.slice(0, 10).map((command) => (
                      <tr key={command.id}>
                        <td className="px-6 py-4 whitespace-nowrap text-sm font-medium text-gray-900">
                          Relé {command.relay_number}
                        </td>
                        <td className="px-6 py-4 whitespace-nowrap text-sm text-gray-500">
                          <span className={`px-2 py-1 rounded-full text-xs font-medium ${
                            command.action === 'on' 
                              ? 'bg-green-100 text-green-800' 
                              : 'bg-red-100 text-red-800'
                          }`}>
                            {command.action.toUpperCase()}
                          </span>
                        </td>
                        <td className="px-6 py-4 whitespace-nowrap text-sm text-gray-500">
                          {command.duration_seconds ? `${command.duration_seconds}s` : 'Indefinido'}
                        </td>
                        <td className="px-6 py-4 whitespace-nowrap text-sm text-gray-500">
                          <span className={`px-2 py-1 rounded-full text-xs font-medium ${
                            command.status === 'completed' ? 'bg-green-100 text-green-800' :
                            command.status === 'failed' ? 'bg-red-100 text-red-800' :
                            command.status === 'sent' ? 'bg-yellow-100 text-yellow-800' :
                            'bg-gray-100 text-gray-800'
                          }`}>
                            {command.status}
                          </span>
                        </td>
                        <td className="px-6 py-4 whitespace-nowrap text-sm text-gray-500">
                          {formatTimestamp(command.created_at)}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            ) : (
              <p className="text-gray-500 text-center py-8">
                Nenhum comando encontrado
              </p>
            )}
          </div>
        </div>
      </div>
    </>
  )
} 