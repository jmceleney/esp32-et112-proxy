import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import viteCompression from 'vite-plugin-compression';
import fs from 'fs';
import path from 'path';

export default defineConfig({
  plugins: [
    preact(),
    viteCompression({
      algorithm: 'gzip',
      ext: '.gz',
      threshold: 1024,
    }),
    {
      name: 'query-string-cache-buster',
      generateBundle(options, bundle) {
        const versionFile = path.resolve('../data/web/version.json');
        let version = 'v1.0.0';
        try {
          if (fs.existsSync(versionFile)) {
            const versionData = JSON.parse(fs.readFileSync(versionFile, 'utf8'));
            version = versionData.version || version;
          }
        } catch (e) {
          console.warn('Could not read version.json, using default version');
        }
        
        // Add version query string to HTML assets
        Object.keys(bundle).forEach(fileName => {
          if (fileName.endsWith('.html')) {
            let html = bundle[fileName].source;
            // Replace asset references with version query strings
            html = html.replace(
              /src="(assets\/[^"]+)"/g, 
              `src="$1?v=${version}"`
            );
            html = html.replace(
              /href="(assets\/[^"]+)"/g, 
              `href="$1?v=${version}"`
            );
            bundle[fileName].source = html;
          }
        });
      }
    }
  ],
  build: {
    outDir: '../data/web',
    emptyOutDir: true,
    minify: 'esbuild',
    rollupOptions: {
      input: 'index.html',
      output: {
        manualChunks: {
          vendor: ['preact', 'preact-router'],
        },
        assetFileNames: 'assets/[name][extname]',
        chunkFileNames: 'assets/[name].js',
        entryFileNames: 'assets/[name].js',
      },
    },
    target: 'es2018',
    cssCodeSplit: false,
  },
  server: {
    port: 3000,
    proxy: {
      '/api': {
        target: 'http://esp32-proxy.local',
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api/, ''),
      },
    },
  },
  define: {
    'process.env.NODE_ENV': JSON.stringify(process.env.NODE_ENV || 'production'),
  },
});