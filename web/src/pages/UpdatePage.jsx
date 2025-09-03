import { useState, useRef } from 'preact/hooks';
import { api } from '../utils/api';

export function UpdatePage() {
  const [selectedFile, setSelectedFile] = useState(null);
  const [uploading, setUploading] = useState(false);
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState(null);
  const [success, setSuccess] = useState(false);
  const [uploadResult, setUploadResult] = useState(null);
  
  const fileInputRef = useRef();

  const handleFileSelect = (e) => {
    const file = e.target.files[0];
    if (file) {
      setSelectedFile(file);
      setError(null);
      setSuccess(false);
      setUploadResult(null);
    }
  };

  const validateFile = (file) => {
    if (!file) {
      return 'Please select a file to upload';
    }
    
    // Check file extension
    const validExtensions = ['.bin', '.hex', '.elf'];
    const fileName = file.name.toLowerCase();
    const hasValidExtension = validExtensions.some(ext => fileName.endsWith(ext));
    
    if (!hasValidExtension) {
      return `Invalid file type. Please select a firmware file (${validExtensions.join(', ')})`;
    }
    
    // Check file size (reasonable limits)
    const maxSize = 10 * 1024 * 1024; // 10MB
    if (file.size > maxSize) {
      return 'File too large. Maximum size is 10MB';
    }
    
    const minSize = 1024; // 1KB
    if (file.size < minSize) {
      return 'File too small. Are you sure this is a valid firmware file?';
    }
    
    return null;
  };

  const startUpload = async () => {
    const validationError = validateFile(selectedFile);
    if (validationError) {
      setError(validationError);
      return;
    }
    
    setUploading(true);
    setError(null);
    setProgress(0);
    
    try {
      const result = await api.uploadFirmware(selectedFile, (progressPercent) => {
        setProgress(progressPercent);
      });
      
      setSuccess(true);
      setUploadResult({
        message: 'Firmware uploaded successfully! The device will restart automatically.',
        details: result
      });
      
      // Clear the form after successful upload
      setSelectedFile(null);
      if (fileInputRef.current) {
        fileInputRef.current.value = '';
      }
      
    } catch (err) {
      setError(err.message || 'Upload failed');
      setProgress(0);
    } finally {
      setUploading(false);
    }
  };

  const clearSelection = () => {
    setSelectedFile(null);
    setError(null);
    setSuccess(false);
    setProgress(0);
    setUploadResult(null);
    if (fileInputRef.current) {
      fileInputRef.current.value = '';
    }
  };

  const formatFileSize = (bytes) => {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const sizes = ['Bytes', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
  };

  return (
    <div>
      <h1 style="margin-bottom: 2rem;">Firmware Update</h1>
      
      {/* Warning */}
      <div class="card" style="background-color: #fff3cd; border: 1px solid #ffeaa7; color: #856404; margin-bottom: 2rem;">
        <h3 style="margin-top: 0; color: #856404;">⚠️ Important Warning</h3>
        <ul style="margin: 0; padding-left: 1.5rem; line-height: 1.6;">
          <li><strong>Do not disconnect power</strong> during the update process</li>
          <li><strong>Do not close this page</strong> until the update completes</li>
          <li>The device will <strong>restart automatically</strong> after successful update</li>
          <li>Only upload <strong>valid ESP32 firmware files</strong> (.bin, .hex, .elf)</li>
          <li>Uploading incorrect firmware may <strong>brick your device</strong></li>
        </ul>
      </div>

      {error && (
        <div class="card" style="background-color: var(--danger-color); color: white; margin-bottom: 1rem;">
          <p style="margin: 0;">❌ {error}</p>
        </div>
      )}
      
      {success && uploadResult && (
        <div class="card" style="background-color: var(--success-color); color: white; margin-bottom: 1rem;">
          <h3 style="margin-top: 0;">✅ Update Successful!</h3>
          <p style="margin: 0;">{uploadResult.message}</p>
          {uploadResult.details && (
            <div style="margin-top: 1rem; padding: 1rem; background-color: rgba(255,255,255,0.1); border-radius: 4px; font-family: monospace; font-size: 0.875rem; white-space: pre-wrap;">
              {uploadResult.details}
            </div>
          )}
        </div>
      )}

      {/* File Selection */}
      <div class="card">
        <h3 class="card-title">Select Firmware File</h3>
        
        <div class="form-group">
          <label class="form-label">Choose Firmware File</label>
          <input
            ref={fileInputRef}
            type="file"
            class="form-control"
            accept=".bin,.hex,.elf"
            onChange={handleFileSelect}
            disabled={uploading}
          />
          <div style="font-size: 0.875rem; color: var(--text-muted); margin-top: 0.5rem;">
            Supported formats: .bin, .hex, .elf • Maximum size: 10MB
          </div>
        </div>
        
        {selectedFile && (
          <div style="background-color: var(--light-color); padding: 1rem; border-radius: 4px; margin-top: 1rem;">
            <h4 style="margin-top: 0;">Selected File:</h4>
            <div style="display: grid; grid-template-columns: auto 1fr auto; gap: 1rem; align-items: center;">
              <div>📄</div>
              <div>
                <div style="font-weight: 500;">{selectedFile.name}</div>
                <div style="font-size: 0.875rem; color: var(--text-muted);">
                  {formatFileSize(selectedFile.size)} • {selectedFile.type || 'Binary file'}
                </div>
              </div>
              <button
                class="btn btn-danger"
                onClick={clearSelection}
                disabled={uploading}
                style="padding: 0.5rem; font-size: 0.875rem;"
              >
                ✕
              </button>
            </div>
          </div>
        )}
      </div>

      {/* Upload Progress */}
      {uploading && (
        <div class="card">
          <h3 class="card-title">🔄 Uploading Firmware</h3>
          
          <div style="margin-bottom: 1rem;">
            <div style="display: flex; justify-content: space-between; margin-bottom: 0.5rem;">
              <span>Upload Progress</span>
              <span>{progress}%</span>
            </div>
            <div style="width: 100%; background-color: #e0e0e0; border-radius: 4px; height: 20px; overflow: hidden;">
              <div
                style={{
                  width: `${progress}%`,
                  height: '100%',
                  backgroundColor: 'var(--primary-color)',
                  transition: 'width 0.3s ease-in-out'
                }}
              />
            </div>
          </div>
          
          <div style="font-size: 0.875rem; color: var(--text-muted); text-align: center;">
            {progress === 0 ? 'Preparing upload...' :
             progress < 100 ? 'Uploading firmware...' :
             'Processing firmware...'}
          </div>
        </div>
      )}

      {/* Upload Controls */}
      <div style="display: flex; gap: 1rem; margin-top: 2rem;">
        <button
          class="btn btn-primary"
          onClick={startUpload}
          disabled={!selectedFile || uploading}
          style="flex: 1;"
        >
          {uploading ? '⏳ Uploading...' : '📤 Start Upload'}
        </button>
        
        <button
          class="btn btn-danger"
          onClick={clearSelection}
          disabled={uploading}
        >
          🗑️ Clear
        </button>
      </div>

      {/* Information */}
      <div class="card" style="margin-top: 2rem;">
        <h3 class="card-title">📋 Update Process</h3>
        
        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 2rem;">
          <div>
            <h4>Before Updating:</h4>
            <ol style="font-size: 0.875rem; line-height: 1.6;">
              <li>Ensure stable power supply</li>
              <li>Verify firmware file integrity</li>
              <li>Note current firmware version</li>
              <li>Close unnecessary browser tabs</li>
            </ol>
          </div>
          
          <div>
            <h4>After Update:</h4>
            <ol style="font-size: 0.875rem; line-height: 1.6;">
              <li>Device will restart automatically</li>
              <li>Wait ~30 seconds for boot</li>
              <li>Reconnect to device IP/hostname</li>
              <li>Verify new firmware version</li>
            </ol>
          </div>
        </div>
        
        <div style="margin-top: 1.5rem; padding: 1rem; background-color: var(--light-color); border-radius: 4px; font-size: 0.875rem;">
          <h4 style="margin-top: 0;">🔧 Build Instructions</h4>
          <p style="margin: 0;">To build firmware for this device:</p>
          <div style="background-color: #1e1e1e; color: #ffffff; padding: 0.75rem; border-radius: 4px; margin-top: 0.5rem; font-family: monospace;">
            # Debug build<br/>
            pio run -e esp32debug<br/><br/>
            # Release build<br/>
            pio run -e esp32release<br/><br/>
            # Output: .pio/build/esp32debug/firmware.bin
          </div>
        </div>
      </div>
    </div>
  );
}