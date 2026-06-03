import { defineConfig } from 'vite';

export default defineConfig({
  root: 'src',
  build: {
    outDir: '../dist',
    emptyOutDir: true,
  },
  server: {
    port: 5174,
    strictPort: false,
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
