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

#include <string>
#include <vector>
#include <uuid/common.h>
#include <uuid/console.h>
#include <uuid/telnet.h>
#include <uuid/log.h>

#include "webpage.h"
#include "settings.h"
#include "tpanels3.h"

//------------------------[ uuid logger ]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//-------------------------------------------------------------------------

extern SETTINGS settings;
extern TPanelS3 tpanels3;
extern String availableNetworks;

// For handlers needing server access (OTA toggle)
static AsyncWebServer* g_server = nullptr;

// Local state (legacy)
static String username;
static String password;
static String wifi_bssid;
static String wifi_password;

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
    <button class="btn iconbtn" id="themeBtn" title="Toggle dark mode">🌓</button>
  </div>
</div>

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

function updateLifeBar(days,hours,minutes,seconds){
  const bar=document.getElementById("lifeBar");
  const txt=document.getElementById("lifeText");
  if(!bar || !txt) return;

  const remSec = Math.max(0, (days*86400) + (hours*3600) + (minutes*60) + seconds);
  const remHoursFloat = remSec / 3600.0;
  const remDaysFloat  = remSec / 86400.0;

  let cls="ok";
  if(remDaysFloat <= 1.0) cls="bad";
  else if(remDaysFloat <= 3.0) cls="warn";

  let blocks = 15;
  let filled = 0;

  if(remHoursFloat >= 24.0){
    blocks = 15;
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
  for(let i=values.length-1;i>=0;i--){
    const v=values[i];
    if(v===null) continue;
    const x=xOf(i), y=yOf(v);
    const isLive = (view.live && view.live[i] === true);

    if (isLive) {
      // Outer ring (green)
      ctx.fillStyle = "#22c55e";
      ctx.beginPath();
      ctx.arc(x, y, Math.max(10, Math.round((window.devicePixelRatio||1)*7)), 0, Math.PI*2);
      ctx.fill();

      // Inner dot (white)
      ctx.fillStyle = "#ffffff";
      ctx.beginPath();
      ctx.arc(x, y, Math.max(6, Math.round((window.devicePixelRatio||1)*4)), 0, Math.PI*2);
      ctx.fill();
    } else {
      ctx.fillStyle = css("--fg");
      ctx.beginPath();
      ctx.arc(x,y, Math.max(6, Math.round((window.devicePixelRatio||1)*4)), 0, Math.PI*2);
      ctx.fill();
    }
    break;
  }

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
      updateLifeBar(Number(latest.life_days), Number(latest.life_hours), Number(latest.life_minutes), Number(latest.life_seconds));
    }

    document.getElementById("status").textContent =
      `Status: ${sensorOk ? "Sensor valid" : "Sensor invalid"}`;
    document.getElementById("updated").textContent =
      `Updated: ${new Date().toLocaleTimeString()}`;

  }catch(e){
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
setInterval(refresh, 15000);
</script>
</body>
</html>
)rawliteral";

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
    const String& user = settings.config.login_email;
    const String& pass = settings.config.login_password;

    // If no credentials configured, leave open (same behavior as config page).
    if (user.length() != 0 && pass.length() != 0) {
        if (!request->authenticate(user.c_str(), pass.c_str())) {
            return request->requestAuthentication();
        }
    }

    request->send(200, "application/json", web_get_config_json());
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
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "text/html", "Login successful!<br><a href='/configuration'>Back</a>");
}

static void handleScan(AsyncWebServerRequest *request) {
    request->send(200, "application/json", availableNetworks);
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
    ESP.restart();
}

static void handleStatus(AsyncWebServerRequest *request) {
    settings.loadConfiguration("/config.json", settings.config);

    DynamicJsonDocument json_config(512);
    json_config["ota_update"] = settings.config.ota_update;
    json_config["wg_mode"]    = settings.config.wg_mode;
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

        // Best practice: do NOT stop/start the AsyncWebServer at runtime.
        // ElegantOTA endpoints are registered once (see register_webpage_routes()).
        // This toggle only enables/disables OTA on the UI side (your index_html can hide/show),
        // and can be checked by your firmware if you want to gate access.
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
    } else {
        request->send(400, "application/json", "{\"error\": \"Unknown feature\"}");
        return;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json", "{\"status\": \"updated\"}");
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

// -------------------- Route registration --------------------
void register_webpage_routes(AsyncWebServer& server) {
    g_server = &server;

    // Register OTA endpoints once. Do NOT stop the webserver at runtime.
    ElegantOTA.begin(g_server);

    // Dashboard + config page split
    server.on("/",              HTTP_GET,  handleDashboard);
    server.on("/configuration", HTTP_GET,  handleConfiguration);
    server.on("/config",        HTTP_GET,  handleConfigRedirect);

    // Dashboard APIs (order matters: longer first)
    server.on("/api/glucose/history", HTTP_GET, handleApiGlucoseHistory);
    server.on("/api/glucose",         HTTP_GET, handleApiGlucose);
    server.on("/api/config",          HTTP_GET, handleApiConfig);

    // Legacy config endpoints (used by index_html JS)
    server.on("/scan",               HTTP_GET,  handleScan);
    server.on("/login",              HTTP_POST, handleLogin);
    server.on("/connect",            HTTP_POST, handleConnect);
    server.on("/status",             HTTP_GET,  handleStatus);
    server.on("/toggle",             HTTP_POST, handleToggleFeature);
    server.on("/setBrightness",      HTTP_POST, handleSetBrightness);
    server.on("/configureWireGuard", HTTP_POST, handleConfigureWireGuard);
    server.on("/configureMQTT",      HTTP_POST, handleConfigureMQTT);
}
