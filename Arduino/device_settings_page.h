#ifndef DEVICE_SETTINGS_PAGE_H
#define DEVICE_SETTINGS_PAGE_H

#include <pgmspace.h>

// Small self-contained page used both by the first-boot AP and by the station
// settings server.  The page deliberately uses the existing /scan and
// /connect portal API so the first-time Wi-Fi flow remains available.
static const char DEVICE_SETTINGS_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EPF frame settings</title><style>
:root{--ink:#1e2933;--muted:#64717d;--line:#d9e0e6;--panel:#fff;--page:#f3f6f8;--accent:#146c94;--good:#16734a;--warn:#946200;--bad:#a22}
*{box-sizing:border-box}body{font:16px system-ui,sans-serif;max-width:720px;margin:0 auto;padding:1.5rem 1rem 3rem;color:var(--ink);background:var(--page)}
h1,h2,p{margin-top:0}h1{margin-bottom:.3rem;font-size:1.8rem}h2{margin-bottom:.2rem;font-size:1.05rem}.muted{color:var(--muted);font-size:.9rem}.eyebrow{color:var(--accent);font-size:.75rem;font-weight:800;letter-spacing:.12em}
.panel{margin-top:1rem;padding:1rem 1.1rem;background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:0 2px 8px rgba(30,41,51,.05)}
.panel-title{display:flex;align-items:flex-start;justify-content:space-between;gap:1rem}.status-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.7rem;margin-top:1rem}.stat{padding:.75rem;background:#f7f9fa;border:1px solid #e5eaee;border-radius:7px;min-width:0}.stat-label{display:block;color:var(--muted);font-size:.78rem;text-transform:uppercase;letter-spacing:.04em}.value{display:block;margin-top:.2rem;font-weight:700;overflow-wrap:anywhere}.detail{display:block;margin-top:.15rem;color:var(--muted);font-size:.85rem;overflow-wrap:anywhere}
.badge{display:inline-block;padding:.25rem .55rem;border-radius:999px;background:#e8edf0;color:var(--muted);font-size:.78rem;font-weight:700;white-space:nowrap}.badge.good{background:#dff3e8;color:var(--good)}.badge.warn{background:#fff0c9;color:var(--warn)}.badge.bad{background:#f8dddd;color:var(--bad)}
label{display:block;margin-top:1rem;font-weight:600}input,select,button{box-sizing:border-box;width:100%;padding:.65rem;margin-top:.3rem;font:inherit}input{border:1px solid #bdc8d0;border-radius:5px;background:#fff}button{margin-top:1.2rem;background:var(--accent);color:#fff;border:0;border-radius:5px;cursor:pointer}.secondary{background:#52616d}.danger{background:var(--bad)}
fieldset.panel{margin-top:1rem}legend{padding:0 .35rem;font-weight:700}.actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:.7rem}.actions button{margin-top:0}.message{min-height:1.3rem;margin:1rem 0 0;white-space:pre-wrap}
@media(max-width:520px){.status-grid{grid-template-columns:1fr}.actions{grid-template-columns:1fr}.panel-title{display:block}.panel-title .badge{margin-top:.6rem}}
</style></head><body>
<header><div class="eyebrow">EPF FRAME</div><h1>Device settings</h1><p class="muted">Configure connectivity and monitor the frame from your local network or tailnet.</p></header>
<section class="panel"><div class="panel-title"><div><h2>Live status</h2><p class="muted">Updates automatically every five seconds.</p></div><span id="statusMode" class="badge">Loading</span></div>
<div class="status-grid">
<div class="stat"><span class="stat-label">Wi-Fi</span><strong id="wifiState" class="value">Loading…</strong><span id="wifiIp" class="detail">—</span></div>
<div class="stat"><span class="stat-label">Tailscale</span><strong id="tailscaleState" class="value">Loading…</strong><span id="tailscaleIp" class="detail">—</span></div>
<div class="stat"><span class="stat-label">EPF server</span><strong id="serverState" class="value">Loading…</strong><span id="serverDetail" class="detail">—</span></div>
<div class="stat"><span class="stat-label">Firmware</span><strong id="firmwareState" class="value">Loading…</strong><span id="memoryState" class="detail">—</span></div>
</div><p id="lastImage" class="muted" style="margin:.9rem 0 0">Last image: waiting for first request.</p></section>
<fieldset id="wifiBox" class="panel"><legend>Wi-Fi connection</legend>
<label for="ssid">Network</label><select id="ssid"><option value="">Scanning for networks…</option></select><input id="ssidManual" autocomplete="off" placeholder="Enter Wi-Fi network name" style="display:none">
<label for="wifiPassword">Wi-Fi password</label><input id="wifiPassword" type="password" autocomplete="off">
</fieldset>
<fieldset class="panel"><legend>Frame connection</legend>
<label for="serverUrl">EPF server URL</label><input id="serverUrl" placeholder="https://frame.example.net">
</fieldset>
<fieldset class="panel"><legend>Portal security</legend>
<p class="muted">This password protects the frame settings page. The username is <b>admin</b>.</p>
<label for="adminPassword">Change admin password</label><input id="adminPassword" type="password" autocomplete="new-password" placeholder="Leave blank to keep current">
</fieldset>
<fieldset class="panel"><legend>Tailscale</legend>
<label><input id="tailscaleEnabled" type="checkbox" style="width:auto"> Enable Tailscale after reboot</label>
<label for="tailscaleName">Device name</label><input id="tailscaleName" value="epf-frame">
<label for="tailscaleKey">Provisioning auth key</label><input id="tailscaleKey" type="password" autocomplete="off" placeholder="Leave blank to keep current">
<p id="tailscaleNote" class="muted"></p></fieldset>
<fieldset class="panel"><legend>Device actions</legend>
<p class="muted">Factory reset deletes saved Wi-Fi, server, portal-password, refresh-rate, and Tailscale settings, then restarts the frame.</p>
<button id="factoryReset" class="danger">Factory reset</button>
</fieldset>
<div class="actions"><button id="save">Save settings</button><button id="restart" class="secondary">Restart</button></div>
<p id="message" class="message"></p>
<script>
let isCaptive=false,savedSsid='';const $=id=>document.getElementById(id), msg=t=>{$('message').textContent=t};
function authHeaders(){const p=prompt('Admin password (cancel for no password)');return p===null?{}:{Authorization:'Basic '+btoa('admin:'+p)}}
function setValue(id,text,kind){const e=$(id);e.textContent=text;e.className='value'+(kind?' '+kind:'')}
function setBadge(id,text,kind){const e=$(id);e.textContent=text;e.className='badge'+(kind?' '+kind:'')}
function renderStatus(st){setBadge('statusMode',st.captive?'Captive setup':'Station mode',st.captive?'warn':'good');setValue('wifiState',st.wifi_connected?'Connected':'Offline',st.wifi_connected?'good':'bad');$('wifiIp').textContent=st.wifi_connected?st.wifi_ip:'No network address';if(!st.tailscale_compiled){setValue('tailscaleState','Not included','warn');$('tailscaleIp').textContent='Use a Tailscale-capable profile'}else if(st.tailscale_enabled){setValue('tailscaleState',st.tailscale_connected?'Connected':'Offline',st.tailscale_connected?'good':'bad');$('tailscaleIp').textContent=st.tailscale_connected?st.tailscale_ip:'Waiting for connection'}else{setValue('tailscaleState','Disabled','warn');$('tailscaleIp').textContent='Wi-Fi only'}setValue('serverState',st.server_url?'Configured':'Not configured',st.server_url?'good':'warn');$('serverDetail').textContent=st.server_url||'Set an EPF server URL';setValue('firmwareState',st.firmware||'Unknown');$('memoryState').textContent=st.heap?Math.round(st.heap/1024)+' KB free heap':'—';const code=Number(st.last_image_http_status||0);$('lastImage').textContent=code?'Last image: '+(st.last_image_success?'received successfully':'request failed')+' (HTTP '+code+')':'Last image: waiting for first request.'}
async function refreshStatus(){try{renderStatus(await (await fetch('/api/status')).json())}catch(e){setBadge('statusMode','Unavailable','bad')}}
async function scan(){try{const r=await fetch('/scan');if(r.status===202){setTimeout(scan,800);return}const a=await r.json();$('ssid').innerHTML='<option value="">Choose a network</option>'+a.map(n=>`<option value="${n.name.replaceAll('&','&amp;').replaceAll('"','&quot;')}">${n.name} (${n.rssi})</option>`).join('');if(savedSsid)$('ssid').value=savedSsid}catch(e){$('ssid').innerHTML='<option value="">Network scan unavailable</option>'}}
async function load(){try{const s=await (await fetch('/api/settings')).json();$('serverUrl').value=s.server_url||'';$('tailscaleEnabled').checked=!!s.tailscale_enabled;$('tailscaleName').value=s.tailscale_name||'epf-frame';$('tailscaleNote').textContent=s.tailscale_compiled?'':'Tailscale support is not included in this firmware.';savedSsid=s.wifi_ssid||'';const st=await (await fetch('/api/status')).json();isCaptive=!!st.captive;if(isCaptive){$('ssid').style.display='block';$('ssidManual').style.display='none';scan()}else{$('ssid').style.display='none';$('ssidManual').style.display='block';$('ssidManual').value=savedSsid}renderStatus(st)}catch(e){setBadge('statusMode','Unavailable','bad')}}
 $('save').onclick=async()=>{const setup=isCaptive?$('ssid').value:$('ssidManual').value;const body={server_url:$('serverUrl').value,tailscale_enabled:$('tailscaleEnabled').checked,tailscale_name:$('tailscaleName').value,wifi_ssid:setup,wifi_password:$('wifiPassword').value,admin_password:$('adminPassword').value,tailscale_auth_key:$('tailscaleKey').value};try{let r;if(isCaptive&&setup){r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:setup,pswd:$('wifiPassword').value,server:$('serverUrl').value,...body})})}else{r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json',...authHeaders()},body:JSON.stringify(body)})}msg(await r.text());if(r.ok&&setup) setTimeout(()=>location.reload(),1500)}catch(e){msg('Save failed: '+e)}};
$('restart').onclick=async()=>{try{const r=await fetch('/api/restart',{method:'POST',headers:authHeaders()});msg(await r.text())}catch(e){msg('Restart failed: '+e)}};
$('factoryReset').onclick=async()=>{if(!confirm('Factory reset this frame? This deletes all saved Wi-Fi, server, portal-password, refresh-rate, and Tailscale settings.'))return;try{const r=await fetch('/api/factory-reset',{method:'POST',headers:authHeaders()});msg(await r.text());if(r.ok)setTimeout(()=>location.href='/',3000)}catch(e){msg('Factory reset failed: '+e)}};
load();setInterval(refreshStatus,5000);
</script></body></html>
)rawliteral";

static const char DEVICE_ADMIN_SETUP_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Secure EPF frame</title><style>
body{font:16px system-ui,sans-serif;max-width:560px;margin:3rem auto;padding:0 1rem;color:#222}
input,button{box-sizing:border-box;width:100%;padding:.7rem;margin-top:.4rem;font:inherit}button{margin-top:1.2rem;background:#146c94;color:#fff;border:0;border-radius:4px}.muted{color:#666;font-size:.95rem}#message{margin-top:1rem;white-space:pre-wrap}
</style></head><body>
<h1>Secure your EPF frame</h1>
<p>Before opening device settings, create the password that protects this page.</p>
<p class="muted">This is the frame’s web-portal password, not the Wi-Fi password. The username is <b>admin</b>.</p>
<label for="password">Admin password</label>
<input id="password" type="password" autocomplete="new-password" minlength="8" placeholder="At least 8 characters">
<label for="confirm">Confirm admin password</label>
<input id="confirm" type="password" autocomplete="new-password" minlength="8">
<button id="save">Create password</button>
<p id="message"></p>
<script>
const $=id=>document.getElementById(id),msg=t=>$('message').textContent=t;
$('save').onclick=async()=>{const p=$('password').value;if(p.length<8){msg('Use at least 8 characters.');return}if(p!==$('confirm').value){msg('The passwords do not match.');return}try{const r=await fetch('/api/bootstrap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({admin_password:p})});const body=await r.text();if(!r.ok){msg(body);return}msg('Password created. Loading settings…');setTimeout(()=>location.href='/',600)}catch(e){msg('Could not create password: '+e)}};
</script></body></html>
)rawliteral";

#endif
