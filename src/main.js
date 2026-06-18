import { OlaMaps } from 'olamaps-web-sdk';

// ── State ────────────────────────────────────────────────
let map = null;
let olaMaps = null;
let apiKey = localStorage.getItem('ola_api_key') || '';
let isDark = true;
let transportMode = 'driving';

const markers = { search: null, origin: null, dest: null, nav: null };
let selectedOrigin = null;
let selectedDest = null;
let lastSearchResult = null;
let toastTimer = null;

// Navigation state
let navRouteCoords = []; // [lat,lng][] handed to the native BLE streamer
let navSteps = [];
let navStepIdx = 0;
let navWatchId = null;
let navTotalDist = 0;
let navTotalDur = 0;
let navPrevPos = null;       // last known {lat, lng}
let navAnimFrame = null;     // rAF handle for dot interpolation
let navArrived = false;      // guard so arrival fires only once
let navAdvancing = false;    // guard against double-advance on same GPS fix
let nativeBackgroundRideActive = false;


// ── Constants ─────────────────────────────────────────────
// In Tauri the window runs as a native app — direct requests work fine.
// In browser dev mode CORS blocks cross-origin fetch, so use the Vite proxy.
const API = window.__TAURI_INTERNALS__ != null
  ? 'https://api.olamaps.io'
  : '/ola-api';

const STYLES = {
  dark: 'https://api.olamaps.io/tiles/vector/v1/styles/default-dark-standard/style.json',
  light: 'https://api.olamaps.io/tiles/vector/v1/styles/default-light-standard/style.json',
};

const PIN_SVG = `<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg">
  <path d="M12 2C8.13 2 5 5.13 5 9c0 5.25 7 13 7 13s7-7.75 7-13c0-3.87-3.13-7-7-7z"/>
  <circle cx="12" cy="9" r="2.5" fill="white" opacity="0.9"/>
</svg>`;

// ── Utils ─────────────────────────────────────────────────
function debounce(fn, ms) {
  let t;
  return (...args) => { clearTimeout(t); t = setTimeout(() => fn(...args), ms); };
}

function toast(msg, duration = 3000) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), duration);
}

// The Android WebView injects this bridge; it forwards the ride to the native
// foreground service, which computes nav frames and streams them over BLE.
function getNativeRideBridge() {
  return window.TrakioAndroidBridge || null;
}

function formatSeconds(secs) {
  if (!secs && secs !== 0) return null;
  const h = Math.floor(secs / 3600);
  const m = Math.round((secs % 3600) / 60);
  if (h > 0) return `${h}h ${m}m`;
  return `${m} min`;
}

function decodePolyline(encoded) {
  const pts = [];
  let i = 0, lat = 0, lng = 0;
  while (i < encoded.length) {
    let b, shift = 0, result = 0;
    do { b = encoded.charCodeAt(i++) - 63; result |= (b & 0x1f) << shift; shift += 5; } while (b >= 0x20);
    lat += (result & 1) ? ~(result >> 1) : result >> 1;
    shift = 0; result = 0;
    do { b = encoded.charCodeAt(i++) - 63; result |= (b & 0x1f) << shift; shift += 5; } while (b >= 0x20);
    lng += (result & 1) ? ~(result >> 1) : result >> 1;
    pts.push([lng / 1e5, lat / 1e5]);
  }
  return pts;
}

// ── API ───────────────────────────────────────────────────
async function searchPlaces(query) {
  if (!apiKey || query.length < 2) return [];
  try {
    const r = await fetch(`${API}/places/v1/autocomplete?input=${encodeURIComponent(query)}&api_key=${apiKey}&language=en`);
    const d = await r.json();
    return d.predictions || [];
  } catch { return []; }
}

async function getPlaceCoords(result) {
  if (result.geometry?.location) {
    return { lat: result.geometry.location.lat, lng: result.geometry.location.lng };
  }
  try {
    const r = await fetch(`${API}/places/v1/details?place_id=${result.place_id}&api_key=${apiKey}`);
    const d = await r.json();
    const loc = d.result?.geometry?.location;
    return loc ? { lat: loc.lat, lng: loc.lng } : null;
  } catch { return null; }
}

async function fetchDirections(origin, dest) {
  try {
    const o = `${origin.lat},${origin.lng}`;
    const d = `${dest.lat},${dest.lng}`;
    // trakio routing API requires POST — GET returns 404
    const r = await fetch(
      `${API}/routing/v1/directions?origin=${o}&destination=${d}&api_key=${apiKey}&mode=${transportMode}`,
      { method: 'POST' }
    );
    const data = await r.json();
    console.log('[directions]', data);
    return data;
  } catch (e) {
    console.error('[directions] fetch error:', e);
    return null;
  }
}

async function reverseGeocode(lat, lng) {
  if (!apiKey) return null;
  try {
    const r = await fetch(`${API}/places/v1/reverse-geocode?latlng=${lat},${lng}&api_key=${apiKey}`);
    const d = await r.json();
    return d.results?.[0] || null;
  } catch { return null; }
}

// ── Map helpers ───────────────────────────────────────────
function makeMarkerEl(type) {
  const el = document.createElement('div');
  el.className = `cm cm-${type}`;
  el.innerHTML = PIN_SVG;
  return el;
}

function placeMarker(type, lngLat, label) {
  if (markers[type]) markers[type].remove();
  const el = makeMarkerEl(type);
  markers[type] = olaMaps.addMarker({ element: el }).setLngLat([lngLat.lng, lngLat.lat]);
  if (label) {
    const popup = olaMaps.addPopup({ offset: 30, closeButton: true })
      .setHTML(`<div class="map-popup">${label}</div>`);
    markers[type].setPopup(popup);
  }
  markers[type].addTo(map);
}

function removeMarker(type) {
  if (markers[type]) { markers[type].remove(); markers[type] = null; }
}

function fitBounds(coords) {
  if (!coords.length) return;
  let minLng = Infinity, maxLng = -Infinity, minLat = Infinity, maxLat = -Infinity;
  for (const [lng, lat] of coords) {
    if (lng < minLng) minLng = lng;
    if (lng > maxLng) maxLng = lng;
    if (lat < minLat) minLat = lat;
    if (lat > maxLat) maxLat = lat;
  }
  const sidebarVisible = window.innerWidth > 680;
  map.fitBounds([[minLng, minLat], [maxLng, maxLat]], {
    padding: { top: 60, bottom: 80, left: sidebarVisible ? 330 : 40, right: 60 },
    duration: 800,
  });
}

function drawRoute(coords) {
  const geojson = { type: 'Feature', geometry: { type: 'LineString', coordinates: coords } };
  if (map.getSource('route')) {
    map.getSource('route').setData(geojson);
  } else {
    map.addSource('route', { type: 'geojson', data: geojson, lineMetrics: true });
    map.addLayer({
      id: 'route-casing',
      type: 'line',
      source: 'route',
      layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-color': '#ffffff', 'line-width': 7, 'line-opacity': 0.15 },
    }, findFirstSymbolLayer());
    map.addLayer({
      id: 'route-line',
      type: 'line',
      source: 'route',
      layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-color': '#a78bfa', 'line-width': 4.5, 'line-opacity': 0.95 },
    }, findFirstSymbolLayer());
  }
}

function findFirstSymbolLayer() {
  const layers = map.getStyle().layers;
  for (const layer of layers) {
    if (layer.type === 'symbol') return layer.id;
  }
  return undefined;
}

function clearRoute() {
  if (map.getLayer('route-line')) map.removeLayer('route-line');
  if (map.getLayer('route-casing')) map.removeLayer('route-casing');
  if (map.getSource('route')) map.removeSource('route');
}

// ── Dropdown renderer ─────────────────────────────────────
function renderDropdown(container, items, onSelect) {
  container.innerHTML = '';
  if (!items.length) {
    container.innerHTML = '<div class="dd-empty">No results found</div>';
    container.classList.add('open');
    return;
  }
  for (const item of items) {
    const el = document.createElement('div');
    el.className = 'dd-item';
    const main = item.structured_formatting?.main_text || item.description;
    const sub = item.structured_formatting?.secondary_text || '';
    el.innerHTML = `
      <svg class="dd-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <path d="M21 10c0 7-9 13-9 13S3 17 3 10a9 9 0 0 1 18 0z"/>
        <circle cx="12" cy="10" r="3"/>
      </svg>
      <div class="dd-text">
        <div class="dd-main">${main}</div>
        ${sub ? `<div class="dd-sub">${sub}</div>` : ''}
      </div>`;
    el.addEventListener('mousedown', (e) => { e.preventDefault(); onSelect(item); });
    container.appendChild(el);
  }
  container.classList.add('open');
}

function closeDropdown(el) { el.classList.remove('open'); el.innerHTML = ''; }

// ── Map init ──────────────────────────────────────────────
async function initMap() {
  if (!apiKey) {
    toast('Enter your trakio API key below to get started');
    return;
  }
  if (map) return;

  try {
    olaMaps = new OlaMaps({ apiKey });

    // init() returns a Promise in SDK v1.4 — must await
    map = await olaMaps.init({
      container: 'map',
      style: STYLES.dark,
      center: [77.5946, 12.9716],
      zoom: 12,
      minZoom: 3,
      maxZoom: 20,
    });

    map.addControl(olaMaps.addNavigationControls({ visualizePitch: false }), 'bottom-right');

    map.on('load', () => toast('Map ready'));

    map.on('click', async (e) => {
      const { lng, lat } = e.lngLat;
      placeMarker('search', { lat, lng });

      const result = await reverseGeocode(lat, lng);
      if (!result) return;

      const name = result.name || result.formatted_address || `${lat.toFixed(5)}, ${lng.toFixed(5)}`;
      const addr = result.formatted_address || '';

      placeMarker('search', { lat, lng }, name);
      lastSearchResult = { lat, lng, name, addr };

      document.getElementById('search-place-name').textContent = name;
      document.getElementById('search-place-addr').textContent = addr;
      document.getElementById('search-card').classList.add('visible');
      document.getElementById('search-hint').classList.add('hidden');

      if (document.querySelector('.tab.active')?.dataset.tab !== 'search') {
        document.querySelector('[data-tab="search"]').click();
      }
    });
  } catch (e) {
    console.error('[initMap]', e);
    map = null;
    olaMaps = null;
    toast('Map failed to load — check your API key');
  }
}

// ── Search panel ──────────────────────────────────────────
function setupSearch() {
  const input = document.getElementById('search-input');
  const dropdown = document.getElementById('search-dropdown');
  const clearBtn = document.getElementById('search-clear');
  const card = document.getElementById('search-card');

  const doSearch = debounce(async (q) => {
    if (!q) { closeDropdown(dropdown); return; }
    if (!map) { toast('Map not loaded — save your API key first'); return; }
    const results = await searchPlaces(q);
    renderDropdown(dropdown, results, async (item) => {
      input.value = item.description;
      clearBtn.classList.add('visible');
      closeDropdown(dropdown);

      const coords = await getPlaceCoords(item);
      if (!coords) { toast('Could not get location details'); return; }

      const name = item.structured_formatting?.main_text || item.description;
      const addr = item.structured_formatting?.secondary_text || '';

      map.flyTo({ center: [coords.lng, coords.lat], zoom: 15, duration: 900 });
      placeMarker('search', coords, name);

      lastSearchResult = { ...coords, name, addr };
      document.getElementById('search-place-name').textContent = name;
      document.getElementById('search-place-addr').textContent = addr;
      card.classList.add('visible');
      document.getElementById('search-hint').classList.add('hidden');
    });
  }, 280);

  input.addEventListener('input', (e) => {
    const v = e.target.value.trim();
    clearBtn.classList.toggle('visible', !!v);
    if (!v) { closeDropdown(dropdown); card.classList.remove('visible'); document.getElementById('search-hint').classList.remove('hidden'); }
    doSearch(v);
  });

  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      const first = dropdown.querySelector('.dd-item');
      if (first) first.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
    }
  });

  input.addEventListener('blur', () => setTimeout(() => closeDropdown(dropdown), 150));

  clearBtn.addEventListener('click', () => {
    input.value = '';
    clearBtn.classList.remove('visible');
    closeDropdown(dropdown);
    card.classList.remove('visible');
    document.getElementById('search-hint').classList.remove('hidden');
    removeMarker('search');
    lastSearchResult = null;
  });

  // "Directions" button on place card
  document.getElementById('search-to-dir').addEventListener('click', () => {
    if (!lastSearchResult) return;
    document.querySelector('[data-tab="directions"]').click();
    document.getElementById('dest-input').value = lastSearchResult.name;
    document.getElementById('dest-clear').classList.add('visible');
    selectedDest = lastSearchResult;
    placeMarker('dest', lastSearchResult, lastSearchResult.name);
    checkDirectionsReady();
  });

  // Copy coordinates
  document.getElementById('search-copy-coords').addEventListener('click', () => {
    if (!lastSearchResult) return;
    const text = `${lastSearchResult.lat.toFixed(6)}, ${lastSearchResult.lng.toFixed(6)}`;
    navigator.clipboard.writeText(text).then(() => toast(`Copied: ${text}`)).catch(() => toast('Copy failed'));
  });
}

// ── Directions panel ──────────────────────────────────────
function setupDirections() {
  const getBtn = document.getElementById('get-directions-btn');
  const routeInfo = document.getElementById('route-info');

  // Waypoint input helper
  function makeWaypoint(inputId, dropdownId, clearId, type) {
    const input = document.getElementById(inputId);
    const dropdown = document.getElementById(dropdownId);
    const clearBtn = document.getElementById(clearId);

    const doSearch = debounce(async (q) => {
      if (!q) { closeDropdown(dropdown); return; }
      if (!map) { toast('Map not loaded — save your API key first'); return; }
      const results = await searchPlaces(q);
      renderDropdown(dropdown, results, async (item) => {
        input.value = item.description;
        clearBtn.classList.add('visible');
        closeDropdown(dropdown);

        const coords = await getPlaceCoords(item);
        if (!coords) { toast('Could not get location details'); return; }

        const name = item.structured_formatting?.main_text || item.description;
        if (type === 'origin') {
          selectedOrigin = { ...coords, name };
          placeMarker('origin', coords, name);
        } else {
          selectedDest = { ...coords, name };
          placeMarker('dest', coords, name);
        }
        checkDirectionsReady();
      });
    }, 280);

    input.addEventListener('input', (e) => {
      const v = e.target.value.trim();
      clearBtn.classList.toggle('visible', !!v);
      if (!v) {
        if (type === 'origin') { selectedOrigin = null; removeMarker('origin'); }
        else { selectedDest = null; removeMarker('dest'); }
        checkDirectionsReady();
        clearRoute();
        routeInfo.classList.remove('visible');
      }
      doSearch(v);
    });

    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        const first = dropdown.querySelector('.dd-item');
        if (first) first.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));
      }
    });

    input.addEventListener('blur', () => setTimeout(() => closeDropdown(dropdown), 150));

    clearBtn.addEventListener('click', () => {
      input.value = '';
      clearBtn.classList.remove('visible');
      closeDropdown(dropdown);
      if (type === 'origin') { selectedOrigin = null; removeMarker('origin'); }
      else { selectedDest = null; removeMarker('dest'); }
      checkDirectionsReady();
      clearRoute();
      routeInfo.classList.remove('visible');
    });
  }

  makeWaypoint('origin-input', 'origin-dropdown', 'origin-clear', 'origin');
  makeWaypoint('dest-input', 'dest-dropdown', 'dest-clear', 'dest');

  // Use current location as origin
  document.getElementById('origin-locate').addEventListener('click', () => {
    if (!map) { toast('Map not loaded — save your API key first'); return; }
    if (!navigator.geolocation) { toast('Geolocation not supported by this browser'); return; }
    toast('Getting your location…');
    navigator.geolocation.getCurrentPosition(
      async (pos) => {
        try {
          const { latitude: lat, longitude: lng } = pos.coords;
          const result = await reverseGeocode(lat, lng);
          const name = result?.formatted_address || 'My Location';
          document.getElementById('origin-input').value = name;
          document.getElementById('origin-clear').classList.add('visible');
          selectedOrigin = { lat, lng, name };
          placeMarker('origin', { lat, lng }, name);
          map.flyTo({ center: [lng, lat], zoom: 14 });
          checkDirectionsReady();
          toast('Location set as origin');
        } catch (e) {
          console.error('[locate-origin]', e);
          toast('Error placing location — see console');
        }
      },
      (err) => {
        if (err.code === 1) toast('Location permission denied — allow it in browser settings');
        else if (err.code === 2) toast('Location unavailable — try again or enter manually');
        else toast(`Location error (${err.message})`);
      },
      { timeout: 15000, enableHighAccuracy: false }
    );
  });

  // Get directions
  getBtn.addEventListener('click', async () => {
    if (!selectedOrigin || !selectedDest) return;

    getBtn.disabled = true;
    getBtn.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="animation:spin 0.8s linear infinite"><path d="M21 12a9 9 0 1 1-6.219-8.56"/></svg> Loading…`;

    const data = await fetchDirections(selectedOrigin, selectedDest);

    getBtn.disabled = false;
    getBtn.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="3 11 22 2 13 21 11 13 3 11"/></svg> Get Directions`;

    if (!data) {
      toast('Network error — check your connection');
      return;
    }

    const status = (data.status || '').toLowerCase();
    if (status === 'zero_results') { toast('No route found between these locations'); return; }
    if (!data.routes?.length) {
      toast(`Directions failed (${data.status || 'unknown error'}) — check your API key`);
      return;
    }

    const route = data.routes[0];
    const leg = route.legs?.[0];
    if (!leg) { toast('Empty route returned by API'); return; }

    // overview_polyline is a raw encoded string in trakio API
    const polylineStr = typeof route.overview_polyline === 'string'
      ? route.overview_polyline
      : (route.overview_polyline?.points ?? '');
    if (!polylineStr) { toast('No route geometry in response'); return; }

    const coords = decodePolyline(polylineStr);
    navRouteCoords = coords.map(([lng, lat]) => [lat, lng]); // Leaflet [lat,lng]
    drawRoute(coords);
    fitBounds(coords);

    // trakio returns readable_duration / readable_distance strings
    // and raw distance (meters) / duration (seconds) numbers
    const distKm = leg.readable_distance
      ? `${leg.readable_distance} km`
      : leg.distance != null ? `${(leg.distance / 1000).toFixed(1)} km` : '—';
    const durText = leg.readable_duration || formatSeconds(leg.duration) || '—';

    document.getElementById('route-duration').textContent = durText;
    document.getElementById('route-distance').textContent = distKm;
    routeInfo.classList.add('visible');

    // Store steps for navigation
    navSteps = leg.steps || [];
    navTotalDist = leg.distance || 0;
    navTotalDur = leg.duration || 0;

    const list = document.getElementById('steps-list');
    list.innerHTML = '';
    navSteps.forEach((step, i) => {
      const li = document.createElement('li');
      li.className = 'step-item';
      const text = step.instructions || step.html_instructions || '';
      li.innerHTML = `<span class="step-num">${i + 1}</span><span class="step-text">${text}</span>`;
      list.appendChild(li);
    });
  });

  // Steps toggle
  document.getElementById('steps-toggle').addEventListener('click', function () {
    this.classList.toggle('open');
    document.getElementById('steps-list').classList.toggle('open');
  });

  // Start Ride
  document.getElementById('start-ride-btn').addEventListener('click', startNavigation);
  document.getElementById('end-ride-btn').addEventListener('click', endNavigation);
}

// ── Map controls ──────────────────────────────────────────
function setupMapControls() {
  document.getElementById('locate-btn').addEventListener('click', () => {
    if (!map) { toast('Map not loaded — save your API key first'); return; }
    if (!navigator.geolocation) { toast('Geolocation not supported by this browser'); return; }
    toast('Getting your location…');
    navigator.geolocation.getCurrentPosition(
      ({ coords: { latitude: lat, longitude: lng } }) => {
        try {
          map.flyTo({ center: [lng, lat], zoom: 15, duration: 900 });
          placeMarker('search', { lat, lng }, 'My Location');
          toast('Centered on your location');
        } catch (e) {
          console.error('[locate-btn]', e);
          toast('Error placing marker — see console');
        }
      },
      (err) => {
        if (err.code === 1) toast('Location permission denied — allow it in browser settings');
        else if (err.code === 2) toast('Location unavailable — try again or enter manually');
        else toast(`Location error (${err.message})`);
      },
      { timeout: 15000, enableHighAccuracy: false }
    );
  });

  document.getElementById('theme-btn').addEventListener('click', () => {
    if (!map) return;
    isDark = !isDark;
    map.setStyle(isDark ? STYLES.dark : STYLES.light);
    document.getElementById('theme-btn').classList.toggle('active', !isDark);
    map.once('styledata', () => {
      if (selectedOrigin && selectedDest) {
        toast('Re-drawing route after style change…');
      }
    });
  });

  document.getElementById('clear-map-btn').addEventListener('click', () => {
    if (!map) return;
    removeMarker('search');
    removeMarker('origin');
    removeMarker('dest');
    clearRoute();

    selectedOrigin = null;
    selectedDest = null;
    lastSearchResult = null;

    ['search-input', 'origin-input', 'dest-input'].forEach(id => {
      document.getElementById(id).value = '';
    });
    ['search-clear', 'origin-clear', 'dest-clear'].forEach(id => {
      document.getElementById(id).classList.remove('visible');
    });
    document.getElementById('search-card').classList.remove('visible');
    document.getElementById('search-hint').classList.remove('hidden');
    document.getElementById('route-info').classList.remove('visible');
    document.getElementById('steps-list').classList.remove('open');
    document.getElementById('steps-toggle').classList.remove('open');

    checkDirectionsReady();
    toast('Map cleared');
  });
}

// ── Tabs ──────────────────────────────────────────────────
function setupTabs() {
  document.querySelectorAll('.tab').forEach(tab => {
    tab.addEventListener('click', () => {
      document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
      document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
      tab.classList.add('active');
      document.getElementById(`panel-${tab.dataset.tab}`).classList.add('active');
    });
  });
}

// ── Transport mode selector ───────────────────────────────
function setupModeSelector() {
  document.querySelectorAll('.mode-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      transportMode = btn.dataset.mode;
      // Re-fetch directions if route is already shown
      if (navSteps.length) {
        document.getElementById('get-directions-btn').click();
      }
    });
  });
}

// ── Navigation helpers ────────────────────────────────────
function haversine(lat1, lng1, lat2, lng2) {
  const R = 6371000;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLng = (lng2 - lng1) * Math.PI / 180;
  const a = Math.sin(dLat / 2) ** 2
    + Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) * Math.sin(dLng / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function lerp(a, b, t) { return a + (b - a) * t; }

function easeInOut(t) { return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t; }

function animateNavDot(fromLng, fromLat, toLng, toLat, durationMs) {
  if (navAnimFrame) cancelAnimationFrame(navAnimFrame);
  const start = performance.now();
  function frame(now) {
    const raw = Math.min((now - start) / durationMs, 1);
    const t = easeInOut(raw);
    markers.nav?.setLngLat([lerp(fromLng, toLng, t), lerp(fromLat, toLat, t)]);
    if (raw < 1) navAnimFrame = requestAnimationFrame(frame);
  }
  navAnimFrame = requestAnimationFrame(frame);
}

function formatDist(meters) {
  return meters >= 1000
    ? `${(meters / 1000).toFixed(1)} km`
    : `${Math.round(meters)} m`;
}

const MANEUVER_ARROWS = {
  'depart': 'M12 19V5M5 12l7-7 7 7',
  'arrive': 'M12 5v14M5 12l7 7 7-7',
  'turn-right': 'M5 12h14M13 5l7 7-7 7',
  'turn-left': 'M19 12H5M11 5l-7 7 7 7',
  'turn-slight-right': 'M5 12h11M13 7l4 5-4 5',
  'turn-slight-left': 'M19 12H8M11 7l-4 5 4 5',
  'turn-sharp-right': 'M5 12h8l4-7M9 5l8 7-8 7',
  'turn-sharp-left': 'M19 12h-8l-4-7M15 5l-8 7 8 7',
  'continue': 'M12 19V5M5 12l7-7 7 7',
  'roundabout': 'M12 2a10 10 0 1 0 0 20M15 8l-3-3-3 3',
  'uturn': 'M4 12h12a4 4 0 0 0 0-8H8',
};

function setNavArrow(maneuver) {
  const path = MANEUVER_ARROWS[maneuver] || MANEUVER_ARROWS['continue'];
  document.getElementById('nav-arrow').innerHTML = `<path d="${path}"/>`;
}

function updateHUD() {
  if (!navSteps.length) return;
  const step = navSteps[navStepIdx];
  document.getElementById('nav-instruction').textContent = step.instructions || '';
  setNavArrow(step.maneuver || 'continue');

  // Distance to next step
  const distToNext = step.distance || 0;
  document.getElementById('nav-dist-next').textContent = formatDist(distToNext);

  // Remaining steps / distance / time
  const remaining = navSteps.slice(navStepIdx);
  const remDist = remaining.reduce((s, st) => s + (st.distance || 0), 0);
  const remDur = remaining.reduce((s, st) => s + (st.duration || 0), 0);

  document.getElementById('nav-step-count').textContent =
    `Step ${navStepIdx + 1} of ${navSteps.length}`;
  document.getElementById('nav-remaining-dist').textContent = formatDist(remDist);
  document.getElementById('nav-remaining-time').textContent = formatSeconds(remDur) || '—';
}

// Note: the compact { heading, distanceToTurn, instruction, route } payload
// the ESP32 renders is computed natively in PiNavigationService.kt so it keeps
// streaming with the screen off. The frontend only kicks off / ends the ride.

// ── Navigation config (tunable) ───────────────────────────
const NAV_CONFIG = {
  advanceThreshold: 35,       // metres — how close to step end before advancing
  arrivalThreshold: 25,       // metres — how close to destination counts as arrived
  navZoom: 17,                // map zoom during navigation
  navPitch: 45,               // map tilt (degrees) for 3-D feel
  trackHeading: true,         // rotate map to match bearing
  watchMaxAge: 3000,          // ms — max age of cached GPS position
  watchTimeout: 15000,        // ms — GPS timeout per fix
  highAccuracy: true,         // request precise GPS
  easeMs: 500,                // camera ease animation duration
};

// ── Start / End navigation ────────────────────────────────
function startNavigation() {
  if (!navSteps.length) { toast('Get directions first'); return; }
  if (!navigator.geolocation) { toast('Geolocation not supported'); return; }

  navStepIdx = 0;
  navArrived = false;
  navAdvancing = false;
  document.getElementById('nav-hud').classList.add('active');
  document.getElementById('app').classList.add('navigating');
  updateHUD();

  // Live position marker — pulsing dot
  const el = document.createElement('div');
  el.className = 'nav-pos-dot';
  el.innerHTML = `<div class="nav-pos-inner"></div><div class="nav-pos-ring"></div>`;
  if (markers.nav) markers.nav.remove();
  markers.nav = olaMaps.addMarker({ element: el, anchor: 'center' })
    .setLngLat([selectedOrigin.lng, selectedOrigin.lat])
    .addTo(map);
  navPrevPos = { lat: selectedOrigin.lat, lng: selectedOrigin.lng };

  // Initial camera
  map.easeTo({
    center: [selectedOrigin.lng, selectedOrigin.lat],
    zoom: NAV_CONFIG.navZoom,
    pitch: NAV_CONFIG.navPitch,
    duration: 800,
  });

  toast('Navigation started');

  // Hand the route to the native foreground service. It owns the GPS loop,
  // computes the compact nav frames, and streams them to the ESP32 over BLE —
  // so streaming keeps working with the screen off. The route geometry
  // (routeCoords) lets it build the local heading-up `route` polyline.
  const routeStartPayload = {
    type: 'route_start',
    origin: selectedOrigin,
    destination: selectedDest,
    destinationName: selectedDest?.name || selectedDest?.formatted_address || 'Destination',
    totalDistance: navTotalDist,   // metres
    totalDuration: navTotalDur,    // seconds
    routeCoords: navRouteCoords,
    steps: navSteps.map(s => ({
      instructions: s.instructions,
      maneuver: s.maneuver || null,
      end_location: s.end_location,
      distance: s.distance,
      duration: s.duration,
    })),
  };

  const nativeRideBridge = getNativeRideBridge();
  nativeBackgroundRideActive = Boolean(nativeRideBridge);
  if (nativeBackgroundRideActive) {
    nativeRideBridge.startBackgroundRide(JSON.stringify(routeStartPayload));
  }

  navWatchId = navigator.geolocation.watchPosition(
    (pos) => {
      const { latitude: lat, longitude: lng, heading, speed } = pos.coords;

      // Smoothly animate dot from previous position to new GPS fix
      if (navPrevPos) {
        animateNavDot(navPrevPos.lng, navPrevPos.lat, lng, lat, NAV_CONFIG.easeMs);
      }
      navPrevPos = { lat, lng };

      // Camera: follow heading when moving (speed in m/s, > 0.3 ≈ walking pace)
      const bearing = NAV_CONFIG.trackHeading && heading != null && speed != null && speed > 0.3
        ? heading : map.getBearing();

      map.easeTo({
        center: [lng, lat],
        bearing,
        zoom: NAV_CONFIG.navZoom,
        pitch: NAV_CONFIG.navPitch,
        duration: NAV_CONFIG.easeMs,
      });

      if (navArrived) return;

      // Advance steps — skip through any steps shorter than the threshold
      // (handles very short steps that GPS flies past between fixes)
      if (!navAdvancing) {
        navAdvancing = true;
        while (navStepIdx < navSteps.length - 1) {
          const step = navSteps[navStepIdx];
          if (!step?.end_location) break;
          const distToEnd = haversine(lat, lng, step.end_location.lat, step.end_location.lng);
          if (distToEnd < NAV_CONFIG.advanceThreshold) {
            navStepIdx++;
            updateHUD();
            toast(navSteps[navStepIdx]?.instructions || '', 4000);
          } else {
            // Update live distance in HUD for current step
            document.getElementById('nav-dist-next').textContent = formatDist(distToEnd);
            break;
          }
        }
        navAdvancing = false;
      }

      // Check arrival at final step
      const lastStep = navSteps[navStepIdx];
      if (navStepIdx === navSteps.length - 1 && lastStep?.end_location) {
        const distToEnd = haversine(lat, lng, lastStep.end_location.lat, lastStep.end_location.lng);
        document.getElementById('nav-dist-next').textContent = formatDist(distToEnd);
        if (distToEnd < NAV_CONFIG.arrivalThreshold) {
          navArrived = true;
          toast('You have arrived at your destination! 🎉', 4000);
          setTimeout(endNavigation, 3000);
        }
      }
    },
    (err) => {
      if (err.code === 1) toast('Location permission denied');
      else if (err.code === 2) toast('GPS signal lost — check your location settings');
      else toast('Location tracking error');
    },
    {
      enableHighAccuracy: NAV_CONFIG.highAccuracy,
      timeout: NAV_CONFIG.watchTimeout,
      maximumAge: NAV_CONFIG.watchMaxAge,
    }
  );
}

function endNavigation() {
  if (navWatchId != null) {
    navigator.geolocation.clearWatch(navWatchId);
    navWatchId = null;
  }
  if (navAnimFrame) { cancelAnimationFrame(navAnimFrame); navAnimFrame = null; }
  navPrevPos = null;
  navArrived = false;
  navAdvancing = false;
  document.getElementById('nav-hud').classList.remove('active');
  document.getElementById('app').classList.remove('navigating');
  if (markers.nav) { markers.nav.remove(); markers.nav = null; }
  map?.easeTo({ pitch: 0, bearing: 0, duration: 600 });
  toast('Navigation ended');

  // Tell the native service the ride is over → it sends route_end over BLE and
  // tears down the BLE connection + GPS loop.
  const nativeRideBridge = getNativeRideBridge();
  if (nativeBackgroundRideActive && nativeRideBridge) {
    nativeRideBridge.stopBackgroundRide();
  }
  nativeBackgroundRideActive = false;
}

// ── Sidebar ───────────────────────────────────────────────
function setupSidebar() {
  const sidebar = document.getElementById('sidebar');
  const toggle = document.getElementById('sidebar-toggle');
  const close = document.getElementById('sidebar-close');

  const isMobile = () => window.innerWidth <= 680;

  function openSidebar() {
    sidebar.classList.add('open');
    toggle.classList.add('show');
    toggle.classList.add('sidebar-open');
    toggle.setAttribute('aria-expanded', 'true');
  }

  function closeSidebar() {
    if (isMobile()) {
      sidebar.classList.remove('open');
      toggle.classList.add('show');
      toggle.classList.remove('sidebar-open');
      toggle.setAttribute('aria-expanded', 'false');
    }
  }

  function toggleSidebar() {
    if (!isMobile()) return;
    if (sidebar.classList.contains('open')) closeSidebar();
    else openSidebar();
  }

  toggle.addEventListener('click', toggleSidebar);
  close.addEventListener('click', closeSidebar);

  if (isMobile()) {
    toggle.classList.add('show');
    toggle.setAttribute('aria-expanded', 'false');
  }

  window.addEventListener('resize', () => {
    if (!isMobile()) {
      sidebar.classList.remove('open');
      toggle.classList.remove('show');
      toggle.classList.remove('sidebar-open');
      toggle.setAttribute('aria-expanded', 'false');
    } else if (!sidebar.classList.contains('open')) {
      toggle.classList.add('show');
      toggle.classList.remove('sidebar-open');
      toggle.setAttribute('aria-expanded', 'false');
    }
  });
}

// ── API key ───────────────────────────────────────────────
function setupApiKey() {
  const input = document.getElementById('api-key-input');
  const btn = document.getElementById('save-api-key');

  if (apiKey) {
    input.value = apiKey;
    setTimeout(initMap, 80);
  }

  btn.addEventListener('click', () => {
    const key = input.value.trim();
    if (!key) { toast('Enter an API key first'); return; }
    apiKey = key;
    localStorage.setItem('ola_api_key', key);
    toast('API key saved');
    if (!map) initMap();
    else toast('API key updated — reload to apply');
  });

  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') btn.click(); });
}

// ── Directions ready check ────────────────────────────────
function checkDirectionsReady() {
  document.getElementById('get-directions-btn').disabled = !(selectedOrigin && selectedDest);
}

// ── Inject spin keyframe ──────────────────────────────────
const style = document.createElement('style');
style.textContent = `@keyframes spin { to { transform: rotate(360deg); } }`;
document.head.appendChild(style);

// ── Boot ──────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  setupTabs();
  setupSidebar();
  setupApiKey();
  setupSearch();
  setupDirections();
  setupModeSelector();
  setupMapControls();
});
