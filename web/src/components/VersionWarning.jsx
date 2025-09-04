import { useState, useEffect } from 'preact/hooks';
import { AlertTriangle } from 'lucide-preact';
import { api } from '../utils/api';

/**
 * Version warning component that checks firmware/filesystem sync
 */
export function VersionWarning() {
  const [versionMismatch, setVersionMismatch] = useState(false);
  const [firmwareVersion, setFirmwareVersion] = useState('');
  const [filesystemVersion, setFilesystemVersion] = useState('');
  const [dismissed, setDismissed] = useState(false);

  useEffect(() => {
    checkVersionSync();
    
    // Check version sync every 30 seconds
    const interval = setInterval(checkVersionSync, 30000);
    return () => clearInterval(interval);
  }, []);

  const checkVersionSync = async () => {
    try {
      // Get firmware version from status
      const status = await api.getStatus();
      const firmwareVersionFromStatus = status.data?.find(item => item.name === 'Firmware Version')?.value || 'Unknown';
      
      // Get filesystem version
      const versionData = await api.getVersion();
      const fsVersion = versionData.filesystem_version || 'Unknown';
      
      setFirmwareVersion(firmwareVersionFromStatus);
      setFilesystemVersion(fsVersion);
      
      // Check if versions don't match or if one is unknown while the other isn't
      const mismatch = (firmwareVersionFromStatus !== fsVersion) && 
                      !(firmwareVersionFromStatus === 'Unknown' && fsVersion === 'Unknown');
      
      setVersionMismatch(mismatch);
      
      // Reset dismissed state if mismatch changes
      if (mismatch !== versionMismatch) {
        setDismissed(false);
      }
    } catch (error) {
      // If we can't check versions, assume no mismatch to avoid false alarms
      console.warn('Failed to check version sync:', error);
      setVersionMismatch(false);
    }
  };

  if (!versionMismatch || dismissed) {
    return null;
  }

  return (
    <div class="version-warning" style={{
      backgroundColor: '#f59e0b',
      color: '#92400e',
      padding: '12px 16px',
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'space-between',
      borderBottom: '1px solid #d97706',
      fontSize: '14px',
      fontWeight: '500'
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
        <AlertTriangle size={20} />
        <div>
          <strong>Version Mismatch Detected:</strong> Firmware and filesystem are out of sync. 
          Web interface may not work correctly.
          <div style={{ fontSize: '12px', marginTop: '4px', opacity: 0.8 }}>
            Firmware: {firmwareVersion} | Filesystem: {filesystemVersion}
          </div>
        </div>
      </div>
      <button
        onClick={() => setDismissed(true)}
        style={{
          background: 'none',
          border: 'none',
          color: '#92400e',
          cursor: 'pointer',
          padding: '4px 8px',
          borderRadius: '4px',
          fontSize: '18px',
          lineHeight: '1'
        }}
        title="Dismiss warning"
      >
        ×
      </button>
    </div>
  );
}