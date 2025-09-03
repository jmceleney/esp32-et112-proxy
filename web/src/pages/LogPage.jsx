import { useState, useEffect, useRef } from 'preact/hooks';
import { api } from '../utils/api';

export function LogPage() {
  const [logs, setLogs] = useState('');
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);
  const [autoScroll, setAutoScroll] = useState(true);
  const [position, setPosition] = useState(0);
  const [isOverflow, setIsOverflow] = useState(false);
  const [isPaused, setIsPaused] = useState(false);
  
  const logContainerRef = useRef();
  const intervalRef = useRef();

  const fetchLogs = async (isInitial = false) => {
    try {
      const response = await api.getLogs(isInitial ? 0 : position);
      
      // Parse response format: position\noverflowFlag\nmessages
      const lines = response.split('\n');
      if (lines.length >= 3) {
        const newPosition = parseInt(lines[0]);
        const overflowFlag = lines[1] === '1';
        const messages = lines.slice(2).join('\n');
        
        setPosition(newPosition);
        setIsOverflow(overflowFlag);
        
        if (isInitial) {
          setLogs(messages);
        } else if (messages.trim()) {
          setLogs(prev => prev + messages);
        }
      }
      
      setError(null);
    } catch (err) {
      console.error('Failed to fetch logs:', err);
      setError(err.message || 'Failed to fetch logs');
    } finally {
      setLoading(false);
    }
  };

  const clearLogs = async () => {
    try {
      await api.clearLogs();
      setLogs('');
      setPosition(0);
      setError(null);
    } catch (err) {
      setError(err.message || 'Failed to clear logs');
    }
  };

  const togglePause = () => {
    setIsPaused(prev => !prev);
  };

  // Auto-scroll to bottom when new logs arrive
  useEffect(() => {
    if (autoScroll && logContainerRef.current && !isPaused) {
      const container = logContainerRef.current;
      container.scrollTop = container.scrollHeight;
    }
  }, [logs, autoScroll, isPaused]);

  // Set up polling
  useEffect(() => {
    fetchLogs(true); // Initial fetch
    
    intervalRef.current = setInterval(() => {
      if (!isPaused) {
        fetchLogs(false);
      }
    }, 2000); // Poll every 2 seconds
    
    return () => {
      if (intervalRef.current) {
        clearInterval(intervalRef.current);
      }
    };
  }, [isPaused, position]);

  // Handle manual scroll to disable auto-scroll
  const handleScroll = (e) => {
    const container = e.target;
    const isScrolledToBottom = 
      Math.abs(container.scrollHeight - container.scrollTop - container.clientHeight) < 10;
    
    setAutoScroll(isScrolledToBottom);
  };

  if (loading) {
    return (
      <div class="loading">
        Loading logs...
      </div>
    );
  }

  return (
    <div>
      <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem;">
        <h1 style="margin: 0;">System Logs</h1>
        
        <div style="display: flex; gap: 0.5rem; align-items: center;">
          <button
            class={`btn ${isPaused ? 'btn-success' : 'btn-primary'}`}
            onClick={togglePause}
            style="padding: 0.5rem 1rem; font-size: 0.875rem;"
          >
            {isPaused ? '▶️ Resume' : '⏸️ Pause'}
          </button>
          
          <button
            class="btn btn-danger"
            onClick={clearLogs}
            style="padding: 0.5rem 1rem; font-size: 0.875rem;"
          >
            🗑️ Clear
          </button>
          
          <button
            class="btn btn-primary"
            onClick={() => fetchLogs(true)}
            style="padding: 0.5rem 1rem; font-size: 0.875rem;"
          >
            🔄 Refresh
          </button>
        </div>
      </div>

      {/* Status indicators */}
      <div style="display: flex; gap: 1rem; margin-bottom: 1rem; font-size: 0.875rem;">
        <div style={`color: ${isPaused ? 'var(--warning-color)' : 'var(--success-color)'};`}>
          {isPaused ? '⏸️ Paused' : '🔄 Live'}
        </div>
        
        {isOverflow && (
          <div style="color: var(--warning-color);">
            ⚠️ Buffer overflow - some logs may be missing
          </div>
        )}
        
        <div style="color: var(--text-muted);">
          Position: {position}
        </div>
        
        <div style={`color: ${autoScroll ? 'var(--success-color)' : 'var(--text-muted)'};`}>
          {autoScroll ? '📄 Auto-scroll ON' : '📄 Auto-scroll OFF'}
        </div>
      </div>

      {error && (
        <div class="card" style="background-color: var(--danger-color); color: white; margin-bottom: 1rem;">
          <p style="margin: 0;">❌ {error}</p>
        </div>
      )}

      {/* Log viewer */}
      <div class="card" style="padding: 0; overflow: hidden;">
        <div
          ref={logContainerRef}
          onScroll={handleScroll}
          style={{
            height: '600px',
            overflow: 'auto',
            padding: '1rem',
            backgroundColor: '#1e1e1e',
            color: '#ffffff',
            fontFamily: 'Monaco, "Cascadia Code", "Roboto Mono", Consolas, "Courier New", monospace',
            fontSize: '0.875rem',
            lineHeight: '1.4',
            whiteSpace: 'pre-wrap',
            wordBreak: 'break-word'
          }}
        >
          {logs || 'No logs available. Waiting for log data...'}
        </div>
      </div>

      {/* Usage tips */}
      <div style="margin-top: 1rem; padding: 1rem; background-color: var(--light-color); border-radius: 4px; font-size: 0.875rem; color: var(--text-muted);">
        <h4 style="margin-top: 0;">💡 Tips:</h4>
        <ul style="margin: 0; padding-left: 1.5rem;">
          <li><strong>Auto-scroll:</strong> Scroll to bottom to enable auto-scroll for new logs</li>
          <li><strong>Pause:</strong> Use pause to stop fetching new logs while investigating</li>
          <li><strong>Clear:</strong> Clear logs to reduce memory usage and start fresh</li>
          <li><strong>Live updates:</strong> Logs refresh automatically every 2 seconds when not paused</li>
        </ul>
      </div>
    </div>
  );
}