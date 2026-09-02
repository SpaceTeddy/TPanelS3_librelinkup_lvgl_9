// webpage.cpp (MERGED: Dashboard + Config + Legacy Config Handlers)
// ------------------------------------------------------------
// - /                 -> Dashboard (dark mode, chart, etc.)
// - /configuration    -> Config UI (index_html) with BasicAuth (uses LLU login_email/login_password)
// - Legacy endpoints used by the Config page are preserved:
//     /scan, /login, /connect, /status, /toggle, /setBrightness,
//     /configureWireGuard, /configureMQTT
// - API endpoints for dashboard:
//     /api/glucose, /api/glucose/history
// - Optional config prefill API (if web_config_api.cpp provided):
//     /api/config
// ------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>

#ifdef ESP32
#include <esp_system.h>
#include <esp_heap_caps.h>
#endif

#include <librelinkup.h>

#include <string>
#include <vector>
#include <uuid/common.h>
#include <uuid/console.h>
#include <uuid/telnet.h>
#include <uuid/log.h>

#include "webpage.h"
#include "settings.h"
#include "tpanels3.h"
#include "main.h"
#include "http_update.h"
#include "mqtt.h"
#include "mqtt_handler.h"
#include "h2_ota.h"
#include "zigbee_h2.h"
#include "app_fsm.h"
#include <Update.h>
#include "ota_handler.h"

extern MQTT         mqtt;
extern PubSubClient mqtt_client;

//------------------------[ uuid logger ]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//-------------------------------------------------------------------------

extern SETTINGS settings;
extern TPanelS3 tpanels3;
extern void     ui_blank_screen_for_reset();

extern LIBRELINKUP librelinkup;
extern String g_h2_fw_version;
extern String g_h2_fw_build;
extern String g_h2_chip_model;
extern String g_h2_chip_rev;
extern String g_h2_chip_mac;
extern String g_h2_chip_cores;
extern String g_h2_chip_cpu_mhz;
extern String g_h2_chip_xtal_mhz;
extern String g_h2_chip_features;
extern String g_h2_last_type;
extern String g_h2_last_json;
extern uint32_t g_h2_last_seen_ms;


// int16_t fix (used by debug endpoint)
extern int16_t glucose_delta;
// For handlers needing server access (OTA toggle)
static AsyncWebServer* g_server = nullptr;

// Forward declarations (used by debug endpoint)
String web_get_glucose_latest_json();

// Local state (legacy)
static String username;
static String password;
static String wifi_bssid;
static String wifi_password;
static uint32_t g_h2_info_req_last_ms = 0;

static void maybe_request_h2_info()
{
    if (h2_ota_in_progress()) return;
    const uint32_t now = millis();
    if ((now - g_h2_info_req_last_ms) < 15000U)
        return;
    g_h2_info_req_last_ms = now;
    h2_send("{\"cmd\":\"version\"}");
    h2_send("{\"cmd\":\"chipinfo\"}");
}

static const char dashboard_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Glucose Dashboard</title>
<style>
  :root{
    --bg:#ffffff;
    --fg:#0b0f14;
    --muted:#6b7280;
    --card:#ffffff;
    --border:#e5e7eb;

    --ok:#16a34a;
    --warn:#eab308;
    --bad:#dc2626;
    --used:#c8ced8;

    --targetFill: rgba(34,197,94,0.12);
    --gridLight: rgba(0,0,0,0.08);

    --tooltipBg: rgba(255,255,255,0.92);
    --tooltipBorder: rgba(0,0,0,0.15);
  }
  body.dark{
    --bg:#0b0f14;
    --fg:#e7eaf0;
    --muted: rgba(231,234,240,.75);
    --card:#0f1620;
    --border:#1e2a3a;
    --used:#3a475a;
    --targetFill: rgba(34,197,94,0.18);
    --gridLight: rgba(231,234,240,0.10);
    --tooltipBg: rgba(15,22,32,0.92);
    --tooltipBorder: rgba(231,234,240,0.18);
  }
  @media (prefers-color-scheme: dark) {
    body:not(.light){
      --bg:#0b0f14;
      --fg:#e7eaf0;
      --muted: rgba(231,234,240,.75);
      --card:#0f1620;
      --border:#1e2a3a;
      --used:#3a475a;
      --targetFill: rgba(34,197,94,0.18);
      --gridLight: rgba(231,234,240,0.10);
      --tooltipBg: rgba(15,22,32,0.92);
      --tooltipBorder: rgba(231,234,240,0.18);
    }
  }

  body{
    margin:0;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
    background:var(--bg);
    color:var(--fg);
    padding:18px;
  }

  .topbar{display:flex; align-items:flex-start; justify-content:space-between; gap:12px;}
  .value{font-size:72px; font-weight:900; letter-spacing:-1px; line-height:1;}
  .unit{font-size:18px; font-weight:700; margin-left:10px; opacity:.75;}
  .sub{margin-top:8px; color:var(--muted); font-size:18px; display:flex; align-items:center; gap:10px;}

  .btn{
    display:inline-flex; align-items:center; gap:8px;
    padding:10px 12px;
    border:1px solid var(--border);
    border-radius:12px;
    background:var(--card);
    color:var(--fg);
    text-decoration:none;
  }
  .iconbtn{width:44px; justify-content:center; cursor:pointer;}

  .life-wrap{margin-top:14px;}
  .life-bar{display:flex; gap:4px; margin-top:10px;}
  .life-seg{flex:1; height:12px; border-radius:4px; background:var(--used);}
  .life-seg.ok{background:var(--ok);}
  .life-seg.warn{background:var(--warn);}
  .life-seg.bad{background:var(--bad);}
  .life-text{margin-top:8px; color:var(--muted); font-size:18px;}

  .card{margin-top:16px; border:1px solid var(--border); background:var(--card); border-radius:16px; padding:14px;}

  .controls{display:flex; gap:8px; align-items:center; flex-wrap:wrap; margin-bottom:10px;}
  .seg button{
    padding:8px 10px;
    border-radius:10px;
    border:1px solid var(--border);
    background:var(--card);
    color:var(--fg);
    cursor:pointer;
  }
  .seg button.active{outline:2px solid var(--ok); outline-offset:-1px;}

  canvas{width:100%; height:320px; display:block; cursor:crosshair;}
  .pills{display:flex; gap:10px; flex-wrap:wrap; margin-top:12px;}
  .pill{padding:6px 10px; border-radius:999px; border:1px solid var(--border); background:transparent; color:var(--muted); font-size:13px;}
</style>
</head>
<body>

<div class="topbar">
  <div>
    <div id="glucose" class="value">--<span class="unit">mg/dL</span></div>
    <div id="subline" class="sub">Δ -- • Trend --</div>

    <div class="life-wrap">
      <div class="life-bar" id="lifeBar"></div>
      <div class="life-text" id="lifeText">--</div>
    </div>
  </div>

  <div style="display:flex; gap:10px; align-items:center; margin-right:0px;">
    <a class="btn" href="/configuration">Config</a>
    <a class="btn" href="/debug">Debug</a>
    <button class="btn iconbtn" id="themeBtn" title="Toggle dark mode">🌓</button>
  </div>
</div>

<!-- Firmware update notice. Hidden unless /api/fw/status reports one. This only
     points at the config page: the install controls stay there, so the open
     dashboard cannot trigger a flash without the config page's authentication. -->
<a id="fwNotice" class="card" href="/configuration"
   style="display:none; text-decoration:none; color:inherit; border-left:4px solid var(--warn);">
  <strong>Firmware update available</strong>
  <span id="fwNoticeVersion" style="color:var(--muted);"></span>
</a>

<div class="card">
  <div class="controls">
    <div class="seg">
      <button id="btn3h">3h</button>
      <button id="btn6h">6h</button>
      <button id="btn12h" class="active">12h</button>
    </div>
    <span class="pill" id="rangeInfo">Range: --</span>
  </div>

  <canvas id="chart"></canvas>

  <div class="pills">
    <div class="pill" id="status">Status: --</div>
    <div class="pill" id="updated">Updated: --</div>
    <div class="pill" id="hover">Cursor: --</div>
  </div>
</div>

<script>
// (Dashboard JS unchanged)
let lastHistory = null;
let view = { values:[], ts:[], low:null, high:null, live:[], from:0, to:0 };
let hoverIndex = -1;
let zoomHours = 12; // default

// Pulse animation for the last live point (green/white with halo)
let pulseActive = false;
let pulseNow = 0;
let pulseRaf = 0;
function prefersReducedMotion(){
  return window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
}
function pulseLoop(t){
  pulseNow = t || performance.now();
  if(!pulseActive){ pulseRaf = 0; return; }
  drawChart();
  pulseRaf = requestAnimationFrame(pulseLoop);
}
function setPulse(active){
  if(prefersReducedMotion()) active = false;
  if(active && !pulseActive){
    pulseActive = true;
    if(!pulseRaf) pulseRaf = requestAnimationFrame(pulseLoop);
  } else if(!active && pulseActive){
    pulseActive = false;
  }
}
document.addEventListener('visibilitychange', ()=>{
  if(document.hidden) setPulse(false);
});

// PAD_T carries the hover value readout, which is drawn above the plot so the
// curve can never run through it. dpr-scaled because the readout font is: it
// has to fit the glyphs plus a 3/4-font gap down to the plot, so the value
// reads as its own row rather than as part of the chart.
const PAD_L=54, PAD_R=14, PAD_T=Math.max(60, Math.round((window.devicePixelRatio||1)*34)), PAD_B=40;

function css(name){ return getComputedStyle(document.body).getPropertyValue(name).trim(); }

function applyThemeFromStorage(){
  const v = localStorage.getItem("theme"); // dark|light|null
  document.body.classList.remove("dark","light");
  if (v==="dark") document.body.classList.add("dark");
  if (v==="light") document.body.classList.add("light");
}
function toggleTheme(){
  const isDark = document.body.classList.contains("dark") ||
    (!document.body.classList.contains("light") && matchMedia("(prefers-color-scheme: dark)").matches);
  localStorage.setItem("theme", isDark ? "light" : "dark");
  applyThemeFromStorage();
  drawChart();
}

function pad2(n){ return String(n).padStart(2,'0'); }
function fmtTime(ts){
  if(!Number.isFinite(ts) || ts<=0) return "--:--";
  const d=new Date(ts*1000);
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}
function fmtDateTime(ts){
  if(!Number.isFinite(ts) || ts<=0) return "--";
  const d=new Date(ts*1000);
  return `${d.getFullYear()}-${pad2(d.getMonth()+1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}
function fmtUptime(ms){
  if(!Number.isFinite(ms) || ms < 0) return "--";
  let s = Math.floor(ms/1000);
  const days = Math.floor(s/86400); s -= days*86400;
  const hours = Math.floor(s/3600); s -= hours*3600;
  const minutes = Math.floor(s/60); s -= minutes*60;
  const seconds = s;
  let out = "";
  if(days > 0) out += `${days}d `;
  out += `${pad2(hours)}h ${pad2(minutes)}m ${pad2(seconds)}s`;
  return out.trim();
}


function ensureLifeBar(count){
  const bar=document.getElementById("lifeBar");
  if(!bar) return;
  if(bar.children.length===count) return;
  bar.innerHTML="";
  for(let i=0;i<count;i++){
    const div=document.createElement("div");
    div.className="life-seg";
    bar.appendChild(div);
  }
}

function updateLifeBar(days,hours,minutes,seconds,runtimeDays){
  const bar=document.getElementById("lifeBar");
  const txt=document.getElementById("lifeText");
  if(!bar || !txt) return;

  const remSec = Math.max(0, (days*86400) + (hours*3600) + (minutes*60) + seconds);
  const remHoursFloat = remSec / 3600.0;
  const remDaysFloat  = remSec / 86400.0;
  const dayBlocks = (runtimeDays === 14 || runtimeDays === 15) ? runtimeDays : 15;

  let cls="ok";
  if(remDaysFloat <= 1.0) cls="bad";
  else if(remDaysFloat <= 3.0) cls="warn";

  let blocks = dayBlocks;
  let filled = 0;

  if(remHoursFloat >= 24.0){
    blocks = dayBlocks;
    filled = Math.ceil(remDaysFloat);
    filled = Math.max(0, Math.min(blocks, filled));
    ensureLifeBar(blocks);

    const dceil = Math.ceil(remDaysFloat);
    txt.textContent = (dceil === 1) ? "1 day remaining" : `${dceil} days remaining`;
  } else if(remHoursFloat >= 1.0){
    blocks = 24;
    filled = Math.ceil(remHoursFloat);
    filled = Math.max(0, Math.min(blocks, filled));
    ensureLifeBar(blocks);

    const hceil = Math.ceil(remHoursFloat);
    txt.textContent = (hceil === 1) ? "1 hour remaining" : `${hceil} hours remaining`;
  } else {
    blocks = 60;
    const remMinFloat = remSec / 60.0;
    filled = Math.ceil(remMinFloat);
    filled = Math.max(0, Math.min(blocks, filled));
    ensureLifeBar(blocks);

    const mceil = Math.ceil(remMinFloat);
    txt.textContent = (mceil === 1) ? "1 minute remaining" : `${mceil} minutes remaining`;
  }

  const segs=[...bar.children];
  for(let i=0;i<segs.length;i++){
    segs[i].classList.remove("ok","warn","bad");
    if(i<filled) segs[i].classList.add(cls);
  }
}


// Warm-up bar: 59 minute blocks (Libre-style warmup ~59min)
function updateWarmupBar(minutesRemaining){
  const bar=document.getElementById("lifeBar");
  const txt=document.getElementById("lifeText");
  if(!bar || !txt) return;

  const blocks = 59;
  const filled = Math.max(0, Math.min(blocks, Math.ceil(Number(minutesRemaining)||0)));

  ensureLifeBar(blocks);
  bar.style.gap = "1px";

  if (filled <= 0) txt.textContent = "Sensor warm-up finished";
  else txt.textContent = (filled === 1) ? "Sensor ready in 1 minute" : `Sensor ready in ${filled} minutes`;

  const segs=[...bar.children];
  for(let i=0;i<segs.length;i++){
    segs[i].classList.remove("ok","warn","bad");
    if(i<filled) segs[i].classList.add("warn");
  }
}

function buildView(history){
  const rawVals = history?.values ?? [];
  const allV = rawVals.map(p => p ? Number(p.v) : null);
  const allT = rawVals.map(p => p ? Number(p.ts) : null);
  const low = history?.low ?? null;
  const high = history?.high ?? null;

  const nowTs = Math.floor(Date.now()/1000);
  const fromTs = nowTs - zoomHours*3600;

  // keep points within [fromTs, nowTs]
  const idx=[];
  for(let i=0;i<allT.length;i++){
    const t = allT[i];
    if(Number.isFinite(t) && t>=fromTs && t<=nowTs) idx.push(i);
  }

  if(!idx.length){
    view = { values: allV, ts: allT, low, high, live: rawVals.map(p=>!!p?.live), from: fromTs, to: nowTs };
  } else {
    view = {
      values: idx.map(i=>allV[i]),
      ts:     idx.map(i=>allT[i]),
      low, high,
      live:   idx.map(i=>!!rawVals[i]?.live),
      from:   fromTs,
      to:     nowTs
    };
  }

  document.getElementById("rangeInfo").textContent =
    `Range: ${fmtTime(view.from)}–${fmtTime(view.to)} (${zoomHours}h)`;
}

function resizeCanvasToDPR(canvas){
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(320, Math.round(rect.width * dpr));
  const h = Math.max(240, Math.round(rect.height * dpr));
  if(canvas.width!==w || canvas.height!==h){
    canvas.width=w; canvas.height=h;
  }
}

function drawAxes(ctx, W, H, plotW, plotH, lo, hi, tsArr){
  // Grid + Y labels (fixed 50 mg/dL steps starting at 50)
  const yOf = (v)=> PAD_T + (1 - ((v - lo)/(hi - lo))) * plotH;

  ctx.strokeStyle = css("--gridLight");
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let val = 50; val <= hi; val += 50){
    const y = yOf(val);
    ctx.moveTo(PAD_L, y);
    ctx.lineTo(PAD_L + plotW, y);
  }
  ctx.stroke();

  // Y labels (50,100,150,...)
  ctx.fillStyle = css("--muted");
  ctx.font = `${Math.round((window.devicePixelRatio||1)*11)}px Arial`;
  for (let val = 50; val <= hi; val += 50){
    const y = yOf(val);
    ctx.fillText(String(val), 8, y + 4);
  }

  // X labels (3–5) based on time range (app-like: axis ends at now)
  const fromTs = view.from || (tsArr && tsArr.length ? tsArr[0] : 0);
  const toTs   = view.to   || (tsArr && tsArr.length ? tsArr[tsArr.length-1] : 0);
  if (Number.isFinite(fromTs) && Number.isFinite(toTs) && toTs > fromTs) {
    const ticks = [0, 0.25, 0.5, 0.75, 1];
    ctx.fillStyle = css("--muted");
    ctx.font = `${Math.round((window.devicePixelRatio||1)*10)}px Arial`;

    for (const f of ticks){
      const t = Math.round(fromTs + f*(toTs-fromTs));
      const label = fmtTime(t);

      let x = PAD_L + f*plotW;
      const y = PAD_T + plotH + 24;

      const tw = ctx.measureText(label).width;

      const minX = PAD_L + 2;
      const maxX = PAD_L + plotW - 2;

      if (f === 0) {
        ctx.textAlign = "left";
        x = Math.max(minX, x);
      } else if (f === 1) {
        ctx.textAlign = "right";
        x = Math.min(maxX, x);
      } else {
        ctx.textAlign = "center";
        x = Math.min(maxX - tw/2, Math.max(minX + tw/2, x));
      }

      ctx.fillText(label, x, y);
    }
    ctx.textAlign = "left";
  }

  // Unit label (inside plot area so it never overlaps top UI)
  const unit = "mg/dL";
  ctx.font = `${Math.round((window.devicePixelRatio||1)*10)}px Arial`;
  const ux = PAD_L + 6;
  const uy = PAD_T + 14;
  const uw = ctx.measureText(unit).width;

  ctx.fillStyle = css("--tooltipBg");
  ctx.strokeStyle = css("--tooltipBorder");
  ctx.lineWidth = 1;
  ctx.beginPath();
  if (ctx.roundRect) ctx.roundRect(ux-6, uy-12, uw+12, 16, 6);
  else ctx.rect(ux-6, uy-12, uw+12, 16);
  ctx.fill();
  ctx.stroke();

  ctx.fillStyle = css("--muted");
  ctx.fillText(unit, ux, uy);
}

function drawChart(){
  const c=document.getElementById("chart");
  resizeCanvasToDPR(c);
  const ctx=c.getContext("2d");
  const W=c.width, H=c.height;

  ctx.clearRect(0,0,W,H);

  const values=view.values || [];
  const tsArr=view.ts || [];
  if(!values.length) return;

  let min=Infinity,max=-Infinity, any=false;
  for(const v of values){
    if(v===null) continue;
    any=true; if(v<min) min=v; if(v>max) max=v;
  }
  if(!any) return;

  const plotW=W-PAD_L-PAD_R;
  const plotH=H-PAD_T-PAD_B;

  // Guard for time scale
  if(!Number.isFinite(view.from) || !Number.isFinite(view.to) || view.to <= view.from){
    return;
  }

  // Fixed Y scale: 50 mg/dL steps starting at 50
  const lo = 50;
  let hi = Math.ceil(max / 50) * 50;
  hi = Math.max(hi, 200);

  const xOfTs = (t)=> PAD_L + ((t - view.from) / (view.to - view.from)) * plotW;
  const xOf = (i)=> xOfTs(tsArr[i]);
  const yOf=v=> PAD_T + (1-((v-lo)/(hi-lo)))*plotH;

  // Target fill
  if(view.low!=null && view.high!=null){
    ctx.fillStyle = css("--targetFill");
    const y1=yOf(view.high), y2=yOf(view.low);
    ctx.fillRect(PAD_L, y1, plotW, y2-y1);
  }

  // Axes/grid
  drawAxes(ctx, W, H, plotW, plotH, lo, hi, tsArr);

  // Line
  ctx.strokeStyle = css("--fg");
  ctx.lineWidth = Math.max(3, Math.round((window.devicePixelRatio||1)*2.5));
  ctx.lineJoin="round"; ctx.lineCap="round";
  ctx.beginPath();
  let started=false;
  for(let i=0;i<values.length;i++){
    const v=values[i];
    if(v===null){ started=false; continue; }
    const x=xOf(i), y=yOf(v);
    if(!started){ ctx.moveTo(x,y); started=true; }
    else ctx.lineTo(x,y);
  }
  ctx.stroke();

  // Last point marker (highlight live point)
let liveFound = false;
for(let i=values.length-1;i>=0;i--){
  const v=values[i];
  if(v===null) continue;
  const x=xOf(i), y=yOf(v);
  const isLive = (view.live && view.live[i] === true);

  if (isLive) {
    liveFound = true;
    const dpr = (window.devicePixelRatio||1);
    const ringR = Math.max(10, Math.round(dpr*7));
    const dotR  = Math.max(6,  Math.round(dpr*4));

    // Pulsing halo (2s period -> frequency halved)
    const tt = (pulseNow || performance.now())/2000;
    const k  = tt - Math.floor(tt); // 0..1
    const ease = 0.5 - 0.5*Math.cos(k*2*Math.PI); // 0..1 smooth
    const haloR = ringR + ease*ringR*1.6;

    ctx.save();
    ctx.globalAlpha = 0.28 * (1.0 - ease);
    ctx.fillStyle = "#22c55e";
    ctx.shadowColor = "rgba(34,197,94,0.9)";
    ctx.shadowBlur  = Math.round(dpr*18*ease);
    ctx.beginPath();
    ctx.arc(x, y, haloR, 0, Math.PI*2);
    ctx.fill();
    ctx.restore();

    // Outer ring (green)
    ctx.fillStyle = "#22c55e";
    ctx.beginPath();
    ctx.arc(x, y, ringR, 0, Math.PI*2);
    ctx.fill();

    // Inner dot (white)
    ctx.fillStyle = "#ffffff";
    ctx.beginPath();
    ctx.arc(x, y, dotR, 0, Math.PI*2);
    ctx.fill();
  } else {
    ctx.fillStyle = css("--fg");
    ctx.beginPath();
    ctx.arc(x,y, Math.max(6, Math.round((window.devicePixelRatio||1)*4)), 0, Math.PI*2);
    ctx.fill();
  }
  break;
}
setPulse(liveFound);

  // Hover/touch cursor — mirrors the LibreLinkUp app: a vertical line through
  // the touched point, a dot on the curve, a value readout above the plot and
  // a time readout pinned below it at the x-axis (both following the cursor's
  // x position, clamped so they never run off the canvas). The device shows
  // the same crosshair (main.cpp: touch_event_cb) but puts its value into the
  // main glucose header instead, where a panel-sized font fits.
  if(hoverIndex>=0 && hoverIndex<values.length && values[hoverIndex]!==null){
    const dpr = (window.devicePixelRatio||1);
    const v=values[hoverIndex];
    const ts=tsArr[hoverIndex];
    const x=xOf(hoverIndex), y=yOf(v);

    // Vertical line
    ctx.strokeStyle = document.body.classList.contains("dark") ? "rgba(231,234,240,0.28)" : "rgba(0,0,0,0.25)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x, PAD_T);
    ctx.lineTo(x, PAD_T+plotH);
    ctx.stroke();

    // Cursor dot on the curve — white with a dark ring, distinct from the
    // pulsing green "live" marker drawn above.
    const dotR = Math.max(5, Math.round(dpr*4.5));
    ctx.fillStyle = "#ffffff";
    ctx.beginPath();
    ctx.arc(x, y, dotR, 0, Math.PI*2);
    ctx.fill();
    ctx.lineWidth = Math.max(2, Math.round(dpr*1.5));
    ctx.strokeStyle = document.body.classList.contains("dark") ? "#14161b" : "#0b0f14";
    ctx.stroke();

    const t1 = `${v} mg/dL`;
    const t2 = fmtTime(ts); // "HH:MM", matching the reference app's cursor label

    // Value readout, pinned in the padding band ABOVE the plot, following x.
    // It used to sit inside the plot, where the curve ran straight through it
    // whenever the reading was near the top of the y-scale.
    ctx.font = `${Math.round(dpr*15)}px Arial`;
    ctx.textBaseline = "alphabetic";
    const t1w = ctx.measureText(t1).width;
    const t1x = Math.min(Math.max(x - t1w/2, PAD_L), PAD_L + plotW - t1w);
    ctx.fillStyle = css("--fg");
    ctx.fillText(t1, t1x, PAD_T - Math.round(dpr*18));

    // Time readout, pinned near the bottom of the plot (the x-axis), following x
    ctx.font = `${Math.round(dpr*11)}px Arial`;
    const t2w = ctx.measureText(t2).width;
    const t2x = Math.min(Math.max(x - t2w/2, PAD_L), PAD_L + plotW - t2w);
    ctx.fillStyle = css("--muted");
    ctx.fillText(t2, t2x, PAD_T + plotH - Math.round(dpr*4));

    document.getElementById("hover").textContent = `Cursor: ${t1} @ ${fmtDateTime(ts)}`;
  } else {
    document.getElementById("hover").textContent = "Cursor: --";
  }
}

function setupHover(){
  const c=document.getElementById("chart");
  function handle(clientX){
    if(!view.values?.length) return;
    const rect=c.getBoundingClientRect();
    const x = (clientX-rect.left) * (c.width/rect.width);

    const plotW = c.width - PAD_L - PAD_R;
    const rel = (x - PAD_L)/plotW;

    if(rel<0 || rel>1 || !Number.isFinite(view.from) || !Number.isFinite(view.to) || view.to<=view.from){
      hoverIndex=-1;
      drawChart();
      return;
    }

    const t = view.from + rel*(view.to - view.from);

    let best=-1, bestDt=1e18;
    for(let i=0;i<view.ts.length;i++){
      const ti=view.ts[i];
      if(!Number.isFinite(ti)) continue;
      const dt=Math.abs(ti - t);
      if(dt<bestDt){ bestDt=dt; best=i; }
    }
    hoverIndex=best;
    drawChart();
  }
  c.addEventListener("mousemove", e=>handle(e.clientX));
  c.addEventListener("mouseleave", ()=>{hoverIndex=-1; drawChart();});
  c.addEventListener("touchstart", e=>{handle(e.touches[0].clientX); e.preventDefault();},{passive:false});
  c.addEventListener("touchmove", e=>{handle(e.touches[0].clientX); e.preventDefault();},{passive:false});
}

function setZoom(h){
  zoomHours=h;
  document.getElementById("btn3h").classList.toggle("active", h===3);
  document.getElementById("btn6h").classList.toggle("active", h===6);
  document.getElementById("btn12h").classList.toggle("active", h===12);
  if(lastHistory){ buildView(lastHistory); hoverIndex=-1; drawChart(); }
}

async function refresh(){
  try{
    const latest = await fetch("/api/glucose", {cache:"no-store"}).then(r=>r.json());

    const sensorOk = (latest.ts_ok === true);
    const mgdl = Number(latest.mgdl);
    const delta = Number(latest.delta);
    const trend = latest.trend ?? "--";

    const g = document.getElementById("glucose");
    const sub = document.getElementById("subline");

    if(sensorOk && Number.isFinite(mgdl) && mgdl > 0){
      const deltaTxt = Number.isFinite(delta)
        ? (delta > 0 ? `+${delta}` : (delta < 0 ? `${delta}` : `±0`))
        : "--";

      g.innerHTML = `${mgdl}<span class="unit">mg/dL</span>`;
      sub.textContent = `Δ ${deltaTxt} • Trend ${trend}`;

      // Colorize large value when outside target range
      const lo = Number(latest.low);
      const hi = Number(latest.high);
      if (Number.isFinite(lo) && Number.isFinite(hi)) {
        if (mgdl < lo) g.style.color = "#dc2626";      // low  -> red
        else if (mgdl > hi) g.style.color = "#dc2626"; // high -> red
        else g.style.color = css("--fg");                // in range
      } else {
        g.style.color = css("--fg");
      }
    } else {
      // Sensor invalid → show placeholders
      g.innerHTML = `---<span class="unit">mg/dL</span>`;
      sub.textContent = `Δ --- • Trend -`;
      g.style.color = css("--fg");
    }

    // Lifetime still independent
    // Warm-up override: prefer sensor_state==SENSOR_STARTING (Libre warmup)
    const sensorState = Number(latest.sensor_state);
    const warmupMin = Number(latest.warmup_min);
    const warmupSec = Number(latest.warmup_sec);
    const warmupActive = (latest.warmup_active === true) || (Number.isFinite(warmupMin) && warmupMin > 0) || (Number.isFinite(warmupSec) && warmupSec > 0);

    if (warmupActive) {
      const m = (Number.isFinite(warmupMin) && warmupMin > 0) ? warmupMin : Math.ceil((Number.isFinite(warmupSec) ? warmupSec : 0) / 60);
      updateWarmupBar(m);
    } else {
      updateLifeBar(
        Number(latest.life_days),
        Number(latest.life_hours),
        Number(latest.life_minutes),
        Number(latest.life_seconds),
        Number(latest.life_runtime_days),
        Number(latest.sensor_state)
      );
    }

    document.getElementById("status").textContent =
      `Status: ${sensorOk ? "Sensor valid" : "Sensor invalid"}`;
    document.getElementById("updated").textContent =
      `Updated: ${new Date().toLocaleTimeString()}`;

  }catch(e){
    const st=document.getElementById("status");
    if(st) st.textContent = "Debug load failed: " + (e && e.message ? e.message : e);
    console.error(e);

    document.getElementById("status").textContent = "Status: /api/glucose error";
  }

  try{
    const hist = await fetch("/api/glucose/history", {cache:"no-store"}).then(r=>r.json());

    // Append live value ONLY if sensor valid
    try{
      const latest = await fetch("/api/glucose", {cache:"no-store"}).then(r=>r.json());
      const mg = Number(latest.mgdl);
      const sensorOk = (latest.ts_ok === true);

      if (sensorOk && Number.isFinite(mg) && mg > 0){
        const arr0 = hist.values || [];

        // Keep max 141 history points
        if (arr0.length > 141)
          hist.values = arr0.slice(arr0.length - 141);

        const arr = hist.values || arr0;

        let lastTs = null;
        for(let i=arr.length-1;i>=0;i--){
          const p = arr[i];
          const ts = p ? Number(p.ts) : null;
          const v  = p ? Number(p.v)  : null;
          if (Number.isFinite(ts) && Number.isFinite(v)) {
            lastTs = ts;
            break;
          }
        }

        const nowTs = Math.floor(Date.now()/1000);
        // Prefer aligned cadence but clamp to "now" if too far behind
        let liveTs = Number.isFinite(lastTs) ? (lastTs + 300) : nowTs;
        if (nowTs - liveTs > 480) liveTs = nowTs;

        hist.values = arr;
        hist.values.push({ v: mg, ts: liveTs, live: true });
      }
    }catch(e){}

    lastHistory = hist;
    buildView(hist);
    drawChart();

  }catch(e){}
}

// init
applyThemeFromStorage();
ensureLifeBar(15);
setupHover();

document.getElementById("themeBtn").addEventListener("click", toggleTheme);
document.getElementById("btn3h").addEventListener("click", ()=>setZoom(3));
document.getElementById("btn6h").addEventListener("click", ()=>setZoom(6));
document.getElementById("btn12h").addEventListener("click", ()=>setZoom(12));
setZoom(12);

window.addEventListener("resize", ()=>drawChart());

refresh();
async function refreshFwNotice(){
  try{
    const d = await fetch("/api/fw/status", {cache:"no-store"}).then(r=>r.json());
    const el = document.getElementById("fwNotice");
    if(!el) return;
    if(d.update_available){
      document.getElementById("fwNoticeVersion").textContent =
        " \u2014 v" + (d.latest_version || "?") + ", installed v" + (d.current_version || "?");
      el.style.display = "block";
    }else{
      el.style.display = "none";
    }
  }catch(e){
    // Leave the notice as it is: a failed poll says nothing about the firmware.
  }
}
refreshFwNotice();
setInterval(refreshFwNotice, 60000);

setInterval(refresh, 15000);
</script>
</body>
</html>
)rawliteral";


// -------------------- Debug page --------------------
static const char debug_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Debug</title>
<style>
  :root{
    --bg:#ffffff;
    --fg:#0b0f14;
    --muted:#6b7280;
    --card:#ffffff;
    --border:#e5e7eb;
    --ok:#16a34a;
    --warn:#eab308;
    --bad:#dc2626;
    --mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  }
  body.dark{
    --bg:#0b0f14;
    --fg:#e7eaf0;
    --muted: rgba(231,234,240,.75);
    --card:#0f1620;
    --border:#1e2a3a;
  }
  @media (prefers-color-scheme: dark) {
    body:not(.light){
      --bg:#0b0f14;
      --fg:#e7eaf0;
      --muted: rgba(231,234,240,.75);
      --card:#0f1620;
      --border:#1e2a3a;
    }
  }

  body{
    margin:0;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
    background:var(--bg);
    color:var(--fg);
    padding:18px;
  }

  .topbar{display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:14px;}
  .btn{
    display:inline-flex; align-items:center; gap:8px;
    padding:10px 12px;
    border:1px solid var(--border);
    border-radius:12px;
    background:var(--card);
    color:var(--fg);
    text-decoration:none;
    cursor:pointer;
    user-select:none;
  }
  .row{display:flex; align-items:center; justify-content:space-between; gap:12px; flex-wrap:wrap;}
  .pill{padding:6px 10px; border-radius:999px; border:1px solid var(--border); background:transparent; color:var(--muted); font-size:13px;}
  .card{border:1px solid var(--border); background:var(--card); border-radius:16px; padding:14px; margin-top:12px;}
  .grid{display:grid; grid-template-columns: repeat(2, minmax(0,1fr)); gap:12px;}
  @media (max-width: 800px){ .grid{grid-template-columns:1fr;} }

  .k{color:var(--muted); font-size:12px; text-transform:uppercase; letter-spacing:.08em;}
  .v{font-size:18px; font-weight:700; margin-top:4px; word-break:break-word;}
  .small{font-size:13px; color:var(--muted); margin-top:6px; word-break:break-word;}
  .hr{ height:1px; background: var(--border); margin:10px 0; }
  .badge{display:inline-flex; align-items:center; gap:8px;}
  .dot{width:10px; height:10px; border-radius:999px; background:var(--warn);}
  .dot.ok{background:var(--ok);}
  .dot.bad{background:var(--bad);}

  details summary{cursor:pointer; color:var(--muted); user-select:none;}
  pre{
    margin:10px 0 0 0;
    padding:12px;
    border-radius:12px;
    border:1px solid var(--border);
    background:rgba(0,0,0,0.06);
    color:var(--fg);
    overflow:auto;
    font-family:var(--mono);
    font-size:12px;
    line-height:1.45;
    white-space:pre;
  }
  body.dark pre{ background: rgba(255,255,255,0.06); }
  #tnOut{height:340px;overflow-y:auto;cursor:text;user-select:text;outline:none;}
  .tn-cur::after{content:'▌';animation:tn-blink 1s step-end infinite;}
  @keyframes tn-blink{50%{opacity:0}}
</style>
</head>
<body>

<div class="topbar">
  <a class="btn" href="/">Dashboard</a>
  <div class="row">
    <span class="pill" id="updated">Updated: --</span>
        <span class="pill" id="status">Status: --</span>
<button class="btn" id="refreshBtn">Refresh</button>
    <a class="btn" href="/configuration">Config</a>
  </div>
</div>

<div class="grid">
  <div class="card">
    <div class="k">Time</div>
    <div class="small" id="timeEpoch">Local epoch: --</div>
    <div class="small" id="timeHuman">--</div>
    <div class="small" id="millis">Uptime: --</div>
  </div>

  <div class="card">
    <div class="k">Wi‑Fi</div>
    <div class="v badge"><span class="dot" id="wifiDot"></span><span id="wifiState">--</span></div>
    <div class="small" id="wifiSsid">SSID: --</div>
    <div class="small" id="wifiRssi">RSSI: --</div>
    <div class="small" id="wifiIp">IP: --</div>
  </div>

  <div class="card">
    <div class="k">Heap</div>
    <div class="small" id="heapFree">--</div>
    <div class="small" id="heapLargest">Largest free block: --</div>
    <div class="small" id="heapInternal">Internal RAM (DMA capable): --</div>
    <div class="small" id="heapPsram">PSRAM available: --</div>
  </div>

  <div class="card">
    <div class="k">Glucose</div>
    <div class="v" id="gMain">--</div>
    <div class="small" id="gMeta">--</div>
    <div class="small" id="gLife">--</div>
  </div>
</div>

<div class="grid">
  <div class="card">
    <div class="k">LibreLinkUp Status</div>
    <div class="v badge"><span class="dot" id="lluTsDot"></span><span id="lluTsState">--</span></div>
    <div class="small" id="lluLastTs">Last ts: --</div>
    <div class="small" id="lluSensorState">Sensor state: --</div>
  </div>

  <div class="card">
    <div class="k">LibreLinkUp Sensor</div>
    <div class="v" id="lluActive">--</div>
    <div class="small" id="lluSensorType">Type: --</div>
    <div class="small" id="lluInactive">--</div>
    <div class="small" id="lluActivation">--</div>
    <div class="small" id="lluExpires">--</div>
    <div class="small" id="lluRemaining">--</div>
  </div>

  <div class="card">
    <div class="k">LibreLinkUp Login</div>
    <div class="v badge"><span class="dot" id="lluLoginDot"></span><span id="lluLoginState">--</span></div>
    <div class="small" id="lluEmail">Email: --</div>
    <div class="small" id="lluAccount">Account: --</div>
    <div class="small" id="lluRegion">Region: --</div>
    <div class="small" id="lluToken">Token: --</div>
    <div class="small" id="lluTokenExp">Expires: --</div>
  </div>

  <div class="card">
    <div class="k">Config Snapshot</div>
    <div class="small" id="cfgMain">--</div>
    <div class="small" id="cfgMore">--</div>
  </div>

  <div class="card">
    <div class="k">H2 Co-Processor</div>
    <div class="v" id="h2Fw">FW: --</div>
    <div class="small" id="h2ChipModel">Model: --</div>
    <div class="small" id="h2ChipPerf">CPU: -- | Cores: -- | XTAL: --</div>
    <div class="small" id="h2ChipRadio">Features: --</div>
    <div class="small" id="h2ChipMac">MAC: --</div>
    <div class="small" id="h2LastSeen">Last seen: --</div>
  </div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:10px;">
    <div>
      <div class="k">Telnet Terminal</div>
      <div class="small">WebSocket bridge. Nur im eigenen LAN verwenden.</div>
    </div>
    <div class="row">
      <input id="tnHost" placeholder="Host (127.0.0.1 = dieses Ger&auml;t)" style="width:220px;"/>
      <input id="tnPort" placeholder="Port" style="width:90px;" inputmode="numeric"/>
      <button class="btn" id="tnConnect">Connect</button>
      <button class="btn" id="tnSelf">This device</button>
      <span class="badge" id="tnState" style="margin-left:8px;">--</span>
      <button class="btn" id="tnDisconnect">Disconnect</button>
      <button class="btn" id="tnClear">Clear</button>
    </div>
  </div>
  <!-- Terminal area: click anywhere to focus hidden input (mobile keyboard) -->
  <pre id="tnOut" tabindex="0" onclick="document.getElementById('tnHidIn').focus()"></pre>
  <input id="tnHidIn" style="position:absolute;opacity:0;pointer-events:none;width:1px;height:1px;"
         autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false"/>
</div>

</div>

<div class="card">
  <details>
    <summary>Raw JSON</summary>
    <pre id="raw">{}</pre>
  </details>
</div>

<script>
function pad2(n){ return String(n).padStart(2,'0'); }
function applyThemeFromStorage(){
  const v = localStorage.getItem("theme"); // dark|light|null
  document.body.classList.remove("dark","light");
  if (v==="dark") document.body.classList.add("dark");
  if (v==="light") document.body.classList.add("light");
}
applyThemeFromStorage();

function fmtDateTime(ts){
  if(!Number.isFinite(ts) || ts<=0) return "--";
  const d=new Date(ts*1000);
  return `${d.getFullYear()}-${pad2(d.getMonth()+1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}
function fmtUptime(ms){
  if(!Number.isFinite(ms) || ms < 0) return "--";
  let s = Math.floor(ms/1000);
  const days = Math.floor(s/86400); s -= days*86400;
  const hours = Math.floor(s/3600); s -= hours*3600;
  const minutes = Math.floor(s/60); s -= minutes*60;
  const seconds = s;
  let out = "";
  if(days > 0) out += days + "d ";
  out += pad2(hours) + "h " + pad2(minutes) + "m " + pad2(seconds) + "s";
  return out.trim();
}

function setDot(el, ok){
  el.classList.remove("ok","bad");
  if(ok===true) el.classList.add("ok");
  else if(ok===false) el.classList.add("bad");
}

async function load(){
  const btn=document.getElementById("refreshBtn");
  if(btn) btn.disabled=true;
  try{
    const resp = await fetch("/api/debug", {cache:"no-store", credentials:"include"});
    if(!resp.ok){ throw new Error(`HTTP ${resp.status}`); }
    let j;
    try{ j = await resp.json(); }
    catch(e){ const t = await resp.text(); throw new Error("Invalid JSON: "+t.slice(0,120)); }


    document.getElementById("updated").textContent = `Updated: ${new Date().toLocaleTimeString()}`;
    document.getElementById("status").textContent = "Status: OK";
    document.getElementById("raw").textContent = JSON.stringify(j, null, 2);

    const epoch = Number(j.time_epoch);
    document.getElementById("timeEpoch").textContent = Number.isFinite(epoch) ? (`Local epoch: ${epoch}`) : "Local epoch: --";
    document.getElementById("timeHuman").textContent = `Local: ${fmtDateTime(epoch)}`;
    document.getElementById("millis").textContent = `Uptime: ${fmtUptime(Number(j.millis) || 0)}`;

    const w = j.wifi || {};
    const wConn = (w.connected === true);
    setDot(document.getElementById("wifiDot"), wConn);
    document.getElementById("wifiState").textContent = wConn ? "Connected" : "Disconnected";
    document.getElementById("wifiSsid").textContent = `SSID: ${w.ssid ?? "--"}`;
    document.getElementById("wifiRssi").textContent = `RSSI: ${Number.isFinite(Number(w.rssi)) ? w.rssi + " dBm" : "--"}`;
    document.getElementById("wifiIp").textContent = `IP: ${w.ip ?? "--"}`;

    const h = j.heap || {};
    // Show a richer heap breakdown when available (ESP32)
    if (h.total_free != null) {
      document.getElementById("heapFree").textContent = `Total free heap: ${h.total_free} Bytes`;
      document.getElementById("heapLargest").textContent = `Largest free block: ${h.largest_free_block ?? "--"} Bytes`;
      document.getElementById("heapInternal").textContent = `Internal RAM (DMA capable): ${h.internal_dma ?? "--"} Bytes`;
      document.getElementById("heapPsram").textContent = `PSRAM available: ${h.psram ?? "--"} Bytes`;
    } else {
      // Fallback (older payload)
      document.getElementById("heapFree").textContent = (h.free != null) ? `Total free heap: ${h.free} Bytes` : "--";
      document.getElementById("heapLargest").textContent = `Largest free block: --`;
      document.getElementById("heapInternal").textContent = `Internal RAM (DMA capable): --`;
      document.getElementById("heapPsram").textContent = `PSRAM available: --`;
    }


    const g = j.glucose || {};
    const tsOk = (g.ts_ok === true);
    const state = Number(g.sensor_state);
    const warm = (g.warmup_active === true);
    document.getElementById("gMain").textContent = tsOk ? `${g.mgdl ?? "--"} mg/dL` : "-- (invalid)";
    document.getElementById("gMeta").textContent =
      `Δ ${g.delta ?? "--"} • Trend ${g.trend ?? "--"} • State ${Number.isFinite(state)?state:"--"}${warm ? " • Warmup" : ""}`;
    document.getElementById("gLife").textContent =
      `Life: ${g.life_days ?? "--"}d ${g.life_hours ?? "--"}h ${g.life_minutes ?? "--"}m ${g.life_seconds ?? "--"}s`;

const llu = j.llu || {};
const st = llu.status || {};
const se = llu.sensor || {};
const lo = llu.login || {};

const tsStatus = Number(st.timestamp_status);
// timestamp_status: 0=unknown/invalid, 1=ok (assuming your logic)
const tsOk2 = (tsStatus === 1 || tsStatus === 2 || tsStatus === 0x01);
setDot(document.getElementById("lluTsDot"), tsOk2);
document.getElementById("lluTsState").textContent = tsOk2 ? "Timestamp OK" : `Timestamp status ${Number.isFinite(tsStatus)?tsStatus:"--"}`;
document.getElementById("lluLastTs").textContent = `Last ts: ${Number(st.last_timestamp_unixtime)||0} (${fmtDateTime(Number(st.last_timestamp_unixtime)||0)})`;
document.getElementById("lluSensorState").textContent = `Sensor state: ${Number(st.sensor_state) ?? "--"} / ${Number(se.sensor_state) ?? "--"}`;

const activeId = se.sensor_id || "--";
const activeSn = se.sensor_sn || "--";
document.getElementById("lluActive").textContent = `Active: ${activeSn} (${activeId})`;
const sensorTypeName = se.sensor_type_name || "Unknown";
const sensorSnSrc    = se.sensor_type_sn_src || "--";
document.getElementById("lluSensorType").textContent = `Type: ${sensorTypeName} (SN: ${sensorSnSrc})`;

const inactId = se.sensor_id_non_active || "--";
const inactSn = se.sensor_sn_non_active || "--";
const inactTs = Number(se.sensor_non_activ_unixtime)||0;
document.getElementById("lluInactive").textContent = `Inactive: ${inactSn} (${inactId}) • last try: ${inactTs?fmtDateTime(inactTs):"--"}`;

const actTs = Number(se.sensor_activation_time)||0;
const runtimeSec = Number(se.sensor_runtime)||0;

let activatedStr = actTs ? fmtDateTime(actTs) : "--";
let expiresStr = "--";
let remainingStr = "--";

if (actTs && runtimeSec) {
  const now = Math.floor(Date.now()/1000);
  const expires = actTs + runtimeSec;
  expiresStr = fmtDateTime(expires);

  let rem = expires - now;
  if (rem > 0) {
    const d = Math.floor(rem/86400); rem -= d*86400;
    const h = Math.floor(rem/3600);  rem -= h*3600;
    const min = Math.floor(rem/60);
    remainingStr = `${d}d ${h}h ${min}m`;
  } else {
    remainingStr = "expired";
  }
}

document.getElementById("lluActivation").textContent = `Activated: ${activatedStr}`;
document.getElementById("lluExpires").textContent    = `Expires:   ${expiresStr}`;
document.getElementById("lluRemaining").textContent  = `Remaining: ${remainingStr}`;

// Green dot if account + user data is present (more reliable than numeric status)
const hasUserData = !!(lo.account_id && String(lo.account_id).length && lo.user_id && String(lo.user_id).length);
setDot(document.getElementById("lluLoginDot"), hasUserData);
document.getElementById("lluLoginState").textContent = hasUserData ? "Logged in" : "Not logged in";
document.getElementById("lluEmail").textContent = `Email: ${lo.email || "--"}`;
document.getElementById("lluAccount").textContent = `Account: ${lo.account_id || "--"} • User: ${lo.user_id || "--"}`;
document.getElementById("lluRegion").textContent = `User region: ${lo.user_country || "--"} • Connection: ${lo.connection_country || "--"} • Conn status: ${Number(lo.connection_status) ?? "--"}`;
document.getElementById("lluToken").textContent = `Token: ${lo.token_present ? (lo.token_preview || "(present)") : "(none)"} • Password set: ${lo.password_set ? "yes" : "no"}`;
const exp = Number(lo.user_token_expires)||0;
document.getElementById("lluTokenExp").textContent = `Expires: ${exp?fmtDateTime(exp):"--"}`;

const cfg = j.config || {};
const brVal = (cfg.brightness ?? "--");
const dimTout = (cfg.display_dim_timeout_s != null) ? (cfg.display_dim_timeout_s === 0 ? "disabled" : cfg.display_dim_timeout_s + " s") : "--";
const onOff = (v)=> (Number(v) ? "ON" : "OFF");
const cfgLines = [
  `OTA: ${onOff(cfg.ota_update ?? 0)} | Channel: ${(cfg.ota_staging ?? 0) ? "Staging" : "Release"} | Force: ${onOff(cfg.ota_force ?? 0)}`,
  `WG: ${onOff(cfg.wg_mode ?? 0)}`,
  `MQTT: ${onOff(cfg.mqtt_mode ?? 0)}`,
  `Master: ${onOff(cfg.mqtt_master_mode ?? 0)}`,
  `Brightness: ${brVal}`,
  `Dim timeout: ${dimTout}`
];
document.getElementById("cfgMain").innerHTML = cfgLines.join("<br>");
document.getElementById("cfgMore").textContent = "";

const h2 = j.h2 || {};
const h2Fw = h2.fw_version || "--";
const h2ChipModel = h2.chip_model || "--";
const h2ChipRev = h2.chip_revision || "--";
const h2ChipMac = h2.chip_mac || "--";
const h2ChipCores = h2.chip_cores || "--";
const h2ChipCpu = h2.chip_cpu_mhz || "--";
const h2Features = h2.chip_features || "--";
const h2Xtal = h2.chip_xtal_mhz || "--";
const h2SeenAgo = Number(h2.last_seen_ms_ago || 0);
const h2SeenTxt = h2.has_data ? (Math.round(h2SeenAgo / 1000) + " s ago") : "--";
document.getElementById("h2Fw").textContent = `FW: ${h2Fw}`;
document.getElementById("h2ChipModel").textContent = `Model: ${h2ChipModel} | Rev: ${h2ChipRev}`;
document.getElementById("h2ChipPerf").textContent = `CPU: ${h2ChipCpu} MHz | Cores: ${h2ChipCores} | XTAL: ${h2Xtal} MHz`;
document.getElementById("h2ChipRadio").textContent = `Features: ${h2Features}`;
document.getElementById("h2ChipMac").textContent = `MAC: ${h2ChipMac}`;
document.getElementById("h2LastSeen").textContent = `Last seen: ${h2SeenTxt} | last type: ${h2.last_type || "--"}`;

  }catch(e){
    document.getElementById("raw").textContent = "Failed to fetch /api/debug: " + (e && e.message ? e.message : e);
    const stEl=document.getElementById("status");
    if(stEl) stEl.textContent = "Status: FAIL (" + (e && e.message ? e.message : e) + ")";
    document.getElementById("updated").textContent = "Updated: --";
  }finally{
    if(btn) btn.disabled=false;
  }
}


// --- Telnet terminal (WebSocket bridge) ---
let tnWs = null;
let tnOutText = '';   // server output accumulated here
let tnInBuf   = '';   // current input line
let tnCurPos  = 0;    // cursor position within tnInBuf
let tnHistory = [];   // sent command history (newest first)
let tnHistIdx = -1;   // -1 = not browsing history
let tnHistSaved='';   // saved draft while browsing history
let tnSearch  = false;// ctrl-r search mode
let tnSrchBuf = '';   // search query
let tnSrchHit = '';   // current match
let tnSrchFrom= 0;    // start index in history for next ctrl-r

function tnEscHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// Find the next history match for tnSrchBuf starting at tnSrchFrom
function tnDoSearch(){
  tnSrchHit='';
  if(!tnSrchBuf) return;
  for(let i=tnSrchFrom;i<tnHistory.length;i++){
    if(tnHistory[i].includes(tnSrchBuf)){ tnSrchHit=tnHistory[i]; tnSrchFrom=i+1; return; }
  }
}

function tnRender(scroll){
  const pre=document.getElementById('tnOut');
  if(!pre) return;
  const atBottom=pre.scrollTop+pre.clientHeight>=pre.scrollHeight-4;
  let inp;
  if(tnSearch){
    // show reverse-i-search prompt with highlighted match
    const q=tnEscHtml(tnSrchBuf);
    let m='';
    if(tnSrchHit){
      const idx=tnSrchHit.indexOf(tnSrchBuf);
      m=tnEscHtml(tnSrchHit.slice(0,idx))+'<mark>'+tnEscHtml(tnSrchBuf)+'</mark>'+tnEscHtml(tnSrchHit.slice(idx+tnSrchBuf.length));
    }
    inp='<span style="color:var(--muted)">(reverse-i-search)`'+q+"': "+m+'</span><span class="tn-cur"></span>';
  } else {
    const b=tnEscHtml(tnInBuf.slice(0,tnCurPos));
    const c=tnEscHtml(tnInBuf.slice(tnCurPos));
    inp=b+'<span class="tn-cur">'+c+'</span>';
  }
  pre.innerHTML=tnEscHtml(tnOutText)+inp;
  if(scroll||atBottom) pre.scrollTop=pre.scrollHeight;
}

function tnLog(data){
  // \033[G (cursor-to-col-1) is the server's erase_current_line — treat as \r
  let s=data.replace(/\x1b\[G/g,'\r');
  s=s.replace(/\x1b\[[0-9;]*[a-zA-Z]/g,'').replace(/\x08/g,'');
  s=s.replace(/\r\n/g,'\n');
  const chunks=s.split('\r');
  let result=tnOutText+chunks[0];
  for(let i=1;i<chunks.length;i++){
    const nl=result.lastIndexOf('\n');
    result=result.slice(0,nl+1)+chunks[i];
  }
  tnOutText=result;
  if(tnOutText.length>20000) tnOutText=tnOutText.slice(-20000);
  tnRender(true);
}
function tnSetState(label,mode){
  const el=document.getElementById("tnState");
  if(!el) return;
  el.textContent=label||"--";
  let bg="rgba(148,163,184,0.15)",bd="rgba(148,163,184,0.28)",fg="rgba(226,232,240,0.9)";
  if(mode==="ok"){  bg="rgba(34,197,94,0.18)";  bd="rgba(34,197,94,0.45)"; }
  if(mode==="warn"){bg="rgba(245,158,11,0.18)"; bd="rgba(245,158,11,0.45)";}
  if(mode==="bad"){ bg="rgba(239,68,68,0.18)";  bd="rgba(239,68,68,0.45)"; }
  el.style.background=bg; el.style.borderColor=bd; el.style.color=fg;
}
function tnStatus(msg){
  tnLog('\n['+new Date().toLocaleTimeString()+'] '+msg+'\n');
  const m=(msg||"").toLowerCase();
  if(m.includes("ws connected")) tnSetState("WS connected","warn");
  else if(m.includes("ready"))        tnSetState("Ready","off");
  else if(m.includes("connecting"))   tnSetState("Connecting…","warn");
  else if(m.includes("connected"))    tnSetState("Connected","ok");
  else if(m.includes("disconnected")) tnSetState("Disconnected","off");
  else if(m.includes("failed")||m.includes("error")||m.includes("bad")) tnSetState("Error","bad");
}
function tnOpen(){
  if(tnWs&&(tnWs.readyState===0||tnWs.readyState===1)) return;
  const proto=(location.protocol==="https:")?"wss://":"ws://";
  tnWs=new WebSocket(proto+location.host+"/ws/telnet");
  tnWs.onopen  =()=>{ tnStatus("WS connected"); };
  tnWs.onclose =()=>{ tnStatus("WS closed"); tnSetState("WS closed","off"); };
  tnWs.onerror =()=>{ tnStatus("WS error");  tnSetState("WS error","bad"); };
  tnWs.onmessage=(ev)=>{
    if(ev.data.charCodeAt(0)===1) tnStatus(ev.data.slice(1));
    else tnLog(ev.data);
  };
}
function tnEnsureOpen(cb){
  tnOpen();
  const t0=Date.now();
  (function w(){if(!tnWs){setTimeout(w,50);return;}if(tnWs.readyState===1){cb();return;}
    if(Date.now()-t0>3000){tnStatus("WS not open");tnSetState("WS error","bad");return;}
    setTimeout(w,50);})();
}
function tnSendRaw(data){
  tnEnsureOpen(()=>{ try{tnWs.send(JSON.stringify({cmd:"send",data}));}catch(e){tnStatus("send failed");} });
}
function tnCommit(line){
  if(line){tnHistory.unshift(line);if(tnHistory.length>200)tnHistory.pop();}
  tnInBuf=''; tnCurPos=0; tnHistIdx=-1; tnHistSaved='';
  tnSearch=false; tnSrchBuf=''; tnSrchHit=''; tnSrchFrom=0;
  tnRender();
  tnSendRaw(line+"\r\n");
}
function tnConnect(){
  const host=(document.getElementById("tnHost").value||"").trim();
  const port=Number((document.getElementById("tnPort").value||"23").trim())||23;
  localStorage.setItem("tnHost",host); localStorage.setItem("tnPort",String(port));
  if(!host){tnStatus("Host fehlt");tnSetState("Error","bad");return;}
  tnSetState("Connecting…","warn");
  tnEnsureOpen(()=>{try{tnWs.send(JSON.stringify({cmd:"connect",host,port}));}catch(e){tnStatus("send failed");}});
}
function tnDisconnect(){
  tnEnsureOpen(()=>{try{tnWs.send(JSON.stringify({cmd:"disconnect"}));}catch(e){tnStatus("send failed");}});
}

// Insert text at cursor
function tnInsert(ch){
  tnInBuf=tnInBuf.slice(0,tnCurPos)+ch+tnInBuf.slice(tnCurPos);
  tnCurPos+=ch.length;
}

function tnInit(){
  const hostEl=document.getElementById("tnHost");
  const portEl=document.getElementById("tnPort");
  const h=localStorage.getItem("tnHost")||"127.0.0.1";
  const p=localStorage.getItem("tnPort")||"23";
  if(hostEl) hostEl.value=h||window.location.hostname;
  if(portEl) portEl.value=p||"23";

  document.getElementById("tnConnect")?.addEventListener("click",tnConnect);
  document.getElementById("tnDisconnect")?.addEventListener("click",tnDisconnect);
  document.getElementById("tnClear")?.addEventListener("click",()=>{
    tnOutText=''; tnInBuf=''; tnCurPos=0; tnRender(true);
  });
  document.getElementById("tnSelf")?.addEventListener("click",()=>{
    const h=document.getElementById("tnHost"); if(h) h.value=window.location.hostname;
  });

  const hidIn=document.getElementById("tnHidIn");
  if(hidIn){
    hidIn.addEventListener("keydown",(e)=>{
      if(tnSearch){
        // --- search mode keys ---
        if(e.key==="Enter"||e.key==="Escape"||(e.ctrlKey&&e.key==="g")){
          e.preventDefault();
          tnSearch=false;
          if(e.key==="Enter"&&tnSrchHit){ tnInBuf=tnSrchHit; tnCurPos=tnInBuf.length; }
          tnSrchBuf=''; tnSrchHit=''; tnSrchFrom=0; tnRender();
        } else if(e.key==="Backspace"){
          e.preventDefault();
          tnSrchBuf=tnSrchBuf.slice(0,-1); tnSrchFrom=0; tnDoSearch(); tnRender();
        } else if(e.ctrlKey&&e.key==="r"){
          e.preventDefault(); tnDoSearch(); tnRender(); // cycle to next match
        }
        return;
      }
      // --- normal mode keys ---
      if(e.key==="Enter"){
        e.preventDefault(); tnCommit(tnInBuf);
      } else if(e.key==="Backspace"){
        e.preventDefault();
        if(tnCurPos>0){tnInBuf=tnInBuf.slice(0,tnCurPos-1)+tnInBuf.slice(tnCurPos);tnCurPos--;tnRender();}
      } else if(e.key==="Delete"){
        e.preventDefault();
        if(tnCurPos<tnInBuf.length){tnInBuf=tnInBuf.slice(0,tnCurPos)+tnInBuf.slice(tnCurPos+1);tnRender();}
      } else if(e.key==="ArrowLeft"){
        e.preventDefault(); if(tnCurPos>0){tnCurPos--;tnRender();}
      } else if(e.key==="ArrowRight"){
        e.preventDefault(); if(tnCurPos<tnInBuf.length){tnCurPos++;tnRender();}
      } else if(e.key==="Home"){
        e.preventDefault(); tnCurPos=0; tnRender();
      } else if(e.key==="End"){
        e.preventDefault(); tnCurPos=tnInBuf.length; tnRender();
      } else if(e.key==="ArrowUp"){
        e.preventDefault();
        if(tnHistIdx===-1) tnHistSaved=tnInBuf;
        if(tnHistIdx<tnHistory.length-1){tnHistIdx++;tnInBuf=tnHistory[tnHistIdx];tnCurPos=tnInBuf.length;tnRender();}
      } else if(e.key==="ArrowDown"){
        e.preventDefault();
        if(tnHistIdx>0){tnHistIdx--;tnInBuf=tnHistory[tnHistIdx];tnCurPos=tnInBuf.length;}
        else if(tnHistIdx===0){tnHistIdx=-1;tnInBuf=tnHistSaved;tnCurPos=tnInBuf.length;}
        tnRender();
      } else if(e.ctrlKey){
        e.preventDefault();
        switch(e.key){
          case 'a': tnCurPos=0; tnRender(); break;
          case 'e': tnCurPos=tnInBuf.length; tnRender(); break;
          case 'k': tnInBuf=tnInBuf.slice(0,tnCurPos); tnRender(); break;
          case 'u': tnInBuf=tnInBuf.slice(tnCurPos); tnCurPos=0; tnRender(); break;
          case 'c': tnInBuf=''; tnCurPos=0; tnRender();
            tnEnsureOpen(()=>{try{tnWs.send(JSON.stringify({cmd:"send",data:"\x03"}));}catch(e){}});
            break;
          case 'r': tnSearch=true; tnSrchBuf=''; tnSrchHit=''; tnSrchFrom=0; tnRender(); break;
        }
      }
    });
    // mobile: printable chars arrive via input event
    hidIn.addEventListener("input",()=>{
      const v=hidIn.value; hidIn.value='';
      if(!v) return;
      const parts=v.split(/\r?\n/);
      if(tnSearch){
        tnSrchBuf+=parts[0]; tnSrchFrom=0; tnDoSearch(); tnRender();
        if(parts.length>1){ // Enter on mobile keyboard
          tnSearch=false;
          if(tnSrchHit){tnInBuf=tnSrchHit;tnCurPos=tnInBuf.length;}
          tnSrchBuf='';tnSrchHit='';tnSrchFrom=0;tnRender();
        }
      } else {
        tnInsert(parts[0]); tnRender();
        if(parts.length>1) tnCommit(tnInBuf); // Enter on mobile keyboard
      }
    });
    document.getElementById("tnOut")?.addEventListener("click",()=>hidIn.focus());
  }

  tnOpen();
}
tnInit();

window.addEventListener("load", ()=>{
  try{
    const st=document.getElementById("status");
    if(st) st.textContent="Status: JS ok";
    document.getElementById("refreshBtn")?.addEventListener("click", load);
    load();
    setInterval(load, 5000);
  }catch(e){
    const st=document.getElementById("status");
    if(st) st.textContent="Status: JS init error: "+(e && e.message ? e.message : e);
    console.error(e);
  }
});
</script>

</body>
</html>
)rawliteral";

static bool ensureConfigAuth(AsyncWebServerRequest *request) {
    const String& user = settings.config.login_email;
    const String& pass = settings.config.login_password;
    if (user.length() != 0 && pass.length() != 0) {
        if (!request->authenticate(user.c_str(), pass.c_str())) {
            request->requestAuthentication();
            return false;
        }
    }
    return true;
}

// -------------------- Debug handlers --------------------
static void handleDebugPage(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    AsyncWebServerResponse* resp = request->beginResponse(200, "text/html; charset=utf-8", debug_html);
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

static void handleApiDebug(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    maybe_request_h2_info();

    JsonDocument doc;

    doc["millis"] = (uint32_t)millis();
    time_t now = time(nullptr);
    doc["time_epoch"] = (uint32_t)now;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["connected"] = WiFi.isConnected();
    wifi["ssid"] = WiFi.SSID();
    wifi["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
    wifi["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");

    JsonObject heap = doc["heap"].to<JsonObject>();
    // Heap stats (ESP-IDF)
#ifdef ESP32
    heap["total_free"] = (uint32_t)esp_get_free_heap_size();
    heap["largest_free_block"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    heap["internal_dma"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap["psram"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap["min_free"] = (uint32_t)ESP.getMinFreeHeap();
#else
    heap["total_free"] = (uint32_t)ESP.getFreeHeap();
#endif


    // Embed the normal /api/glucose payload
    JsonDocument glu;
    DeserializationError e1 = deserializeJson(glu, web_get_glucose_latest_json());
    if (!e1) {
        doc["glucose"] = glu.as<JsonVariant>();
    } else {
        doc["glucose_parse_error"] = e1.c_str();
        doc["glucose_raw"] = web_get_glucose_latest_json();
    }

    // Provide delta as int16_t sanity check
    doc["glucose_delta_int16"] = (int)glucose_delta;

    // LibreLinkUp status + sensor snapshot
    JsonObject llu = doc["llu"].to<JsonObject>();

    JsonObject st = llu["status"].to<JsonObject>();
    st["timestamp_status"] = (uint8_t)librelinkup.status().timestamp_status;
    st["sensor_state"] = (uint8_t)librelinkup.status().sensor_state;
    st["last_timestamp_unixtime"] = (uint32_t)librelinkup.status().last_timestamp_unixtime;

    JsonObject se = llu["sensor"].to<JsonObject>();
    se["sensor_state"] = (uint8_t)librelinkup.sensor_data().sensor_state;
    se["sensor_sn_non_active"] = librelinkup.sensor_data().sensor_sn_non_active;
    se["sensor_id_non_active"] = librelinkup.sensor_data().sensor_id_non_active;
    se["sensor_non_activ_unixtime"] = (uint32_t)librelinkup.sensor_data().sensor_non_activ_unixtime;
    se["sensor_id"] = librelinkup.sensor_data().sensor_id;
    se["sensor_sn"] = librelinkup.sensor_data().sensor_sn;
    se["sensor_type_dtid"] = (uint16_t)librelinkup.sensor_data().sensor_type_dtid;
    {
        // Prefer SN-based detection; fall back to non-active SN before touching dtid
        const String &sn_a = librelinkup.sensor_data().sensor_sn;
        const String &sn_b = librelinkup.sensor_data().sensor_sn_non_active;
        const String &sn   = (sn_a.length() >= 5) ? sn_a : sn_b;
        se["sensor_type_name"]   = librelinkup.sensor_device_type_to_string(
                                       librelinkup.get_sensor_device_type_from_sn(sn));
        se["sensor_type_sn_src"] = sn.length() >= 5 ? sn : String("--");
    }
    se["sensor_runtime"] = (uint32_t)librelinkup.sensor_data().sensor_runtime;
    se["sensor_activation_time"] = (uint32_t)librelinkup.sensor_data().sensor_activation_time;

// LibreLinkUp login snapshot (no secrets in cleartext)
JsonObject lo = llu["login"].to<JsonObject>();
lo["email"] = librelinkup.login_data().email;
lo["user_id"] = librelinkup.login_data().user_id;
lo["account_id"] = librelinkup.login_data().account_id;
lo["user_country"] = librelinkup.login_data().user_country;
lo["connection_country"] = librelinkup.login_data().connection_country;
lo["connection_status"] = (int16_t)librelinkup.login_data().connection_status;
lo["user_token_expires"] = (uint32_t)librelinkup.login_data().user_token_expires;
lo["user_login_status"] = (uint8_t)librelinkup.login_data().user_login_status;

// Security helpers
lo["password_set"] = (librelinkup.login_data().password.length() > 0);
lo["token_present"] = (librelinkup.login_data().user_token.length() > 0);

// Short token preview (safe)
if (librelinkup.login_data().user_token.length() > 10) {
    lo["token_preview"] =
        librelinkup.login_data().user_token.substring(0, 6) + "..." +
        librelinkup.login_data().user_token.substring(librelinkup.login_data().user_token.length() - 4);
} else {
    lo["token_preview"] = String("");
}


    // Add a small config snapshot (no secrets)
    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["ota_update"]  = settings.config.ota_update;
    cfg["ota_staging"] = settings.config.ota_staging;
    cfg["ota_force"]   = settings.config.ota_force;
    cfg["wg_mode"] = settings.config.wg_mode;
    cfg["mqtt_mode"] = settings.config.mqtt_mode;
    cfg["mqtt_master_mode"] = settings.config.mqtt_master_mode;
    cfg["brightness"] = settings.config.brightness;
    cfg["display_dim_timeout_s"] = settings.config.display_dim_timeout_s;

    JsonObject h2 = doc["h2"].to<JsonObject>();
    h2["fw_version"] = g_h2_fw_version;
    h2["fw_build"] = g_h2_fw_build;
    h2["chip_model"] = g_h2_chip_model;
    h2["chip_revision"] = g_h2_chip_rev;
    h2["chip_mac"] = g_h2_chip_mac;
    h2["chip_cores"] = g_h2_chip_cores;
    h2["chip_cpu_mhz"] = g_h2_chip_cpu_mhz;
    h2["chip_xtal_mhz"] = g_h2_chip_xtal_mhz;
    h2["chip_features"] = g_h2_chip_features;
    h2["last_type"] = g_h2_last_type;
    h2["last_seen_ms_ago"] = (g_h2_last_seen_ms > 0) ? (uint32_t)(millis() - g_h2_last_seen_ms) : 0;
    h2["has_data"] = (g_h2_last_seen_ms > 0);
    h2["last_json"] = g_h2_last_json;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json; charset=utf-8", out);
}


// -------------------- Dashboard/API hooks (implemented elsewhere) --------------------
// Provide these in web_glucose_api.cpp (recommended). Weak fallbacks keep compilation working.
__attribute__((weak)) String web_get_glucose_latest_json() { return String("{\"mgdl\":null,\"low\":null,\"high\":null,\"delta\":null,\"trend\":\"--\",\"ts_ok\":false}"); }
__attribute__((weak)) String web_get_glucose_history_json() { return String("{\"low\":null,\"high\":null,\"values\":[]}"); }

// Optional: config prefill API (provide web_config_api.cpp). Weak fallback keeps compilation working.
__attribute__((weak)) String web_get_config_json() { return String("{\"ok\":false}"); }

// -------------------- Handlers: Dashboard + Config --------------------
static void handleDashboard(AsyncWebServerRequest *request) {
    request->send(200, "text/html", dashboard_html);
}

static void handleConfiguration(AsyncWebServerRequest *request) {
    const String& user = settings.config.login_email;
    const String& pass = settings.config.login_password;

    // If no credentials configured, leave open
    if (user.length() == 0 || pass.length() == 0) {
        request->send(200, "text/html", index_html);
        return;
    }

    if (!request->authenticate(user.c_str(), pass.c_str())) {
        return request->requestAuthentication();
    }

    request->send(200, "text/html", index_html);
}

static void handleConfigRedirect(AsyncWebServerRequest *request) {
    request->redirect("/configuration");
}

static void handleApiGlucose(AsyncWebServerRequest *request) {
    request->send(200, "application/json; charset=utf-8", web_get_glucose_latest_json());
}

static void handleApiGlucoseHistory(AsyncWebServerRequest *request) {
    request->send(200, "application/json; charset=utf-8", web_get_glucose_history_json());
}

static void handleApiConfig(AsyncWebServerRequest *request) {
    // Prefill endpoint used by config page JS. Protect it with the same BasicAuth as /configuration.
    if (!ensureConfigAuth(request)) return;

    request->send(200, "application/json", web_get_config_json());
}

static String g_config_restore_body;

/**
 * @brief Restores settings from a backup JSON (as produced by /api/config).
 *
 * Writes the uploaded body straight to the config file and reloads it via
 * settings.loadConfiguration() — reuses its field parsing/migration logic
 * instead of duplicating it here. Reboots on success so every subsystem
 * (WiFi, MQTT, WireGuard) re-initializes from the restored config.
 */
static void handleRestoreConfigBody(AsyncWebServerRequest *request,
                                     uint8_t *data, size_t len,
                                     size_t index, size_t total)
{
    if (!ensureConfigAuth(request)) return;

    if (index == 0) { g_config_restore_body = ""; g_config_restore_body.reserve(total); }
    for (size_t i = 0; i < len; i++) g_config_restore_body += (char)data[i];
    if (index + len < total) return;

    JsonDocument doc;
    if (deserializeJson(doc, g_config_restore_body) || !doc["login_email"].is<const char*>()) {
        request->send(400, "application/json", "{\"error\":\"Invalid config backup JSON\"}");
        g_config_restore_body = "";
        return;
    }

    File f = LittleFS.open(settings.config_filename, FILE_WRITE);
    if (!f) {
        request->send(500, "application/json", "{\"error\":\"Failed to write config file\"}");
        g_config_restore_body = "";
        return;
    }
    f.print(g_config_restore_body);
    f.close();
    g_config_restore_body = "";

    settings.loadConfiguration(settings.config_filename, settings.config);
    librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);

    logger.notice("Config restored from uploaded backup, rebooting");
    request->send(200, "application/json", "{\"status\":\"restored\"}");
    delay(300); // let the telnet/log transport flush the notice above before the reset drops the connection
    ui_blank_screen_for_reset();
    ESP.restart();
}

static void handleApiFwStatus(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    maybe_request_h2_info();

    JsonDocument doc;
    const String fw = fw_update_get_status_json();
    if (deserializeJson(doc, fw) != DeserializationError::Ok)
    {
        request->send(200, "application/json; charset=utf-8", fw);
        return;
    }

    JsonObject h2 = doc["h2"].to<JsonObject>();
    h2["fw_version"] = g_h2_fw_version;
    h2["fw_build"] = g_h2_fw_build;
    h2["chip_model"] = g_h2_chip_model;
    h2["chip_revision"] = g_h2_chip_rev;
    h2["chip_mac"] = g_h2_chip_mac;
    h2["chip_cores"] = g_h2_chip_cores;
    h2["chip_cpu_mhz"] = g_h2_chip_cpu_mhz;
    h2["chip_xtal_mhz"] = g_h2_chip_xtal_mhz;
    h2["chip_features"] = g_h2_chip_features;
    h2["last_seen_ms_ago"] = (g_h2_last_seen_ms > 0) ? (uint32_t)(millis() - g_h2_last_seen_ms) : 0;
    h2["has_data"] = (g_h2_last_seen_ms > 0);

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json; charset=utf-8", out);
}

static void handleApiFwCheck(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    fw_update_request_check_now();
    request->send(202, "application/json; charset=utf-8", "{\"status\":\"scheduled\"}");
}

static void handleApiFwInstall(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    String msg;
    if (!fw_update_request_install(msg)) {
        request->send(409, "application/json; charset=utf-8",
                      String("{\"status\":\"rejected\",\"message\":\"") + msg + "\"}");
        return;
    }
    request->send(202, "application/json; charset=utf-8",
                  String("{\"status\":\"accepted\",\"message\":\"") + msg + "\"}");
}

// -------------------- Legacy handlers used by index_html --------------------
static void handleLogin(AsyncWebServerRequest *request) {
    if (request->hasParam("username", true)) {
        username = request->getParam("username", true)->value();
    }
    if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
    }

    settings.config.login_email    = username;
    settings.config.login_password = password;
    librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "text/html", "Login successful!<br><a href='/configuration'>Back</a>");
}

static void handleScan(AsyncWebServerRequest *request) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        request->send(200, "application/json", "{\"scanning\":true}");
        return;
    }
    if (n < 0) {
        // No scan running — start one async and tell client to retry
        WiFi.scanNetworks(true);
        request->send(200, "application/json", "{\"scanning\":true}");
        return;
    }
    // Scan complete — return results
    String json = "[";
    bool first = true;
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    request->send(200, "application/json", json);
}

static void handleConnect(AsyncWebServerRequest *request) {
    if (request->hasParam("networks", true)) {
        wifi_bssid = request->getParam("networks", true)->value();
        settings.config.wifi_bssid = wifi_bssid;
    }
    if (request->hasParam("wifiPassword", true)) {
        wifi_password = request->getParam("wifiPassword", true)->value();
        settings.config.wifi_password = wifi_password;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    ui_blank_screen_for_reset();
    ESP.restart();
}

static void handleStatus(AsyncWebServerRequest *request) {
    // Deliberately does NOT reload from flash. Every setter already updates
    // settings.config in RAM, so a reload can only lose state -- and it did:
    // /setBrightness writes RAM only (persisting on every slider step would
    // hammer the filesystem), so each /status call reverted the backlight to
    // the last saved value. Restoring from flash belongs in the restore
    // handler, not in a status read.
    JsonDocument json_config;
    json_config["ota_update"]        = settings.config.ota_update;
    json_config["ota_staging"]       = settings.config.ota_staging;
    json_config["ota_force"]         = settings.config.ota_force;
    json_config["wg_mode"]           = settings.config.wg_mode;
    json_config["mqtt_mode"]         = settings.config.mqtt_mode;
    json_config["mqtt_master_mode"]  = settings.config.mqtt_master_mode;
    // Live backlight value -- dimming and auto-brightness write it directly.
    json_config["brightness"]        = settings.config.brightness;
    // Setpoint for the slider: survives a dim ramp, follows the
    // auto-brightness target, and is what undim restores to.
    extern AppFsm g_fsm;
    json_config["brightness_set"]    = g_fsm.brightness_before_dim;
    json_config["dim_active"]        = g_fsm.display_dim_active ? 1 : 0;
    json_config["auto_brightness"]   = settings.config.auto_brightness;
    json_config["auto_bri_min"]      = settings.config.auto_bri_min;
    json_config["auto_bri_max"]      = settings.config.auto_bri_max;
    // Whether any paired device currently reports illuminance -- without
    // one, auto-brightness has nothing to follow.
    uint16_t ambient_lux = 0;
    const bool lux_ok = zigbee_h2_ambient_lux(ambient_lux);
    json_config["lux_available"]     = lux_ok ? 1 : 0;
    json_config["lux"]               = lux_ok ? (int)ambient_lux : -1;

    String jsonResponse;
    serializeJson(json_config, jsonResponse);
    request->send(200, "application/json", jsonResponse);
}

static void handleToggleFeature(AsyncWebServerRequest *request) {
    if (!(request->hasParam("feature") && request->hasParam("status"))) {
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
        return;
    }

    String feature = request->getParam("feature")->value();
    int status = request->getParam("status")->value().toInt();

    if (feature == "ota_update") {
        settings.config.ota_update = status;
        logger.notice("OTA_Update: %d", settings.config.ota_update);
        settings.saveConfiguration(settings.config_filename, settings.config);

    } else if (feature == "ota_staging") {
        settings.config.ota_staging = status;
        logger.notice("OTA_Staging: %d", settings.config.ota_staging);
        fw_update_request_check_now();
        settings.saveConfiguration(settings.config_filename, settings.config);

    } else if (feature == "ota_force") {
        settings.config.ota_force = status;
        logger.notice("OTA_Force: %d", settings.config.ota_force);
        fw_update_request_check_now();
        settings.saveConfiguration(settings.config_filename, settings.config);

    } else if (feature == "wg_mode") {
        settings.config.wg_mode = status;
        logger.notice("wg_mode: %d", settings.config.wg_mode);
        setup_wg(settings.config.wg_mode);
    } else if (feature == "mqtt_mode") {
        settings.config.mqtt_mode = status;
        logger.notice("mqtt_mode: %d", settings.config.mqtt_mode);
    } else if (feature == "mqtt_master_mode") {
        settings.config.mqtt_master_mode = status;
        logger.notice("mqtt_master_mode: %d", settings.config.mqtt_master_mode);
        mqtt_publish_ha_discovery();
    } else if (feature == "ha_discovery") {
        settings.config.ha_discovery = status;
        logger.notice("ha_discovery: %d", settings.config.ha_discovery);
        mqtt_publish_ha_discovery();
    } else if (feature == "auto_brightness") {
        settings.config.auto_brightness = status;
        logger.notice("auto_brightness: %d", settings.config.auto_brightness);
    } else {
        request->send(400, "application/json", "{\"error\": \"Unknown feature\"}");
        return;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json", "{\"status\": \"updated\"}");
}

/// Endpoints of the auto-brightness curve: level in the dark and level in
/// bright surroundings. Together they set both the floor and the slope.
static void handleSetAutoBriRange(AsyncWebServerRequest *request) {
    if (!request->hasParam("min", true) || !request->hasParam("max", true)) {
        request->send(400, "application/json", "{\"error\": \"Missing min or max\"}");
        return;
    }
    long lo = request->getParam("min", true)->value().toInt();
    long hi = request->getParam("max", true)->value().toInt();
    if (lo < 0)   lo = 0;
    if (hi > 255) hi = 255;
    if (lo >= hi) {
        request->send(400, "application/json",
                      "{\"error\": \"min must be below max\"}");
        return;
    }
    settings.config.auto_bri_min = (uint8_t)lo;
    settings.config.auto_bri_max = (uint8_t)hi;
    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json",
                  "{\"auto_bri_min\": " + String(lo) + ", \"auto_bri_max\": " + String(hi) + "}");
}

static void handleSetDimTimeout(AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
        request->send(400, "application/json", "{\"error\": \"Missing value\"}");
        return;
    }
    long secs = request->getParam("value", true)->value().toInt();
    if (secs < 0) secs = 0;
    if (secs > 86400) secs = 86400;
    settings.config.display_dim_timeout_s = (uint32_t)secs;
    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json",
                  "{\"display_dim_timeout_s\": " + String((unsigned long)secs) + "}");
}

static void handleSetBrightness(AsyncWebServerRequest *request) {
    if (!request->hasParam("value")) {
        request->send(400, "application/json", "{\"error\": \"Invalid parameters\"}");
        return;
    }

    int brightness = request->getParam("value")->value().toInt();
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    settings.config.brightness = brightness;
    tpanels3.set_backlight_brightness(brightness);

    // Without this, a brightness set here while display_dim_active is still
    // true from an earlier auto-dim (its own bookkeeping untouched by this
    // direct write) leaves the FSM believing it is mid-dim at a value that no
    // longer matches reality -- and, since undim_target only ever gets armed
    // by this same call, real touch/motion notify_user_activity() afterward
    // computes its undim target from stale state. The MQTT "brightness"
    // command already does this; this path did not, and was the one setter
    // that could run unauthenticated from anywhere on the network.
    // Not notify_user_activity() itself: that forces an immediate
    // wake-to-target whenever brightness reads 0, undoing "value=0" right
    // back to ~50 on the next poll.
    extern AppFsm g_fsm;
    app_fsm_notify_brightness_set(g_fsm, (uint8_t)brightness);

    request->send(200, "application/json", "{\"brightness\": " + String(brightness) + "}");
}

static void handleConfigureWireGuard(AsyncWebServerRequest *request) {
    String privateKey, publicKey, presharedKey, ipAddress, endpoint, allowedIPs;
    int endpointPort = 0;

    if (request->hasParam("privateKey", true))   privateKey   = request->getParam("privateKey", true)->value();
    if (request->hasParam("publicKey", true))    publicKey    = request->getParam("publicKey", true)->value();
    if (request->hasParam("presharedKey", true)) presharedKey = request->getParam("presharedKey", true)->value();
    if (request->hasParam("ipAddress", true))    ipAddress    = request->getParam("ipAddress", true)->value();
    if (request->hasParam("endpoint", true))     endpoint     = request->getParam("endpoint", true)->value();
    if (request->hasParam("endpointPort", true)) endpointPort = request->getParam("endpointPort", true)->value().toInt();
    if (request->hasParam("allowedIPs", true))   allowedIPs   = request->getParam("allowedIPs", true)->value();

    const bool ok = !privateKey.isEmpty() && !publicKey.isEmpty() && !presharedKey.isEmpty() &&
                    !ipAddress.isEmpty() && !endpoint.isEmpty() && endpointPort > 0 && !allowedIPs.isEmpty();

    if (!ok) {
        logger.notice("Missing WireGuard parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
        return;
    }

    settings.config.wgPrivateKey   = privateKey;
    settings.config.wgPublicKey    = publicKey;
    settings.config.wgPresharedKey = presharedKey;
    settings.config.wgIpAddress    = ipAddress;
    settings.config.wgEndpoint     = endpoint;
    settings.config.wgEndpointPort = endpointPort;
    settings.config.wgAllowedIPs   = allowedIPs;

    logger.notice("WireGuard configuration parsed and saved");
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "application/json", "{\"status\": \"WireGuard configuration saved\"}");
}

static String g_wifi_networks_body;

static void handleConfigureWiFiNetworksBody(AsyncWebServerRequest *request,
                                             uint8_t *data, size_t len,
                                             size_t index, size_t total)
{
    if (index == 0) { g_wifi_networks_body = ""; g_wifi_networks_body.reserve(total); }
    for (size_t i = 0; i < len; i++) g_wifi_networks_body += (char)data[i];
    if (index + len < total) return;

    JsonDocument doc;
    if (deserializeJson(doc, g_wifi_networks_body) || !doc["networks"].is<JsonArray>()) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        g_wifi_networks_body = "";
        return;
    }

    settings.config.wifi_networks.clear();
    for (JsonObject net : doc["networks"].as<JsonArray>()) {
        if (settings.config.wifi_networks.size() >= WIFI_NETWORKS_MAX) break;
        SETTINGS::WifiNetwork n;
        n.ssid     = net["ssid"].as<String>();
        n.password = net["password"].as<String>();
        if (n.ssid.length() > 0)
            settings.config.wifi_networks.push_back(n);
    }
    if (!settings.config.wifi_networks.empty()) {
        settings.config.wifi_bssid    = settings.config.wifi_networks[0].ssid;
        settings.config.wifi_password = settings.config.wifi_networks[0].password;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    g_wifi_networks_body = "";

    request->send(200, "application/json", "{\"status\":\"saved\",\"count\":" +
                  String(settings.config.wifi_networks.size()) + "}");
    ui_blank_screen_for_reset();
    ESP.restart();
}

static void handleConfigureMQTT(AsyncWebServerRequest *request) {
    String serverName, user, pass;
    int port = 0;

    if (request->hasParam("server", true))   serverName = request->getParam("server", true)->value();
    if (request->hasParam("port", true))     port       = request->getParam("port", true)->value().toInt();
    if (request->hasParam("username", true)) user       = request->getParam("username", true)->value();
    if (request->hasParam("password", true)) pass       = request->getParam("password", true)->value();

    if (serverName.isEmpty() || port <= 0) {
        logger.notice("Missing 'server' or 'port' parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing server or port parameters\"}");
        return;
    }

    settings.config.mqttServer   = serverName;
    settings.config.mqtt_port    = port;
    settings.config.mqttUsername = user;
    settings.config.mqttPassword = pass;

    // Apply to the live client too, not just settings.config - otherwise this
    // form silently has no effect until the next reboot (see mqtt.h). Drop the
    // current connection so setup_mqtt() reconnects with the new broker/port
    // on its next call instead of staying attached to the old one.
    mqtt.applyConfig(serverName, (uint16_t)port, user, pass);
    if (mqtt_client.connected())
        mqtt_client.disconnect();

    logger.notice("MQTT configuration parsed and saved");
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "application/json", "{\"status\": \"MQTT configuration saved\"}");
}


// -------------------- Telnet WebSocket bridge --------------------
#include <errno.h>
// Browser can't do raw TCP; this bridges WebSocket <-> Telnet TCP (LAN use).
#include <Ticker.h>
#include <map>

static AsyncWebSocket g_ws_telnet("/ws/telnet");
static Ticker g_telnet_ticker;

struct TelnetSession {
    AsyncWebSocketClient* ws = nullptr;
    WiFiClient tcp;
    bool tcp_connected = false;
    String host;
    uint16_t port = 23;
};

static std::map<uint32_t, TelnetSession> g_telnet_sessions;

// Protocol: status messages are prefixed with '\x01' (SOH); everything else
// is raw terminal data forwarded as-is.  No JSON — this avoids parse failures
// when the terminal output itself contains JSON-like characters, and ensures
// the browser sees actual CR/LF and ANSI bytes rather than JSON escape sequences.
static void telnet_send_status(AsyncWebSocketClient* c, const String& msg) {
    if (!c) return;
    String out("\x01");
    out += msg;
    c->text(out);
}

static void telnet_send_data(AsyncWebSocketClient* c, const String& data) {
    if (!c) return;
    c->text(data);   // raw terminal bytes — no encoding
}

static void telnet_service() {
    for (auto it = g_telnet_sessions.begin(); it != g_telnet_sessions.end(); ) {
        auto& s = it->second;
        if (!s.ws || s.ws->status() != WS_CONNECTED) {
            if (s.tcp_connected) s.tcp.stop();
            it = g_telnet_sessions.erase(it);
            continue;
        }

        if (s.tcp_connected && s.tcp.connected()) {
            // Collect all available TCP bytes into ONE message per tick so the
            // browser receives a single WebSocket frame (AsyncWebSocket may coalesce
            // rapid c->text() calls into one frame, breaking JSON.parse).
            String all_data;
            all_data.reserve(512);
            while (s.tcp.available() && all_data.length() < 4096) {
                uint8_t b = (uint8_t)s.tcp.read();
                // Minimal TELNET negotiation handling (IAC)
                if (b == 255) { // IAC
                    if (s.tcp.available() < 2) continue; // partial — skip
                    uint8_t cmd = (uint8_t)s.tcp.read();
                    uint8_t opt = (uint8_t)s.tcp.read();
                    // Respond by refusing options (DO->WONT, WILL->DONT)
                    if (cmd == 253) { uint8_t resp[3] = {255, 252, opt}; s.tcp.write(resp, 3); }
                    else if (cmd == 251) { uint8_t resp[3] = {255, 254, opt}; s.tcp.write(resp, 3); }
                    // Drop negotiation bytes from output
                    continue;
                }
                all_data += (char)b;
            }
            if (all_data.length()) telnet_send_data(s.ws, all_data);
        } else {
            s.tcp_connected = false;
        }
        ++it;
    }

    // Stop ticker if no sessions
    if (g_telnet_sessions.empty()) {
        g_telnet_ticker.detach();
    }
}

static std::map<uint32_t, bool> g_ws_auth_ok;

static inline void ws_set_authorized(AsyncWebSocketClient* c, bool ok) {
    if (!c) return;
    if (ok) {
        g_ws_auth_ok[c->id()] = true;
    } else {
        g_ws_auth_ok.erase(c->id());
    }
}

static inline bool ws_is_authorized(AsyncWebSocketClient* c) {
    if (!c) return false;
    auto it = g_ws_auth_ok.find(c->id());
    return (it != g_ws_auth_ok.end()) && it->second;
}

static void ws_telnet_on_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                              AwsEventType type, void *arg, uint8_t *data, size_t len) {
    (void)server;

    if (type == WS_EVT_CONNECT) {
        // WS auth: some ESPAsyncWebServer builds do not provide reliable access
        // to the originating HTTP request (and BasicAuth headers) here.
        // The debug page itself is already access-controlled in your setup,
        // so we allow the WS connection and keep the terminal usable.
        ws_set_authorized(client, true);

        TelnetSession sess;
        sess.ws = client;
        g_telnet_sessions[client->id()] = sess;

        telnet_send_status(client, "ready");
        if (!g_telnet_ticker.active()) {
            g_telnet_ticker.attach_ms(20, telnet_service);
        }
        return;
    }


    if (type == WS_EVT_DISCONNECT) {
        ws_set_authorized(client, false);
        auto it = g_telnet_sessions.find(client->id());
        if (it != g_telnet_sessions.end()) {
            if (it->second.tcp_connected) it->second.tcp.stop();
            g_telnet_sessions.erase(it);
        }
        return;
    }

    if (type == WS_EVT_DATA) {
        if (!ws_is_authorized(client)) { client->close(); return; }
        auto it = g_telnet_sessions.find(client->id());
        if (it == g_telnet_sessions.end()) return;
        TelnetSession& sess = it->second;

        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (!info || info->final != 1 || info->index != 0) return;
        if (info->opcode != WS_TEXT) return;

        String msg;
        msg.reserve(len + 1);
        for (size_t i=0;i<len;i++) msg += (char)data[i];

        JsonDocument d;
        DeserializationError e = deserializeJson(d, msg);
        if (e) {
            telnet_send_status(client, "bad json");
            return;
        }

        const char* cmd = d["cmd"] | "";
        if (strcmp(cmd, "connect") == 0) {
            const char* host = d["host"] | "";
            uint16_t port = (uint16_t)(d["port"] | 23);
            if (!host || strlen(host)==0) { telnet_send_status(client, "host missing"); return; }

            if (sess.tcp_connected) sess.tcp.stop();
            sess.host = host;
            sess.port = port;

            // Connecting to our OWN address does not work on this lwIP build.
            // The port sets LWIP_HAVE_LOOPIF=1, which compiles out ip4_output's
            // "destination equals my own address -> loop back internally"
            // shortcut. Such a packet is put on the wire instead, the AP does
            // not reflect it, and connect() sits in select() until it times out
            // (visible as errno 119 EINPROGRESS). The dedicated lo0 interface
            // at 127.0.0.1 is the path this configuration actually provides.
            // Done here rather than in the browser so a host already stored in
            // localStorage keeps working.
            const IPAddress sta_ip = WiFi.localIP();
            const IPAddress ap_ip  = WiFi.softAPIP();
            if ((sta_ip && sess.host == sta_ip.toString()) ||
                (ap_ip  && sess.host == ap_ip.toString())) {
                logger.debug("[ws-telnet] %s is our own address, using 127.0.0.1 (lo0)",
                             sess.host.c_str());
                sess.host = "127.0.0.1";
            }

            sess.tcp.setTimeout(2000);

            telnet_send_status(client, "connecting...");

            // "connect failed" on its own says nothing about why. Arduino logs
            // the real reason via ESP_LOGE to Serial only, which is useless on
            // a deployed device -- so carry errno back to the browser and into
            // the uuid log (telnet) as well.
            errno = 0;
            bool ok = sess.tcp.connect(sess.host.c_str(), sess.port);
            const int connect_errno = errno;
            sess.tcp_connected = ok;

            if (ok) {
                telnet_send_status(client, "connected");
            } else {
                logger.err("[ws-telnet] connect to %s:%u failed, errno=%d (%s)",
                           sess.host.c_str(), (unsigned)sess.port,
                           connect_errno, strerror(connect_errno));
                String why("connect failed, errno=");
                why += connect_errno;
                why += " (";
                why += strerror(connect_errno);
                why += ")";
                telnet_send_status(client, why);
            }
            return;
        }

        if (strcmp(cmd, "disconnect") == 0) {
            if (sess.tcp_connected) sess.tcp.stop();
            sess.tcp_connected = false;
            telnet_send_status(client, "disconnected");
            return;
        }

        if (strcmp(cmd, "send") == 0) {
            const char* payload = d["data"] | "";
            if (!sess.tcp_connected || !sess.tcp.connected()) {
                telnet_send_status(client, "tcp not connected");
                return;
            }
            if (payload && strlen(payload)) {
                sess.tcp.write((const uint8_t*)payload, strlen(payload));
            }
            return;
        }
    }
}

// -------------------- H2 OTA file-upload page & handler --------------------


static uint8_t* g_h2_upload_buf = nullptr;
static size_t   g_h2_upload_pos = 0;
static bool     g_h2_upload_ok  = false;

// -------------------- S3 firmware upload --------------------
// Same idea as the H2 upload, but the image is written straight into the OTA
// partition instead of being buffered: at ~2.5 MB it would otherwise sit in
// PSRAM twice. Reuses the ElegantOTA callbacks so the display shows the same
// progress screen and ota_in_progress gates the main loop as usual.

static bool g_fw_upload_ok = false;

static void fw_restart_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the response reach the browser
    ESP.restart();
    vTaskDelete(nullptr);
}

static void handleFwUploadBody(AsyncWebServerRequest *request, const String& filename,
                               size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        // Checked here, not in the completion handler: the body arrives first,
        // and an unauthenticated upload would already be in flash by then.
        if (!ensureConfigAuth(request)) return;

        g_fw_upload_ok = false;
        const size_t total = request->contentLength();
        logger.notice("[FW] upload start: %s (%u bytes)", filename.c_str(), (unsigned)total);
        if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN)) {
            logger.err("[FW] Update.begin failed: %s", Update.errorString());
            return;
        }
        onOTAStart();
    }

    if (!Update.isRunning()) return;

    if (Update.write(data, len) != len) {
        logger.err("[FW] write failed: %s", Update.errorString());
        Update.abort();
        onOTAEnd(false);
        return;
    }
    onOTAProgress(index + len, request->contentLength());

    if (final) {
        g_fw_upload_ok = Update.end(true);
        if (!g_fw_upload_ok)
            logger.err("[FW] Update.end failed: %s", Update.errorString());
        logger.notice("[FW] upload done: %u bytes, ok=%d",
                      (unsigned)(index + len), (int)g_fw_upload_ok);
        onOTAEnd(g_fw_upload_ok);
    }
}

// ---------------------------------------------------------------------------
// LittleFS file upload
//
// Replaces individual files without touching the rest of the filesystem. The
// PlatformIO `uploadfs` target rewrites the whole partition, which would take
// the accumulated daily glucose files with it -- so refreshing something like
// the Zigbee device database needs a path that only writes what it is given.
// ---------------------------------------------------------------------------

static File     g_fs_upload;
static bool     g_fs_upload_ok;
static bool     g_fs_upload_answered;   // a response was already sent
static String   g_fs_upload_target;   // final name
static String   g_fs_upload_temp;     // ".part" name written to first
static String   g_fs_upload_error;

/// Rejects anything that could escape the filesystem root or name nothing.
static bool fsUploadPathOk(const String &path) {
    if (path.length() < 2 || path[0] != '/') return false;
    if (path.indexOf("..") >= 0) return false;
    if (path.indexOf('\\') >= 0) return false;
    return true;
}

static void handleFsUploadBody(AsyncWebServerRequest *request, const String &filename,
                               size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        g_fs_upload_ok = false;
        g_fs_upload_answered = false;
        g_fs_upload_error = "";

        // Checked on the first chunk, not in the completion handler: the body
        // arrives first and would already be written by then. On failure
        // requestAuthentication() has already sent a 401, so the completion
        // handler must not send a second response over the top of it -- doing
        // so reports a misleading 500 and hides the real reason.
        if (!ensureConfigAuth(request)) {
            g_fs_upload_answered = true;
            return;
        }

        // An explicit ?path= wins, so a file can be stored under a name other
        // than the one it happens to have on the uploading machine.
        String target;
        if (request->hasParam("path")) {
            target = request->getParam("path")->value();
        } else {
            target = "/";
            target += filename;
        }
        if (!target.startsWith("/")) target = "/" + target;

        if (!fsUploadPathOk(target)) {
            g_fs_upload_error = "invalid path";
            logger.err("[FS] rejected upload path: %s", target.c_str());
            return;
        }

        const size_t total = request->contentLength();
        const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        // The existing copy is only removed once the new one is complete, so
        // the temporary needs room alongside it.
        if (total > 0 && total > freeBytes) {
            g_fs_upload_error = "not enough space";
            logger.err("[FS] %s needs %u B, %u B free",
                       target.c_str(), (unsigned)total, (unsigned)freeBytes);
            return;
        }

        g_fs_upload_target = target;
        g_fs_upload_temp   = target + ".part";

        LittleFS.remove(g_fs_upload_temp);
        g_fs_upload = LittleFS.open(g_fs_upload_temp, "w");
        if (!g_fs_upload) {
            g_fs_upload_error = "cannot open file";
            logger.err("[FS] cannot open %s for writing", g_fs_upload_temp.c_str());
            return;
        }
        logger.notice("[FS] upload start: %s (%u bytes)",
                      target.c_str(), (unsigned)total);
    }

    if (!g_fs_upload) return;

    if (len > 0 && g_fs_upload.write(data, len) != len) {
        // A short write means the filesystem is full or failing; stop rather
        // than let a truncated file reach its final name.
        g_fs_upload_error = "write failed";
        logger.err("[FS] write failed at offset %u", (unsigned)index);
        g_fs_upload.close();
        LittleFS.remove(g_fs_upload_temp);
        return;
    }

    if (final) {
        size_t written = g_fs_upload.size();
        g_fs_upload.close();

        // Only now is the previous version replaced, so an interrupted upload
        // leaves the old file intact instead of a half-written one.
        LittleFS.remove(g_fs_upload_target);
        if (LittleFS.rename(g_fs_upload_temp, g_fs_upload_target)) {
            g_fs_upload_ok = true;
            logger.notice("[FS] upload done: %s, %u bytes",
                          g_fs_upload_target.c_str(), (unsigned)written);
        } else {
            g_fs_upload_error = "rename failed";
            logger.err("[FS] rename %s -> %s failed",
                       g_fs_upload_temp.c_str(), g_fs_upload_target.c_str());
            LittleFS.remove(g_fs_upload_temp);
        }
    }
}

static void handleFsUploadDone(AsyncWebServerRequest *request) {
    if (g_fs_upload_answered) return;   // 401 already went out

    String body;
    if (g_fs_upload_ok) {
        body = "{\"status\":\"ok\",\"path\":\"" + g_fs_upload_target +
               "\",\"used\":" + String(LittleFS.usedBytes()) +
               ",\"total\":" + String(LittleFS.totalBytes()) + "}";
    } else {
        const String reason = g_fs_upload_error.length()
            ? g_fs_upload_error
            : String("no file in request -- send it as multipart form data");
        body = "{\"status\":\"failed\",\"error\":\"" + reason + "\"}";
    }
    request->send(g_fs_upload_ok ? 200 : 500,
                  "application/json; charset=utf-8", body);
}

static void handleFwUploadDone(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *resp = request->beginResponse(
        g_fw_upload_ok ? 200 : 500, "application/json; charset=utf-8",
        g_fw_upload_ok ? "{\"status\":\"ok\",\"message\":\"rebooting\"}"
                       : "{\"status\":\"failed\"}");
    resp->addHeader("Connection", "close");
    request->send(resp);

    // ElegantOTA restarts from its own loop(); this path has to do it itself.
    if (g_fw_upload_ok)
        xTaskCreate(fw_restart_task, "fw_restart", 2048, nullptr, 1, nullptr);
}

// -------------------- H2 Zigbee handlers --------------------
// Write actions go through h2_enqueue(): these run on the AsyncTCP task, and
// h2_send() writes the UART directly from the loop task.

// Shared plumbing for the H2 write routes. Before this, each handler repeated
// the same parameter check, the same 400/503 bodies and the same 202 -- and a
// new one could silently omit a range check. Authentication stays an explicit
// line in each handler: /poll and /refresh are open on purpose, and hiding
// that decision inside a helper would make the exception invisible.
static const char *const H2_JSON = "application/json; charset=utf-8";

static bool h2_reject(AsyncWebServerRequest *request, const String &why) {
    request->send(400, H2_JSON, "{\"status\":\"rejected\",\"message\":\"" + why + "\"}");
    return false;
}

/// Reads a required numeric POST field and range-checks it. Answers on failure.
static bool h2_param(AsyncWebServerRequest *request, const char *name,
                     long lo, long hi, long &out) {
    if (!request->hasParam(name, true))
        return h2_reject(request, String(name) + " missing");
    out = request->getParam(name, true)->value().toInt();
    if (out < lo || out > hi)
        return h2_reject(request, String(name) + " out of range");
    return true;
}

/// Queues one command and answers. Every H2 write route ends here.
static void h2_dispatch(AsyncWebServerRequest *request, const char *cmd) {
    if (!h2_enqueue(cmd)) {
        request->send(503, H2_JSON, "{\"status\":\"rejected\",\"message\":\"queue full\"}");
        return;
    }
    request->send(202, H2_JSON, "{\"status\":\"accepted\"}");
}

static void handleH2Devices(AsyncWebServerRequest *request) {
    String body;
    h2_devices_json(body);
    request->send(200, "application/json; charset=utf-8", body);
}

static void handleH2Status(AsyncWebServerRequest *request) {
    String body; h2_status_json(body);
    request->send(200, "application/json; charset=utf-8", body);
}

static void handleH2ScanResult(AsyncWebServerRequest *request) {
    String body; h2_scan_json(body);
    request->send(200, "application/json; charset=utf-8", body);
}

/// Starts a network scan. Authenticated: it occupies the radio for several
/// seconds, during which the coordinator does not serve its devices.
static void handleH2ScanStart(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    // Optional with a default, so it is clamped here rather than rejected.
    long dur = 3;
    if (request->hasParam("dur", true)) dur = request->getParam("dur", true)->value().toInt();
    if (dur < 1) dur = 1;
    if (dur > 5) dur = 5;
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"scan\",\"dur\":%ld}", dur);
    h2_dispatch(request, cmd);
}

/// Reboots the coordinator. Authenticated -- it takes the Zigbee network down
/// for a few seconds.
static void handleH2Reboot(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    h2_dispatch(request, "{\"cmd\":\"reboot\"}");
}

/// Asks the H2 to re-send its device list; the reply refills the registry.
static void handleH2Refresh(AsyncWebServerRequest *request) {
    h2_enqueue("{\"cmd\":\"status\"}");
    h2_dispatch(request, "{\"cmd\":\"list\"}");
}

/// Removes one device: asks it to leave (works only while it is awake), drops
/// it from the H2's table either way, and clears the local registry entry so
/// the UI updates immediately instead of waiting for the next list reply.
static void handleH2Remove(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;

    long addr;
    if (!h2_param(request, "addr", 1, 0xFFFF, addr)) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"remove\",\"addr\":%ld}", addr);
    bool queued = h2_enqueue(cmd);
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"forget\",\"addr\":%ld}", addr);
    queued = h2_enqueue(cmd) && queued;

    h2_dev_forget((uint16_t)addr);
    mqtt_remove_zigbee_device((uint16_t)addr);   // no orphaned HA entities

    // Not h2_dispatch(): two commands were queued, and the local entry is
    // already gone even if the queue could not take them.
    if (!queued) {
        request->send(503, H2_JSON,
                      "{\"status\":\"partial\",\"message\":\"queue full, local entry cleared\"}");
        return;
    }
    request->send(202, H2_JSON, "{\"status\":\"accepted\"}");
}

/// Asks one device to report its attributes. The H2 ignores this for sleepy
/// battery devices (pollDevice() bails on !canPoll), so it is safe to offer on
/// every row -- it just does nothing there.
static void handleH2Poll(AsyncWebServerRequest *request) {
    long addr;
    if (!h2_param(request, "addr", 1, 0xFFFF, addr)) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"poll\",\"addr\":%ld}", addr);
    h2_dispatch(request, cmd);
}

/// Switches a device on or off. The H2 uses the endpoint it stored for the
/// device, so only the address is sent.
/// Sets a device's brightness (0-100 percent) via Level Control's Move To
/// Level With On/Off. Mirrors handleH2Switch(): validated the same way,
/// enqueued the same way, since the H2 command it produces is symmetric.
static void handleH2Level(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;

    long addr, value;
    if (!h2_param(request, "addr", 1, 0xFFFF, addr)) return;
    if (!h2_param(request, "value", 0, 100, value)) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"level\",\"addr\":%ld,\"value\":%ld}", addr, value);
    h2_dispatch(request, cmd);
}

/// Philips/Signify Hue motion sensor extras (SML001-SML004). Both attributes
/// are manufacturer-specific ZCL extensions -- not part of the ZHA database,
/// hardcoded here for this one device family rather than generalised, same
/// as data/local_devices.json on the H2 is a hand-curated exception rather
/// than something the exporter derives. Endpoint 2 and the cluster/attribute
/// IDs come from zhaquirks.philips.motion / zhaquirks.philips.PhilipsOccupancySensing.
/// Write-only: the H2 never reads these back, so the controls do not reflect
/// the device's actual current state, only the last command sent.
static const uint16_t PHILIPS_MANUF_CODE = 0x100B;

/// Toggles the Hue motion sensor's LED trigger indicator (Basic cluster
/// 0x0000, attribute 0x0033, bool, manufacturer-specific).
static void handleH2Led(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;

    long addr, on_raw;
    if (!h2_param(request, "addr", 1, 0xFFFF, addr)) return;
    if (!h2_param(request, "on", 0, 1, on_raw)) return;
    const bool on = (on_raw != 0);

    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"write_attr\",\"addr\":%ld,\"ep\":2,\"cluster\":\"0\",\"attr\":\"51\","
             "\"type\":\"bool\",\"value\":%d,\"manuf\":%u}",
             addr, on ? 1 : 0, (unsigned)PHILIPS_MANUF_CODE);
    h2_dispatch(request, cmd);
}

/// Sets the Hue motion sensor's PIR sensitivity (Occupancy Sensing cluster
/// 0x0406, attribute 0x0030, uint8, manufacturer-specific). 0/1/2 mirror the
/// Low/Medium/High levels Home Assistant's ZHA integration shows for it.
static void handleH2Sensitivity(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;

    long addr, level;
    if (!h2_param(request, "addr", 1, 0xFFFF, addr)) return;
    if (!h2_param(request, "level", 0, 2, level)) return;

    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"write_attr\",\"addr\":%ld,\"ep\":2,\"cluster\":\"1030\",\"attr\":\"48\","
             "\"type\":\"uint8\",\"value\":%ld,\"manuf\":%u}",
             addr, level, (unsigned)PHILIPS_MANUF_CODE);
    h2_dispatch(request, cmd);
}

static void handleH2Switch(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;

    long addr;
    if (!h2_param(request, "addr", 1, 0xFFFF, addr)) return;
    if (!request->hasParam("state", true)) { h2_reject(request, "state missing"); return; }
    const String state = request->getParam("state", true)->value();
    if (state != "on" && state != "off" && state != "toggle") {
        h2_reject(request, "state must be on, off or toggle");
        return;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"%s\",\"addr\":%ld}", state.c_str(), addr);
    h2_dispatch(request, cmd);
}

/// Opens the Zigbee network for joining. Authenticated: without it anyone on
/// the LAN could attach a device to the network.
static void handleH2Permit(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;

    // Optional with a default, so it is clamped rather than rejected.
    // 0 closes the network again; permit-join is one byte, hence 254.
    long seconds = 120;
    if (request->hasParam("seconds", true))
        seconds = request->getParam("seconds", true)->value().toInt();
    if (seconds < 0)   seconds = 0;
    if (seconds > 254) seconds = 254;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permit\",\"seconds\":%ld}", seconds);
    h2_dispatch(request, cmd);
}

static void handleH2OtaStatus(AsyncWebServerRequest *request)
{
    char buf[96];
    size_t w = h2_ota_written();
    size_t t = h2_ota_total();
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"written\":%u,\"total\":%u}",
             h2_ota_in_progress() ? "true" : "false",
             (unsigned)w, (unsigned)t);
    request->send(200, "application/json", buf);
}

static void handleH2OtaUploadDone(AsyncWebServerRequest *request)
{
    if (g_h2_upload_ok) {
        request->send(202, "application/json", "{\"status\":\"started\"}");
    } else {
        request->send(500, "application/json", "{\"error\":\"upload or ota start failed\"}");
    }
    g_h2_upload_ok = false;
}

static void handleH2OtaUploadBody(AsyncWebServerRequest *request,
                                   const String& /*filename*/,
                                   size_t index, uint8_t *data, size_t len, bool final)
{
    static constexpr size_t MAX_FW = 1536 * 1024; // 1.5 MB — well above H2 OTA partition

    if (index == 0) {
        // Abort any stale buffer
        if (g_h2_upload_buf) { heap_caps_free(g_h2_upload_buf); g_h2_upload_buf = nullptr; }
        g_h2_upload_pos = 0;
        g_h2_upload_ok  = false;
        g_h2_upload_buf = (uint8_t*)heap_caps_malloc(MAX_FW, MALLOC_CAP_SPIRAM);
        if (!g_h2_upload_buf) {
            logger.warning("[H2-OTA] PSRAM alloc failed");
            return;
        }
    }

    if (!g_h2_upload_buf) return; // alloc failed earlier

    if (g_h2_upload_pos + len > MAX_FW) {
        logger.warning("[H2-OTA] firmware too large");
        heap_caps_free(g_h2_upload_buf); g_h2_upload_buf = nullptr;
        return;
    }

    memcpy(g_h2_upload_buf + g_h2_upload_pos, data, len);
    g_h2_upload_pos += len;

    if (final) {
        size_t fw_size = g_h2_upload_pos;
        uint8_t* buf   = g_h2_upload_buf;
        g_h2_upload_buf = nullptr; // ownership passes to h2_ota
        g_h2_upload_pos = 0;
        g_h2_upload_ok  = h2_ota_start_from_buffer(buf, fw_size);
        if (!g_h2_upload_ok) heap_caps_free(buf);
        logger.notice("[H2-OTA] upload done: %u bytes, started=%d", (unsigned)fw_size, g_h2_upload_ok);
    }
}

void register_webpage_routes(AsyncWebServer& server) {
    g_server = &server;

    // Register OTA endpoints once. Do NOT stop the webserver at runtime.
    ElegantOTA.begin(g_server);

// Telnet terminal (WebSocket bridge)
g_ws_telnet.onEvent(ws_telnet_on_event);
server.addHandler(&g_ws_telnet);

    // Dashboard + config page split
    server.on("/",              HTTP_GET,  handleDashboard);
    server.on("/configuration", HTTP_GET,  handleConfiguration);
    server.on("/config",        HTTP_GET,  handleConfigRedirect);


    // Debug page
    server.on("/debug",     HTTP_GET, handleDebugPage);
    server.on("/api/debug", HTTP_GET, handleApiDebug);

    // Dashboard APIs (order matters: longer first)
    server.on("/api/glucose/history", HTTP_GET, handleApiGlucoseHistory);
    server.on("/api/glucose",         HTTP_GET, handleApiGlucose);
    server.on("/api/config",          HTTP_GET, handleApiConfig);
    server.on("/api/config/restore",  HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              handleRestoreConfigBody);
    server.on("/api/fw/status",       HTTP_GET,  handleApiFwStatus);
    server.on("/api/fw/check",        HTTP_POST, handleApiFwCheck);
    server.on("/api/fw/install",      HTTP_POST, handleApiFwInstall);
    server.on("/api/fw/upload",       HTTP_POST, handleFwUploadDone, handleFwUploadBody);
    server.on("/api/fs/upload",       HTTP_POST, handleFsUploadDone, handleFsUploadBody);
    server.on("/api/h2/devices",      HTTP_GET,  handleH2Devices);
    server.on("/api/h2/status",       HTTP_GET,  handleH2Status);
    server.on("/api/h2/scan",         HTTP_GET,  handleH2ScanResult);
    server.on("/api/h2/scan",         HTTP_POST, handleH2ScanStart);
    server.on("/api/h2/reboot",       HTTP_POST, handleH2Reboot);
    server.on("/api/h2/refresh",      HTTP_POST, handleH2Refresh);
    server.on("/api/h2/permit",       HTTP_POST, handleH2Permit);
    server.on("/api/h2/remove",       HTTP_POST, handleH2Remove);
    server.on("/api/h2/switch",       HTTP_POST, handleH2Switch);
    server.on("/api/h2/level",        HTTP_POST, handleH2Level);
    server.on("/api/h2/led",          HTTP_POST, handleH2Led);
    server.on("/api/h2/sensitivity",  HTTP_POST, handleH2Sensitivity);
    server.on("/api/h2/poll",         HTTP_POST, handleH2Poll);
    server.on("/api/h2/ota/status",   HTTP_GET,  handleH2OtaStatus);
    server.on("/api/h2/ota/upload",   HTTP_POST, handleH2OtaUploadDone, handleH2OtaUploadBody);

    // Legacy config endpoints (used by index_html JS)
    server.on("/scan",               HTTP_GET,  handleScan);
    server.on("/login",              HTTP_POST, handleLogin);
    server.on("/connect",            HTTP_POST, handleConnect);
    server.on("/status",             HTTP_GET,  handleStatus);
    server.on("/toggle",             HTTP_POST, handleToggleFeature);
    server.on("/setBrightness",      HTTP_POST, handleSetBrightness);
    server.on("/setDimTimeout",      HTTP_POST, handleSetDimTimeout);
    server.on("/setAutoBriRange",    HTTP_POST, handleSetAutoBriRange);
    server.on("/configureWireGuard",   HTTP_POST, handleConfigureWireGuard);
    server.on("/configureMQTT",        HTTP_POST, handleConfigureMQTT);
    server.on("/configureWiFiNetworks", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              handleConfigureWiFiNetworksBody);
}
