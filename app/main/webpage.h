// webpage.h  (1:1 DROP-IN)
// ------------------------------------------------------------
// Drop-in replacement for your existing webpage.h
// Change: SSID override UI:
// - Keep dropdown (name="networks")
// - Add text field "wifiSsidManual" (without name)
// - On submit: if the text field is filled, set "networks" to that value
//   (without backend changes).
// ------------------------------------------------------------

#ifndef webpage_H
#define webpage_H

#pragma once
#include <ESPAsyncWebServer.h>

/**
 * Registers all HTTP routes/handlers on the server.
 * Call this ONCE after creating the AsyncWebServer (e.g. in setup_OTA(true)).
 */
void register_webpage_routes(AsyncWebServer& server);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
<script>
(function(){
  // Theme is controlled on the Dashboard. Config page only reads localStorage.
  const v = localStorage.getItem("theme"); // "dark" | "light" | null
  document.documentElement.classList.remove("dark","light");
  if(v==="dark") document.documentElement.classList.add("dark");
  if(v==="light") document.documentElement.classList.add("light");
})();
</script>

    <title>LibreLinkup Client User Login and Settings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
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

    --btnBg: var(--card);
    --btnFg: var(--fg);
  }
  html.dark{
    --bg:#0b0f14;
    --fg:#e7eaf0;
    --muted: rgba(231,234,240,.75);
    --card:#0f1620;
    --border:#1e2a3a;
    --btnBg: var(--card);
    --btnFg: var(--fg);
  }
  @media (prefers-color-scheme: dark){
    html:not(.light){
      --bg:#0b0f14;
      --fg:#e7eaf0;
      --muted: rgba(231,234,240,.75);
      --card:#0f1620;
      --border:#1e2a3a;
      --btnBg: var(--card);
      --btnFg: var(--fg);
    }
  }

  body{
    margin:0;
    padding:18px;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
    background:var(--bg);
    color:var(--fg);
  }

  /* Old layout uses .container blocks */
  .container{
    max-width: 780px;
    margin: 0 auto 14px auto;
    padding: 16px;
    border-radius: 16px;
    border: 1px solid var(--border);
    background: var(--card);
    box-shadow: none;
  }

  h1,h2{
    margin: 0 0 12px 0;
    letter-spacing: -0.2px;
  }
  p, label{
    color: var(--muted);
  }

  input, select, textarea{
    width:100%;
    box-sizing:border-box;
    padding: 10px 12px;
    border-radius: 12px;
    border: 1px solid var(--border);
    background: transparent;
    color: var(--fg);
    outline: none;
  }

  button, .btn, a.button, input[type="submit"]{
    display:inline-flex;
    align-items:center;
    justify-content:center;
    gap:8px;
    padding:10px 12px;
    border-radius: 12px;
    border: 1px solid var(--border);
    background: var(--btnBg);
    color: var(--btnFg);
    cursor:pointer;
    text-decoration:none;
  }

  .row{ display:flex; gap:10px; flex-wrap:wrap; align-items:center; }
  .pill{
    padding:6px 10px;
    border-radius:999px;
    border:1px solid var(--border);
    color:var(--muted);
    font-size:13px;
  }

  /* Slider */
  input[type="range"]{ width: 100%; }

  /* Remove Config-page appearance toggle (theme is controlled on dashboard only) */
  #darkModeToggle{ display:none !important; }
  label[for="darkModeToggle"]{ display:none !important; }

  /* Make links look like dashboard buttons */
  a{ color: inherit; }


  /* iPhone-like switches (restore original UX) */
  .switch-container{
    display:flex;
    align-items:center;
    justify-content:space-between;
    gap:12px;
    margin: 10px 0;
  }
  .switch-label{
    color: var(--fg);
    font-weight: 600;
  }
  .switch{
    position: relative;
    display: inline-block;
    width: 52px;
    height: 30px;
    flex: 0 0 auto;
  }
  .switch input{
    opacity: 0;
    width: 0;
    height: 0;
  }
  .slider{
    position:absolute;
    cursor:pointer;
    top:0; left:0; right:0; bottom:0;
    background: var(--used, #c8ced8);
    transition: .2s;
    border-radius: 999px;
    border: 1px solid var(--border);
  }
  .slider:before{
    position:absolute;
    content:"";
    height:24px; width:24px;
    left:3px; top:50%;
    transform: translateY(-50%);
    background: var(--card);
    transition: .2s;
    border-radius: 999px;
    border: 1px solid var(--border);
  }
  input:checked + .slider{
    background: var(--ok);
    border-color: rgba(0,0,0,0);
  }
  input:checked + .slider:before{
    transform: translate(22px,-50%);
    border-color: rgba(0,0,0,0);
  }


  .topbar{
    max-width: 780px;
    margin: 0 auto 14px auto;
    display:flex;
    align-items:center;
    justify-content:space-between;
    gap:12px;
  }
  .topbar h1{
    margin:0;
    font-size: 22px;
    letter-spacing:-0.2px;
  }
</style>
</head>
<body id="body">
<div class="topbar">
  <h1>Config</h1>
  <a class="btn" href="/">Dashboard</a>
</div>

<div class="container">
    <h2>LibreLinkup User Login</h2>
    <form action="/login" method="post">
        <label for="username">Username:</label>
        <input type="text" id="username" name="username">
        <label for="password">Password:</label>
        <input type="password" id="password" name="password">
        <input type="submit" value="Login">
    </form>
</div>

<div class="container">
    <h2>WiFi Scan</h2>
    <button onclick="scanWifi()">Scan WiFi Networks</button>
    <ul id="wifi-list"></ul>

    <h2>Connect to WiFi</h2>
    <form id="wifiConnectForm" action="/connect" method="post">
        <label for="networks">Select Network:</label>
        <select id="networks" name="networks"></select>

        <label for="wifiSsidManual">SSID (manual override, optional):</label>
        <input type="text" id="wifiSsidManual" placeholder="Type SSID here to override selection">
        <div class="hint">Wenn dieses Feld gefuellt ist, wird es statt der Auswahl verwendet.</div>

        <label for="wifiPassword">Password:</label>
        <input type="password" id="wifiPassword" name="wifiPassword">
        <input type="submit" value="Connect">
    </form>
</div>

<div class="container">
    <h2>Feature Settings</h2>
    <div class="switch-container">
        <span class="switch-label">OTA Update</span>
        <label class="switch">
            <input type="checkbox" id="otaToggle" onchange="toggleFeature('ota_update', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">WireGuard</span>
        <label class="switch">
            <input type="checkbox" id="wireguardToggle" onchange="toggleFeature('wg_mode', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">MQTT</span>
        <label class="switch">
            <input type="checkbox" id="mqttToggle" onchange="toggleFeature('mqtt_mode', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">MQTT Master Mode</span>
        <label class="switch">
            <input type="checkbox" id="mqttMasterToggle" onchange="toggleFeature('mqtt_master_mode', this.checked)">
            <span class="slider"></span>
        </label>
    </div>

    <div class="brightness-container">
        <div class="brightness-label">Brightness: <span id="brightnessValue">50</span></div>
        <input type="range" id="brightnessSlider" min="0" max="255" value="50" oninput="updateBrightness(this.value)">
    </div>
</div>

<div class="container">
    <h2>WireGuard Configuration</h2>
    <form id="wireguardForm">
        <label for="wgPrivateKey">Private Key:</label>
        <input type="text" id="wgPrivateKey" name="wgPrivateKey">

        <label for="wgPublicKey">Public Key:</label>
        <input type="text" id="wgPublicKey" name="wgPublicKey">

        <label for="wgPresharedKey">Preshared Key:</label>
        <input type="text" id="wgPresharedKey" name="wgPresharedKey">

        <label for="wgIpAddress">IP Address:</label>
        <input type="text" id="wgIpAddress" name="wgIpAddress">

        <label for="wgEndpoint">Endpoint:</label>
        <input type="text" id="wgEndpoint" name="wgEndpoint">

        <label for="wgEndpointPort">Endpoint Port:</label>
        <input type="number" id="wgEndpointPort" name="wgEndpointPort" min="1" max="65535">

        <label for="wgAllowedIPs">Allowed IPs:</label>
        <input type="text" id="wgAllowedIPs" name="wgAllowedIPs">

        <button type="button" onclick="configureWireGuard()">Save WireGuard Config</button>
    </form>
</div>

<div class="container">
    <h2>MQTT Configuration</h2>
    <form id="mqttForm">
        <label for="mqttServer">Server Address:</label>
        <input type="text" id="mqttServer" name="mqttServer">

        <label for="mqttPort">Port:</label>
        <input type="number" id="mqttPort" name="mqttPort" min="1" max="65535">

        <label for="mqttUsername">Username:</label>
        <input type="text" id="mqttUsername" name="mqttUsername">

        <label for="mqttPassword">Password:</label>
        <input type="password" id="mqttPassword" name="mqttPassword">

        <button type="button" onclick="configureMQTT()">Save MQTT Config</button>
    </form>
</div>

</div>

<script>
    document.addEventListener('DOMContentLoaded', (event) => {
        fetch('/status')
            .then(response => response.json())
            .then(data => {
                document.getElementById('otaToggle').checked = data.ota_update === 1;
                document.getElementById('wireguardToggle').checked = data.wg_mode === 1;
                document.getElementById('mqttToggle').checked = data.mqtt_mode === 1;
                if (document.getElementById('mqttMasterToggle')) document.getElementById('mqttMasterToggle').checked = data.mqtt_master_mode === 1;
                document.getElementById('brightnessSlider').value = data.brightness;
                document.getElementById('brightnessValue').textContent = data.brightness;
            })
            .catch(error => console.error('Error loading status:', error));
    });

    // --- SSID manual override: if wifiSsidManual is not empty, submit that as "networks" ---
    document.addEventListener('DOMContentLoaded', () => {
        const form = document.getElementById('wifiConnectForm');
        const manual = document.getElementById('wifiSsidManual');
        const select = document.getElementById('networks');

        if (!form || !manual || !select) return;

        form.addEventListener('submit', () => {
            const overrideSsid = (manual.value || '').trim();
            if (overrideSsid.length > 0) {
                // Ensure the posted field "networks" becomes the manual SSID
                let found = false;
                for (let i = 0; i < select.options.length; i++) {
                    if (select.options[i].value === overrideSsid) {
                        found = true;
                        select.selectedIndex = i;
                        break;
                    }
                }
                if (!found) {
                    const opt = document.createElement('option');
                    opt.value = overrideSsid;
                    opt.textContent = `${overrideSsid} (manual)`;
                    select.appendChild(opt);
                    select.value = overrideSsid;
                }
            }
        });

        // Optional UX: if user selects dropdown, clear manual override
        select.addEventListener('change', () => {
            manual.value = '';
        });
    });

    function toggleDarkMode(isEnabled) {
        document.getElementById('body').classList.toggle('dark-mode', isEnabled);
    }

    function scanWifi() {
        fetch('/scan')
            .then(response => response.json())
            .then(data => {
                let wifiList = document.getElementById('wifi-list');
                let networkSelect = document.getElementById('networks');
                wifiList.innerHTML = '';
                networkSelect.innerHTML = '';

                data.forEach(network => {
                    let li = document.createElement('li');
                    li.textContent = `SSID: ${network.ssid}, Signal: ${network.rssi}`;
                    wifiList.appendChild(li);

                    let option = document.createElement('option');
                    option.value = network.ssid;
                    option.textContent = network.ssid;
                    networkSelect.appendChild(option);
                });
            })
            .catch(error => console.error('Error scanning WiFi:', error));
    }

    function toggleFeature(feature, isEnabled) {
        fetch(`/toggle?feature=${feature}&status=${isEnabled ? 1 : 0}`, { method: 'POST' })
            .then(response => {
                if (!response.ok) {
                    throw new Error(`HTTP error! status: ${response.status}`);
                }
                return response.json();
            })
            .then(data => console.log(`Feature ${feature} set to: ${data.status}`))
            .catch(error => console.error('Error toggling feature:', error));
    }

    function updateBrightness(value) {
        document.getElementById('brightnessValue').textContent = value;
        fetch(`/setBrightness?value=${value}`, { method: 'POST' })
            .then(response => {
                if (!response.ok) {
                    throw new Error(`HTTP error! status: ${response.status}`);
                }
                return response.json();
            })
            .then(data => console.log(`Brightness set to: ${data.brightness}`))
            .catch(error => console.error('Error setting brightness:', error));
    }

    function configureWireGuard() {
        const privateKey = document.getElementById('wgPrivateKey').value;
        const publicKey = document.getElementById('wgPublicKey').value;
        const presharedKey = document.getElementById('wgPresharedKey').value;
        const ipAddress = document.getElementById('wgIpAddress').value;
        const endpoint = document.getElementById('wgEndpoint').value;
        const endpointPort = document.getElementById('wgEndpointPort').value;
        const allowedIPs = document.getElementById('wgAllowedIPs').value;

        const formData = new URLSearchParams();
        formData.append("privateKey", privateKey);
        formData.append("publicKey", publicKey);
        formData.append("presharedKey", presharedKey);
        formData.append("ipAddress", ipAddress);
        formData.append("endpoint", endpoint);
        formData.append("endpointPort", endpointPort);
        formData.append("allowedIPs", allowedIPs);

        fetch('/configureWireGuard', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: formData.toString()
        })
        .then(response => {
            if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
            return response.json();
        })
        .then(data => {
            console.log('WireGuard configuration saved:', data);
            alert('WireGuard configuration saved successfully.');
        })
        .catch(error => console.error('Error saving WireGuard configuration:', error));
    }

    function configureMQTT() {
        const server = document.getElementById('mqttServer').value;
        const port = document.getElementById('mqttPort').value;
        const username = document.getElementById('mqttUsername').value;
        const password = document.getElementById('mqttPassword').value;

        const formData = new URLSearchParams();
        formData.append("server", server);
        formData.append("port", port);
        formData.append("username", username);
        formData.append("password", password);

        fetch('/configureMQTT', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: formData.toString()
        })
        .then(response => {
            if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
            return response.json();
        })
        .then(data => {
            console.log('MQTT configuration saved:', data);
            alert('MQTT configuration saved successfully.');
        })
        .catch(error => console.error('Error saving MQTT configuration:', error));
    }
</script>

<script>
(function(){
  function setVal(id, v){
    const el = document.getElementById(id);
    if(!el) return;
    if(v === undefined || v === null) return;
    // Don't overwrite password fields with empty strings
    if(el.type === "password" && String(v).length === 0) return;
    el.value = String(v);
  }
  function setCheck(id, v){
    const el = document.getElementById(id);
    if(!el) return;
    el.checked = (Number(v) === 1 || v === true);
  }

  async function prefill(){
    try{
      const r = await fetch("/api/config", {cache:"no-store"});
      if(!r.ok) return;
      const cfg = await r.json();

      // LLU login
      setVal("username", cfg.login_email);
      setVal("password", cfg.login_password);

      // WiFi: user requested wifi_bssid -> SSID/manual field
      setVal("wifiSsidManual", cfg.wifi_bssid);
      setVal("wifiPassword", cfg.wifi_password);

      // MQTT
      setVal("mqttServer", cfg.mqttServer);
      setVal("mqttPort", cfg.mqttPort);
      setVal("mqttUsername", cfg.mqttUsername);
      setVal("mqttPassword", cfg.mqttPassword);

      // WireGuard
      setVal("wgPrivateKey", cfg.wgPrivateKey);
      setVal("wgPublicKey", cfg.wgPublicKey);
      setVal("wgPresharedKey", cfg.wgPresharedKey);
      setVal("wgIpAddress", cfg.wgIpAddress);
      setVal("wgEndpoint", cfg.wgEndpoint);
      setVal("wgEndpointPort", cfg.wgEndpointPort);
      setVal("wgAllowedIPs", cfg.wgAllowedIPs);

      // Toggles + brightness (existing /status logic may also update these)
      setCheck("otaToggle", cfg.ota_update);
      setCheck("wireguardToggle", cfg.wg_mode);
      setCheck("mqttToggle", cfg.mqtt_mode);
      setCheck("mqttMasterToggle", cfg.mqtt_master_mode);

      const bs = document.getElementById("brightnessSlider");
      const bv = document.getElementById("brightnessValue");
      if(bs && (cfg.brightness !== undefined && cfg.brightness !== null)){
        bs.value = String(cfg.brightness);
        if(bv) bv.textContent = String(cfg.brightness);
      }
    }catch(e){}
  }

  document.addEventListener("DOMContentLoaded", prefill);
})();
</script>

</body>
</html>

)rawliteral";

#endif // webpage_H
