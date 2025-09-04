/**
 * API utility functions for ESP32 communication
 */

const API_BASE = process.env.NODE_ENV === 'development' ? '/api' : '';

/**
 * Generic fetch wrapper with error handling
 */
async function apiCall(endpoint, options = {}) {
  try {
    const response = await fetch(API_BASE + endpoint, {
      headers: {
        'Content-Type': 'application/json',
        ...options.headers,
      },
      ...options,
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`);
    }

    const contentType = response.headers.get('content-type');
    if (contentType && contentType.includes('application/json')) {
      return await response.json();
    } else {
      return await response.text();
    }
  } catch (error) {
    console.error(`API Error for ${endpoint}:`, error);
    throw error;
  }
}

/**
 * GET request
 */
export async function apiGet(endpoint) {
  return apiCall(endpoint);
}

/**
 * POST request
 */
export async function apiPost(endpoint, data = null) {
  const options = {
    method: 'POST',
  };

  if (data) {
    if (data instanceof FormData) {
      // Don't set Content-Type for FormData, let browser set it
      options.headers = {};
      options.body = data;
    } else {
      options.body = JSON.stringify(data);
    }
  }

  return apiCall(endpoint, options);
}

/**
 * Specific API endpoints
 */
export const api = {
  // Status and monitoring
  getStatus: () => apiGet('/status.json'),
  getVersion: () => apiGet('/version.json'),
  getLogs: (position = 0, chunkSize = 8192) => 
    apiGet(`/logdata?position=${position}&chunk_size=${chunkSize}`),
  clearLogs: () => apiPost('/logclear'),

  // Configuration
  getConfig: () => apiGet('/config.json'),
  updateConfig: (config) => {
    const formData = new FormData();
    Object.entries(config).forEach(([key, value]) => {
      if (typeof value === 'boolean') {
        // Send checkbox values as 'on' when true, or don't send when false
        // The backend checks for presence of the parameter to determine true/false
        if (value) {
          formData.append(key, 'on');
        }
        // Note: false values are not appended, backend treats missing params as false
      } else {
        formData.append(key, value);
      }
    });
    return apiPost('/config', formData);
  },

  // Debug
  sendDebugCommand: (slave, reg, func, count) => {
    const formData = new FormData();
    formData.append('slave', slave);
    formData.append('reg', reg);
    formData.append('func', func);
    formData.append('count', count);
    return apiPost('/debug', formData);
  },

  // System actions
  reboot: () => apiPost('/reboot'),
  resetWifi: () => apiPost('/wifi'),
  
  // Firmware update
  uploadFirmware: (file, onProgress) => {
    return new Promise((resolve, reject) => {
      const formData = new FormData();
      formData.append('file', file);

      const xhr = new XMLHttpRequest();
      
      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable && onProgress) {
          onProgress(Math.round((e.loaded / e.total) * 100));
        }
      });

      xhr.addEventListener('load', () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          try {
            // Try to parse as JSON, fall back to raw text
            const response = xhr.responseText;
            const contentType = xhr.getResponseHeader('content-type');
            if (contentType && contentType.includes('application/json')) {
              resolve(JSON.parse(response));
            } else {
              resolve(response);
            }
          } catch (e) {
            // If JSON parsing fails, return raw text
            resolve(xhr.responseText);
          }
        } else {
          reject(new Error(`Upload failed: ${xhr.status} ${xhr.statusText}`));
        }
      });

      xhr.addEventListener('error', () => {
        reject(new Error('Upload failed'));
      });

      xhr.open('POST', API_BASE + '/update');
      xhr.send(formData);
    });
  },

  // Metrics
  getMetrics: () => apiGet('/metrics'),
};