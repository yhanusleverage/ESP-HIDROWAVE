// API para operações específicas de uma regra (GET, PUT, DELETE)
import { createClient } from '@supabase/supabase-js';

const supabaseUrl = process.env.NEXT_PUBLIC_SUPABASE_URL;
const supabaseKey = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY;

if (!supabaseUrl || !supabaseKey) {
  throw new Error('Defina NEXT_PUBLIC_SUPABASE_URL y NEXT_PUBLIC_SUPABASE_ANON_KEY en .env.local');
}

const supabase = createClient(supabaseUrl, supabaseKey);

export default async function handler(req, res) {
    const { id } = req.query;

    if (req.method === 'GET') {
        // Buscar regra específica
        try {
            const { data, error } = await supabase
                .from('decision_rules')
                .select('*')
                .eq('rule_id', id)
                .single();

            if (error) {
                return res.status(404).json({ error: 'Regra não encontrada' });
            }

            return res.status(200).json({ rule: data });
        } catch (error) {
            return res.status(500).json({ error: error.message });
        }
    }

    if (req.method === 'PUT') {
        // Atualizar regra
        try {
            const rule = req.body;
            
            const { data, error } = await supabase
                .from('decision_rules')
                .update({
                    rule_name: rule.name,
                    rule_description: rule.description,
                    rule_json: rule,
                    enabled: rule.enabled !== false,
                    priority: rule.priority || 50,
                    updated_at: new Date().toISOString()
                })
                .eq('rule_id', id)
                .select()
                .single();

            if (error) {
                return res.status(500).json({ error: error.message });
            }

            return res.status(200).json({ rule: data });
        } catch (error) {
            return res.status(500).json({ error: error.message });
        }
    }

    if (req.method === 'DELETE') {
        // Deletar regra
        try {
            const { error } = await supabase
                .from('decision_rules')
                .delete()
                .eq('rule_id', id);

            if (error) {
                return res.status(500).json({ error: error.message });
            }

            return res.status(200).json({ success: true });
        } catch (error) {
            return res.status(500).json({ error: error.message });
        }
    }

    return res.status(405).json({ error: 'Method not allowed' });
}

