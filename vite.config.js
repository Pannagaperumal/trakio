import { defineConfig } from 'vite';

export default defineConfig({
  root: 'src',
  build: {
    outDir: '../dist',
    emptyOutDir: true,
  },
  server: {
    port: 5173,
    strictPort: true,
    watch: { ignored: ['**/src-tauri/**'] },
    proxy: {
      // All /ola-api/* requests are forwarded to api.olamaps.io
      // This sidesteps CORS in the browser dev environment
      '/ola-api': {
        target: 'https://api.olamaps.io',
        changeOrigin: true,
        secure: true,
        rewrite: (path) => path.replace(/^\/ola-api/, ''),
      },
    },
  },
  clearScreen: false,
});
