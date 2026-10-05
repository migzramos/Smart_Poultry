#pragma once
#include <WebServer.h>
// Dashboard HTML stored in program memory saves RAM.
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Poultry Monitor</title>
<style>
  :root{
    --bg:#0f1117;--card:#1a1d27;--border:#2a2d3a;
    --accent:#f59e0b;--green:#22c55e;--red:#ef4444;
    --blue:#3b82f6;--text:#e2e8f0;--muted:#64748b;
    --warn-low:#f59e0b;--warn-high:#ef4444;
  }
  .light{
    --bg:#f0f4f8;--card:#ffffff;--border:#d1d5db;
    --text:#1e293b;--muted:#64748b;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh;padding:1rem}
  h1{text-align:center;font-size:1.5rem;font-weight:700;margin-bottom:1rem;color:var(--accent)}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:1rem;max-width:900px;margin:0 auto}
  .card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:1.25rem}
  .card h2{font-size:.85rem;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);margin-bottom:.75rem}
  .big-val{font-size:2.8rem;font-weight:800;line-height:1}
  .unit{font-size:1rem;font-weight:400;color:var(--muted)}
  .badge{display:inline-flex;align-items:center;gap:.4rem;padding:.3rem .75rem;border-radius:999px;font-size:.78rem;font-weight:600;margin:.25rem .2rem}
  .badge.on{background:#16532440;color:var(--green);border:1px solid var(--green)}
  .badge.off{background:#1e293b;color:var(--muted);border:1px solid var(--border)}
  .badge.warn{background:#7c2d1240;color:var(--red);border:1px solid var(--red)}
  .dot{width:8px;height:8px;border-radius:50%;background:currentColor}
  .feed-bar-bg{background:var(--border);border-radius:999px;height:18px;overflow:hidden;margin:.5rem 0}
  .feed-bar-fill{height:100%;border-radius:999px;background:linear-gradient(90deg,var(--accent),var(--green));transition:width .6s ease}
  .feed-pct{font-size:1.4rem;font-weight:700}
  .feed-warning{margin-top:.4rem;color:var(--warn-high);font-weight:700}
  .caption{font-size:.72rem;color:var(--muted);margin-top:.25rem}
  button{cursor:pointer;border:none;border-radius:8px;padding:.6rem 1.2rem;font-size:.9rem;font-weight:600;transition:opacity .15s,transform .1s}
  button:active{transform:scale(.97)}
  .btn-feed{background:var(--accent);color:#000;width:100%;margin-top:.75rem}
  .btn-feed:disabled{opacity:.55;cursor:not-allowed}
  .btn-theme{background:transparent;border:1px solid var(--border);color:var(--text);padding:.35rem .8rem;font-size:.8rem}
  .topbar{display:flex;justify-content:space-between;align-items:center;max-width:900px;margin:0 auto .75rem}
  .status-dot{width:10px;height:10px;border-radius:50%;display:inline-block;margin-right:6px}
  .ok{background:var(--green)}.err{background:var(--red)}.na{background:var(--muted)}
  fieldset{border:1px solid var(--border);border-radius:8px;padding:.75rem;margin-top:.5rem}
  legend{font-size:.8rem;color:var(--muted);padding:0 .4rem}
  label{display:block;font-size:.82rem;color:var(--muted);margin:.4rem 0 .15rem}
  input[type=number]{width:100%;background:var(--bg);border:1px solid var(--border);border-radius:6px;color:var(--text);padding:.4rem .6rem;font-size:.9rem}
  .btn-save{background:var(--blue);color:#fff;width:100%;margin-top:.6rem}
  .btn-cal{background:#374151;color:var(--text);flex:1;font-size:.82rem}
  .row{display:flex;gap:.5rem}
  .temp-color-ok{color:var(--green)}.temp-color-warn{color:var(--warn-low)}.temp-color-hot{color:var(--warn-high)}
  .fade{opacity:0;transition:opacity .4s}.fade.show{opacity:1}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
  .pulsing{animation:pulse 800ms infinite}
</style>
</head>
<body>
<div class="topbar">
  <span style="font-weight:700;color:var(--accent)">&#127413; SmartPoultry</span>
  <button class="btn-theme" onclick="toggleTheme()" id="themeBtn">&#9788; Light</button>
</div>
<h1>Poultry Monitoring &amp; Control</h1>
<div class="grid">
  <!-- Temperature & Humidity -->
  <div class="card">
    <h2>Temperature &amp; Humidity</h2>
    <div style="display:flex;gap:1.5rem;align-items:flex-end">
      <div>
        <div class="big-val" id="tempVal">--</div>
        <div class="unit">°C</div>
      </div>
      <div>
        <div style="font-size:1.8rem;font-weight:700" id="humVal">--</div>
        <div class="unit">% RH</div>
      </div>
    </div>
  </div>
  <!-- Feed Level -->
  <div class="card">
    <h2>Feed Level</h2>
    <div class="feed-pct"><span id="feedPct">--</span> %</div>
    <div class="caption">Estimated from hopper distance</div>
    <div class="feed-warning" id="feedWarning" role="status" hidden>Low feed: refill soon (25% or less)</div>
    <div class="feed-bar-bg"><div class="feed-bar-fill" id="feedBar" style="width:0%"></div></div>
    <button class="btn-feed" id="feedBtn" onclick="dispenseFeed()">&#9654; Dispense Feed Now</button>
  </div>
  <!-- Status Badges -->
  <div class="card">
    <h2>Actuator Command Status</h2>
    <div id="fanBadge"  class="badge off"><div class="dot"></div>Fan</div>
    <div id="heatBadge" class="badge off"><div class="dot"></div>Heat Lamp</div>
    <div id="buzzBadge" class="badge off"><div class="dot"></div>Buzzer</div>
    <div id="servoBadge" class="badge off">Servo: IDLE</div>
    <div class="caption">Shows ESP32 commands, not load feedback</div>
    <div style="margin-top:.75rem;font-size:.78rem;color:var(--muted)">
      <span class="status-dot na" id="connDot"></span><span id="connTxt">Connecting...</span>
    </div>
  </div>
  <!-- Settings -->
  <div class="card">
    <h2>Settings &amp; Calibration</h2>
    <fieldset>
      <legend>Thresholds (°C)</legend>
      <label>Fan ON above (°C)</label>
      <input type="number" id="cfgFan" min="-20" max="80" step="0.5">
      <label>Heat ON below (°C)</label>
      <input type="number" id="cfgHeat" min="-20" max="80" step="0.5">
      <label>Buzzer alarm at/below (%)</label>
      <input type="number" id="cfgLow" min="0" max="100">
      <div class="caption">Dashboard warning appears at 25%; buzzer default is 20%.</div>
      <label>Auto-feed interval (min, 0=off)</label>
      <input type="number" id="cfgAuto" min="0" max="1440">
      <button class="btn-save" onclick="saveConfig()">&#10003; Save Settings</button>
    </fieldset>
    <fieldset style="margin-top:.75rem">
      <legend>Calibrate Hopper</legend>
      <div class="row">
        <button class="btn-cal" onclick="calibrate('full')">&#9660; Set FULL</button>
        <button class="btn-cal" onclick="calibrate('empty')">&#9650; Set EMPTY</button>
      </div>
    </fieldset>
  </div>
</div>
<script>
(function(){
  // Poll server for current sensor data regularly.
  const $ = id => document.getElementById(id);
  let dark = true;
  function toggleTheme(){
    dark = !dark;
    document.body.className = dark ? '' : 'light';
    $('themeBtn').textContent = dark ? '\u2600 Light' : '\u263D Dark';
  }
  window.toggleTheme = toggleTheme;
  // Apply temperature color based on threshold values.
  function tempColor(t, fan, heat){
    if(t >= fan) return 'temp-color-hot';
    if(t <= heat) return 'temp-color-warn';
    return 'temp-color-ok';
  }
  // Update badge element with on/off/warn state.
  function badge(el, on, warn){
    el.className = 'badge ' + (on ? (warn ? 'warn' : 'on') : 'off');
  }
  let fanThr=31, heatThr=27;
  // Fetch fresh data and update all dashboard elements.
  function poll(){
    fetch('/api/data').then(r=>{
      if(!r.ok) throw new Error('data request failed');
      return r.json();
    }).then(d=>{
      const t = Number.isFinite(d.temp) ? d.temp : NaN;
      const h = Number.isFinite(d.hum) ? d.hum : NaN;
      const f = Number.isFinite(d.feed) ? d.feed : NaN;
      const tv = $('tempVal');
      tv.textContent = isNaN(t) ? '--' : t.toFixed(1);
      tv.className = 'big-val' + (isNaN(t) ? '' : ' ' + tempColor(t, fanThr, heatThr));
      $('humVal').textContent = isNaN(h) ? '--' : h.toFixed(1);
      const fp = Math.max(0, Math.min(100, isNaN(f)?0:f));
      $('feedPct').textContent = isNaN(f) ? '--' : fp.toFixed(0);
      $('feedBar').style.width = fp + '%';
      $('feedWarning').hidden = d.feedWarn !== true;
      badge($('fanBadge'),  d.fan,    false);
      badge($('heatBadge'), d.heat,   false);
      badge($('buzzBadge'), d.buzzer, true);
      const servoState = ['IDLE', 'OPEN', 'CLOSE'].includes(d.servo) ? d.servo : 'N/A';
      badge($('servoBadge'), servoState !== 'IDLE' && servoState !== 'N/A', false);
      $('servoBadge').textContent = 'Servo: ' + servoState;
      if(d.buzzer) $('buzzBadge').classList.add('pulsing');
      else         $('buzzBadge').classList.remove('pulsing');
      $('connDot').className='status-dot ok';
      $('connTxt').textContent='Connected';
    }).catch(()=>{
      $('connDot').className='status-dot err';
      $('connTxt').textContent='Disconnected; statuses stale';
      $('tempVal').textContent='--';
      $('tempVal').className='big-val';
      $('humVal').textContent='--';
      $('feedPct').textContent='--';
      $('feedBar').style.width='0%';
      $('feedWarning').hidden=true;
    });
  }
  // Load current config values into settings form fields.
  function loadConfig(){
    fetch('/api/config').then(r=>{
      if(!r.ok) throw new Error('config request failed');
      return r.json();
    }).then(d=>{
      $('cfgFan').value  = d.fan;
      $('cfgHeat').value = d.heat;
      $('cfgLow').value  = d.low;
      $('cfgAuto').value = d.auto;
      fanThr=d.fan; heatThr=d.heat;
    }).catch(()=>{});
  }
  // Post updated config values to server endpoint.
  window.saveConfig = function(){
    const body = new URLSearchParams({
      fan:  $('cfgFan').value,
      heat: $('cfgHeat').value,
      low:  $('cfgLow').value,
      auto: $('cfgAuto').value
    });
    fetch('/api/config',{method:'POST',body}).then(async r=>{
      const data = await r.json();
      if(!r.ok || !data.ok) throw new Error(data.error || 'Settings were rejected');
      return data;
    }).then(d=>{
      fanThr=d.fan; heatThr=d.heat;
      alert('Saved!');
    }).catch(error=>alert('Save failed: '+error.message));
  };
  // Trigger feed dispense and show loading state.
  window.dispenseFeed = function(){
    const btn = $('feedBtn');
    btn.disabled=true; btn.textContent='Dispensing...';
    fetch('/api/feed',{method:'POST'}).then(async r=>{
      const data = await r.json();
      if(!r.ok || !data.ok) throw new Error(data.error || 'Feed request failed');
    }).then(()=>{
      setTimeout(()=>{ btn.disabled=false; btn.textContent='\u25B6 Dispense Feed Now'; }, 2500);
    }).catch(error=>{
      btn.disabled=false; btn.textContent='\u25B6 Dispense Feed Now';
      alert('Dispense failed: '+error.message);
    });
  };
  // Send calibration point full or empty to server.
  window.calibrate = function(p){
    fetch('/api/calibrate?p='+encodeURIComponent(p),{method:'POST'}).then(r=>{
      if(!r.ok) throw new Error('calibration failed');
      return r.json();
    }).then(d=>{
      if(!d.ok) throw new Error('calibration failed');
      alert('Calibrated: '+p);
    }).catch(()=>alert('Calibration failed: check the sensor reading and hopper position.'));
  };
  poll(); loadConfig();
  setInterval(poll, 2000);
})();
</script>
</body>
</html>
)rawliteral";
