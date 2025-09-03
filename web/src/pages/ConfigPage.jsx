import { useState, useEffect } from 'preact/hooks';
import { api } from '../utils/api';
import { XCircle, Save } from '../components/Icons';

export function ConfigPage() {
  const [config, setConfig] = useState({
    hostname: '',
    pi: 1000, // Polling interval
    clientIsRTU: true,
    // RTU Settings
    mb: 9600,  // baud rate
    md: 8,     // data bits
    mp: 0,     // parity (0=None, 2=Even, 3=Odd)
    ms: 1,     // stop bits
    mr: -1,    // RTS pin
    // TCP Settings  
    sip: '',   // server IP
    tp2: 502,  // server port
    // Secondary RTU Settings
    mb2: 9600,
    md2: 8,
    mp2: 0,
    ms2: 1,
    mr2: -1,
    // TCP Server Settings
    tp3: 502,
    // Serial Debug Settings
    sb: 115200,
    sd: 8,
    sp: 0,
    ss: 1,
    // Network Settings
    useStaticIP: false,
    staticIP: '',
    staticGateway: '',
    staticSubnet: ''
  });
  
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);
  const [success, setSuccess] = useState(false);
  const [formErrors, setFormErrors] = useState({});

  useEffect(() => {
    // Load current configuration (this would need a backend endpoint)
    // For now, using defaults as the backend doesn't provide a config GET endpoint
  }, []);

  const validateForm = () => {
    const errors = {};
    
    // Validate hostname
    if (!config.hostname.trim()) {
      errors.hostname = 'Hostname is required';
    }
    
    // Validate polling interval
    if (config.pi < 100) {
      errors.pi = 'Polling interval must be at least 100ms';
    }
    
    // Validate IP addresses if static IP is enabled
    if (config.useStaticIP) {
      const ipRegex = /^(?:[0-9]{1,3}\.){3}[0-9]{1,3}$/;
      if (!config.staticIP || !ipRegex.test(config.staticIP)) {
        errors.staticIP = 'Invalid IP address';
      }
      if (!config.staticGateway || !ipRegex.test(config.staticGateway)) {
        errors.staticGateway = 'Invalid gateway IP';
      }
      if (!config.staticSubnet || !ipRegex.test(config.staticSubnet)) {
        errors.staticSubnet = 'Invalid subnet mask';
      }
    }
    
    // Validate TCP server IP if not RTU
    if (!config.clientIsRTU) {
      const ipRegex = /^(?:[0-9]{1,3}\.){3}[0-9]{1,3}$/;
      if (!config.sip || !ipRegex.test(config.sip)) {
        errors.sip = 'Invalid server IP address';
      }
    }
    
    setFormErrors(errors);
    return Object.keys(errors).length === 0;
  };

  const handleSubmit = async (e) => {
    e.preventDefault();
    
    if (!validateForm()) {
      return;
    }
    
    setLoading(true);
    setError(null);
    
    try {
      await api.updateConfig(config);
      setSuccess(true);
      setTimeout(() => setSuccess(false), 3000);
    } catch (err) {
      setError(err.message || 'Failed to update configuration');
    } finally {
      setLoading(false);
    }
  };

  const handleInputChange = (field, value) => {
    setConfig(prev => ({ ...prev, [field]: value }));
    // Clear field error when user starts typing
    if (formErrors[field]) {
      setFormErrors(prev => ({ ...prev, [field]: null }));
    }
  };

  const renderFieldError = (field) => {
    if (formErrors[field]) {
      return <div style="color: var(--danger-color); font-size: 0.875rem; margin-top: 0.25rem;">{formErrors[field]}</div>;
    }
    return null;
  };

  return (
    <div>
      <h1 style="margin-bottom: 2rem;">Configuration</h1>
      
      {error && (
        <div class="card" style="background-color: var(--danger-color); color: white; margin-bottom: 1rem;">
          <p style="margin: 0;"><XCircle size={16} style="display: inline; margin-right: 0.25rem;" />{error}</p>
        </div>
      )}
      
      {success && (
        <div class="card" style="background-color: var(--success-color); color: white; margin-bottom: 1rem;">
          <p style="margin: 0;">✅ Configuration updated successfully!</p>
        </div>
      )}

      <form onSubmit={handleSubmit}>
        {/* General Settings */}
        <div class="card">
          <h3 class="card-title">General Settings</h3>
          
          <div class="form-group">
            <label class="form-label" for="hostname">Hostname</label>
            <input
              type="text"
              id="hostname"
              class="form-control"
              value={config.hostname}
              onInput={(e) => handleInputChange('hostname', e.target.value)}
              placeholder="esp32-proxy"
            />
            {renderFieldError('hostname')}
          </div>
          
          <div class="form-group">
            <label class="form-label" for="pi">Polling Interval (ms)</label>
            <input
              type="number"
              id="pi"
              class="form-control"
              min="100"
              value={config.pi}
              onInput={(e) => handleInputChange('pi', parseInt(e.target.value))}
            />
            {renderFieldError('pi')}
          </div>
          
          <div class="form-group">
            <div class="form-check">
              <input
                type="checkbox"
                id="clientIsRTU"
                class="form-check-input"
                checked={config.clientIsRTU}
                onChange={(e) => handleInputChange('clientIsRTU', e.target.checked)}
              />
              <label class="form-label" for="clientIsRTU">Modbus Client is RTU</label>
            </div>
          </div>
        </div>

        {/* RTU Client Settings */}
        {config.clientIsRTU && (
          <div class="card">
            <h3 class="card-title">Modbus RTU Client</h3>
            
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
              <div class="form-group">
                <label class="form-label" for="mb">Baud Rate</label>
                <input
                  type="number"
                  id="mb"
                  class="form-control"
                  value={config.mb}
                  onInput={(e) => handleInputChange('mb', parseInt(e.target.value))}
                />
              </div>
              
              <div class="form-group">
                <label class="form-label" for="md">Data Bits</label>
                <input
                  type="number"
                  id="md"
                  class="form-control"
                  min="5"
                  max="8"
                  value={config.md}
                  onInput={(e) => handleInputChange('md', parseInt(e.target.value))}
                />
              </div>
            </div>
            
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
              <div class="form-group">
                <label class="form-label" for="mp">Parity</label>
                <select
                  id="mp"
                  class="form-control"
                  value={config.mp}
                  onChange={(e) => handleInputChange('mp', parseInt(e.target.value))}
                >
                  <option value={0}>None</option>
                  <option value={2}>Even</option>
                  <option value={3}>Odd</option>
                </select>
              </div>
              
              <div class="form-group">
                <label class="form-label" for="ms">Stop Bits</label>
                <select
                  id="ms"
                  class="form-control"
                  value={config.ms}
                  onChange={(e) => handleInputChange('ms', parseInt(e.target.value))}
                >
                  <option value={1}>1 bit</option>
                  <option value={2}>1.5 bits</option>
                  <option value={3}>2 bits</option>
                </select>
              </div>
            </div>
            
            <div class="form-group">
              <label class="form-label" for="mr">RTS Pin</label>
              <select
                id="mr"
                class="form-control"
                value={config.mr}
                onChange={(e) => handleInputChange('mr', parseInt(e.target.value))}
              >
                <option value={-1}>Auto</option>
                <option value={4}>D4</option>
                <option value={13}>D13</option>
                <option value={14}>D14</option>
                <option value={18}>D18</option>
                <option value={19}>D19</option>
                <option value={21}>D21</option>
                <option value={22}>D22</option>
                <option value={23}>D23</option>
                <option value={25}>D25</option>
                <option value={26}>D26</option>
                <option value={27}>D27</option>
                <option value={32}>D32</option>
                <option value={33}>D33</option>
              </select>
            </div>
          </div>
        )}

        {/* TCP Client Settings */}
        {!config.clientIsRTU && (
          <div class="card">
            <h3 class="card-title">Modbus TCP Client Settings</h3>
            
            <div style="display: grid; grid-template-columns: 2fr 1fr; gap: 1rem;">
              <div class="form-group">
                <label class="form-label" for="sip">Server IP Address</label>
                <input
                  type="text"
                  id="sip"
                  class="form-control"
                  value={config.sip}
                  onInput={(e) => handleInputChange('sip', e.target.value)}
                  placeholder="192.168.1.100"
                />
                {renderFieldError('sip')}
              </div>
              
              <div class="form-group">
                <label class="form-label" for="tp2">Server Port</label>
                <input
                  type="number"
                  id="tp2"
                  class="form-control"
                  min="1"
                  max="65535"
                  value={config.tp2}
                  onInput={(e) => handleInputChange('tp2', parseInt(e.target.value))}
                />
              </div>
            </div>
          </div>
        )}

        {/* Secondary RTU Server Settings */}
        <div class="card">
          <h3 class="card-title">Modbus Secondary RTU (Server/Slave)</h3>
          
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
            <div class="form-group">
              <label class="form-label" for="mb2">Baud Rate</label>
              <input
                type="number"
                id="mb2"
                class="form-control"
                value={config.mb2}
                onInput={(e) => handleInputChange('mb2', parseInt(e.target.value))}
              />
            </div>
            
            <div class="form-group">
              <label class="form-label" for="md2">Data Bits</label>
              <input
                type="number"
                id="md2"
                class="form-control"
                min="5"
                max="8"
                value={config.md2}
                onInput={(e) => handleInputChange('md2', parseInt(e.target.value))}
              />
            </div>
          </div>
          
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
            <div class="form-group">
              <label class="form-label" for="mp2">Parity</label>
              <select
                id="mp2"
                class="form-control"
                value={config.mp2}
                onChange={(e) => handleInputChange('mp2', parseInt(e.target.value))}
              >
                <option value={0}>None</option>
                <option value={2}>Even</option>
                <option value={3}>Odd</option>
              </select>
            </div>
            
            <div class="form-group">
              <label class="form-label" for="ms2">Stop Bits</label>
              <select
                id="ms2"
                class="form-control"
                value={config.ms2}
                onChange={(e) => handleInputChange('ms2', parseInt(e.target.value))}
              >
                <option value={1}>1 bit</option>
                <option value={2}>1.5 bits</option>
                <option value={3}>2 bits</option>
              </select>
            </div>
          </div>
          
          <div class="form-group">
            <label class="form-label" for="mr2">RTS Pin</label>
            <select
              id="mr2"
              class="form-control"
              value={config.mr2}
              onChange={(e) => handleInputChange('mr2', parseInt(e.target.value))}
            >
              <option value={-1}>Auto</option>
              <option value={4}>D4</option>
              <option value={13}>D13</option>
              <option value={14}>D14</option>
              <option value={18}>D18</option>
              <option value={19}>D19</option>
              <option value={21}>D21</option>
              <option value={22}>D22</option>
              <option value={23}>D23</option>
              <option value={25}>D25</option>
              <option value={26}>D26</option>
              <option value={27}>D27</option>
              <option value={32}>D32</option>
              <option value={33}>D33</option>
            </select>
          </div>
        </div>

        {/* TCP Server Settings */}
        <div class="card">
          <h3 class="card-title">Modbus Secondary TCP Server</h3>
          <div class="form-group">
            <label class="form-label" for="tp3">Port Number</label>
            <input
              type="number"
              id="tp3"
              class="form-control"
              min="1"
              max="65535"
              value={config.tp3}
              onInput={(e) => handleInputChange('tp3', parseInt(e.target.value))}
            />
          </div>
        </div>

        {/* Serial Debug Settings */}
        <div class="card">
          <h3 class="card-title">Serial (Debug)</h3>
          
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
            <div class="form-group">
              <label class="form-label" for="sb">Baud Rate</label>
              <input
                type="number"
                id="sb"
                class="form-control"
                value={config.sb}
                onInput={(e) => handleInputChange('sb', parseInt(e.target.value))}
              />
            </div>
            
            <div class="form-group">
              <label class="form-label" for="sd">Data Bits</label>
              <input
                type="number"
                id="sd"
                class="form-control"
                min="5"
                max="8"
                value={config.sd}
                onInput={(e) => handleInputChange('sd', parseInt(e.target.value))}
              />
            </div>
          </div>
          
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
            <div class="form-group">
              <label class="form-label" for="sp">Parity</label>
              <select
                id="sp"
                class="form-control"
                value={config.sp}
                onChange={(e) => handleInputChange('sp', parseInt(e.target.value))}
              >
                <option value={0}>None</option>
                <option value={2}>Even</option>
                <option value={3}>Odd</option>
              </select>
            </div>
            
            <div class="form-group">
              <label class="form-label" for="ss">Stop Bits</label>
              <select
                id="ss"
                class="form-control"
                value={config.ss}
                onChange={(e) => handleInputChange('ss', parseInt(e.target.value))}
              >
                <option value={1}>1 bit</option>
                <option value={2}>1.5 bits</option>
                <option value={3}>2 bits</option>
              </select>
            </div>
          </div>
        </div>

        {/* Network Settings */}
        <div class="card">
          <h3 class="card-title">Network Settings</h3>
          
          <div class="form-group">
            <div class="form-check">
              <input
                type="checkbox"
                id="useStaticIP"
                class="form-check-input"
                checked={config.useStaticIP}
                onChange={(e) => handleInputChange('useStaticIP', e.target.checked)}
              />
              <label class="form-label" for="useStaticIP">Use Static IP</label>
            </div>
          </div>
          
          {config.useStaticIP && (
            <div>
              <div class="form-group">
                <label class="form-label" for="staticIP">Static IP Address</label>
                <input
                  type="text"
                  id="staticIP"
                  class="form-control"
                  value={config.staticIP}
                  onInput={(e) => handleInputChange('staticIP', e.target.value)}
                  placeholder="192.168.1.100"
                />
                {renderFieldError('staticIP')}
              </div>
              
              <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem;">
                <div class="form-group">
                  <label class="form-label" for="staticGateway">Gateway IP</label>
                  <input
                    type="text"
                    id="staticGateway"
                    class="form-control"
                    value={config.staticGateway}
                    onInput={(e) => handleInputChange('staticGateway', e.target.value)}
                    placeholder="192.168.1.1"
                  />
                  {renderFieldError('staticGateway')}
                </div>
                
                <div class="form-group">
                  <label class="form-label" for="staticSubnet">Subnet Mask</label>
                  <input
                    type="text"
                    id="staticSubnet"
                    class="form-control"
                    value={config.staticSubnet}
                    onInput={(e) => handleInputChange('staticSubnet', e.target.value)}
                    placeholder="255.255.255.0"
                  />
                  {renderFieldError('staticSubnet')}
                </div>
              </div>
            </div>
          )}
        </div>

        {/* Submit Button */}
        <div style="display: flex; gap: 1rem; margin-top: 2rem;">
          <button
            type="submit"
            class="btn btn-primary"
            disabled={loading}
            style="flex: 1;"
          >
            {loading ? <>⏳ Saving...</> : <><Save size={16} style="margin-right: 0.25rem;" />Save Configuration</>}
          </button>
        </div>
      </form>
    </div>
  );
}