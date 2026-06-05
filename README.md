# trakio

trakio is a lightweight Tauri desktop map app built with Vite, vanilla JavaScript, and the `olamaps-web-sdk`.

It enables place search, route planning, and turn-by-turn navigation using the Ola Maps API. The UI includes a searchable sidebar, directions panel, transport mode selection, map markers, route playback, and support for a bike-mounted tripper IoT direction display.

## Features

- Search for addresses and places with autocomplete
- Set origin and destination for directions
- Choose transport mode: driving, bike, auto, walking, cycling
- Display route distance and duration
- Show step-by-step navigation instructions
- Sync navigation cues to a connected bike IoT tripper display
- Save and reuse an Ola Maps API key locally
- Responsive desktop layout powered by Tauri

## Getting Started

### Prerequisites

- Node.js 18+ / npm
- Rust toolchain (for Tauri)
- Tauri CLI installed globally (`cargo install tauri-cli`)

### Install dependencies

```bash
npm install
```

### Development

Run the frontend and Tauri dev environment:

```bash
npm run tauri dev
```

### Build

```bash
npm run tauri build
```

## Configuration

trakio requires an Ola Maps API key.

1. Launch the app
2. Paste your API key into the sidebar footer
3. Save the key to enable search, place details, and routing

If you need an API key, obtain one from the Ola Maps service or your app provider.

## Project Structure

- `src/index.html` — app shell and layout
- `src/styles.css` — app styling
- `src/main.js` — map logic, search, directions, and navigation
- `src-tauri/tauri.conf.json` — Tauri desktop configuration

## NPM Scripts

- `npm run dev` — start Vite development server
- `npm run build` — build frontend assets
- `npm run preview` — preview production build
- `npm run tauri` — run Tauri CLI

## Notes

- The app uses a proxy during development to avoid CORS issues.
- In production, Tauri bundles the app as a native desktop application.

## License

This project is currently unlicensed.
