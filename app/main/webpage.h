#ifndef webpage_H
#define webpage_H

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
    <title>LibreLinkup Client User Login and Settings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body {
            font-family: 'Arial', sans-serif;
            background-color: #f8f9fa;
            margin: 0;
            padding: 20px;
            color: #333;
            transition: background-color 0.3s, color 0.3s;
        }
        .container {
            max-width: 600px;
            margin: auto;
            background: white;
            padding: 20px;
            border-radius: 10px;
            box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);
            margin-bottom: 20px;
            transition: background-color 0.3s, color 0.3s;
        }
        h2 {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 5px;
        }
        input[type="text"], input[type="password"], input[type="number"] {
            width: 100%;
            padding: 10px;
            margin-bottom: 10px;
            border: 1px solid #ccc;
            border-radius: 5px;
            background-color: #fff;
            color: #333;
            transition: background-color 0.3s, color 0.3s;
        }
        button, input[type="submit"] {
            background-color: #007bff;
            color: white;
            border: none;
            padding: 10px 20px;
            cursor: pointer;
            border-radius: 5px;
            transition: background-color 0.3s, color 0.3s;
        }
        button:hover, input[type="submit"]:hover {
            background-color: #0056b3;
        }
        .switch-container, .brightness-container {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 15px;
        }
        .switch-label {
            font-size: 0.9em;
        }
        .switch {
            position: relative;
            display: inline-block;
            width: 30px;
            height: 17px;
        }
        .switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: #ccc;
            transition: .4s;
            border-radius: 17px;
        }
        .slider:before {
            position: absolute;
            content: "";
            height: 13px;
            width: 13px;
            left: 2px;
            bottom: 2px;
            background-color: white;
            transition: .4s;
            border-radius: 50%;
        }
        input:checked + .slider {
            background-color: #007bff;
        }
        input:checked + .slider:before {
            transform: translateX(13px);
        }
        ul {
            list-style-type: none;
            padding: 0;
        }
        li {
            background: #e9ecef;
            margin-bottom: 5px;
            padding: 10px;
            border-radius: 5px;
        }
        /* Dark Mode Styling */
        .dark-mode {
            background-color: #333;
            color: #f8f9fa;
        }
        .dark-mode .container {
            background-color: #444;
            color: #f8f9fa;
        }
        .dark-mode input[type="text"],
        .dark-mode input[type="password"],
        .dark-mode input[type="number"] {
            background-color: #555;
            color: #f8f9fa;
            border: 1px solid #888;
        }
        .dark-mode button,
        .dark-mode input[type="submit"] {
            background-color: #1a73e8;
            color: #fff;
        }
        .dark-mode button:hover,
        .dark-mode input[type="submit"]:hover {
            background-color: #0056b3;
        }
        .dark-mode .slider {
            background-color: #888;
        }
        .dark-mode .slider:before {
            background-color: #fff;
        }
        .dark-mode li {
            background-color: #555;
        }
    </style>
</head>
<body id="body">
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
    <form action="/connect" method="post">
        <label for="networks">Select Network:</label>
        <select id="networks" name="networks"></select>
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

<div class="container">
    <h2>Appearance</h2>
    <div class="switch-container">
        <span class="switch-label">Dark Mode</span>
        <label class="switch">
            <input type="checkbox" id="darkModeToggle" onchange="toggleDarkMode(this.checked)">
            <span class="slider"></span>
        </label>
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
                document.getElementById('brightnessSlider').value = data.brightness;
                document.getElementById('brightnessValue').textContent = data.brightness;
            })
            .catch(error => console.error('Error loading status:', error));
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
        headers: {
            'Content-Type': 'application/x-www-form-urlencoded'
        },
        body: formData.toString()
    })
    .then(response => {
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
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
        headers: {
            'Content-Type': 'application/x-www-form-urlencoded'
        },
        body: formData.toString()
    })
    .then(response => {
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        return response.json();
    })
    .then(data => {
        console.log('MQTT configuration saved:', data);
        alert('MQTT configuration saved successfully.');
    })
    .catch(error => console.error('Error saving MQTT configuration:', error));
}
</script>
</body>
</html>

    )rawliteral";

#endif //