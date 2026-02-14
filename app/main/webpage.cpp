// webpage.cpp (DROP-IN v11: adds /api/config + protected autofill)
// ------------------------------------------------------------
// Based on your current webpage.cpp reference (v10-ish).
// Adds:
//   - /api/config endpoint returning all settings as JSON
//   - Protected with same BasicAuth as /configuration
// Notes:
//   - This file expects: #include "settings.h" and extern SETTINGS settings;
//   - JSON payload is produced by web_get_config_json() implemented in web_config_api.cpp
// ------------------------------------------------------------

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "webpage.h"
#include "settings.h"

extern SETTINGS settings;

// --- forward decl (implemented in web_config_api.cpp) ---
__attribute__((weak)) String web_get_config_json() {
  return String("{\"error\":\"web_config_api.cpp missing\"}");
}

static const char dashboard_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Libre Dashboard</title>
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
let view = { values:[], ts:[], low:null, high:null };
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

function buildView(history){
  const raw = history?.values ?? [];
  const allV = raw.map(p => p ? Number(p.v) : null);
  const allT = raw.map(p => p ? Number(p.ts) : null);
  const low = history?.low ?? null;
  const high = history?.high ?? null;

  let lastTs=null;
  for(let i=allT.length-1;i>=0;i--){
    if(Number.isFinite(allT[i]) && allV[i]!==null){ lastTs=allT[i]; break; }
  }
  if(!Number.isFinite(lastTs)){
    view={values:allV, ts:allT, low, high};
    return;
  }

  const fromTs = lastTs - zoomHours*3600;
  const idx=[];
  for(let i=0;i<allT.length;i++){
    if(Number.isFinite(allT[i]) && allT[i]>=fromTs) idx.push(i);
  }
  if(!idx.length){
    view={values:allV, ts:allT, low, high};
    return;
  }
  view={ values: idx.map(i=>allV[i]), ts: idx.map(i=>allT[i]), low, high };

  document.getElementById("rangeInfo").textContent =
    `Range: ${fmtTime(view.ts[0])}–${fmtTime(view.ts[view.ts.length-1])} (${zoomHours}h)`;
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

  // X labels (3–5)
  const n = tsArr.length;
  if(n < 2) return;
  const ticks = [0, Math.round((n-1)*0.25), Math.round((n-1)*0.50), Math.round((n-1)*0.75), n-1];
  const uniq = Array.from(new Set(ticks)).sort((a,b)=>a-b);

  ctx.fillStyle = css("--muted");
  ctx.font = `${Math.round((window.devicePixelRatio||1)*10)}px Arial`;
  for (const ti of uniq){
    const x = PAD_L + (ti/(n-1))*plotW;
    const t = fmtTime(tsArr[ti]);
    ctx.fillText(t, x-16, PAD_T + plotH + 24);
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

  // Fixed Y scale: 50 mg/dL steps starting at 50
  const lo = 50;
  let hi = Math.ceil(max / 50) * 50;
  hi = Math.max(hi, 200);

  const xOf=i=> PAD_L + (i/(values.length-1))*plotW;
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

  // Last point marker
  for(let i=values.length-1;i>=0;i--){
    const v=values[i];
    if(v===null) continue;
    const x=xOf(i), y=yOf(v);
    ctx.fillStyle = css("--fg");
    ctx.beginPath();
    ctx.arc(x,y, Math.max(6, Math.round((window.devicePixelRatio||1)*4)), 0, Math.PI*2);
    ctx.fill();
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
    if(rel<0 || rel>1) hoverIndex=-1;
    else hoverIndex = Math.round(rel*(view.values.length-1));
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
    const mgdl = latest.mgdl ?? "--";
    const delta = Number(latest.delta);
    const trend = latest.trend ?? "--";
    const deltaTxt = Number.isFinite(delta) ? (delta>0?`+${delta}`:`${delta}`) : "--";

    const g=document.getElementById("glucose");
    g.innerHTML = `${mgdl}<span class="unit">mg/dL</span>`;
    document.getElementById("subline").textContent = `Δ ${deltaTxt} • Trend ${trend}`;

    const m = Number(mgdl);
    const lo = Number(latest.low);
    const hi = Number(latest.high);
    if(Number.isFinite(m) && Number.isFinite(lo) && Number.isFinite(hi)){
      if(m < lo) g.style.color = "#2563eb";
      else if(m > hi) g.style.color = "#dc2626";
      else g.style.color = css("--fg");
    }

    updateLifeBar(Number(latest.life_days), Number(latest.life_hours), Number(latest.life_minutes), Number(latest.life_seconds));

    document.getElementById("status").textContent = `Status: ${latest.ts_ok ? "Time OK" : "Time invalid"}`;
    document.getElementById("updated").textContent = `Updated: ${new Date().toLocaleTimeString()}`;
  }catch(e){
    document.getElementById("status").textContent = "Status: /api/glucose error";
  }

  try{
    const hist = await fetch("/api/glucose/history", {cache:"no-store"}).then(r=>r.json());
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

// --- API forward declarations (implemented in web_glucose_api.cpp / web_config_api.cpp) ---
String web_get_glucose_latest_json();
String web_get_glucose_history_json();
String web_get_config_json();

// ------------ Handlers / Routes ------------
static void handleDashboard(AsyncWebServerRequest *request) { request->send(200, "text/html", dashboard_html); }

static bool require_config_auth(AsyncWebServerRequest *request) {
  const String& user = settings.config.login_email;
  const String& pass = settings.config.login_password;

  // If not configured yet: allow /configuration, but DO NOT allow /api/config
  if (user.length() == 0 || pass.length() == 0) {
    return true;
  }
  if (!request->authenticate(user.c_str(), pass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

static void handleConfiguration(AsyncWebServerRequest *request) {
  const String& user = settings.config.login_email;
  const String& pass = settings.config.login_password;

  if (user.length() == 0 || pass.length() == 0) {
    request->send(200, "text/html", index_html);
    return;
  }
  if (!request->authenticate(user.c_str(), pass.c_str())) {
    return request->requestAuthentication();
  }
  request->send(200, "text/html", index_html);
}

static void handleConfigRedirect(AsyncWebServerRequest *request) { request->redirect("/configuration"); }

static void handleApiGlucose(AsyncWebServerRequest *request) { request->send(200, "application/json", web_get_glucose_latest_json()); }
static void handleApiGlucoseHistory(AsyncWebServerRequest *request) { request->send(200, "application/json", web_get_glucose_history_json()); }

// NEW: /api/config (protected)
static void handleApiConfig(AsyncWebServerRequest *request) {
  const String& user = settings.config.login_email;
  const String& pass = settings.config.login_password;

  if (user.length() == 0 || pass.length() == 0) {
    request->send(403, "application/json", "{\"error\":\"not configured\"}");
    return;
  }
  if (!request->authenticate(user.c_str(), pass.c_str())) {
    return request->requestAuthentication();
  }
  request->send(200, "application/json", web_get_config_json());
}

// weak fallbacks (compile even without API implementation)
__attribute__((weak)) String web_get_glucose_latest_json() { return String("{\"mgdl\":null,\"low\":null,\"high\":null,\"ts_ok\":false}"); }
__attribute__((weak)) String web_get_glucose_history_json() { return String("{\"low\":null,\"high\":null,\"values\":[]}"); }

void register_webpage_routes(AsyncWebServer& server) {
  server.on("/",                    HTTP_GET, handleDashboard);
  server.on("/configuration",       HTTP_GET, handleConfiguration);
  server.on("/config",              HTTP_GET, handleConfigRedirect);

  server.on("/api/glucose/history", HTTP_GET, handleApiGlucoseHistory);
  server.on("/api/glucose",         HTTP_GET, handleApiGlucose);

  // NEW:
  server.on("/api/config",          HTTP_GET, handleApiConfig);
}
