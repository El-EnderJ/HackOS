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

static constexpr const char *TAG_RD = "RemoteDash";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr uint8_t AP_CHANNEL      = 1U;
static constexpr uint8_t AP_MAX_CONN     = 4U;
static constexpr size_t HTTPD_MAX_URI    = 20U;
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

        ESP_LOGI(TAG_RD, "HTTP server started with %d endpoints", 16);
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

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Factory
// ═══════════════════════════════════════════════════════════════════════════════

AppBase *createRemoteDashboardApp()
{
    return new (std::nothrow) RemoteDashboardApp();
}
