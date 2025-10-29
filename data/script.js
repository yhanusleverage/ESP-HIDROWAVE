let chart;
const maxDataPoints = 30;
let timeLabels = [];
let phData = [];
let ecData = [];

function initializeChart() {
    const ctx = document.getElementById('sensorChart').getContext('2d');
    chart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: timeLabels,
            datasets: [{
                label: 'pH',
                data: phData,
                borderColor: 'rgb(75, 192, 192)',
                tension: 0.4,
                yAxisID: 'y-ph'
            }, {
                label: 'EC (µS/cm)',
                data: ecData,
                borderColor: 'rgb(153, 102, 255)',
                tension: 0.4,
                yAxisID: 'y-ec'
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            scales: {
                'y-ph': {
                    type: 'linear',
                    position: 'left',
                    min: 4,
                    max: 7,
                    grid: {
                        color: 'rgba(255,255,255,0.1)'
                    },
                    ticks: {
                        color: 'white'
                    }
                },
                'y-ec': {
                    type: 'linear',
                    position: 'right',
                    min: 0,
                    max: 500,
                    grid: {
                        color: 'rgba(255,255,255,0.1)'
                    },
                    ticks: {
                        color: 'white'
                    }
                },
                x: {
                    ticks: {
                        color: 'white'
                    }
                }
            },
            plugins: {
                legend: {
                    labels: {
                        color: 'white'
                    }
                }
            }
        }
    });
}

function updateClock() {
    const now = new Date();
    document.getElementById('current-time').textContent = now.toLocaleTimeString();
}

function updateWiFiStrength(rssi) {
    const wifiText = document.getElementById('wifi-strength');
    let strength;
    let icon;
    
    if (rssi >= -50) {
        strength = 'Excelente';
        icon = '📶';
    } else if (rssi >= -70) {
        strength = 'Bom';
        icon = '📶';
    } else {
        strength = 'Fraco';
        icon = '📶';
    }
    
    wifiText.innerHTML = `${icon} WiFi: ${strength}`;
}

function updateChartScales() {
    if (ecData.length > 0 && phData.length > 0) {  // Verifica se há dados
        // Calcula limites para EC
        const maxEC = Math.max(...ecData) * 1.2;  
        const minEC = Math.min(...ecData) * 0.8;  

        // Calcula limites para pH
        const maxPH = Math.max(...phData) * 1.1;  
        const minPH = Math.max(0, Math.min(...phData) * 0.9);  

        // Atualiza as escalas
        chart.options.scales['y-ec'].max = maxEC;
        chart.options.scales['y-ec'].min = minEC;
        chart.options.scales['y-ph'].max = maxPH;
        chart.options.scales['y-ph'].min = minPH;

        console.log(`Escalas atualizadas - EC: ${minEC} a ${maxEC}, pH: ${minPH} a ${maxPH}`);
    }
}

function fetchSensorData() {
    fetch('/sensors')
        .then(response => response.json())
        .then(data => {
            const now = new Date().toLocaleTimeString();
            
            // Atualizar valores
            document.getElementById('temp-value').textContent = data.temperature.toFixed(1);
            document.getElementById('ph-value').textContent = data.ph.toFixed(2);
            document.getElementById('ec-value').textContent = data.ec.toFixed(0);
            document.getElementById('tds-value').textContent = data.tds.toFixed(0);
            
            // Atualizar gráfico
            timeLabels.push(now);
            phData.push(data.ph);
            ecData.push(data.ec);

            if (timeLabels.length > maxDataPoints) {
                timeLabels.shift();
                phData.shift();
                ecData.shift();
            }

            // Atualizar escalas dinamicamente
            updateChartScales();
            
            chart.update('none');
            
            // Atualizar status dos tanques
            document.getElementById('main-tank-status').textContent = 
                data.mainTankLow ? "BAIXO" : "OK";
            document.getElementById('nutrient-tank-status').textContent = 
                data.nutrientTankLow ? "BAIXO" : "OK";
        })
        .catch(error => {
            console.error('Erro ao buscar dados:', error);
        });
}

function toggleRelay(relay, seconds = 0) {
    const url = `/toggle${relay}` + (seconds ? `?seconds=${seconds}` : '');
    fetch(url)
        .then(response => {
            if (!response.ok) {
                throw new Error('Erro ao controlar relé');
            }
        })
        .catch(error => {
            console.error('Erro:', error);
            alert('Erro ao controlar relé');
        });
}

// Adicionar função para resetar o gráfico
function resetChart() {
    timeLabels.length = 0;
    phData.length = 0;
    ecData.length = 0;
    
    chart.data.labels = timeLabels;
    chart.data.datasets[0].data = phData;
    chart.data.datasets[1].data = ecData;
    
    chart.update('none');
    
    console.log('Gráfico resetado');
}

document.addEventListener('DOMContentLoaded', () => {
    initializeChart();
    initializeButtons();
    
    // Adicionar botão de reset
    const resetButton = document.createElement('button');
    resetButton.className = 'reset-button';
    resetButton.innerHTML = '🔄 Resetar Gráfico';
    resetButton.onclick = resetChart;
    document.querySelector('.chart-section').appendChild(resetButton);
    
    setInterval(updateClock, 1000);
    setInterval(fetchSensorData, 2000);
});

function initializeButtons() {
    console.log("Inicializando botões..."); // Debug
    const container = document.getElementById('buttons');
    if (!container) {
        console.error("Container de botões não encontrado!");
        return;
    }
    
    for(let i = 1; i <= 8; i++) {
        const buttonContainer = document.createElement('div');
        buttonContainer.className = 'button-container';
        
        const button = document.createElement('button');
        button.className = 'button off';
        button.dataset.relay = i;
        button.textContent = `${relayNames[i]} (OFF)`;
        
        if(i <= 5) { // Bombas peristálticas com timer
            const inputGroup = document.createElement('div');
            inputGroup.className = 'input-group';
            
            const input = document.createElement('input');
            input.type = 'number';
            input.className = 'timer-input';
            input.min = '1';
            input.placeholder = 'seg';
            
            const label = document.createElement('span');
            label.className = 'timer-label';
            label.textContent = 'segundos';
            
            button.onclick = () => {
                const seconds = parseInt(input.value) || 0;
                if(seconds > 0) {
                    toggleRelay(i, seconds);
                } else {
                    alert('Por favor, insira um número de segundos válido');
                }
            };
            
            buttonContainer.appendChild(button);
            inputGroup.appendChild(input);
            inputGroup.appendChild(label);
            buttonContainer.appendChild(inputGroup);
        } else { // Acionadores sem timer
            button.onclick = () => toggleRelay(i);
            buttonContainer.appendChild(button);
        }
        
        container.appendChild(buttonContainer);
    }
}