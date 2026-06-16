#!/usr/bin/env node

import { WebSocketServer, WebSocket } from 'ws';

const PORT = 9001;
const DISPLAY_URL = `file:///home/pannaga/Desktop/pro/trakio/pi-display/index.html?ws=ws://localhost:${PORT}`;
const POINTS_PER_SEGMENT = 6;
const TICK_MS = 200;

const anchorRouteCoords = [
  [12.9352, 77.6245],
  [12.9360, 77.6250],
  [12.9370, 77.6258],
  [12.9380, 77.6265],
  [12.9390, 77.6275],
  [12.9400, 77.6285],
  [12.9410, 77.6295],
  [12.9420, 77.6308],
  [12.9430, 77.6320],
  [12.9440, 77.6330],
  [12.9450, 77.6340],
  [12.9460, 77.6350],
  [12.9470, 77.6360],
  [12.9480, 77.6370],
  [12.9490, 77.6380],
  [12.9500, 77.6390]
];

const routeCoords = densifyRoute(anchorRouteCoords, POINTS_PER_SEGMENT);

const routeSteps = [
  {
    maneuver: 'depart',
    instructions: 'Head northeast and ease onto Indiranagar Road',
    distance: 520,
    end_location: { lat: 12.9360, lng: 77.6250 }
  },
  {
    maneuver: 'continue',
    instructions: 'Continue straight on 12th Main Road',
    distance: 760,
    end_location: { lat: 12.9380, lng: 77.6265 }
  },
  {
    maneuver: 'turn-right',
    instructions: 'Turn right onto 100 Feet Road and stay in lane',
    distance: 640,
    end_location: { lat: 12.9410, lng: 77.6295 }
  },
  {
    maneuver: 'continue',
    instructions: 'Continue ahead toward Koramangala 5th Block',
    distance: 420,
    end_location: { lat: 12.9500, lng: 77.6390 }
  }
];

const stepEndCoordIdxs = routeSteps.map((step) => findCoordIndex(routeCoords, step.end_location));
const SIMULATED_CALL_IDX = Math.floor(routeCoords.length * 0.35);

let currentStepIdx = 0;
let currentCoordIdx = 0;
let simulationRunning = false;
let simulationInterval = null;
let simulatedCallSent = false;

const wss = new WebSocketServer({ port: PORT });

console.log(`\ntrakio test WebSocket server running on ws://localhost:${PORT}`);
console.log(`Open: ${DISPLAY_URL}\n`);
console.log('Commands:');
console.log('  [s] - Start simulation');
console.log('  [x] - Stop simulation');
console.log('  [q] - Quit\n');

wss.on('connection', (ws) => {
  console.log('Client connected');

  ws.on('message', (message) => {
    console.log(`Received: ${message}`);
  });

  ws.on('close', () => {
    console.log('Client disconnected');
    stopSimulation();
  });

  ws.on('error', (error) => {
    console.error('WebSocket error:', error.message);
  });

  if (!simulationRunning && wss.clients.size > 0) {
    setTimeout(() => {
      if (wss.clients.size > 0 && !simulationRunning) {
        startSimulation();
      }
    }, 500);
  }
});

function sendRouteStart(ws) {
  const msg = {
    type: 'route_start',
    origin: {
      lat: routeCoords[0][0],
      lng: routeCoords[0][1],
      name: 'Indiranagar'
    },
    destination: {
      lat: routeCoords[routeCoords.length - 1][0],
      lng: routeCoords[routeCoords.length - 1][1],
      name: 'Koramangala'
    },
    routeCoords,
    steps: routeSteps,
    display_mode: 'map'
  };

  ws.send(JSON.stringify(msg));
  console.log('Sent route_start');
}

function sendPositionUpdate(ws) {
  if (currentCoordIdx >= routeCoords.length) {
    const msg = {
      type: 'position',
      lat: routeCoords[routeCoords.length - 1][0],
      lng: routeCoords[routeCoords.length - 1][1],
      heading: 45,
      step_idx: routeSteps.length - 1,
      step: 'Arriving at destination',
      dist_next: '0 m',
      arrived: true
    };

    ws.send(JSON.stringify(msg));
    console.log('Sent arrived message');
    stopSimulation();
    return;
  }

  while (currentStepIdx < stepEndCoordIdxs.length - 1 && currentCoordIdx > stepEndCoordIdxs[currentStepIdx]) {
    currentStepIdx += 1;
  }

  const [lat, lng] = routeCoords[currentCoordIdx];
  const step = routeSteps[currentStepIdx];
  const distToNext = getRemainingStepDistance(currentCoordIdx, currentStepIdx);
  const heading = getHeading(currentCoordIdx);

  if (!simulatedCallSent && currentCoordIdx >= SIMULATED_CALL_IDX) {
    simulatedCallSent = true;
    ws.send(JSON.stringify({
      type: 'incoming_call',
      caller: 'Aarav Calling'
    }));
    console.log('Sent incoming_call');
  }

  const msg = {
    type: 'position',
    lat,
    lng,
    heading,
    step_idx: currentStepIdx,
    step: step?.instructions || 'Continue',
    dist_next: formatDistance(distToNext),
    arrived: false
  };

  ws.send(JSON.stringify(msg));
  console.log(`Position: [${lat.toFixed(4)}, ${lng.toFixed(4)}] Step ${currentStepIdx + 1}/${routeSteps.length}`);

  currentCoordIdx += 1;
}

function startSimulation() {
  if (simulationRunning) {
    return;
  }

  if (wss.clients.size === 0) {
    console.log('Waiting for client connection...');
    return;
  }

  console.log('\nStarting navigation simulation...\n');
  simulationRunning = true;
  currentStepIdx = 0;
  currentCoordIdx = 0;
  simulatedCallSent = false;

  for (const ws of wss.clients) {
    if (ws.readyState === WebSocket.OPEN) {
      sendRouteStart(ws);
    }
  }

  simulationInterval = setInterval(() => {
    for (const ws of wss.clients) {
      if (ws.readyState === WebSocket.OPEN) {
        sendPositionUpdate(ws);
      }
    }
  }, TICK_MS);
}

function stopSimulation() {
  if (simulationInterval) {
    clearInterval(simulationInterval);
    simulationInterval = null;
  }

  simulationRunning = false;
  console.log('\nSimulation stopped\n');
}

function formatDistance(meters) {
  if (meters >= 1000) {
    return `${(meters / 1000).toFixed(1)} km`;
  }

  return `${Math.round(meters)} m`;
}

function densifyRoute(points, subdivisions) {
  const dense = [];

  for (let idx = 0; idx < points.length - 1; idx += 1) {
    const [startLat, startLng] = points[idx];
    const [endLat, endLng] = points[idx + 1];

    if (idx === 0) {
      dense.push([startLat, startLng]);
    }

    for (let step = 1; step <= subdivisions; step += 1) {
      const t = step / subdivisions;
      dense.push([
        startLat + ((endLat - startLat) * t),
        startLng + ((endLng - startLng) * t)
      ]);
    }
  }

  return dense;
}

function findCoordIndex(points, target) {
  return points.findIndex(([lat, lng]) => Math.abs(lat - target.lat) < 1e-9 && Math.abs(lng - target.lng) < 1e-9);
}

function getRemainingStepDistance(coordIdx, stepIdx) {
  const step = routeSteps[stepIdx];
  if (!step) {
    return 0;
  }

  const stepEndIdx = stepEndCoordIdxs[stepIdx];
  const stepStartIdx = stepIdx === 0 ? 0 : stepEndCoordIdxs[stepIdx - 1] + 1;
  const span = Math.max(1, stepEndIdx - stepStartIdx + 1);
  const remaining = Math.max(0, stepEndIdx - coordIdx);
  return Math.max(0, Math.round(step.distance * (remaining / span)));
}

function getHeading(coordIdx) {
  const [lat, lng] = routeCoords[coordIdx];
  const [nextLat, nextLng] = routeCoords[Math.min(coordIdx + 1, routeCoords.length - 1)];
  const y = Math.sin((nextLng - lng) * (Math.PI / 180)) * Math.cos(nextLat * (Math.PI / 180));
  const x =
    Math.cos(lat * (Math.PI / 180)) * Math.sin(nextLat * (Math.PI / 180))
    - Math.sin(lat * (Math.PI / 180)) * Math.cos(nextLat * (Math.PI / 180)) * Math.cos((nextLng - lng) * (Math.PI / 180));
  return (Math.atan2(y, x) * (180 / Math.PI) + 360) % 360;
}

if (process.stdin.isTTY) {
  process.stdin.setRawMode(true);
}

process.stdin.resume();
process.stdin.setEncoding('utf8');
process.stdin.on('data', (key) => {
  switch (key) {
    case 's':
    case 'S':
      startSimulation();
      break;
    case 'x':
    case 'X':
      stopSimulation();
      break;
    case 'q':
    case 'Q':
    case '\u0003':
      console.log('\nShutting down...\n');
      wss.close();
      process.exit(0);
      break;
    default:
      break;
  }
});
