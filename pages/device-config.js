import { useState, useEffect } from 'react';
import Head from 'next/head';

export default function DeviceConfig() {
    const [devices, setDevices] = useState([]);
    const [selectedDevice, setSelectedDevice] = useState(null);
    const [isLoading, setIsLoading] = useState(false);
    const [showAddForm, setShowAddForm] = useState(false);

    // Formulario para nuevo dispositivo
    const [newDevice, setNewDevice] = useState({
        device_id: '',
        device_name: '',
        location: '',
        device_type: 'ESP32_HYDROPONIC',
        firmware_version: '2.1.0',
        ip_address: '',
        mac_address: '',
        wifi_ssid: '',
        wifi_password: '',
        capabilities: {
            relays: 16,
            sensors: []
        }
    });

    useEffect(() => {
        loadDevices();
    }, []);

    const loadDevices = async () => {
        setIsLoading(true);
        try {
            const response = await fetch('/api/device-discovery');
            const data = await response.json();
            if (data.success) {
                setDevices(data.devices);
            }
        } catch (error) {
            console.error('Error cargando dispositivos:', error);
        } finally {
            setIsLoading(false);
        }
    };

    const handleAddDevice = async (e) => {
        e.preventDefault();
        setIsLoading(true);
        
        try {
            const response = await fetch('/api/device-discovery', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(newDevice),
            });
            
            const data = await response.json();
            if (data.success) {
                setDevices([...devices, data.device]);
                setShowAddForm(false);
                setNewDevice({
                    device_id: '',
                    device_name: '',
                    location: '',
                    device_type: 'ESP32_HYDROPONIC',
                    firmware_version: '2.1.0',
                    ip_address: '',
                    mac_address: '',
                    wifi_ssid: '',
                    wifi_password: '',
                    capabilities: {
                        relays: 16,
                        sensors: []
                    }
                });
            }
        } catch (error) {
            console.error('Error agregando dispositivo:', error);
        } finally {
            setIsLoading(false);
        }
    };

    const handleUpdateDevice = async (deviceId, updateData) => {
        try {
            const response = await fetch(`/api/device-discovery?device_id=${deviceId}`, {
                method: 'PUT',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(updateData),
            });
            
            const data = await response.json();
            if (data.success) {
                setDevices(devices.map(device => 
                    device.device_id === deviceId ? data.device : device
                ));
            }
        } catch (error) {
            console.error('Error actualizando dispositivo:', error);
        }
    };

    const handleDeleteDevice = async (deviceId) => {
        if (!confirm('¿Estás seguro de eliminar este dispositivo?')) return;
        
        try {
            const response = await fetch(`/api/device-discovery?device_id=${deviceId}`, {
                method: 'DELETE',
            });
            
            const data = await response.json();
            if (data.success) {
                setDevices(devices.filter(device => device.device_id !== deviceId));
            }
        } catch (error) {
            console.error('Error eliminando dispositivo:', error);
        }
    };

    const generateMAC = () => {
        const mac = Array.from({length: 6}, () => 
            Math.floor(Math.random() * 256).toString(16).padStart(2, '0')
        ).join(':').toUpperCase();
        setNewDevice({...newDevice, mac_address: mac});
    };

    return (
        <div className="device-config-page">
            <Head>
                <title>Configuración de Dispositivos - HydroWave</title>
            </Head>

            <div className="page-header">
                <h1>🔌 Configuración de Dispositivos</h1>
                <p>Gestiona múltiples ESP32 en tu sistema hidropónico</p>
            </div>

            {/* Lista de dispositivos */}
            <div className="devices-section">
                <div className="section-header">
                    <h2>Dispositivos Registrados</h2>
                    <button 
                        className="btn btn-primary"
                        onClick={() => setShowAddForm(true)}
                    >
                        ➕ Agregar Dispositivo
                    </button>
                </div>

                {isLoading ? (
                    <div className="loading">Cargando dispositivos...</div>
                ) : (
                    <div className="devices-grid">
                        {devices.map(device => (
                            <DeviceCard 
                                key={device.device_id}
                                device={device}
                                onUpdate={handleUpdateDevice}
                                onDelete={handleDeleteDevice}
                                onSelect={setSelectedDevice}
                            />
                        ))}
                    </div>
                )}
            </div>

            {/* Formulario para agregar dispositivo */}
            {showAddForm && (
                <div className="modal-overlay">
                    <div className="modal-content">
                        <div className="modal-header">
                            <h3>➕ Agregar Nuevo Dispositivo</h3>
                            <button 
                                className="close-btn"
                                onClick={() => setShowAddForm(false)}
                            >
                                ×
                            </button>
                        </div>
                        
                        <form onSubmit={handleAddDevice} className="device-form">
                            <div className="form-section">
                                <h4>📋 Informaciones Básicas</h4>
                                
                                <div className="form-group">
                                    <label>Nome do Dispositivo *</label>
                                    <input
                                        type="text"
                                        value={newDevice.device_name}
                                        onChange={(e) => setNewDevice({...newDevice, device_name: e.target.value})}
                                        placeholder="Ex: Estufa Principal"
                                        required
                                    />
                                </div>

                                <div className="form-group">
                                    <label>Device ID *</label>
                                    <input
                                        type="text"
                                        value={newDevice.device_id}
                                        onChange={(e) => setNewDevice({...newDevice, device_id: e.target.value})}
                                        placeholder="ESP32_HIDRO_001"
                                        required
                                    />
                                </div>

                                <div className="form-group">
                                    <label>Localização *</label>
                                    <input
                                        type="text"
                                        value={newDevice.location}
                                        onChange={(e) => setNewDevice({...newDevice, location: e.target.value})}
                                        placeholder="Ex: Estufa Principal, Reservatório"
                                        required
                                    />
                                </div>

                                <div className="form-group">
                                    <label>Tipo de Dispositivo</label>
                                    <select
                                        value={newDevice.device_type}
                                        onChange={(e) => setNewDevice({...newDevice, device_type: e.target.value})}
                                    >
                                        <option value="ESP32_HYDROPONIC">ESP32 Hidropônico</option>
                                        <option value="ESP32_SENSOR">ESP32 Sensor</option>
                                        <option value="ESP32_CONTROL">ESP32 Controle</option>
                                    </select>
                                </div>
                            </div>

                            <div className="form-section">
                                <h4>🌐 Configurações de Rede</h4>
                                
                                <div className="form-group">
                                    <label>Endereço IP *</label>
                                    <input
                                        type="text"
                                        value={newDevice.ip_address}
                                        onChange={(e) => setNewDevice({...newDevice, ip_address: e.target.value})}
                                        placeholder="192.168.1.100"
                                        required
                                    />
                                </div>

                                <div className="form-group">
                                    <label>Endereço MAC</label>
                                    <div className="input-group">
                                        <input
                                            type="text"
                                            value={newDevice.mac_address}
                                            onChange={(e) => setNewDevice({...newDevice, mac_address: e.target.value})}
                                            placeholder="AA:BB:CC:DD:EE:FF"
                                        />
                                        <button type="button" onClick={generateMAC} className="btn btn-secondary">
                                            Gerar
                                        </button>
                                    </div>
                                </div>

                                <div className="form-group">
                                    <label>WiFi SSID</label>
                                    <input
                                        type="text"
                                        value={newDevice.wifi_ssid}
                                        onChange={(e) => setNewDevice({...newDevice, wifi_ssid: e.target.value})}
                                        placeholder="HydroWave_2.4G"
                                    />
                                </div>

                                <div className="form-group">
                                    <label>Senha WiFi</label>
                                    <input
                                        type="password"
                                        value={newDevice.wifi_password}
                                        onChange={(e) => setNewDevice({...newDevice, wifi_password: e.target.value})}
                                        placeholder="••••••••"
                                    />
                                </div>
                            </div>

                            <div className="form-section">
                                <h4>🔧 Configuração de Hardware</h4>
                                
                                <div className="form-group">
                                    <label>Número de Relés</label>
                                    <select
                                        value={newDevice.capabilities.relays}
                                        onChange={(e) => setNewDevice({
                                            ...newDevice, 
                                            capabilities: {...newDevice.capabilities, relays: parseInt(e.target.value)}
                                        })}
                                    >
                                        <option value="8">8 Relés</option>
                                        <option value="16">16 Relés</option>
                                        <option value="32">32 Relés</option>
                                    </select>
                                </div>

                                <div className="form-group">
                                    <label>Sensores Conectados</label>
                                    <div className="checkbox-group">
                                        {['ph', 'tds', 'temperature', 'humidity', 'water_level'].map(sensor => (
                                            <label key={sensor} className="checkbox-label">
                                                <input
                                                    type="checkbox"
                                                    checked={newDevice.capabilities.sensors.includes(sensor)}
                                                    onChange={(e) => {
                                                        const sensors = e.target.checked
                                                            ? [...newDevice.capabilities.sensors, sensor]
                                                            : newDevice.capabilities.sensors.filter(s => s !== sensor);
                                                        setNewDevice({
                                                            ...newDevice,
                                                            capabilities: {...newDevice.capabilities, sensors}
                                                        });
                                                    }}
                                                />
                                                {sensor.charAt(0).toUpperCase() + sensor.slice(1)}
                                            </label>
                                        ))}
                                    </div>
                                </div>
                            </div>

                            <div className="form-actions">
                                <button type="button" className="btn btn-secondary" onClick={() => setShowAddForm(false)}>
                                    Cancelar
                                </button>
                                <button type="submit" className="btn btn-primary" disabled={isLoading}>
                                    {isLoading ? 'Guardando...' : '💾 Guardar Dispositivo'}
                                </button>
                            </div>
                        </form>
                    </div>
                </div>
            )}

            <style jsx>{`
                .device-config-page {
                    padding: 2rem;
                    max-width: 1200px;
                    margin: 0 auto;
                }

                .page-header {
                    text-align: center;
                    margin-bottom: 2rem;
                }

                .page-header h1 {
                    color: #2563eb;
                    margin-bottom: 0.5rem;
                }

                .section-header {
                    display: flex;
                    justify-content: space-between;
                    align-items: center;
                    margin-bottom: 1.5rem;
                }

                .devices-grid {
                    display: grid;
                    grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
                    gap: 1rem;
                }

                .device-card {
                    background: white;
                    border-radius: 8px;
                    padding: 1.5rem;
                    box-shadow: 0 2px 4px rgba(0,0,0,0.1);
                    border: 1px solid #e2e8f0;
                }

                .device-header {
                    display: flex;
                    justify-content: space-between;
                    align-items: center;
                    margin-bottom: 1rem;
                }

                .device-name {
                    font-weight: 600;
                    color: #1e293b;
                }

                .device-status {
                    padding: 0.25rem 0.5rem;
                    border-radius: 4px;
                    font-size: 0.8rem;
                    font-weight: 500;
                }

                .status-online {
                    background: #dcfce7;
                    color: #166534;
                }

                .status-offline {
                    background: #fef2f2;
                    color: #dc2626;
                }

                .device-info {
                    display: grid;
                    gap: 0.5rem;
                    margin-bottom: 1rem;
                }

                .info-row {
                    display: flex;
                    justify-content: space-between;
                    font-size: 0.9rem;
                }

                .info-label {
                    color: #64748b;
                }

                .info-value {
                    color: #1e293b;
                    font-weight: 500;
                }

                .device-actions {
                    display: flex;
                    gap: 0.5rem;
                }

                .btn {
                    padding: 0.5rem 1rem;
                    border: none;
                    border-radius: 4px;
                    cursor: pointer;
                    font-weight: 500;
                    transition: all 0.2s;
                }

                .btn-primary {
                    background: #2563eb;
                    color: white;
                }

                .btn-secondary {
                    background: #64748b;
                    color: white;
                }

                .btn-danger {
                    background: #dc2626;
                    color: white;
                }

                .btn:hover {
                    opacity: 0.9;
                    transform: translateY(-1px);
                }

                .modal-overlay {
                    position: fixed;
                    top: 0;
                    left: 0;
                    right: 0;
                    bottom: 0;
                    background: rgba(0,0,0,0.5);
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    z-index: 1000;
                }

                .modal-content {
                    background: white;
                    border-radius: 8px;
                    width: 90%;
                    max-width: 600px;
                    max-height: 90vh;
                    overflow-y: auto;
                }

                .modal-header {
                    display: flex;
                    justify-content: space-between;
                    align-items: center;
                    padding: 1rem 1.5rem;
                    border-bottom: 1px solid #e2e8f0;
                }

                .close-btn {
                    background: none;
                    border: none;
                    font-size: 1.5rem;
                    cursor: pointer;
                    color: #64748b;
                }

                .device-form {
                    padding: 1.5rem;
                }

                .form-section {
                    margin-bottom: 2rem;
                }

                .form-section h4 {
                    color: #2563eb;
                    margin-bottom: 1rem;
                    padding-bottom: 0.5rem;
                    border-bottom: 1px solid #e2e8f0;
                }

                .form-group {
                    margin-bottom: 1rem;
                }

                .form-group label {
                    display: block;
                    margin-bottom: 0.5rem;
                    font-weight: 500;
                    color: #374151;
                }

                .form-group input,
                .form-group select {
                    width: 100%;
                    padding: 0.75rem;
                    border: 1px solid #d1d5db;
                    border-radius: 4px;
                    font-size: 1rem;
                }

                .input-group {
                    display: flex;
                    gap: 0.5rem;
                }

                .input-group input {
                    flex: 1;
                }

                .checkbox-group {
                    display: grid;
                    grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
                    gap: 0.5rem;
                }

                .checkbox-label {
                    display: flex;
                    align-items: center;
                    gap: 0.5rem;
                    font-weight: normal;
                }

                .form-actions {
                    display: flex;
                    gap: 1rem;
                    justify-content: flex-end;
                    margin-top: 2rem;
                    padding-top: 1rem;
                    border-top: 1px solid #e2e8f0;
                }

                .loading {
                    text-align: center;
                    padding: 2rem;
                    color: #64748b;
                }
            `}</style>
        </div>
    );
}

// Componente para tarjeta de dispositivo
function DeviceCard({ device, onUpdate, onDelete, onSelect }) {
    return (
        <div className="device-card">
            <div className="device-header">
                <div className="device-name">{device.device_name}</div>
                <div className={`device-status ${device.status === 'online' ? 'status-online' : 'status-offline'}`}>
                    {device.status === 'online' ? '🟢 Online' : '🔴 Offline'}
                </div>
            </div>
            
            <div className="device-info">
                <div className="info-row">
                    <span className="info-label">ID:</span>
                    <span className="info-value">{device.device_id}</span>
                </div>
                <div className="info-row">
                    <span className="info-label">IP:</span>
                    <span className="info-value">{device.ip_address}</span>
                </div>
                <div className="info-row">
                    <span className="info-label">Ubicación:</span>
                    <span className="info-value">{device.location}</span>
                </div>
                <div className="info-row">
                    <span className="info-label">Relés:</span>
                    <span className="info-value">{device.capabilities?.relays || 0}</span>
                </div>
                <div className="info-row">
                    <span className="info-label">Sensores:</span>
                    <span className="info-value">{device.capabilities?.sensors?.length || 0}</span>
                </div>
            </div>
            
            <div className="device-actions">
                <button 
                    className="btn btn-primary"
                    onClick={() => onSelect(device)}
                >
                    ⚙️ Configurar
                </button>
                <button 
                    className="btn btn-secondary"
                    onClick={() => onUpdate(device.device_id, { status: 'online' })}
                >
                    🔄 Actualizar
                </button>
                <button 
                    className="btn btn-danger"
                    onClick={() => onDelete(device.device_id)}
                >
                    🗑️ Eliminar
                </button>
            </div>
        </div>
    );
}
