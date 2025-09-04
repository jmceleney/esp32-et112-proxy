import { Link } from 'preact-router';
import { useState } from 'preact/hooks';
import { api } from '../utils/api';
import { Home, BarChart3, Settings, Wrench, FileText, Upload, Wifi, RotateCcw, AlertTriangle, CheckCircle, XCircle, Clock } from './Icons';

export function Navigation({ currentRoute, hostname }) {
  const [showWifiDialog, setShowWifiDialog] = useState(false);
  const [showRebootDialog, setShowRebootDialog] = useState(false);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);

  const handleWifiReset = async () => {
    setLoading(true);
    setError(null);
    
    try {
      await api.resetWifi();
      setShowWifiDialog(false);
      alert('WiFi settings reset! Device will restart and enter configuration mode.');
    } catch (err) {
      setError(err.message || 'Failed to reset WiFi settings');
    } finally {
      setLoading(false);
    }
  };

  const handleReboot = async () => {
    setLoading(true);
    setError(null);
    
    try {
      await api.reboot();
      setShowRebootDialog(false);
      alert('Device rebooting... Please wait 30 seconds before reconnecting.');
    } catch (err) {
      setError(err.message || 'Failed to reboot device');
    } finally {
      setLoading(false);
    }
  };

  const isActive = (route) => currentRoute === route;

  return (
    <>
      <nav class="navigation">
        <div class="nav-container">
          <Link href="/" class="nav-brand">
            <div class="nav-brand-icon">E</div>
            <span>{hostname || 'ET112 Proxy'}</span>
          </Link>
          
          <div class="nav-menu">
            <Link class={`nav-link ${isActive('/') ? 'active' : ''}`} href="/">
              <Home size={18} /> Home
            </Link>
            <Link class={`nav-link ${isActive('/status') ? 'active' : ''}`} href="/status">
              <BarChart3 size={18} /> Status
            </Link>
            <Link class={`nav-link ${isActive('/config') ? 'active' : ''}`} href="/config">
              <Settings size={18} /> Config
            </Link>
            <Link class={`nav-link ${isActive('/debug') ? 'active' : ''}`} href="/debug">
              <Wrench size={18} /> Debug
            </Link>
            <Link class={`nav-link ${isActive('/log') ? 'active' : ''}`} href="/log">
              <FileText size={18} /> Logs
            </Link>
            <Link class={`nav-link ${isActive('/update') ? 'active' : ''}`} href="/update">
              <Upload size={18} /> Update
            </Link>
          </div>

          <div class="nav-actions">
            <button 
              class="nav-link danger btn-link" 
              onClick={() => setShowWifiDialog(true)}
              disabled={loading}
              title="Reset WiFi Settings"
            >
              <Wifi size={18} />
            </button>
            <button 
              class="nav-link danger btn-link" 
              onClick={() => setShowRebootDialog(true)}
              disabled={loading}
              title="Reboot Device"
            >
              <RotateCcw size={18} />
            </button>
          </div>
        </div>
      </nav>

      {/* WiFi Reset Confirmation Dialog */}
      {showWifiDialog && (
        <div class="modal-overlay">
          <div class="modal">
            <h3><AlertTriangle size={20} style="display: inline; margin-right: 0.5rem;" />WiFi Reset Confirmation</h3>
            <p>This will reset all WiFi settings and restart the device in configuration mode.</p>
            <ul style="margin: 1rem 0; padding-left: 1.5rem; line-height: 1.6;">
              <li>Current WiFi credentials will be erased</li>
              <li>Device will restart and create an access point</li>
              <li>You'll need to reconnect and reconfigure WiFi</li>
              <li>This action cannot be undone</li>
            </ul>
            
            {error && (
              <div class="error-message" style="margin-bottom: 1rem; color: var(--danger-color);">
                <XCircle size={16} style="display: inline; margin-right: 0.25rem;" />{error}
              </div>
            )}
            
            <div class="modal-actions">
              <button 
                class="btn btn-danger" 
                onClick={handleWifiReset}
                disabled={loading}
              >
                {loading ? <><Clock size={16} style="margin-right: 0.25rem;" /> Resetting...</> : <><RotateCcw size={16} style="margin-right: 0.25rem;" />Reset WiFi</>}
              </button>
              <button 
                class="btn btn-secondary" 
                onClick={() => setShowWifiDialog(false)}
                disabled={loading}
              >
                Cancel
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Reboot Confirmation Dialog */}
      {showRebootDialog && (
        <div class="modal-overlay">
          <div class="modal">
            <h3><RotateCcw size={20} style="display: inline; margin-right: 0.5rem;" />Reboot Confirmation</h3>
            <p>This will restart the ESP32 device immediately.</p>
            <ul style="margin: 1rem 0; padding-left: 1.5rem; line-height: 1.6;">
              <li>Device will restart and take ~30 seconds to boot</li>
              <li>All current connections will be lost</li>
              <li>Configuration and data will be preserved</li>
              <li>You'll need to refresh this page after restart</li>
            </ul>
            
            {error && (
              <div class="error-message" style="margin-bottom: 1rem; color: var(--danger-color);">
                <XCircle size={16} style="display: inline; margin-right: 0.25rem;" />{error}
              </div>
            )}
            
            <div class="modal-actions">
              <button 
                class="btn btn-danger" 
                onClick={handleReboot}
                disabled={loading}
              >
                {loading ? <><Clock size={16} style="margin-right: 0.25rem;" /> Rebooting...</> : <><RotateCcw size={16} style="margin-right: 0.25rem;" />Reboot Device</>}
              </button>
              <button 
                class="btn btn-secondary" 
                onClick={() => setShowRebootDialog(false)}
                disabled={loading}
              >
                Cancel
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}