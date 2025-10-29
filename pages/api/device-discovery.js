// API para descubrimiento y gestión de múltiples ESP32
import { createClient } from '@supabase/supabase-js'

const supabaseUrl = process.env.NEXT_PUBLIC_SUPABASE_URL
const supabaseKey = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY
const supabase = createClient(supabaseUrl, supabaseKey)
export default async function handler(req, res) {
    const { method } = req;

    switch (method) {
        case 'GET':
            return handleDeviceDiscovery(req, res);
        case 'POST':
            return handleDeviceRegistration(req, res);
        case 'PUT':
            return handleDeviceUpdate(req, res);
        case 'DELETE':
            return handleDeviceRemoval(req, res);
        default:
            res.setHeader('Allow', ['GET', 'POST', 'PUT', 'DELETE']);
            return res.status(405).end(`Method ${method} Not Allowed`);
    }
}

// Descubrimiento de dispositivos
async function handleDeviceDiscovery(req, res) {
    try {
        const { type, location, status } = req.query;
        
        // Buscar en base de datos
        const devices = await discoverDevicesFromDB({ type, location, status });
        
        // Descubrimiento en red local (mDNS/Bonjour)
        const networkDevices = await discoverDevicesInNetwork();
        
        // Combinar resultados
        const allDevices = [...devices, ...networkDevices];
        
        res.status(200).json({
            success: true,
            devices: allDevices,
            total: allDevices.length,
            timestamp: new Date().toISOString()
        });
    } catch (error) {
        console.error('Error en descubrimiento:', error);
        res.status(500).json({ success: false, error: error.message });
    }
}

// Registro de nuevo dispositivo
async function handleDeviceRegistration(req, res) {
    try {
        const deviceData = req.body;
        
        // Validar datos requeridos
        const requiredFields = ['device_id', 'device_name', 'location', 'ip_address'];
        const missingFields = requiredFields.filter(field => !deviceData[field]);
        
        if (missingFields.length > 0) {
            return res.status(400).json({
                success: false,
                error: `Campos requeridos faltantes: ${missingFields.join(', ')}`
            });
        }
        
        // Verificar si el dispositivo ya existe
        const existingDevice = await findDeviceByID(deviceData.device_id);
        if (existingDevice) {
            return res.status(409).json({
                success: false,
                error: 'Dispositivo ya registrado'
            });
        }
        
        // Registrar dispositivo
        const newDevice = await registerDevice(deviceData);
        
        res.status(201).json({
            success: true,
            device: newDevice,
            message: 'Dispositivo registrado exitosamente'
        });
    } catch (error) {
        console.error('Error en registro:', error);
        res.status(500).json({ success: false, error: error.message });
    }
}

// Actualización de dispositivo
async function handleDeviceUpdate(req, res) {
    try {
        const { device_id } = req.query;
        const updateData = req.body;
        
        if (!device_id) {
            return res.status(400).json({
                success: false,
                error: 'device_id es requerido'
            });
        }
        
        const updatedDevice = await updateDevice(device_id, updateData);
        
        if (!updatedDevice) {
            return res.status(404).json({
                success: false,
                error: 'Dispositivo no encontrado'
            });
        }
        
        res.status(200).json({
            success: true,
            device: updatedDevice,
            message: 'Dispositivo actualizado exitosamente'
        });
    } catch (error) {
        console.error('Error en actualización:', error);
        res.status(500).json({ success: false, error: error.message });
    }
}

// Eliminación de dispositivo
async function handleDeviceRemoval(req, res) {
    try {
        const { device_id } = req.query;
        
        if (!device_id) {
            return res.status(400).json({
                success: false,
                error: 'device_id es requerido'
            });
        }
        
        const deleted = await removeDevice(device_id);
        
        if (!deleted) {
            return res.status(404).json({
                success: false,
                error: 'Dispositivo no encontrado'
            });
        }
        
        res.status(200).json({
            success: true,
            message: 'Dispositivo eliminado exitosamente'
        });
    } catch (error) {
        console.error('Error en eliminación:', error);
        res.status(500).json({ success: false, error: error.message });
    }
}

// Funciones auxiliares
async function discoverDevicesFromDB(filters = {}) {
    // Implementar consulta a Supabase
    const { data, error } = await supabase
        .from('device_status')
        .select('*')
        .match(filters);
    
    if (error) throw error;
    return data || [];
}

async function discoverDevicesInNetwork() {
    // Implementar descubrimiento mDNS/Bonjour
    // Esto requiere una librería como 'mdns' o 'bonjour'
    return [];
}

async function findDeviceByID(device_id) {
    const { data, error } = await supabase
        .from('device_status')
        .select('*')
        .eq('device_id', device_id)
        .single();
    
    if (error && error.code !== 'PGRST116') throw error;
    return data;
}

async function registerDevice(deviceData) {
    const deviceRecord = {
        ...deviceData,
        created_at: new Date().toISOString(),
        updated_at: new Date().toISOString(),
        status: 'active',
        last_seen: new Date().toISOString()
    };
    
    const { data, error } = await supabase
        .from('device_status')
        .insert([deviceRecord])
        .select()
        .single();
    
    if (error) throw error;
    return data;
}

async function updateDevice(device_id, updateData) {
    const { data, error } = await supabase
        .from('device_status')
        .update({
            ...updateData,
            updated_at: new Date().toISOString()
        })
        .eq('device_id', device_id)
        .select()
        .single();
    
    if (error) throw error;
    return data;
}

async function removeDevice(device_id) {
    const { data, error } = await supabase
        .from('device_status')
        .delete()
        .eq('device_id', device_id)
        .select()
        .single();
    
    if (error) throw error;
    return data;
}
