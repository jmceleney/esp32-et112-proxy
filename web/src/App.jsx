import { Router, Route } from 'preact-router';
import { useState } from 'preact/hooks';
import { Navigation } from './components/Navigation';
import { HomePage } from './pages/HomePage';
import { StatusPage } from './pages/StatusPage';
import { ConfigPage } from './pages/ConfigPage';
import { DebugPage } from './pages/DebugPage';
import { LogPage } from './pages/LogPage';
import { UpdatePage } from './pages/UpdatePage';

export function App() {
  const [currentRoute, setCurrentRoute] = useState('/');

  const handleRouteChange = (e) => {
    setCurrentRoute(e.url);
    
    // Update page title based on route
    const routeTitle = {
      '/': 'Home',
      '/status': 'Status',
      '/config': 'Configuration',
      '/debug': 'Debug',
      '/log': 'Logs',
      '/update': 'Firmware Update'
    };
    document.title = `ESP32 ET112 Proxy - ${routeTitle[e.url] || 'Unknown'}`;
  };

  return (
    <div class="app">
      <Navigation currentRoute={currentRoute} />
      <main class="main-content">
        <Router onChange={handleRouteChange}>
          <Route path="/" component={HomePage} />
          <Route path="/status" component={StatusPage} />
          <Route path="/config" component={ConfigPage} />
          <Route path="/debug" component={DebugPage} />
          <Route path="/log" component={LogPage} />
          <Route path="/update" component={UpdatePage} />
        </Router>
      </main>
    </div>
  );
}