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
#include "mqtt_handler.h"
#include "h2_ota.h"

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

const PAD_L=54, PAD_R=14, PAD_T=16, PAD_B=40;

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

  // Hover tooltip
  if(hoverIndex>=0 && hoverIndex<values.length && values[hoverIndex]!==null){
    const v=values[hoverIndex];
    const ts=tsArr[hoverIndex];
    const x=xOf(hoverIndex), y=yOf(v);

    ctx.strokeStyle = document.body.classList.contains("dark") ? "rgba(231,234,240,0.28)" : "rgba(0,0,0,0.25)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x, PAD_T);
    ctx.lineTo(x, PAD_T+plotH);
    ctx.stroke();

    const t1 = `${v} mg/dL`;
    const t2 = fmtDateTime(ts);

    ctx.font = `${Math.round((window.devicePixelRatio||1)*12)}px Arial`;
    const tw = Math.max(ctx.measureText(t1).width, ctx.measureText(t2).width);
    const bx = Math.min(x + 12, W - tw - 24);
    const by = Math.max(y - 54, 22);

    ctx.fillStyle = css("--tooltipBg");
    ctx.strokeStyle = css("--tooltipBorder");
    ctx.beginPath();
    if (ctx.roundRect) ctx.roundRect(bx, by-18, tw+16, 60, 8);
    else ctx.rect(bx, by-18, tw+16, 60);
    ctx.fill(); ctx.stroke();

    ctx.fillStyle = css("--fg");
    ctx.fillText(t1, bx+8, by);
    ctx.fillStyle = css("--muted");
    ctx.font = `${Math.round((window.devicePixelRatio||1)*10)}px Arial`;
    ctx.fillText(t2, bx+8, by+34);

    document.getElementById("hover").textContent = `Cursor: ${t1} @ ${t2}`;
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
      <input id="tnHost" placeholder="Host (z.B. 192.168.178.20)" style="width:220px;"/>
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
  const h=localStorage.getItem("tnHost")||"";
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
    settings.loadConfiguration(settings.config_filename, settings.config);

    JsonDocument json_config;
    json_config["ota_update"]        = settings.config.ota_update;
    json_config["ota_staging"]       = settings.config.ota_staging;
    json_config["ota_force"]         = settings.config.ota_force;
    json_config["wg_mode"]           = settings.config.wg_mode;
    json_config["mqtt_mode"]         = settings.config.mqtt_mode;
    json_config["mqtt_master_mode"]  = settings.config.mqtt_master_mode;
    json_config["brightness"]        = settings.config.brightness;

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
    } else {
        request->send(400, "application/json", "{\"error\": \"Unknown feature\"}");
        return;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json", "{\"status\": \"updated\"}");
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

    logger.notice("MQTT configuration parsed and saved");
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "application/json", "{\"status\": \"MQTT configuration saved\"}");
}


// -------------------- Telnet WebSocket bridge --------------------
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
            sess.tcp.setTimeout(2000);

            telnet_send_status(client, "connecting...");
            bool ok = sess.tcp.connect(sess.host.c_str(), sess.port);
            sess.tcp_connected = ok;
            telnet_send_status(client, ok ? "connected" : "connect failed");
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

static const char H2_OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>H2 Firmware Update</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;display:flex;
       justify-content:center;align-items:center;min-height:100vh;padding:20px}
  .card{background:#16213e;border-radius:12px;padding:32px;width:100%;max-width:480px;
        box-shadow:0 4px 24px #0005}
  h2{font-size:1.3rem;margin-bottom:6px;color:#90caf9}
  p{font-size:.85rem;color:#888;margin-bottom:24px}
  label{display:block;font-size:.8rem;color:#aaa;margin-bottom:6px}
  input[type=file]{width:100%;padding:10px;background:#0f3460;border:1px solid #1a5276;
                   border-radius:6px;color:#e0e0e0;cursor:pointer;margin-bottom:20px}
  button{width:100%;padding:12px;background:#2196f3;color:#fff;font-size:1rem;font-weight:bold;
         border:none;border-radius:6px;cursor:pointer;transition:background .2s}
  button:hover{background:#1565c0}
  button:disabled{background:#555;cursor:not-allowed}
  .bar-wrap{margin-top:16px}
  .bar-label{font-size:.75rem;color:#aaa;margin-bottom:4px;display:flex;justify-content:space-between}
  .bar-bg{background:#0f3460;border-radius:4px;height:10px;overflow:hidden}
  .bar{width:0%;height:100%;transition:width .4s}
  .bar-upload{background:#42a5f5}
  .bar-flash{background:#66bb6a}
  #status{margin-top:16px;font-size:.85rem;text-align:center;min-height:1.2em;color:#aaa}
  .ok{color:#66bb6a!important}.err{color:#ef5350!important}
</style></head><body>
<div class="card">
  <h2>H2 Firmware Update</h2>
  <p>Select compiled <code>.bin</code> for the ESP32-H2 coordinator and click Flash.</p>
  <label>Firmware file</label>
  <input type="file" id="fw" accept=".bin">
  <button id="btn" onclick="go()">Flash to H2</button>

  <div class="bar-wrap" id="wrap-upload" style="display:none">
    <div class="bar-label"><span>Upload zum S3</span><span id="pct-upload">0%</span></div>
    <div class="bar-bg"><div class="bar bar-upload" id="bar-upload"></div></div>
  </div>

  <div class="bar-wrap" id="wrap-flash" style="display:none">
    <div class="bar-label"><span>Flash auf H2</span><span id="pct-flash">0%</span></div>
    <div class="bar-bg"><div class="bar bar-flash" id="bar-flash"></div></div>
  </div>

  <div id="status"></div>
</div>
<script>
var pollTimer=null;

function setStatus(msg,cls){var s=document.getElementById('status');s.textContent=msg;s.className=cls||'';}

function setUpload(p){
  document.getElementById('wrap-upload').style.display='block';
  document.getElementById('bar-upload').style.width=p+'%';
  document.getElementById('pct-upload').textContent=p+'%';
}
function setFlash(written,total){
  var p=total>0?Math.round(written/total*100):0;
  document.getElementById('wrap-flash').style.display='block';
  document.getElementById('bar-flash').style.width=p+'%';
  document.getElementById('pct-flash').textContent=p+'% ('+Math.round(written/1024)+'/'+ Math.round(total/1024)+' KB)';
}

function pollFlash(){
  fetch('/api/h2/ota/status')
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.active){
        setFlash(d.written,d.total);
        setStatus('Flashing...');
      } else {
        setFlash(d.written,d.total);
        clearInterval(pollTimer);
        if(d.written>0 && d.written>=d.total){
          setStatus('H2 flashed successfully - rebooting!','ok');
        } else {
          setStatus('Flash abgeschlossen.','ok');
        }
      }
    })
    .catch(function(){});
}

function go(){
  var f=document.getElementById('fw').files[0];
  if(!f){setStatus('Please select a .bin file.','err');return;}
  document.getElementById('btn').disabled=true;
  setUpload(0);
  setStatus('Uploading...');

  var fd=new FormData();fd.append('firmware',f);
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/api/h2/ota/upload');
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable) setUpload(Math.round(e.loaded/e.total*100));
  };
  xhr.onload=function(){
    setUpload(100);
    if(xhr.status===202){
      setStatus('Upload done - flashing H2...');
      pollTimer=setInterval(pollFlash,1000);
    } else {
      setStatus('Fehler: '+xhr.responseText,'err');
      document.getElementById('btn').disabled=false;
    }
  };
  xhr.onerror=function(){setStatus('Netzwerkfehler.','err');document.getElementById('btn').disabled=false;};
  xhr.send(fd);
}
</script></body></html>
)rawliteral";

static uint8_t* g_h2_upload_buf = nullptr;
static size_t   g_h2_upload_pos = 0;
static bool     g_h2_upload_ok  = false;

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

static void handleH2OtaPage(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", H2_OTA_HTML);
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
    server.on("/h2ota",               HTTP_GET,  handleH2OtaPage);
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
    server.on("/configureWireGuard",   HTTP_POST, handleConfigureWireGuard);
    server.on("/configureMQTT",        HTTP_POST, handleConfigureMQTT);
    server.on("/configureWiFiNetworks", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              handleConfigureWiFiNetworksBody);
}
