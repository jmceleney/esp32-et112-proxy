import { useState, useEffect } from 'preact/hooks';
import { api } from '../utils/api';

export function HomePage() {
  const [metrics, setMetrics] = useState({
    voltage: null,
    current: null,
    power: null,
    frequency: null,
    uptime: null
  });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);
  const [lastUpdate, setLastUpdate] = useState(null);

  const fetchMetrics = async () => {
    try {
      const response = await api.getStatus();
      
      // Parse the response data array to find our metrics
      const data = response.data || [];
      const metricsMap = {};
      
      data.forEach(item => {
        if (item.name && item.value) {
          metricsMap[item.name] = item.value;
        }
      });

      // Extract key metrics with proper parsing
      setMetrics({
        voltage: parseFloat(metricsMap['Volts']?.replace(' V', '')) || null,
        current: parseFloat(metricsMap['Amps']?.replace(' A', '')) || null,
        power: parseFloat(metricsMap['Watts']?.replace(' W', '')) || null,
        frequency: parseFloat(metricsMap['Frequency']?.replace(' Hz', '')) || null,
        uptime: metricsMap['ESP Uptime'] || null,
        cpuCore0: parseFloat(metricsMap['ESP CPU Core 0 Load']?.replace('%', '')) || null,
        cpuCore1: parseFloat(metricsMap['ESP CPU Core 1 Load']?.replace('%', '')) || null
      });

      setError(null);
      setLastUpdate(new Date());
    } catch (err) {
      console.error('Failed to fetch metrics:', err);
      setError(err.message || 'Failed to fetch metrics');
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    // Initial fetch
    fetchMetrics();
    
    // Set up auto-refresh every 2 seconds
    const interval = setInterval(fetchMetrics, 2000);
    
    return () => clearInterval(interval);
  }, []);

  const formatValue = (value, unit, precision = 1) => {
    if (value === null || value === undefined) return '---';
    return `${value.toFixed(precision)}${unit}`;
  };

  const getStatusColor = () => {
    if (loading) return 'var(--warning-color)';
    if (error) return 'var(--danger-color)';
    if (metrics.power !== null) return 'var(--success-color)';
    return 'var(--text-muted)';
  };

  const getStatusText = () => {
    if (loading) return 'Loading...';
    if (error) return 'Connection Error';
    if (metrics.power !== null) return 'Online';
    return 'No Data';
  };

  return (
    <div>
      <div class="page-header">
        <h1 class="page-title">ESP32 ET112 Energy Monitor</h1>
        <p class="page-subtitle">
          Real-time power monitoring from Carlo Gavazzi ET112 energy meter
        </p>
      </div>

      {/* Status Indicator */}
      <div class="card">
        <div style="display: flex; align-items: center; justify-content: space-between;">
          <div style="display: flex; align-items: center; gap: 0.75rem;">
            <div style={`width: 12px; height: 12px; border-radius: 50%; background: ${getStatusColor()};`}></div>
            <span class="font-semibold">{getStatusText()}</span>
          </div>
          {lastUpdate && (
            <span class="text-muted text-sm">
              Last updated: {lastUpdate.toLocaleTimeString()}
            </span>
          )}
        </div>
      </div>

      {error && (
        <div class="card" style="background-color: var(--danger-color); color: white; margin-bottom: 1.5rem;">
          <p style="margin: 0;">❌ {error}</p>
        </div>
      )}

      {/* Power Metrics Grid */}
      <div class="grid-3">
        {/* Voltage */}
        <div class="card metric-card" style="background: linear-gradient(135deg, #10b981 0%, #059669 100%);">
          <div class="card-title">Voltage</div>
          <div class="metric-value">
            {loading ? '---' : formatValue(metrics.voltage, '', 1)}
          </div>
          <div class="metric-unit">Volts</div>
        </div>

        {/* Current */}
        <div class="card metric-card" style="background: linear-gradient(135deg, #f59e0b 0%, #d97706 100%);">
          <div class="card-title">Current</div>
          <div class="metric-value">
            {loading ? '---' : formatValue(metrics.current, '', 2)}
          </div>
          <div class="metric-unit">Amperes</div>
        </div>

        {/* Power */}
        <div class="card metric-card" style="background: linear-gradient(135deg, #2563eb 0%, #1d4ed8 100%);">
          <div class="card-title">Active Power</div>
          <div class="metric-value">
            {loading ? '---' : formatValue(metrics.power, '', 0)}
          </div>
          <div class="metric-unit">Watts</div>
        </div>
      </div>

      {/* Additional Metrics */}
      <div class="grid-4">
        <div class="card">
          <div class="card-title">⚡ Frequency</div>
          <div style="font-size: 1.5rem; font-weight: 600; color: var(--primary-color);">
            {loading ? '---' : formatValue(metrics.frequency, ' Hz', 2)}
          </div>
        </div>

        <div class="card">
          <div class="card-title">🖥️ CPU Core 0</div>
          <div style="font-size: 1.5rem; font-weight: 600; color: var(--secondary-color);">
            {loading ? '---' : formatValue(metrics.cpuCore0, '%', 1)}
          </div>
        </div>

        <div class="card">
          <div class="card-title">🖥️ CPU Core 1</div>
          <div style="font-size: 1.5rem; font-weight: 600; color: var(--secondary-color);">
            {loading ? '---' : formatValue(metrics.cpuCore1, '%', 1)}
          </div>
        </div>

        <div class="card">
          <div class="card-title">⏱️ System Uptime</div>
          <div style="font-size: 1.5rem; font-weight: 600; color: var(--primary-color);">
            {loading ? '---' : (metrics.uptime || 'Unknown')}
          </div>
        </div>
      </div>

      {/* Quick Actions */}
      <div class="card">
        <div class="card-title">⚡ Quick Actions</div>
        <div style="display: flex; gap: 1rem; flex-wrap: wrap;">
          <a href="/status" class="btn btn-primary">📊 View Detailed Status</a>
          <a href="/config" class="btn btn-secondary">⚙️ Configuration</a>
          <a href="/debug" class="btn btn-secondary">🔧 Debug Tools</a>
        </div>
      </div>

      {/* Information Cards */}
      <div class="grid-2">
        <div class="card">
          <div class="card-title">📡 About This Device</div>
          <div class="text-sm" style="line-height: 1.6;">
            <p>This ESP32 device acts as a Modbus RTU/TCP gateway for the Carlo Gavazzi ET112 energy meter, providing real-time power monitoring data.</p>
            <ul style="margin: 0.5rem 0; padding-left: 1.5rem;">
              <li>Real-time energy monitoring</li>
              <li>Modbus RTU client → TCP server</li>
              <li>Compatible with Victron GX devices</li>
              <li>Web-based configuration</li>
            </ul>
          </div>
        </div>

        <div class="card">
          <div class="card-title">🔗 Quick Links</div>
          <div class="text-sm">
            <div style="display: flex; flex-direction: column; gap: 0.5rem;">
              <a href="/log" style="color: var(--primary-color); text-decoration: none;">📝 View System Logs</a>
              <a href="/update" style="color: var(--primary-color); text-decoration: none;">📤 Update Firmware</a>
              <a href="/debug" style="color: var(--primary-color); text-decoration: none;">🔧 Test Modbus Communication</a>
              <a href="/metrics" style="color: var(--primary-color); text-decoration: none;">📈 Prometheus Metrics</a>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}