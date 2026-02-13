// webpage.cpp (DROP-IN: Dashboard + Hover Tooltip + Farben)
// ------------------------------------------------------------
// Routes:
//   /                  -> Dashboard (Glukose + Chart)
//   /configuration      -> Config UI (dein bisheriges index_html)
//   /config             -> Redirect auf /configuration
//   /api/glucose        -> latest JSON
//   /api/glucose/history-> history JSON
// ------------------------------------------------------------

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "webpage.h"

// Dashboard HTML ist absichtlich HIER im .cpp definiert (nicht im Header),
// damit es garantiert im gleichen Translation Unit sichtbar ist.
static const char dashboard_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>Glukose Dashboard</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 16px; }
    .topbar { display:flex; align-items:center; justify-content:space-between; gap:12px; }
    .value { font-size: 56px; font-weight: 700; line-height:1; }
    .unit { font-size: 16px; opacity: .7; margin-left: 8px; }
    .meta { opacity:.7; font-size: 13px; }
    .btn { display:inline-block; padding:10px 12px; border:1px solid #ccc; border-radius:10px; text-decoration:none; color:#111; }
    .card { margin-top: 14px; padding: 14px; border:1px solid #e5e5e5; border-radius: 14px; }
    canvas { width: 100%; height: 220px; display:block; cursor: crosshair; }
    .row { display:flex; gap:12px; flex-wrap:wrap; }
    .pill { padding:6px 10px; border-radius:999px; background:#f2f2f2; font-size:13px; }
  </style>
</head>
<body>
  <div class="topbar">
    <div>
      <div id="glucose" class="value">--<span class="unit">mg/dL</span></div>
      <div id="subline" class="meta">Δ -- mg/dL • Trend --</div>
      <div id="targets" class="meta">Target: -- - --</div>
    </div>
    <a class="btn" href="/configuration">Configuration</a>
  </div>

  <div class="card">
    <canvas id="chart" width="900" height="320"></canvas>
    <div class="row" style="margin-top:10px">
      <div class="pill" id="status">Status: --</div>
      <div class="pill" id="updated">Update: --</div>
    </div>
  </div>

<script>
let lastSig = 0;
let lastHistory = null;
let hoverIndex = -1;

// Layout constants (müssen mit drawChart() übereinstimmen)
const PAD_L = 30, PAD_R = 10, PAD_T = 10, PAD_B = 24;

async function fetchJson(url) {
  const r = await fetch(url, {cache:"no-store"});
  if (!r.ok) throw new Error(url + " HTTP " + r.status);
  return await r.json();
}

function drawChart(history) {
  const c = document.getElementById("chart");
  const ctx = c.getContext("2d");
  const W = c.width, H = c.height;

  ctx.clearRect(0,0,W,H);

  const values = (history && history.values) ? history.values : [];
  if (!values.length) {
    ctx.font = "18px Arial";
    ctx.fillStyle = "#000";
    ctx.fillText("Keine Daten", 20, 40);
    return;
  }

  // min/max nur aus non-null
  let minV = Infinity, maxV = -Infinity;
  let anyPoint = false;
  for (const v of values) {
    if (v === null) continue;
    anyPoint = true;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  if (!anyPoint) {
    ctx.font = "18px Arial";
    ctx.fillStyle = "#000";
    ctx.fillText("Keine History-Daten", 20, 40);
    return;
  }

  const plotW = W - PAD_L - PAD_R;
  const plotH = H - PAD_T - PAD_B;

  const range = Math.max(40, maxV - minV);
  const lo = minV - range*0.15;
  const hi = maxV + range*0.15;

  function xOf(i) { return PAD_L + (i/(values.length-1))*plotW; }
  function yOf(v) { return PAD_T + (1 - ((v - lo)/(hi - lo)))*plotH; }

  // grid
  ctx.globalAlpha = 0.15;
  ctx.strokeStyle = "#000";
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let i=0;i<=4;i++){
    const y = PAD_T + (i/4)*plotH;
    ctx.moveTo(PAD_L, y); ctx.lineTo(PAD_L+plotW, y);
  }
  ctx.stroke();
  ctx.globalAlpha = 1;

  // target lines (orange)
    const low = history.low ?? null;
    const high = history.high ?? null;

    ctx.globalAlpha = 0.9;
    ctx.strokeStyle = "#f97316";   // schönes Orange
    ctx.lineWidth = 1.5;

    ctx.beginPath();
    if (low !== null) {
    const y = yOf(low);
    ctx.moveTo(PAD_L, y);
    ctx.lineTo(PAD_L + plotW, y);
    }
    if (high !== null) {
    const y = yOf(high);
    ctx.moveTo(PAD_L, y);
    ctx.lineTo(PAD_L + plotW, y);
    }
    ctx.stroke();

    ctx.globalAlpha = 1;

  // line segments with alert colors (wie LVGL: normal/alert + gaps)
  ctx.lineWidth = 2;
  for (let i=1;i<values.length;i++){
    const v1 = values[i-1];
    const v2 = values[i];
    if (v1 === null || v2 === null) continue;

    if (low !== null && v2 < low) ctx.strokeStyle = "#2563eb";          // blau (unter Low)
    else if (high !== null && v2 > high) ctx.strokeStyle = "#dc2626";   // rot  (über High)
    else ctx.strokeStyle = "#111111";                                   // normal

    ctx.beginPath();
    ctx.moveTo(xOf(i-1), yOf(v1));
    ctx.lineTo(xOf(i),   yOf(v2));
    ctx.stroke();
  }

  // last valid point marker
  for (let i = values.length-1; i >= 0; i--) {
    const v = values[i];
    if (v === null) continue;
    const x = xOf(i), y = yOf(v);
    ctx.fillStyle = "#000";
    ctx.beginPath();
    ctx.arc(x,y,4,0,Math.PI*2);
    ctx.fill();
    break;
  }

  // ---- Hover overlay (Tooltip) ----
  if (hoverIndex >= 0 && hoverIndex < values.length) {
    const v = values[hoverIndex];
    if (v !== null) {
      const x = xOf(hoverIndex);
      const y = yOf(v);

      // crosshair line
      ctx.strokeStyle = "rgba(0,0,0,0.35)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, PAD_T);
      ctx.lineTo(x, PAD_T + plotH);
      ctx.stroke();

      // highlight point
      ctx.fillStyle = "#000";
      ctx.beginPath();
      ctx.arc(x, y, 5, 0, Math.PI*2);
      ctx.fill();

      // tooltip box
      const text = v + " mg/dL";
      ctx.font = "14px Arial";
      const tw = ctx.measureText(text).width;
      const bx = Math.min(x + 10, W - (tw + 16) - 10);
      const by = Math.max(y - 28, 18);

      ctx.fillStyle = "rgba(255,255,255,0.92)";
      ctx.strokeStyle = "rgba(0,0,0,0.2)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.rect(bx, by - 16, tw + 16, 22);
      ctx.fill();
      ctx.stroke();

      ctx.fillStyle = "#000";
      ctx.fillText(text, bx + 8, by);
    }
  }
}

function setupHover() {
  const canvas = document.getElementById("chart");
  if (!canvas) return;

  canvas.addEventListener("mousemove", function(e) {
    if (!lastHistory || !lastHistory.values || lastHistory.values.length === 0) return;

    const rect = canvas.getBoundingClientRect();
    const x = (e.clientX - rect.left) * (canvas.width / rect.width);

    const plotW = canvas.width - PAD_L - PAD_R;
    const rel = (x - PAD_L) / plotW;

    if (rel < 0 || rel > 1) hoverIndex = -1;
    else hoverIndex = Math.round(rel * (lastHistory.values.length - 1));

    drawChart(lastHistory);
  });

  canvas.addEventListener("mouseleave", function() {
    hoverIndex = -1;
    if (lastHistory) drawChart(lastHistory);
  });

  // Touch (optional): tippe/ziehe auf dem Graph
  canvas.addEventListener("touchstart", function(e) {
    if (!lastHistory || !lastHistory.values) return;
    const t = e.touches[0];
    const rect = canvas.getBoundingClientRect();
    const x = (t.clientX - rect.left) * (canvas.width / rect.width);

    const plotW = canvas.width - PAD_L - PAD_R;
    const rel = (x - PAD_L) / plotW;

    if (rel < 0 || rel > 1) hoverIndex = -1;
    else hoverIndex = Math.round(rel * (lastHistory.values.length - 1));

    drawChart(lastHistory);
    e.preventDefault();
  }, {passive:false});

  canvas.addEventListener("touchmove", function(e) {
    if (!lastHistory || !lastHistory.values) return;
    const t = e.touches[0];
    const rect = canvas.getBoundingClientRect();
    const x = (t.clientX - rect.left) * (canvas.width / rect.width);

    const plotW = canvas.width - PAD_L - PAD_R;
    const rel = (x - PAD_L) / plotW;

    if (rel < 0 || rel > 1) hoverIndex = -1;
    else hoverIndex = Math.round(rel * (lastHistory.values.length - 1));

    drawChart(lastHistory);
    e.preventDefault();
  }, {passive:false});
}

async function refresh() {
  try {
    const latest = await fetchJson("/api/glucose");
    const mgdl = latest.mgdl ?? "--";
    const low = latest.low ?? "--";
    const high = latest.high ?? "--";
    const d = Number(delta);
    const deltaTxt = Number.isFinite(d) ? (d > 0 ? `+${d}` : `${d}`) : "--";
    document.getElementById("subline").textContent = `Δ ${deltaTxt} mg/dL • Trend ${trend}`;

    const el = document.getElementById("glucose");
    el.innerHTML = `${mgdl}<span class="unit">mg/dL</span>`;

    // Farbe je nach Limit
    const m = Number(mgdl);
    const lo = Number(low);
    const hi = Number(high);
    if (Number.isFinite(m) && Number.isFinite(lo) && Number.isFinite(hi)) {
      if (m < lo) el.style.color = "#2563eb";      // blau
      else if (m > hi) el.style.color = "#dc2626"; // rot
      else el.style.color = "#111111";             // normal
    }

    document.getElementById("targets").textContent = `Target: ${low} - ${high}`;
    document.getElementById("status").textContent = `Status: ${latest.ts_ok ? "Zeit OK" : "Zeit ungültig"}`;
    document.getElementById("updated").textContent = `Update: ${new Date().toLocaleTimeString()}`;
  } catch(e) {
    document.getElementById("status").textContent = "Status: Fehler /api/glucose";
  }

  try {
    const history = await fetchJson("/api/glucose/history");
    const sig = JSON.stringify(history).length;

    // Wichtig: lastHistory immer aktualisieren (für Hover)
    lastHistory = history;

    if (sig !== lastSig) {
      drawChart(history);
      lastSig = sig;
    } else {
      drawChart(history);
    }
  } catch(e) {
    // ignore
  }
}

setupHover();
refresh();
setInterval(refresh, 15000);
</script>
</body>
</html>
)rawliteral";

// ------------ Forward decls ------------
static void handleDashboard(AsyncWebServerRequest *request);
static void handleConfiguration(AsyncWebServerRequest *request);
static void handleConfigRedirect(AsyncWebServerRequest *request);
static void handleApiGlucose(AsyncWebServerRequest *request);
static void handleApiGlucoseHistory(AsyncWebServerRequest *request);

// These can be overridden by providing web_glucose_api.cpp
__attribute__((weak)) String web_get_glucose_latest_json() {
    // Placeholder (so it compiles even without web_glucose_api.cpp)
    return String("{\"mgdl\":null,\"low\":null,\"high\":null,\"ts_ok\":false}");
}

__attribute__((weak)) String web_get_glucose_history_json() {
    // Placeholder
    return String("{\"low\":null,\"high\":null,\"values\":[]}");
}

// ------------ Handlers ------------
static void handleDashboard(AsyncWebServerRequest *request) {
    request->send(200, "text/html", dashboard_html);
}

static void handleConfiguration(AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
}

static void handleConfigRedirect(AsyncWebServerRequest *request) {
    request->redirect("/configuration");
}

static void handleApiGlucose(AsyncWebServerRequest *request) {
    request->send(200, "application/json", web_get_glucose_latest_json());
}

static void handleApiGlucoseHistory(AsyncWebServerRequest *request) {
    request->send(200, "application/json", web_get_glucose_history_json());
}

// ------------ Route registration ------------
void register_webpage_routes(AsyncWebServer& server) {
    server.on("/",                    HTTP_GET, handleDashboard);
    server.on("/configuration",       HTTP_GET, handleConfiguration);
    server.on("/config",              HTTP_GET, handleConfigRedirect);

    // Wichtig: LÄNGERE Route zuerst (sonst matched /api/glucose auch /api/glucose/history)
    server.on("/api/glucose/history", HTTP_GET, handleApiGlucoseHistory);
    server.on("/api/glucose",         HTTP_GET, handleApiGlucose);

    // Falls du weitere bestehende Routes hattest (scan/login/connect/...),
    // und sie in deinem alten webpage.cpp definiert sind: diese Datei ersetzt alles.
    // Dann musst du sie hier wieder ergänzen ODER du nutzt diese Datei nur als Patch-Basis.
}
