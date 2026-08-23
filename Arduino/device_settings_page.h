#ifndef DEVICE_SETTINGS_PAGE_H
#define DEVICE_SETTINGS_PAGE_H

#include <pgmspace.h>

// Small self-contained page used both by the first-boot AP and by the station
// settings server.  The page deliberately uses the existing /scan and
// /connect portal API so the first-time Wi-Fi flow remains available.
static const char DEVICE_SETTINGS_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EPF frame settings</title><style>
body{font:16px system-ui,sans-serif;max-width:560px;margin:2rem auto;padding:0 1rem;color:#222}
label{display:block;margin-top:1rem;font-weight:600}input,select,button{box-sizing:border-box;width:100%;padding:.65rem;margin-top:.3rem;font:inherit}
button{margin-top:1.2rem;background:#146c94;color:#fff;border:0;border-radius:4px}.muted{color:#666;font-size:.9rem}
fieldset{margin-top:1.5rem;padding:0 1rem 1rem;border:1px solid #ccc}legend{font-weight:700}
#message{margin-top:1rem;white-space:pre-wrap}
</style></head><body>
<h1>EPF frame settings</h1><p class="muted">Use this page for first-time Wi-Fi setup or remote device settings.</p>
<fieldset id="wifiBox"><legend>Wi-Fi setup</legend>
<label for="ssid">Network</label><input id="ssid" list="networks" autocomplete="off"><datalist id="networks"></datalist>
<label for="wifiPassword">Wi-Fi password</label><input id="wifiPassword" type="password" autocomplete="off">
</fieldset>
<fieldset><legend>Frame</legend>
<label for="serverUrl">EPF server URL</label><input id="serverUrl" placeholder="http://192.168.1.134:15001">
</fieldset>
<fieldset><legend>Portal security</legend>
<p class="muted">This password protects the frame settings page. The username is <b>admin</b>.</p>
<label for="adminPassword">Change admin password</label><input id="adminPassword" type="password" autocomplete="new-password" placeholder="Leave blank to keep current">
</fieldset>
<fieldset><legend>Tailscale</legend>
<label><input id="tailscaleEnabled" type="checkbox" style="width:auto"> Enable Tailscale after reboot</label>
<label for="tailscaleName">Device name</label><input id="tailscaleName" value="epf-frame">
<label for="tailscaleKey">Provisioning auth key</label><input id="tailscaleKey" type="password" autocomplete="off" placeholder="Leave blank to keep current">
<p id="tailscaleNote" class="muted"></p></fieldset>
<button id="save">Save settings</button><button id="restart">Restart</button>
<p id="message"></p><pre id="status" class="muted"></pre>
<script>
let isCaptive=false;const $=id=>document.getElementById(id), msg=t=>{$('message').textContent=t};
function authHeaders(){const p=prompt('Admin password (cancel for no password)');return p===null?{}:{Authorization:'Basic '+btoa('admin:'+p)}}
async function scan(){try{const r=await fetch('/scan');if(r.status===202){setTimeout(scan,800);return}const a=await r.json();$('networks').innerHTML=a.map(n=>`<option value="${n.name.replaceAll('&','&amp;').replaceAll('"','&quot;')}">${n.name} (${n.rssi})</option>`).join('')}catch(e){}}
async function load(){try{const s=await (await fetch('/api/settings')).json();$('serverUrl').value=s.server_url||'';$('tailscaleEnabled').checked=!!s.tailscale_enabled;$('tailscaleName').value=s.tailscale_name||'epf-frame';$('tailscaleNote').textContent=s.tailscale_compiled?'':'Tailscale support is not included in this firmware.';if(s.wifi_ssid)$('ssid').value=s.wifi_ssid;const st=await (await fetch('/api/status')).json();isCaptive=!!st.captive;if(isCaptive)scan()}catch(e){}}
$('save').onclick=async()=>{const setup=$('ssid').value;const body={server_url:$('serverUrl').value,tailscale_enabled:$('tailscaleEnabled').checked,tailscale_name:$('tailscaleName').value,wifi_ssid:setup,wifi_password:$('wifiPassword').value,admin_password:$('adminPassword').value,tailscale_auth_key:$('tailscaleKey').value};try{let r;if(isCaptive&&setup){r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:setup,pswd:$('wifiPassword').value,server:$('serverUrl').value,...body})})}else{r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json',...authHeaders()},body:JSON.stringify(body)})}msg(await r.text());if(r.ok&&setup) setTimeout(()=>location.reload(),1500)}catch(e){msg('Save failed: '+e)}};
$('restart').onclick=async()=>{try{const r=await fetch('/api/restart',{method:'POST',headers:authHeaders()});msg(await r.text())}catch(e){msg('Restart failed: '+e)}};
load();setInterval(async()=>{try{$('status').textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,2)}catch(e){}},5000);
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
