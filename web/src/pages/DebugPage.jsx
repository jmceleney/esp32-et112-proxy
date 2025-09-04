import { useState } from 'preact/hooks';
import { api } from '../utils/api';
import { XCircle, Wrench, CheckCircle, Trash2, Clock, FileText, Lightbulb } from '../components/Icons';

const MODBUS_FUNCTIONS = {
  1: 'Read Coils',
  2: 'Read Discrete Inputs', 
  3: 'Read Holding Registers',
  4: 'Read Input Registers'
};

export function DebugPage() {
  const [debugForm, setDebugForm] = useState({
    slave: 1,
    reg: 1,
    func: 3,
    count: 1
  });
  
  const [debugResult, setDebugResult] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);
  const [history, setHistory] = useState([]);

  const handleFormChange = (field, value) => {
    setDebugForm(prev => ({ ...prev, [field]: parseInt(value) }));
  };

  const executeDebugCommand = async (e) => {
    e.preventDefault();
    
    setLoading(true);
    setError(null);
    setDebugResult(null);
    
    const startTime = Date.now();
    
    try {
      const response = await api.sendDebugCommand(
        debugForm.slave,
        debugForm.reg,
        debugForm.func,
        debugForm.count
      );
      
      const endTime = Date.now();
      const duration = endTime - startTime;
      
      // Handle JSON response from new /debug.json endpoint
      const result = {
        timestamp: new Date().toLocaleTimeString(),
        command: `Slave ${debugForm.slave}, Function ${debugForm.func}, Register ${debugForm.reg}, Count ${debugForm.count}`,
        duration: `${duration}ms`,
        response: response,
        success: response.success || false,
        rawData: response
      };
      
      setDebugResult(result);
      
      // Add to history (keep last 10 entries)
      setHistory(prev => [result, ...prev.slice(0, 9)]);
      
    } catch (err) {
      const result = {
        timestamp: new Date().toLocaleTimeString(),
        command: `Slave ${debugForm.slave}, Function ${debugForm.func}, Register ${debugForm.reg}, Count ${debugForm.count}`,
        duration: `${Date.now() - startTime}ms`,
        response: { error_message: err.message, success: false },
        success: false,
        error: true,
        rawData: null
      };
      
      setDebugResult(result);
      setError(err.message || 'Debug command failed');
    } finally {
      setLoading(false);
    }
  };

  const clearHistory = () => {
    setHistory([]);
    setDebugResult(null);
    setError(null);
  };

  const formatDebugResponse = (response) => {
    if (!response || typeof response !== 'object') {
      return 'Invalid response format';
    }

    let output = [];
    
    // Add command details
    const clientStr = response.client_type ? ` (${response.client_type.toUpperCase()})` : '';
    output.push(`Command: Function ${response.function}, Slave ${response.slave}, Register ${response.register}, Count ${response.count}${clientStr}`);
    output.push(`Timestamp: ${response.timestamp}ms`);
    
    if (response.success) {
      output.push(`Status: SUCCESS`);
      output.push(`Raw hex response: ${response.raw_hex}`);
      output.push(`Byte count: ${response.byte_count}`);
      
      if (response.values && response.values.length > 0) {
        const valueStrings = response.values.map((val, idx) => `[${idx}]: ${val} (0x${val.toString(16).padStart(4, '0')})`);
        output.push(`16-bit values: ${valueStrings.join(', ')}`);
      }
    } else {
      output.push(`Status: ERROR (code ${response.error_code})`);
      output.push(`Error: ${response.error_message}`);
    }
    
    // Add debug logs if available
    if (response.debug_logs && response.debug_logs.trim()) {
      output.push('\nDebug logs:');
      output.push(response.debug_logs.trim());
    }
    
    return output.join('\n');
  };

  return (
    <div>
      <h1 style="margin-bottom: 2rem;">Modbus Debug Tools</h1>
      
      {error && (
        <div class="card" style="background-color: var(--danger-color); color: white; margin-bottom: 1rem;">
          <p style="margin: 0;"><XCircle size={16} style="display: inline; margin-right: 0.25rem;" />{error}</p>
        </div>
      )}

      {/* Debug Command Form */}
      <div class="card">
        <h3 class="card-title">Send Modbus Command</h3>
        
        <form onSubmit={executeDebugCommand}>
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 1rem;">
            <div class="form-group">
              <label class="form-label" for="slave">Slave ID</label>
              <input
                type="number"
                id="slave"
                class="form-control"
                min="0"
                max="247"
                value={debugForm.slave}
                onInput={(e) => handleFormChange('slave', e.target.value)}
              />
            </div>
            
            <div class="form-group">
              <label class="form-label" for="func">Function Code</label>
              <select
                id="func"
                class="form-control"
                value={debugForm.func}
                onChange={(e) => handleFormChange('func', e.target.value)}
              >
                {Object.entries(MODBUS_FUNCTIONS).map(([code, name]) => (
                  <option key={code} value={code}>
                    {code} - {name}
                  </option>
                ))}
              </select>
            </div>
          </div>
          
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 1rem;">
            <div class="form-group">
              <label class="form-label" for="reg">Register Address</label>
              <input
                type="number"
                id="reg"
                class="form-control"
                min="0"
                max="65535"
                value={debugForm.reg}
                onInput={(e) => handleFormChange('reg', e.target.value)}
              />
            </div>
            
            <div class="form-group">
              <label class="form-label" for="count">Count</label>
              <input
                type="number"
                id="count"
                class="form-control"
                min="1"
                max="125"
                value={debugForm.count}
                onInput={(e) => handleFormChange('count', e.target.value)}
              />
            </div>
          </div>
          
          <div style="display: flex; gap: 1rem;">
            <button
              type="submit"
              class="btn btn-primary"
              disabled={loading}
              style="flex: 1;"
            >
              {loading ? <><Clock size={16} style="margin-right: 0.25rem;" /> Sending...</> : <><Wrench size={16} style="margin-right: 0.25rem;" />Send Command</>}
            </button>
            
            <button
              type="button"
              class="btn btn-danger"
              onClick={clearHistory}
            >
              <Trash2 size={16} style="margin-right: 0.25rem;" />Clear History
            </button>
          </div>
        </form>
      </div>

      {/* Current Result */}
      {debugResult && (
        <div class="card">
          <h3 class="card-title">
            {debugResult.success ? <><CheckCircle size={18} style="display: inline; margin-right: 0.25rem;" />Command Result</> : <><XCircle size={18} style="display: inline; margin-right: 0.25rem;" />Command Failed</>}
          </h3>
          
          <div style="background-color: var(--light-color); padding: 1rem; border-radius: 4px; margin-bottom: 1rem;">
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; font-size: 0.875rem;">
              <div><strong>Command:</strong> {debugResult.command}</div>
              <div><strong>Time:</strong> {debugResult.timestamp}</div>
              <div><strong>Duration:</strong> {debugResult.duration}</div>
              <div><strong>Status:</strong> 
                <span style={`color: ${debugResult.success ? 'var(--success-color)' : 'var(--danger-color)'};`}>
                  {debugResult.success ? 'Success' : 'Failed'}
                </span>
              </div>
            </div>
          </div>
          
          <div style="background-color: #1e1e1e; color: #ffffff; padding: 1rem; border-radius: 4px; font-family: monospace; font-size: 0.875rem; white-space: pre-wrap; overflow-x: auto;">
            {debugResult.rawData ? formatDebugResponse(debugResult.rawData) : (debugResult.response?.error_message || 'No response data')}
          </div>
        </div>
      )}

      {/* Command History */}
      {history.length > 0 && (
        <div class="card">
          <h3 class="card-title">Command History</h3>
          
          <div style="max-height: 400px; overflow-y: auto;">
            {history.map((item, index) => (
              <div 
                key={index}
                style="border-bottom: 1px solid var(--border-color); padding: 1rem 0; margin-bottom: 1rem;"
              >
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.5rem;">
                  <div style="font-size: 0.875rem; color: var(--text-muted);">
                    {item.timestamp} • {item.duration}
                  </div>
                  <div style={`font-size: 0.875rem; font-weight: bold; color: ${item.success ? 'var(--success-color)' : 'var(--danger-color)'};`}>
                    {item.success ? <><CheckCircle size={14} style="display: inline; margin-right: 0.25rem;" />Success</> : <><XCircle size={14} style="display: inline; margin-right: 0.25rem;" />Failed</>}
                  </div>
                </div>
                
                <div style="font-size: 0.875rem; margin-bottom: 0.5rem;">
                  <strong>Command:</strong> {item.command}
                </div>
                
                <div style="background-color: #f8f9fa; padding: 0.75rem; border-radius: 4px; font-family: monospace; font-size: 0.8125rem; max-height: 150px; overflow-y: auto;">
                  {item.rawData ? formatDebugResponse(item.rawData) : (item.response?.error_message || 'No response data')}
                </div>
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Quick Reference */}
      <div class="card">
        <h3 class="card-title"><FileText size={18} style="display: inline; margin-right: 0.25rem;" />Modbus Quick Reference</h3>
        
        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 2rem;">
          <div>
            <h4>Function Codes:</h4>
            <ul style="font-size: 0.875rem; line-height: 1.6;">
              <li><strong>01:</strong> Read Coils (0x references)</li>
              <li><strong>02:</strong> Read Discrete Inputs (1x references)</li>
              <li><strong>03:</strong> Read Holding Registers (4x references)</li>
              <li><strong>04:</strong> Read Input Registers (3x references)</li>
            </ul>
          </div>
          
          <div>
            <h4>ET112 Common Registers:</h4>
            <ul style="font-size: 0.875rem; line-height: 1.6;">
              <li><strong>0:</strong> Voltage L1-N</li>
              <li><strong>6:</strong> Current L1</li>
              <li><strong>12:</strong> Power L1</li>
              <li><strong>46:</strong> Frequency</li>
              <li><strong>72:</strong> Total Active Energy Import</li>
            </ul>
          </div>
        </div>
        
        <div style="margin-top: 1rem; padding: 1rem; background-color: var(--light-color); border-radius: 4px; font-size: 0.875rem;">
          <h4 style="margin-top: 0;"><Lightbulb size={16} style="display: inline; margin-right: 0.25rem;" />Tips:</h4>
          <ul style="margin: 0; padding-left: 1.5rem;">
            <li>Use Function 3 (Read Holding Registers) for most ET112 data</li>
            <li>Start with Slave ID 1 and Register 0, Count 1 for basic testing</li>
            <li>Check the logs for detailed Modbus communication traces</li>
            <li>Values are returned as raw hex - interpretation depends on register type</li>
          </ul>
        </div>
      </div>
    </div>
  );
}