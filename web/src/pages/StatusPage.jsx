import { useState, useEffect } from 'preact/hooks';

export function StatusPage() {
  const [statusData, setStatusData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);

  const fetchStatusData = async () => {
    try {
      // Fetch both status data and filesystem version
      const [statusResponse, versionResponse] = await Promise.all([
        fetch('/status.json'),
        fetch('/version.json')
      ]);

      if (!statusResponse.ok) {
        throw new Error(`HTTP error! status: ${statusResponse.status}`);
      }

      const statusData = await statusResponse.json();
      
      // Try to fetch filesystem version, but don't fail if it's not available
      let filesystemVersion = 'Unknown';
      try {
        if (versionResponse.ok) {
          const versionData = await versionResponse.json();
          filesystemVersion = versionData.filesystem_version || 'Unknown';
        }
      } catch (versionErr) {
        console.warn('Failed to fetch filesystem version:', versionErr);
      }

      // Insert filesystem version right after firmware version in the data array
      const modifiedData = [...statusData.data];
      const firmwareVersionIndex = modifiedData.findIndex(item => item.name === 'Firmware Version');
      
      if (firmwareVersionIndex !== -1) {
        // Insert filesystem version right after firmware version
        modifiedData.splice(firmwareVersionIndex + 1, 0, {
          name: 'Filesystem Version',
          value: filesystemVersion
        });
      } else {
        // If firmware version not found, add filesystem version after build time
        const buildTimeIndex = modifiedData.findIndex(item => item.name === 'Firmware Build Time');
        const insertIndex = buildTimeIndex !== -1 ? buildTimeIndex + 1 : 2; // fallback to index 2
        modifiedData.splice(insertIndex, 0, {
          name: 'Filesystem Version', 
          value: filesystemVersion
        });
      }

      setStatusData({ ...statusData, data: modifiedData });
      setError(null);
    } catch (err) {
      console.error('Failed to fetch status data:', err);
      setError(err.message);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    // Initial fetch
    fetchStatusData();
    
    // Set up polling interval
    const interval = setInterval(fetchStatusData, 5000); // Poll every 5 seconds
    
    return () => clearInterval(interval);
  }, []);

  const formatUptime = (uptimeStr) => {
    // uptimeStr is already formatted from the backend
    return uptimeStr;
  };

  const getStatusClass = (value) => {
    if (typeof value === 'string') {
      if (value.toLowerCase().includes('yes') || value.toLowerCase().includes('connected')) {
        return 'status-ok';
      } else if (value.toLowerCase().includes('no') || value.toLowerCase().includes('not connected')) {
        return 'status-error';
      }
    }
    return '';
  };

  const renderMetricsTable = (items, title) => {
    const metricsWithWatermarks = items.filter(item => 
      item.low !== undefined && item.high !== undefined && 
      item.low !== "" && item.high !== ""
    );
    
    if (metricsWithWatermarks.length === 0) return null;

    return (
      <div class="card">
        <h3 class="card-title">{title}</h3>
        <div class="table-responsive">
          <table class="table">
            <thead>
              <tr>
                <th>Name</th>
                <th>Current</th>
                <th>Low</th>
                <th>High</th>
              </tr>
            </thead>
            <tbody>
              {metricsWithWatermarks.map((item, index) => (
                <tr key={index}>
                  <td>{item.name}</td>
                  <td>{item.value}</td>
                  <td>{item.low}</td>
                  <td>{item.high}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    );
  };

  const renderSystemInfoTable = (items, title) => {
    const systemInfo = items.filter(item => 
      item.low === undefined || item.high === undefined || 
      item.low === "" || item.high === ""
    );

    if (systemInfo.length === 0) return null;

    return (
      <div class="card">
        <h3 class="card-title">{title}</h3>
        <div class="table-responsive">
          <table class="table">
            <tbody>
              {systemInfo.map((item, index) => (
                <tr key={index}>
                  <td style="font-weight: 500;">{item.name}</td>
                  <td class={getStatusClass(item.value)}>{item.value}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    );
  };

  if (loading) {
    return (
      <div class="loading">
        Loading status data...
      </div>
    );
  }

  if (error) {
    return (
      <div class="card">
        <h2 class="card-title">Error Loading Status</h2>
        <p class="status-error">Failed to load status data: {error}</p>
        <button class="btn btn-primary" onClick={fetchStatusData}>
          Retry
        </button>
      </div>
    );
  }

  if (!statusData || !statusData.data) {
    return (
      <div class="card">
        <h2 class="card-title">No Status Data</h2>
        <p>No status data available.</p>
      </div>
    );
  }

  return (
    <div>
      <h1 style="margin-bottom: 2rem;">System Status</h1>
      
      <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 2rem;">
        {renderSystemInfoTable(statusData.data, "System Information")}
        {renderMetricsTable(statusData.data, "Energy Metrics")}
      </div>

    </div>
  );
}