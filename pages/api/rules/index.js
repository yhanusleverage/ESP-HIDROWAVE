// API para gerenciar regras do motor de decisões
import { createClient } from '@supabase/supabase-js';

const supabaseUrl = process.env.NEXT_PUBLIC_SUPABASE_URL;
const supabaseKey = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY;

if (!supabaseUrl || !supabaseKey) {
  throw new Error('Defina NEXT_PUBLIC_SUPABASE_URL y NEXT_PUBLIC_SUPABASE_ANON_KEY en .env.local');
}

const supabase = createClient(supabaseUrl, supabaseKey);

export default async function handler(req, res) {
    if (req.method === 'GET') {
        // Listar regras
        try {
            const { data, error } = await supabase
                .from('decision_rules')
                .select('*')
                .order('priority', { ascending: false });

            if (error) {
                return res.status(500).json({ error: error.message });
            }

            return res.status(200).json({ rules: data || [] });
        } catch (error) {
            return res.status(500).json({ error: error.message });
        }
    }

    if (req.method === 'POST') {
        // Criar nova regra
        try {
            const rule = req.body;
            
            const { data, error } = await supabase
                .from('decision_rules')
                .insert([{
                    rule_id: rule.id,
                    rule_name: rule.name,
                    rule_description: rule.description,
                    rule_json: rule,
                    enabled: rule.enabled !== false,
                    priority: rule.priority || 50,
                    device_id: process.env.DEFAULT_DEVICE_ID || 'ESP32_HIDRO_001'
                }])
                .select()
                .single();

            if (error) {
                return res.status(500).json({ error: error.message });
            }

            return res.status(201).json({ rule: data });
        } catch (error) {
            return res.status(500).json({ error: error.message });
        }
    }

    return res.status(405).json({ error: 'Method not allowed' });
}

