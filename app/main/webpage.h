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
<meta charset="UTF-8">
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

  button:disabled{ opacity:0.5; cursor:not-allowed; }
  @keyframes spin{ to{ transform:rotate(360deg); } }
  .spinner{
    display:inline-block; width:11px; height:11px;
    border:2px solid currentColor; border-top-color:transparent;
    border-radius:50%; animation:spin 0.7s linear infinite;
    vertical-align:middle;
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
    font-weight: 400;
  }
  .switch{
    position: relative;
    display: inline-block;
    width: 36px;
    height: 20px;
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
    height:14px; width:14px;
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
    transform: translate(16px,-50%);
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
    <h2>Brightness &amp; Display</h2>
    <div class="brightness-container">
        <div class="brightness-label">Brightness: <span id="brightnessValue">50</span></div>
        <input type="range" id="brightnessSlider" min="0" max="255" value="50" oninput="updateBrightness(this.value)">
    </div>
    <div style="margin-top:14px;">
        <div class="brightness-label">Dim timeout: <span id="dimTimeoutDisplay">--</span></div>
        <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:6px;">
            <input type="number" id="dimTimeoutInput" min="0" max="3600" step="30" value="300"
                   style="width:90px;padding:6px 8px;border-radius:6px;border:1px solid var(--border);background:var(--card);color:var(--fg);">
            <span style="color:var(--muted);font-size:0.9em;">seconds &nbsp;(0 = disabled)</span>
            <button type="button" onclick="saveDimTimeout()">Apply</button>
            <span id="dimTimeoutStatus" style="color:var(--muted);font-size:0.9em;"></span>
        </div>
    </div>
</div>

<div class="container">
    <h2>WiFi Networks</h2>
    <p>Up to 5 networks. The device tries all of them on connect.</p>

    <div id="wifiNetworksList" style="margin-bottom:12px;"></div>

    <div style="border:1px solid var(--border);border-radius:12px;padding:12px;margin-bottom:10px;">
        <label style="color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em;">Add network</label>
        <div class="row" style="margin-top:8px;margin-bottom:8px;">
            <input type="text" id="wifiAddSsid" placeholder="SSID" style="flex:1;">
            <button type="button" onclick="scanWifiForAdd()">Scan</button>
            <select id="wifiScanSelect" style="flex:1;" onchange="document.getElementById('wifiAddSsid').value=this.value"></select>
        </div>
        <div class="row" style="margin-bottom:8px;">
            <input type="password" id="wifiAddPassword" placeholder="Password (leave empty for open networks)" style="flex:1;">
        </div>
        <button type="button" onclick="addManualNetwork()">+ Add</button>
    </div>
    <div class="row">
        <button type="button" id="btnSaveWifi" onclick="saveWifiNetworks()">Save &amp; Reboot</button>
        <span id="wifiSaveStatus" class="pill"></span>
    </div>
</div>

<div class="container">
    <h2>MQTT Configuration</h2>
    <div class="switch-container">
        <span class="switch-label">MQTT <span style="color:var(--muted);font-weight:400;font-size:0.9em;">(enable general MQTT functionality)</span></span>
        <label class="switch">
            <input type="checkbox" id="mqttToggle" onchange="toggleFeature('mqtt_mode', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">MQTT Master Mode <span style="color:var(--muted);font-weight:400;font-size:0.9em;">(enabled: ESP32 fetches LibreLinkUp data and publishes to MQTT; disabled: LLU data is provided by external source)</span></span>
        <label class="switch">
            <input type="checkbox" id="mqttMasterToggle" onchange="toggleFeature('mqtt_master_mode', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">Home Assistant Discovery <span style="color:var(--muted);font-weight:400;font-size:0.9em;">(ESP32 device is announced to Home Assistant via MQTT Auto-Discovery)</span></span>
        <label class="switch">
            <input type="checkbox" id="haDiscoveryToggle" onchange="toggleFeature('ha_discovery', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
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

<div class="container">
    <h2>WireGuard Configuration</h2>
    <div class="switch-container">
        <span class="switch-label">WireGuard</span>
        <label class="switch">
            <input type="checkbox" id="wireguardToggle" onchange="toggleFeature('wg_mode', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
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
    <h2>Firmware Update</h2>
    <div class="switch-container">
        <span class="switch-label">OTA Update</span>
        <label class="switch">
            <input type="checkbox" id="otaToggle" onchange="toggleFeature('ota_update', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">OTA Staging Channel <span style="color:var(--muted);font-weight:400;font-size:0.9em;">(enabled: checks staging manifest; disabled: checks release manifest)</span></span>
        <label class="switch">
            <input type="checkbox" id="otaStagingToggle" onchange="toggleFeature('ota_staging', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <div class="switch-container">
        <span class="switch-label">Force update / allow downgrade <span style="color:var(--muted);font-weight:400;font-size:0.9em;">(enabled: installs manifest version regardless of version direction)</span></span>
        <label class="switch">
            <input type="checkbox" id="otaForceToggle" onchange="toggleFeature('ota_force', this.checked)">
            <span class="slider"></span>
        </label>
    </div>
    <p class="hint">Checks GitHub manifest for a newer firmware and installs it after confirmation.</p>
    <div class="row" style="margin-bottom:10px;">
        <button type="button" id="btnCheck" onclick="checkFirmwareUpdate()">Check now</button>
        <button type="button" id="btnInstall" onclick="installFirmwareUpdate()">Install update</button>
    </div>
    <div class="pill">Current: <span id="fwCurrent">--</span></div>
    <div class="pill">Latest: <span id="fwLatest">--</span></div>
    <div class="pill">Available: <span id="fwAvailable">--</span></div>
    <div class="pill">Status: <span id="fwStatus">--</span></div>
    <div class="pill">Error: <span id="fwError">--</span></div>

    <h2 style="margin-top:1.5em;">Manual Firmware Upload</h2>
    <p class="hint">Flash a compiled .bin straight to the S3. The device reboots into the
       new image when the transfer completes.</p>
    <div style="margin-bottom:12px;">
        <input type="file" id="s3FwFile" accept=".bin" style="margin-bottom:10px;">
        <div class="row">
            <button type="button" id="s3FlashBtn" onclick="s3FlashStart()">Flash to S3</button>
            <span id="s3StatusPill" class="pill" style="flex:1;text-align:center;">idle</span>
        </div>
    </div>
    <div id="s3UploadWrap" style="display:none;">
        <div style="font-size:12px;color:var(--muted);margin-bottom:3px;display:flex;justify-content:space-between;">
            <span>Upload &amp; flash</span><span id="s3Pct">0%</span>
        </div>
        <div style="background:var(--border);border-radius:4px;height:8px;overflow:hidden;">
            <div id="s3Bar" style="width:0%;height:100%;background:#42a5f5;transition:width .3s;"></div>
        </div>
    </div>
</div>

<div class="container">
    <h2>H2 Co-Processor Firmware Update</h2>
    <p class="hint">Flash a compiled .bin directly to the ESP32-H2 Zigbee coordinator via UART.</p>
    <div class="pill">H2 FW: <span id="h2FwCurrent">--</span></div>
    <div class="pill">H2 Chip: <span id="h2ChipInfo">--</span></div>
    <div class="pill">H2 Seen: <span id="h2Seen">--</span></div>
    <div style="margin-bottom:12px;">
        <input type="file" id="h2FwFile" accept=".bin" style="margin-bottom:10px;">
        <div class="row">
            <button type="button" id="h2FlashBtn" onclick="h2FlashStart()">Flash to H2</button>
            <span id="h2StatusPill" class="pill" style="flex:1;text-align:center;">idle</span>
        </div>
    </div>
    <div id="h2UploadWrap" style="display:none;margin-bottom:8px;">
        <div style="font-size:12px;color:var(--muted);margin-bottom:3px;display:flex;justify-content:space-between;">
            <span>Upload &rarr; S3</span><span id="h2PctUp">0%</span>
        </div>
        <div style="background:var(--border);border-radius:4px;height:8px;overflow:hidden;">
            <div id="h2BarUp" style="width:0%;height:100%;background:#42a5f5;transition:width .3s;"></div>
        </div>
    </div>
    <div id="h2FlashWrap" style="display:none;">
        <div style="font-size:12px;color:var(--muted);margin-bottom:3px;display:flex;justify-content:space-between;">
            <span>Flash &rarr; H2</span><span id="h2PctFlash">0%</span>
        </div>
        <div style="background:var(--border);border-radius:4px;height:8px;overflow:hidden;">
            <div id="h2BarFlash" style="width:0%;height:100%;background:#66bb6a;transition:width .4s;"></div>
        </div>
    </div>
</div>

<div class="container">
    <h2>Zigbee Devices</h2>
    <p style="color:var(--muted);font-size:0.9em;">Devices paired to the ESP32-H2. The list is
       kept by the S3 and filled from the H2's list, join and sensor messages.</p>

    <div style="margin-bottom:12px;">
        <span class="pill">Channel: <span id="zbCh">--</span></span>
        <span class="pill">PAN: <span id="zbPan">--</span></span>
        <span class="pill">Role: <span id="zbRole">--</span></span>
        <span class="pill">Devices: <span id="zbCount">--</span></span>
        <span class="pill">H2 heap: <span id="zbHeap">--</span></span>
        <span class="pill">H2 uptime: <span id="zbUp">--</span></span>
    </div>

    <div class="row" style="margin-bottom:12px;">
        <input type="number" id="zbSeconds" min="0" max="254" value="120"
               style="width:90px;padding:6px 8px;border-radius:6px;border:1px solid var(--border);background:var(--card);color:var(--fg);">
        <span style="color:var(--muted);font-size:0.9em;">seconds</span>
        <button type="button" onclick="zbPermit()">Open Pairing</button>
        <button type="button" onclick="zbPermitClose()">Close Pairing</button>
        <button type="button" onclick="zbRefresh()">Reload List</button>
        <button type="button" onclick="zbScan()">Scan Networks</button>
        <span id="zbStatus" style="color:var(--muted);font-size:0.9em;"></span>
    </div>

    <div style="overflow-x:auto;">
      <table style="width:100%;border-collapse:collapse;font-size:0.9em;">
        <thead><tr id="zbHead">
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Address</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Model</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Vendor</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">EP</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Profile</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">State</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Motion</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Temp</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Battery</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Switch</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Brightness</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Seen</th>
          <th style="border-bottom:1px solid var(--border);"></th>
        </tr></thead>
        <tbody id="zbRows"></tbody>
      </table>
    </div>
    <p id="zbEmpty" style="color:var(--muted);font-size:0.9em;margin-top:10px;">
       No devices yet. "Reload List" queries the H2.</p>

    <h2 style="margin-top:1.5em;">Visible Zigbee Networks</h2>
    <p style="color:var(--muted);font-size:0.9em;">Which Zigbee networks the coordinator can
       see and on which channel. This finds crowded channels caused by other Zigbee networks
       - it does not show WiFi interference, which shares the same 2.4 GHz band.</p>
    <div style="overflow-x:auto;">
      <table style="width:100%;border-collapse:collapse;font-size:0.9em;">
        <thead><tr>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Channel</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">PAN</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Extended PAN</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Joinable</th>
          <th style="text-align:left;padding:6px;border-bottom:1px solid var(--border);color:var(--muted);font-weight:600;">Capacity</th>
        </tr></thead>
        <tbody id="zbNets"></tbody>
      </table>
    </div>
    <p id="zbNetsEmpty" style="color:var(--muted);font-size:0.9em;margin-top:10px;">
       No scan yet.</p>
</div>

<div class="container">
    <h2>Backup</h2>
    <p style="color:var(--muted);font-size:0.9em;">Downloads all settings (WiFi, LibreLinkUp, MQTT, WireGuard) as a JSON file, including passwords/keys — store it somewhere safe.</p>
    <button type="button" id="btnBackup" onclick="downloadConfigBackup()">Download Config Backup</button>

    <h2 style="margin-top:1.5em;">Restore</h2>
    <p style="color:var(--muted);font-size:0.9em;">Loads a previously downloaded backup file and overwrites all current settings. The device reboots automatically afterwards.</p>
    <input type="file" id="restoreFile" accept="application/json,.json">
    <button type="button" id="btnRestore" onclick="restoreConfigBackup()">Restore from File</button>
</div>

</div>

<script>
    document.addEventListener('DOMContentLoaded', (event) => {
        fetch('/status')
            .then(response => response.json())
            .then(data => {
                document.getElementById('otaToggle').checked = data.ota_update === 1;
                if (document.getElementById('otaStagingToggle')) document.getElementById('otaStagingToggle').checked = data.ota_staging === 1;
                if (document.getElementById('otaForceToggle')) document.getElementById('otaForceToggle').checked = data.ota_force === 1;
                document.getElementById('wireguardToggle').checked = data.wg_mode === 1;
                document.getElementById('mqttToggle').checked = data.mqtt_mode === 1;
                if (document.getElementById('mqttMasterToggle')) document.getElementById('mqttMasterToggle').checked = data.mqtt_master_mode === 1;
                document.getElementById('brightnessSlider').value = data.brightness;
                document.getElementById('brightnessValue').textContent = data.brightness;
            })
            .catch(error => console.error('Error loading status:', error));
    });

    document.addEventListener('DOMContentLoaded', () => {
        refreshFirmwareUpdateStatus();
        setInterval(refreshFirmwareUpdateStatus, 15000);
    });

    // --- S3 manual firmware upload --------------------------------------------
    function s3SetStatus(t){ document.getElementById('s3StatusPill').textContent = t; }
    function s3SetPct(p){
        document.getElementById('s3UploadWrap').style.display = 'block';
        document.getElementById('s3Bar').style.width = p + '%';
        document.getElementById('s3Pct').textContent = p + '%';
    }

    function s3FlashStart(){
        const f = document.getElementById('s3FwFile').files[0];
        if(!f){ s3SetStatus('Please select a .bin file'); return; }
        if(!confirm('Flash "' + f.name + '" to the S3?\n\nThe device reboots into the new '
                  + 'image when the transfer completes. Do not power it off meanwhile.')) return;

        document.getElementById('s3FlashBtn').disabled = true;
        s3SetStatus('Uploading...');
        const fd = new FormData();
        fd.append('firmware', f);
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/fw/upload');
        xhr.upload.onprogress = e => { if(e.lengthComputable) s3SetPct(Math.round(e.loaded/e.total*100)); };
        xhr.onload = () => {
            s3SetPct(100);
            if(xhr.status === 200){
                s3SetStatus('Done - rebooting');
            } else if(xhr.status === 401){
                s3SetStatus('Login required');
                document.getElementById('s3FlashBtn').disabled = false;
            } else {
                s3SetStatus('Failed: ' + xhr.status);
                document.getElementById('s3FlashBtn').disabled = false;
            }
        };
        // The device reboots as soon as it has answered, so a dropped connection
        // right after a completed upload is the expected outcome, not an error.
        xhr.onerror = () => { s3SetStatus('Connection closed - device rebooting?'); };
        xhr.send(fd);
    }

    // --- Zigbee devices -------------------------------------------------------
    function zbSay(msg, kind){
        const el = document.getElementById('zbStatus');
        if(!el) return;
        el.textContent = msg || '';
        el.style.color = kind === 'ok'  ? 'var(--ok)'
                       : kind === 'bad' ? 'var(--bad)' : 'var(--muted)';
    }

    function zbAge(s){
        if(s < 60)   return s + ' s';
        if(s < 3600) return Math.floor(s/60) + ' min';
        return Math.floor(s/3600) + ' h';
    }

    function zbProfile(z){
        const dim = '<span style="color:var(--muted);">-</span>';
        if(!z) return dim;
        if(z === 'matched') return '<span style="color:var(--ok);" title="setup came from the ZHA database">matched</span>';
        if(z === 'known')   return '<span title="in the database, but it carries nothing actionable - running on heuristics">known</span>';
        if(z === 'unknown') return '<span style="color:var(--muted);" title="not in the ZHA database - running on heuristics">unknown</span>';
        return '<span style="color:var(--muted);">' + z + '</span>';
    }

    function zbLevelCell(d){
        if(d.level === undefined || d.level === null || d.level < 0)
            return '<span style="color:var(--muted);">-</span>';
        return '<input type="range" min="0" max="100" value="' + d.level
             + '" data-zblevel="' + d.addr + '"'
             + ' style="width:90px;vertical-align:middle;" />'
             + ' <span data-zblevelval="' + d.addr + '" style="color:var(--muted);font-size:0.85em;">'
             + d.level + '%</span>';
    }

    function zbCell(html){
        return '<td style="padding:6px;border-bottom:1px solid var(--border);">' + html + '</td>';
    }

    async function zbLoad(){
        const body = document.getElementById('zbRows');
        if(!body) return;
        try{
            const res = await fetch('/api/h2/devices');
            const list = await res.json();
            document.getElementById('zbEmpty').style.display = list.length ? 'none' : 'block';
            body.innerHTML = '';
            const dim = '<span style="color:var(--muted);">-</span>';
            for(const d of list){
                const tr = document.createElement('tr');
                tr.innerHTML =
                    zbCell(d.hex) +
                    zbCell(d.model || dim) +
                    zbCell(d.mfr   || dim) +
                    zbCell(d.ep) +
                    // Says whether the device is driven by the ZHA database or
                    // by the coordinator's guesswork -- "known" means the
                    // database has the model but nothing this firmware can act
                    // on, so it is still running on heuristics.
                    zbCell(zbProfile(d.zha)) +
                    zbCell(d.online ? '<span style="color:var(--ok);">online</span>'
                                    : '<span style="color:var(--bad);">offline</span>') +
                    zbCell(d.occ < 0 ? dim : (d.occ ? 'yes' : 'no')) +
                    zbCell(d.temp === null ? dim : d.temp.toFixed(1) + ' &deg;C') +
                    zbCell(d.bat  <  0    ? dim : d.bat + ' %') +
                    // A device reporting occupancy is a sensor: the H2 mirrors its
                    // motion state into "on" (coordinator_zigbee.cpp), so "on"
                    // alone does not mean switchable.
                    zbCell((d.on < 0 || d.occ >= 0) ? dim :
                        '<button type="button" data-zbsw="' + d.addr + '" data-zbstate="on"'
                      + ' style="padding:3px 10px;font-size:0.85em;margin-right:4px;'
                      + (d.on ? 'border-color:var(--ok);color:var(--ok);font-weight:600;' : '') + '">On</button>'
                      + '<button type="button" data-zbsw="' + d.addr + '" data-zbstate="off"'
                      + ' style="padding:3px 10px;font-size:0.85em;'
                      + (d.on ? '' : 'border-color:var(--bad);color:var(--bad);font-weight:600;') + '">Off</button>') +
                    zbCell(zbLevelCell(d)) +
                    zbCell('<span style="color:var(--muted);">' + zbAge(d.age_s) + '</span>') +
                    zbCell('<button type="button" data-zbpoll="' + d.addr +
                           '" style="padding:4px 10px;font-size:0.85em;margin-right:6px;">Poll</button>' +
                           '<button type="button" data-zbaddr="' + d.addr +
                           '" data-zbname="' + (d.model || d.hex) +
                           '" style="padding:4px 10px;font-size:0.85em;">Remove</button>');
                body.appendChild(tr);
            }
        }catch(e){ zbSay('device list unreachable','bad'); }
    }

    function zbUptime(s){
        if(s < 3600)  return Math.floor(s/60) + ' min';
        if(s < 86400) return Math.floor(s/3600) + ' h';
        return Math.floor(s/86400) + ' d';
    }

    async function zbLoadStatus(){
        try{
            const d = await (await fetch('/api/h2/status')).json();
            const set = (id,v) => { const e=document.getElementById(id); if(e) e.textContent = v; };
            if(!d.valid){ set('zbCh','--'); set('zbPan','--'); set('zbRole','--');
                          set('zbCount','--'); set('zbHeap','--'); set('zbUp','--'); return; }
            set('zbCh', d.ch < 0 ? '--' : d.ch);
            set('zbPan', d.pan);
            set('zbRole', d.role || '--');
            set('zbCount', d.devices < 0 ? '--' : d.devices);
            set('zbHeap', Math.round(d.heap/1024) + ' kB');
            set('zbUp', zbUptime(d.uptime_s));
        }catch(e){}
    }

    async function zbLoadNets(){
        const body = document.getElementById('zbNets');
        if(!body) return;
        try{
            const d = await (await fetch('/api/h2/scan')).json();
            body.innerHTML = '';
            document.getElementById('zbNetsEmpty').style.display = d.nets.length ? 'none' : 'block';
            for(const n of d.nets){
                const tr = document.createElement('tr');
                tr.innerHTML =
                    zbCell(n.ch) + zbCell('0x' + (n.pan||0).toString(16).toUpperCase().padStart(4,'0')) +
                    zbCell('<span style="color:var(--muted);">' + (n.epid||'-') + '</span>') +
                    zbCell(n.join ? '<span style="color:var(--ok);">yes</span>' : 'no') +
                    zbCell('<span style="color:var(--muted);">router ' + (n.router?'y':'n') +
                           ', end device ' + (n.ed?'y':'n') + '</span>');
                body.appendChild(tr);
            }
        }catch(e){}
    }

    async function zbScan(){
        const r = await zbPost('/api/h2/scan', 'dur=3');
        if(r.status === 401){ zbSay('login required','bad'); return; }
        zbSay(r.ok ? 'scanning - the radio is busy for a few seconds' : 'scan rejected',
              r.ok ? 'ok' : 'bad');
        if(r.ok) setTimeout(zbLoadNets, 6000);
    }

    async function zbPost(url, body){
        const res = await fetch(url, {method:'POST',
            headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: body || ''});
        return {ok: res.ok, status: res.status};
    }

    async function zbPermit(){
        const s = parseInt(document.getElementById('zbSeconds').value, 10) || 120;
        const r = await zbPost('/api/h2/permit', 'seconds=' + s);
        if(r.status === 401){ zbSay('login required','bad'); return; }
        zbSay(r.ok ? ('pairing open for ' + s + ' s - pair the device now') : 'rejected',
              r.ok ? 'ok' : 'bad');
        if(r.ok) setTimeout(() => zbPost('/api/h2/refresh').then(() => setTimeout(zbLoad, 1500)),
                            s * 1000 + 500);
    }

    async function zbPermitClose(){
        const r = await zbPost('/api/h2/permit', 'seconds=0');
        if(r.status === 401){ zbSay('login required','bad'); return; }
        zbSay(r.ok ? 'pairing closed' : 'rejected', r.ok ? 'ok' : 'bad');
    }

    async function zbRefresh(){
        await zbPost('/api/h2/refresh');
        zbSay('query sent');
        setTimeout(zbLoad, 1200);
    }

    // Delegated: rows are rebuilt on every refresh.
    document.addEventListener('DOMContentLoaded', () => {
        const body = document.getElementById('zbRows');
        if(!body) return;
        body.addEventListener('click', async (ev) => {
            const sw = ev.target.closest('button[data-zbsw]');
            if(sw){
                const addr = sw.dataset.zbsw, state = sw.dataset.zbstate;
                sw.disabled = true;
                const r = await zbPost('/api/h2/switch', 'addr=' + addr + '&state=' + state);
                sw.disabled = false;
                if(r.status === 401){ zbSay('login required','bad'); return; }
                if(!r.ok){ zbSay('switch rejected','bad'); return; }
                // The H2 does not track the commanded state -- only an attribute
                // report does. Poll the device so the list shows reality.
                zbSay('switched ' + state, 'ok');
                // The S3 registry only learns from list/join replies, so ask for
                // one. The H2 tracks the commanded state itself since 1.0.40, so
                // this already shows the new value.
                setTimeout(async () => {
                    await zbPost('/api/h2/refresh');
                    setTimeout(zbLoad, 800);
                }, 400);
                // Second pass: poll the device and read back what it really is,
                // in case the command never reached it.
                setTimeout(async () => {
                    await zbPost('/api/h2/poll', 'addr=' + addr);
                    setTimeout(async () => { await zbPost('/api/h2/refresh');
                                             setTimeout(zbLoad, 800); }, 1200);
                }, 2500);
                return;
            }
            const poll = ev.target.closest('button[data-zbpoll]');
            if(poll){
                poll.disabled = true;
                const r = await zbPost('/api/h2/poll', 'addr=' + poll.dataset.zbpoll);
                poll.disabled = false;
                zbSay(r.ok ? 'poll sent - battery devices sleep through it and stay unchanged'
                           : 'poll rejected', r.ok ? 'ok' : 'bad');
                setTimeout(async () => { await zbPost('/api/h2/refresh');
                                         setTimeout(zbLoad, 800); }, 1500);
                return;
            }
            const btn = ev.target.closest('button[data-zbaddr]');
            if(!btn) return;
            const name = btn.dataset.zbname, addr = btn.dataset.zbaddr;
            if(!confirm('Remove "' + name + '"?\n\nThe device is asked to leave and is dropped '
                      + 'from the H2 table.\n\nThis only sticks while the device is online. An '
                      + 'offline or sleeping device never receives the leave request, keeps the '
                      + 'network key and reappears in the list as soon as it reports again - then '
                      + 'remove it once more while it shows "online".')) return;
            btn.disabled = true;
            const r = await zbPost('/api/h2/remove', 'addr=' + addr);
            if(r.status === 401){ zbSay('login required','bad'); btn.disabled = false; return; }
            zbSay(r.ok ? ('"' + name + '" removed') : 'remove rejected', r.ok ? 'ok' : 'bad');
            zbLoad();
        });
        // Ask the H2 once when the page opens. The boot-time request in
        // setup_UART_IPC() goes out while the H2 is still starting up, so
        // without this the status bar and the list stay empty until someone
        // presses "Reload List".
        // Live label while dragging -- no request, just keeps the % in sync
        // with the handle so the number does not lag behind the thumb.
        body.addEventListener('input', (ev) => {
            const sl = ev.target.closest('input[data-zblevel]');
            if(!sl) return;
            const label = body.querySelector('span[data-zblevelval="' + sl.dataset.zblevel + '"]');
            if(label) label.textContent = sl.value + '%';
        });
        // Fires once on release, not per drag tick -- same cascade as the
        // on/off switch: send, then refresh the list so it reflects reality
        // rather than the optimistic value shown while dragging.
        body.addEventListener('change', async (ev) => {
            const sl = ev.target.closest('input[data-zblevel]');
            if(!sl) return;
            const addr = sl.dataset.zblevel, value = sl.value;
            sl.disabled = true;
            const r = await zbPost('/api/h2/level', 'addr=' + addr + '&value=' + value);
            sl.disabled = false;
            if(r.status === 401){ zbSay('login required','bad'); return; }
            zbSay(r.ok ? 'brightness set' : 'brightness rejected', r.ok ? 'ok' : 'bad');
            setTimeout(async () => { await zbPost('/api/h2/refresh');
                                     setTimeout(zbLoad, 800); }, 400);
        });

        zbLoad(); zbLoadNets();
        zbPost('/api/h2/refresh').then(() => setTimeout(() => {
            zbLoadStatus(); zbLoad();
        }, 1200));

        setInterval(zbLoad, 5000);
        // Re-ask the H2 now and then so heap and uptime stay meaningful.
        setInterval(() => zbPost('/api/h2/refresh').then(
            () => setTimeout(zbLoadStatus, 1200)), 30000);
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

    // ---- Multi-WiFi management ----
    let wifiNetworks = []; // [{ssid, password}]

    function renderWifiList() {
        const container = document.getElementById('wifiNetworksList');
        if (!container) return;
        if (wifiNetworks.length === 0) {
            container.innerHTML = '<p style="color:var(--muted);font-size:13px;">No networks saved yet.</p>';
            return;
        }
        container.innerHTML = wifiNetworks.map((n, i) =>
            `<div class="row" style="margin-bottom:6px;">
               <span style="flex:1;">${escHtml(n.ssid)}</span>
               <span style="color:var(--muted);font-size:13px;flex:1;">${n.password.length > 0 ? '********' : '(open)'}</span>
               <button type="button" onclick="removeNetwork(${i})">Remove</button>
             </div>`
        ).join('');
    }

    function escHtml(s) {
        return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
    }

    function removeNetwork(i) {
        wifiNetworks.splice(i, 1);
        renderWifiList();
    }

    function addNetwork(ssid, password) {
        ssid = (ssid || '').trim();
        if (!ssid) { alert('SSID darf nicht leer sein.'); return; }
        if (wifiNetworks.length >= 5) { alert('Maximal 5 Netzwerke möglich.'); return; }
        if (wifiNetworks.find(n => n.ssid === ssid)) { alert('Dieses Netzwerk ist bereits in der Liste.'); return; }
        wifiNetworks.push({ ssid, password: password || '' });
        renderWifiList();
    }

    function addManualNetwork() {
        const ssid = (document.getElementById('wifiAddSsid').value || '').trim();
        const pw   = document.getElementById('wifiAddPassword').value || '';
        addNetwork(ssid, pw);
        document.getElementById('wifiAddSsid').value = '';
        document.getElementById('wifiAddPassword').value = '';
    }

    function addScannedNetwork() {
        const sel = document.getElementById('wifiScanSelect');
        if (!sel || !sel.value) { alert('Bitte zuerst scannen und ein Netzwerk auswählen.'); return; }
        const pw = document.getElementById('wifiAddPassword').value || '';
        addNetwork(sel.value, pw);
        document.getElementById('wifiAddPassword').value = '';
    }

    async function scanWifiForAdd() {
        const sel = document.getElementById('wifiScanSelect');
        if (!sel) return;
        sel.innerHTML = '<option>Scanning...</option>';
        for (let attempt = 0; attempt < 12; attempt++) {
            try {
                const data = await fetch('/scan', {cache:'no-store'}).then(r => r.json());
                if (data.scanning) {
                    await new Promise(r => setTimeout(r, 1500));
                    continue;
                }
                sel.innerHTML = '';
                if (!data.length) { sel.innerHTML = '<option>No networks found</option>'; return; }
                data.forEach(n => {
                    const opt = document.createElement('option');
                    opt.value = n.ssid;
                    opt.textContent = n.ssid + ' (' + n.rssi + ' dBm)';
                    sel.appendChild(opt);
                });
                // Pre-fill SSID field with first result so Add works without manual change
                const ssidField = document.getElementById('wifiAddSsid');
                if (ssidField && data.length) ssidField.value = data[0].ssid;
                return;
            } catch(e) {
                sel.innerHTML = '<option>Scan failed</option>';
                return;
            }
        }
        sel.innerHTML = '<option>Scan timeout</option>';
    }

    async function saveWifiNetworks() {
        const btn = document.getElementById('btnSaveWifi');
        const status = document.getElementById('wifiSaveStatus');
        if (btn) btn.disabled = true;
        if (status) status.textContent = 'Saving...';
        try {
            const r = await fetch('/configureWiFiNetworks', {
                method: 'POST',
                headers: { 'Content-Type': 'text/plain' },
                body: JSON.stringify({ networks: wifiNetworks })
            });
            const data = await r.json();
            if (r.ok) {
                if (status) status.textContent = 'Saved ' + data.count + ' network(s). Rebooting...';
            } else {
                if (status) status.textContent = 'Error: ' + (data.error || r.status);
            }
        } catch(e) {
            if (status) status.textContent = 'Request failed.';
        } finally {
            if (btn) btn.disabled = false;
        }
    }

    function scanWifi() { scanWifiForAdd(); } // legacy alias

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

    function saveDimTimeout() {
        const inp = document.getElementById('dimTimeoutInput');
        const st  = document.getElementById('dimTimeoutStatus');
        const disp = document.getElementById('dimTimeoutDisplay');
        if (!inp) return;
        const val = parseInt(inp.value, 10);
        if (isNaN(val) || val < 0) { if(st) st.textContent = 'invalid'; return; }
        if(st) st.textContent = '...';
        const fd = new FormData();
        fd.append('value', val);
        fetch('/setDimTimeout', { method: 'POST', body: fd })
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
            .then(() => {
                if(st) st.textContent = val === 0 ? 'disabled' : 'set to ' + val + ' s';
                if(disp) disp.textContent = val === 0 ? 'disabled' : val + ' s';
                setTimeout(() => { if(st) st.textContent = ''; }, 3000);
            })
            .catch(e => { if(st) st.textContent = 'error: ' + (e.message || e); });
    }

    const FW_SPINNER = '<span class="spinner"></span> ';

    function setFwBtnsDisabled(disabled) {
        ['btnCheck','btnInstall'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.disabled = disabled;
        });
    }

    function setFwBtnLabel(id, html) {
        const el = document.getElementById(id);
        if (el) el.innerHTML = html;
    }

    function renderFwStatus(data) {
        const setText = (id, value) => {
            const el = document.getElementById(id);
            if (el) el.textContent = (value === undefined || value === null || value === "") ? "--" : String(value);
        };
        setText("fwCurrent", data.current_version);
        setText("fwLatest", data.latest_version);
        setText("fwAvailable", data.update_available ? "yes" : "no");
        setText("fwStatus", data.status);
        setText("fwError", data.last_error);
        const h2 = data.h2 || {};
        const h2Fw = h2.fw_version || "--";
        const h2Chip = `model=${h2.chip_model || "--"} rev=${h2.chip_revision || "--"} cores=${h2.chip_cores || "--"} cpu=${h2.chip_cpu_mhz || "--"}MHz mac=${h2.chip_mac || "--"}`;
        const h2Seen = h2.has_data ? (Math.round((Number(h2.last_seen_ms_ago || 0))/1000) + " s ago") : "--";
        setText("h2FwCurrent", h2Fw);
        setText("h2ChipInfo", h2Chip);
        setText("h2Seen", h2Seen);

    }

    async function pollFwStatus(whileStatus, intervalMs, maxMs) {
        const deadline = Date.now() + maxMs;
        while (Date.now() < deadline) {
            await new Promise(r => setTimeout(r, intervalMs));
            try {
                const r = await fetch('/api/fw/status', {cache:'no-store'});
                if (!r.ok) break;
                const data = await r.json();
                renderFwStatus(data);
                if (data.status !== whileStatus) break;
            } catch(e) { break; }
        }
    }

    async function refreshFirmwareUpdateStatus() {
        try {
            const r = await fetch('/api/fw/status', {cache: 'no-store'});
            if (!r.ok) throw new Error(`HTTP ${r.status}`);
            renderFwStatus(await r.json());
        } catch (e) {
            console.error('Error loading firmware status:', e);
        }
    }

    async function checkFirmwareUpdate() {
        setFwBtnsDisabled(true);
        setFwBtnLabel('btnCheck', FW_SPINNER + 'Checking...');
        try {
            const r = await fetch('/api/fw/check', {method: 'POST'});
            if (!r.ok) throw new Error(`HTTP ${r.status}`);
            await pollFwStatus('checking', 1000, 30000);
        } catch (e) {
            console.error('Error requesting firmware check:', e);
            alert('Firmware check request failed.');
        } finally {
            setFwBtnsDisabled(false);
            setFwBtnLabel('btnCheck', 'Check now');
        }
    }

    async function installFirmwareUpdate() {
        if (!confirm('Install available firmware update now? Device will reboot on success.')) return;
        setFwBtnsDisabled(true);
        setFwBtnLabel('btnInstall', FW_SPINNER + 'Installing...');
        try {
            const r = await fetch('/api/fw/install', {method: 'POST'});
            const body = await r.json().catch(() => ({}));
            if (!r.ok) throw new Error(body.message || `HTTP ${r.status}`);
            await pollFwStatus('installing', 2000, 120000);
        } catch (e) {
            console.error('Error requesting firmware install:', e);
            alert(`Firmware install request failed: ${e.message || e}`);
        } finally {
            setFwBtnsDisabled(false);
            setFwBtnLabel('btnInstall', 'Install update');
        }
    }

    // ── H2 Co-Processor OTA ──────────────────────────────────────
    let h2PollTimer = null;

    function h2SetStatus(msg) {
        const el = document.getElementById('h2StatusPill');
        if (el) el.textContent = msg;
    }
    function h2SetUpload(pct) {
        document.getElementById('h2UploadWrap').style.display = 'block';
        document.getElementById('h2BarUp').style.width = pct + '%';
        document.getElementById('h2PctUp').textContent = pct + '%';
    }
    function h2SetFlash(written, total) {
        const pct = total > 0 ? Math.round(written / total * 100) : 0;
        document.getElementById('h2FlashWrap').style.display = 'block';
        document.getElementById('h2BarFlash').style.width = pct + '%';
        document.getElementById('h2PctFlash').textContent =
            pct + '% (' + Math.round(written/1024) + ' / ' + Math.round(total/1024) + ' KB)';
    }
    function h2PollFlash() {
        fetch('/api/h2/ota/status', {cache:'no-store'})
            .then(r => r.json())
            .then(d => {
                h2SetFlash(d.written, d.total);
                if (d.active) {
                    h2SetStatus('Flashing...');
                } else {
                    clearInterval(h2PollTimer); h2PollTimer = null;
                    document.getElementById('h2FlashBtn').disabled = false;
                    h2SetStatus(d.written >= d.total && d.total > 0 ? 'Done - H2 rebooting' : 'Finished');
                }
            })
            .catch(() => {});
    }
    function h2FlashStart() {
        const f = document.getElementById('h2FwFile').files[0];
        if (!f) { h2SetStatus('Please select a .bin file'); return; }
        document.getElementById('h2FlashBtn').disabled = true;
        document.getElementById('h2UploadWrap').style.display = 'none';
        document.getElementById('h2FlashWrap').style.display = 'none';
        h2SetStatus('Uploading...');
        const fd = new FormData();
        fd.append('firmware', f);
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/h2/ota/upload');
        xhr.upload.onprogress = e => { if (e.lengthComputable) h2SetUpload(Math.round(e.loaded/e.total*100)); };
        xhr.onload = () => {
            h2SetUpload(100);
            if (xhr.status === 202) {
                h2SetStatus('Flashing...');
                h2PollTimer = setInterval(h2PollFlash, 1000);
            } else {
                h2SetStatus('Upload error: ' + xhr.status);
                document.getElementById('h2FlashBtn').disabled = false;
            }
        };
        xhr.onerror = () => { h2SetStatus('Network error'); document.getElementById('h2FlashBtn').disabled = false; };
        xhr.send(fd);
    }
    // ─────────────────────────────────────────────────────────────

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

    async function downloadConfigBackup() {
        try {
            const r = await fetch('/api/config', {cache: 'no-store'});
            if (!r.ok) throw new Error(`HTTP error! status: ${r.status}`);
            const text = await r.text();

            const blob = new Blob([text], {type: 'application/json'});
            const url = URL.createObjectURL(blob);
            const ts = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');

            const a = document.createElement('a');
            a.href = url;
            a.download = `librelinkup-config-${ts}.json`;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
        } catch (error) {
            console.error('Error downloading config backup:', error);
            alert('Backup failed: ' + error.message);
        }
    }

    async function restoreConfigBackup() {
        const fileInput = document.getElementById('restoreFile');
        const file = fileInput.files[0];
        if (!file) {
            alert('Choose a backup JSON file first.');
            return;
        }
        if (!confirm('This overwrites ALL current settings with the backup file and reboots the device. Continue?')) {
            return;
        }
        try {
            const text = await file.text();
            const r = await fetch('/api/config/restore', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: text
            });
            const data = await r.json().catch(() => ({}));
            if (!r.ok) throw new Error(data.error || `HTTP error! status: ${r.status}`);
            alert('Settings restored. Device is rebooting...');
        } catch (error) {
            console.error('Error restoring config backup:', error);
            alert('Restore failed: ' + error.message);
        }
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

      // Multi-WiFi: populate from wifi_networks array (or migrate legacy)
      if (Array.isArray(cfg.wifi_networks) && cfg.wifi_networks.length > 0) {
        wifiNetworks = cfg.wifi_networks.map(n => ({ ssid: n.ssid || '', password: n.password || '' }));
      } else if (cfg.wifi_bssid) {
        wifiNetworks = [{ ssid: cfg.wifi_bssid, password: cfg.wifi_password || '' }];
      }
      renderWifiList();

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
      setCheck("otaStagingToggle", cfg.ota_staging);
      setCheck("otaForceToggle", cfg.ota_force);
      setCheck("wireguardToggle", cfg.wg_mode);
      setCheck("mqttToggle", cfg.mqtt_mode);
      setCheck("mqttMasterToggle", cfg.mqtt_master_mode);
      setCheck("haDiscoveryToggle", cfg.ha_discovery !== undefined ? cfg.ha_discovery : 1);

      const bs = document.getElementById("brightnessSlider");
      const bv = document.getElementById("brightnessValue");
      if(bs && (cfg.brightness !== undefined && cfg.brightness !== null)){
        bs.value = String(cfg.brightness);
        if(bv) bv.textContent = String(cfg.brightness);
      }
      if(cfg.display_dim_timeout_s !== undefined && cfg.display_dim_timeout_s !== null){
        const di = document.getElementById("dimTimeoutInput");
        const dd = document.getElementById("dimTimeoutDisplay");
        if(di) di.value = String(cfg.display_dim_timeout_s);
        if(dd) dd.textContent = cfg.display_dim_timeout_s === 0 ? "disabled" : cfg.display_dim_timeout_s + " s";
      }
    }catch(e){}
  }

  document.addEventListener("DOMContentLoaded", () => { prefill(); renderWifiList(); });
})();
</script>

</body>
</html>

)rawliteral";

#endif // webpage_H
