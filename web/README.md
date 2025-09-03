# ESP32 ET112 Proxy - Modern Web UI

This directory contains the modern Preact-based web interface for the ESP32 ET112 Modbus Proxy.

## Overview

The modern UI provides:
- **Responsive design** optimized for mobile and desktop
- **Real-time status updates** without page refresh
- **Modern UX** with component-based architecture
- **Lightweight bundle** optimized for ESP32's limited storage

## Development Setup

### Prerequisites
- Node.js 16+ and npm
- ESP32 development environment set up

### Installation
```bash
# Navigate to web directory
cd web

# Install dependencies
npm install

# Start development server with proxy
npm run dev
```

The development server runs on `http://localhost:3000` and proxies API calls to your ESP32 device at `http://esp32-proxy.local`.

### Development Workflow
1. **Frontend Development**: Run `npm run dev` for hot reloading
2. **API Testing**: Use the proxy configuration to test against actual device
3. **Build for Production**: Run `npm run build` to generate optimized bundle

## Project Structure

```
web/
├── public/           # Static assets
│   └── index.html    # HTML template
├── src/              # Source code
│   ├── components/   # Reusable components
│   │   └── Navigation.jsx
│   ├── pages/        # Page components
│   │   ├── HomePage.jsx
│   │   ├── StatusPage.jsx
│   │   ├── ConfigPage.jsx
│   │   ├── DebugPage.jsx
│   │   ├── LogPage.jsx
│   │   └── UpdatePage.jsx
│   ├── utils/        # Utility functions
│   ├── App.jsx       # Main app component
│   ├── main.jsx      # Entry point
│   └── style.css     # Global styles
├── package.json      # Dependencies and scripts
├── vite.config.js    # Build configuration
└── README.md         # This file
```

## Build Process

### Automatic Building
The frontend is automatically built during the ESP32 firmware build process:
1. `scripts/pre_build.py` detects the `web/` directory
2. Runs `npm install` if `node_modules` doesn't exist
3. Executes `npm run build` to create optimized bundle
4. Output is placed in `data/web/` for SPIFFS/LittleFS upload

### Manual Building
```bash
# Build for production
npm run build

# Clean build artifacts
npm run clean
```

### Build Optimizations
- **Minification**: Code is minified using Terser
- **Compression**: Gzip compression for assets >1KB
- **Bundle splitting**: Vendor libraries separated from app code
- **Tree shaking**: Unused code eliminated
- **CSS optimization**: Styles are minified and inlined

## Deployment

### ESP32 Integration
1. **Build frontend**: `npm run build` (or automatic via PlatformIO)
2. **Upload filesystem**: PlatformIO uploads `data/` directory to ESP32
3. **Access modern UI**: Navigate to `http://esp32-ip/app`

### Routes
- `/app` - Modern Preact application entry point
- `/assets/*` - Static assets (JS, CSS, images)
- Legacy routes (`/status`, `/config`, etc.) continue to work

## API Integration

The frontend consumes existing JSON endpoints:
- `GET /status.json` - System status and metrics
- `GET /logdata` - Log entries for real-time display
- `POST /config` - Configuration updates
- `POST /debug` - Debug commands

## Browser Support

**Minimum Requirements:**
- Chrome 63+
- Firefox 67+
- Safari 11.1+
- Edge 79+

## Bundle Size Analysis

**Production build targets:**
- **Main bundle**: ~15-25KB gzipped
- **Vendor bundle**: ~8-12KB gzipped  
- **CSS**: ~3-5KB gzipped
- **Total**: ~30-45KB (fits comfortably in ESP32 flash)

## Performance Considerations

### ESP32 Constraints
- **Flash storage**: LittleFS partition for web assets
- **RAM usage**: Minimal impact on ESP32 heap
- **Network**: Optimized for slow WiFi connections

### Optimizations Applied
- Aggressive code splitting
- Lazy loading for non-essential components
- Efficient polling intervals (5 seconds for status)
- Local caching of static assets
- Compressed asset delivery

## Future Enhancements

**Planned Features:**
- Real-time WebSocket updates
- Advanced configuration forms  
- Live charting for energy metrics
- Mobile app-like PWA features
- Dark/light theme toggle
- Configuration import/export

## Troubleshooting

### Build Issues
```bash
# Clear cache and reinstall
rm -rf node_modules package-lock.json
npm install

# Check Node.js version
node --version  # Should be 16+
```

### Runtime Issues
1. **404 on /app**: Frontend not built or not uploaded to ESP32
2. **Blank page**: Check browser console for JavaScript errors
3. **API errors**: Verify ESP32 endpoints are responding
4. **Styling issues**: CSS might not be loading properly

### Development Tips
- Use browser DevTools Network tab to monitor API calls
- Enable ESP32 serial debug output for filesystem errors
- Use `npm run dev` for local development with hot reload
- Test on actual mobile devices for responsive design