// web_ui.ino — Web interface
// Tries home WiFi first; falls back to AP "Solo6C" / "audio1234" at 192.168.4.1

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#define WIFI_STA_SSID   "AP"           // home WiFi SSID  (leave empty to skip STA)
#define WIFI_STA_PASS   "PASS1234"     // home WiFi password
#define WIFI_AP_SSID    "Solo6C"
#define WIFI_AP_PASS    "audio1234"
#define WIFI_TIMEOUT_MS 10000          // ms to wait for STA before falling back
#define LED_STATUS_PIN  15
#define LED_BRIGHTNESS  10             // 15% of 255

static WebServer  _server(80);
static WiFiServer _sseServer(81);
static WiFiClient _sseClient;

// ── JSON state ────────────────────────────────────────────────────────────────
static String stateJSON() {
  uint8_t sl, sr;
  seg7GetPair(&sl, &sr);
  String j = "{";
  j += "\"vol\":"    + String(vol)    + ",";
  j += "\"bas\":"    + String(bas)    + ",";
  j += "\"treb\":"   + String(treb)   + ",";
  j += "\"ball\":"   + String(ball)   + ",";
  j += "\"in\":"     + String(in)     + ",";
  j += "\"stereo\":" + String(stereo) + ",";
  j += "\"mode\":"   + String(mode)   + ",";
  j += "\"gain1\":"  + String(gain1)  + ",";
  j += "\"gain2\":"  + String(gain2)  + ",";
  j += "\"gain3\":"  + String(gain3)  + ",";
  j += "\"gain4\":"  + String(gain4)  + ",";
  j += "\"gain5\":"  + String(gain5)  + ",";
  j += "\"power\":"  + String(power)  + ",";
  j += "\"mute\":"   + String(mute)   + ",";
  j += "\"seg_left\":"  + String(sl) + ",";
  j += "\"seg_right\":" + String(sr);
  j += "}";
  return j;
}

// ── /set handler ──────────────────────────────────────────────────────────────
static void handleSet() {
  bool changed = false;

  if (_server.hasArg("vol"))  { vol    = constrain(_server.arg("vol").toInt(),  12, 87); changed = true; setDisplay(PARAM_VOL); }
  if (_server.hasArg("bas"))  { bas    = constrain(_server.arg("bas").toInt(),  -7,  7); changed = true; setDisplay(PARAM_BAS); }
  if (_server.hasArg("treb")) { treb   = constrain(_server.arg("treb").toInt(), -7,  7); changed = true; setDisplay(PARAM_TREB); }
  if (_server.hasArg("ball")) { ball   = constrain(_server.arg("ball").toInt(), -6,  6); changed = true; setDisplay(PARAM_BALL); }
  if (_server.hasArg("stereo")) { stereo = constrain(_server.arg("stereo").toInt(), 0, 2); changed = true; setDisplay(PARAM_ST); }
  if (_server.hasArg("mode"))   { mode   = constrain(_server.arg("mode").toInt(),   0, 1); changed = true; setDisplay(PARAM_MODE); }

  if (_server.hasArg("in")) {
    byte newIn = constrain(_server.arg("in").toInt(), 2, 3);  // only inputs 2–3 wired
    if (power || mute) {
      // device is muted/standby — stage the input for when it comes back
      in_old = newIn;
    } else {
      in = newIn;
      switch (in) {
        case 2: gain0=gain3; break; case 3: gain0=gain4; break;
      }
      changed = true; setDisplay(PARAM_IN);
    }
  }

  auto applyGain = [&](int idx, int val) {
    val = constrain(val, 0, 6);
    switch (idx) {
      case 3: gain3=val; if(in==2) gain0=val; break;
      case 4: gain4=val; if(in==3) gain0=val; break;
    }
    changed = true; setDisplay(PARAM_GAIN);
  };
  for (int g = 1; g <= 5; g++) {
    String k = "gain" + String(g);
    if (_server.hasArg(k)) applyGain(g, _server.arg(k).toInt());
  }

  if (_server.hasArg("power")) {
    int req = _server.arg("power").toInt();
    if (req && !power) { power=1; if (in != 7) in_old=in; in=7; }
    else if (!req && power) {
      power=0; mute=0; in=in_old;
      switch(in){case 2:gain0=gain3;break;case 3:gain0=gain4;break;}
    }
    changed = true; updateDisplay();
  }

  if (_server.hasArg("mute")) {
    int req = _server.arg("mute").toInt();
    if (req && !mute) { mute=1; if (in != 7) in_old=in; in=7; }
    else if (!req && mute) { mute=0; in=in_old;
      switch(in){case 2:gain0=gain3;break;case 3:gain0=gain4;break;}
    }
    changed = true; updateDisplay();
  }

  if (changed) { times = millis(); w = 1; audio(); }
  _server.send(200, "application/json", stateJSON());
}

// ── HTML page (stored in flash) ───────────────────────────────────────────────
static const char WEB_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Solo6C</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0f0f1a;color:#cfd8dc;font-family:system-ui,sans-serif;padding:16px;max-width:600px}
h1{color:#00e676;font-size:.95rem;letter-spacing:3px;margin-bottom:14px}
.card{background:#1a1a2e;border-radius:8px;padding:12px;margin-bottom:10px}
.row{display:flex;flex-wrap:wrap;gap:10px;align-items:center}
lbl{font-size:.7rem;color:#546e7a;text-transform:uppercase;letter-spacing:1px;display:block;margin-bottom:6px}
.display{display:flex;gap:16px;justify-content:center;padding:14px 0}
svg.digit{height:100px;filter:drop-shadow(0 0 6px #ffaa0055)}
polygon,circle{fill:#ffaa00;opacity:.08;transition:opacity .08s}
.grp{display:flex;flex-wrap:wrap;gap:6px}
button{background:#0f0f1a;border:1px solid #546e7a;color:#cfd8dc;padding:5px 13px;
       border-radius:6px;cursor:pointer;font-size:.82rem;transition:.15s}
button:hover{border-color:#00e676}
button.on{background:#00e676;color:#000;border-color:#00e676;font-weight:600}
#bPwr.on{background:#f44336;border-color:#f44336;color:#fff}
#bMut.on{background:#ff9800;border-color:#ff9800;color:#000}
.sl-row{display:flex;align-items:center;gap:8px;margin-bottom:7px}
.sl-lbl{min-width:58px;font-size:.8rem}
input[type=range]{flex:1;accent-color:#00e676;cursor:pointer}
.sl-val{min-width:52px;text-align:right;color:#00e676;font-size:.82rem;font-variant-numeric:tabular-nums}
.gain-wrap{display:flex;gap:12px;align-items:flex-end;margin-top:4px}
.gcol{display:flex;flex-direction:column;align-items:center;gap:3px;opacity:.4;transition:.2s}
.gcol.cur{opacity:1}
.gcol lbl{color:#00e676;font-weight:600;font-size:.7rem}
input.vsl{writing-mode:vertical-lr;direction:rtl;height:72px;accent-color:#00e676;cursor:pointer}
.gv{font-size:.72rem;color:#00e676}
.dimmed{opacity:.35;pointer-events:none}
</style>
</head>
<body>
<h1>&#11042; SOLO6C</h1>

<div class="card">
  <lbl>Display</lbl>
  <div class="display">
    <svg class="digit" viewBox="0 0 54 94">
      <polygon id="d0A" points="6,2 48,2 52,6 48,10 6,10 2,6"/>
      <polygon id="d0B" points="52,6 52,47 44,43 44,10"/>
      <polygon id="d0C" points="52,47 52,88 44,84 44,51"/>
      <polygon id="d0D" points="6,84 48,84 52,88 48,92 6,92 2,88"/>
      <polygon id="d0E" points="2,47 10,51 10,84 2,88"/>
      <polygon id="d0F" points="2,6 10,10 10,43 2,47"/>
      <polygon id="d0G" points="6,43 48,43 52,47 48,51 6,51 2,47"/>
      <circle  id="d0P" cx="50" cy="91" r="3"/>
    </svg>
    <svg class="digit" viewBox="0 0 54 94">
      <polygon id="d1A" points="6,2 48,2 52,6 48,10 6,10 2,6"/>
      <polygon id="d1B" points="52,6 52,47 44,43 44,10"/>
      <polygon id="d1C" points="52,47 52,88 44,84 44,51"/>
      <polygon id="d1D" points="6,84 48,84 52,88 48,92 6,92 2,88"/>
      <polygon id="d1E" points="2,47 10,51 10,84 2,88"/>
      <polygon id="d1F" points="2,6 10,10 10,43 2,47"/>
      <polygon id="d1G" points="6,43 48,43 52,47 48,51 6,51 2,47"/>
      <circle  id="d1P" cx="50" cy="91" r="3"/>
    </svg>
  </div>
</div>

<div class="card row">
  <div class="grp">
    <button id="bPwr" onclick="tog('power')">POWER</button>
    <button id="bMut" onclick="tog('mute')">MUTE</button>
  </div>
  <div>
    <lbl>Input</lbl>
    <div class="grp" id="gIn">
      <button onclick="sp('in',2)">A1</button><button onclick="sp('in',3)">A2</button>
    </div>
  </div>
</div>

<div class="card" id="ctls">
  <div class="sl-row"><span class="sl-lbl">Volume</span>
    <input type="range" id="svol" min="12" max="87" oninput="lbl('vol',this.value)" onchange="sp('vol',this.value)">
    <span class="sl-val" id="lvol">0 dB</span></div>
  <div class="sl-row"><span class="sl-lbl">Bass</span>
    <input type="range" id="sbas" min="-7" max="7" oninput="lbl('bas',this.value)" onchange="sp('bas',this.value)">
    <span class="sl-val" id="lbas">0 dB</span></div>
  <div class="sl-row"><span class="sl-lbl">Treble</span>
    <input type="range" id="streb" min="-7" max="7" oninput="lbl('treb',this.value)" onchange="sp('treb',this.value)">
    <span class="sl-val" id="ltreb">0 dB</span></div>
  <div class="sl-row"><span class="sl-lbl">Balance</span>
    <input type="range" id="sball" min="-6" max="6" oninput="lbl('ball',this.value)" onchange="sp('ball',this.value)">
    <span class="sl-val" id="lball">C</span></div>
  <lbl style="margin-top:10px">Input Gain (0-20 dB per channel)</lbl>
  <div class="gain-wrap" id="gGain"></div>
</div>

<div class="card">
  <lbl>Stereo / Mono</lbl>
  <div class="grp" id="gSt">
    <button onclick="sp('stereo',0)">STEREO</button>
    <button onclick="sp('stereo',1)">Lch Mono</button>
    <button onclick="sp('stereo',2)">Rch Mono</button>
  </div>
</div>
<div class="card">
  <lbl>Mode</lbl>
  <div class="grp" id="gMd">
    <button onclick="sp('mode',0)">BYPASS</button>
    <button onclick="sp('mode',1)">TONE</button>
  </div>
</div>

<div class="card">
  <lbl>Firmware update</lbl>
  <form id="fOta" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:4px">
    <input type="file" id="fwFile" accept=".bin" style="color:#cfd8dc;font-size:.8rem;flex:1">
    <button type="button" onclick="otaUpload()">Upload</button>
    <span id="otaSt" style="font-size:.78rem;color:#546e7a"></span>
  </form>
</div>

<script>
const SEGS = ['A','B','C','D','E','F','G','P'];
let st = {}, drag = false;

function dig(pfx, val) {
  SEGS.forEach((s,i) => {
    const el = document.getElementById(pfx+s);
    if (el) el.style.opacity = ((val >> i) & 1) ? '1' : '0.08';
  });
}

function lbl(p, v) {
  v = parseInt(v);
  const fmts = {
    vol:  () => (v-87) + ' dB',
    bas:  () => (v>=0?'+':'') + v*2 + ' dB',
    treb: () => (v>=0?'+':'') + v*2 + ' dB',
    ball: () => v===0 ? 'C' : (v>0 ? 'R+'+v : 'L+'+Math.abs(v))
  };
  const el = document.getElementById('l'+p);
  if (el && fmts[p]) el.textContent = fmts[p]();
}

function upd(s) {
  st = s;
  dig('d0', s.seg_left);
  dig('d1', s.seg_right);

  document.getElementById('bPwr').classList.toggle('on', !!s.power);
  document.getElementById('bMut').classList.toggle('on', !!s.mute);
  document.getElementById('ctls').classList.toggle('dimmed', !!s.power);

  document.querySelectorAll('#gIn button').forEach((b,i) => b.classList.toggle('on', i+2===s.in));

  if (!drag) {
    ['vol','bas','treb','ball'].forEach(p => {
      const el = document.getElementById('s'+p);
      if (el) el.value = s[p];
    });
  }
  ['vol','bas','treb','ball'].forEach(p => lbl(p, s[p]));

  const gg = document.getElementById('gGain');
  if (!gg.children.length) {
    for (let i = 0; i < 2; i++) {
      const d = document.createElement('div');
      d.className = 'gcol'; d.id = 'gc'+i;
      d.innerHTML = '<lbl>A'+(i+1)+'</lbl>' +
        '<input class="vsl" type="range" orient="vertical" min="0" max="6" value="0"' +
        ' oninput="glbl('+i+',this.value)" onchange="sg('+(i+2)+',this.value)">' +
        '<span class="gv" id="gv'+i+'">0 dB</span>';
      gg.appendChild(d);
    }
  }
  [s.gain3,s.gain4].forEach((g,i) => {
    document.getElementById('gc'+i).classList.toggle('cur', i+2===s.in);
    if (!drag) gg.children[i].querySelector('input').value = g;
    document.getElementById('gv'+i).textContent = (g*2)+' dB';
  });

  document.querySelectorAll('#gSt button').forEach((b,i) => b.classList.toggle('on', i===s.stereo));
  document.querySelectorAll('#gMd button').forEach((b,i) => b.classList.toggle('on', i===s.mode));
}

function glbl(i, v) { document.getElementById('gv'+i).textContent = (parseInt(v)*2)+' dB'; }
function sg(i, v)   { fetch('/set?gain'+(i+1)+'='+v).then(r=>r.json()).then(upd).catch(console.error); }
function sp(p, v)   { drag=false; fetch('/set?'+p+'='+v).then(r=>r.json()).then(upd).catch(console.error); }
function tog(p)     { sp(p, st[p] ? 0 : 1); }

document.addEventListener('pointerdown', e => { if (e.target.type==='range') drag=true; });
document.addEventListener('pointerup',   () => drag=false);

const _wg = {gIn:{p:'in',min:2,max:3}, gSt:{p:'stereo',min:0,max:2}, gMd:{p:'mode',min:0,max:1}};
document.addEventListener('wheel', e => {
  const dir = e.deltaY < 0 ? 1 : -1;
  const el  = e.target;
  if (el.type === 'range') {
    e.preventDefault();
    const nv = Math.min(parseInt(el.max), Math.max(parseInt(el.min), parseInt(el.value) + dir));
    if (nv !== parseInt(el.value)) {
      el.value = nv;
      el.dispatchEvent(new Event('input'));
      el.dispatchEvent(new Event('change'));
    }
    return;
  }
  const grp = el.closest('.grp');
  if (grp && _wg[grp.id]) {
    e.preventDefault();
    const c = _wg[grp.id];
    const nv = Math.min(c.max, Math.max(c.min, st[c.p] + dir));
    if (nv !== st[c.p]) sp(c.p, nv);
    return;
  }
  if (el.closest('.display')) {
    e.preventDefault();
    const nv = Math.min(87, Math.max(12, st.vol + dir));
    if (nv !== st.vol) sp('vol', nv);
  }
}, {passive: false});

fetch('/state').then(r=>r.json()).then(upd);
const evtSrc = new EventSource('http://' + location.hostname + ':81/');
evtSrc.onmessage = e => { try { upd(JSON.parse(e.data)); } catch(_) {} };

function otaUpload() {
  const f = document.getElementById('fwFile').files[0];
  if (!f) return;
  const st = document.getElementById('otaSt');
  const fd = new FormData();
  fd.append('firmware', f);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.upload.onprogress = e => {
    if (e.lengthComputable) st.textContent = Math.round(e.loaded/e.total*100) + '%';
  };
  xhr.onload = () => {
    st.style.color = xhr.responseText === 'OK' ? '#00e676' : '#f44336';
    st.textContent = xhr.responseText === 'OK' ? 'Done — rebooting…' : 'Error: ' + xhr.responseText;
  };
  xhr.onerror = () => { st.style.color='#f44336'; st.textContent='Upload failed'; };
  st.style.color = '#cfd8dc'; st.textContent = '0%';
  xhr.send(fd);
}
</script>
</body>
</html>
)html";

// ── Endpoint handlers ─────────────────────────────────────────────────────────
static void handleRoot()  { _server.send_P(200, "text/html", WEB_HTML); }
static void handleState() { _server.send(200, "application/json", stateJSON()); }

static void handleOtaDone() {
  bool ok = !Update.hasError();
  _server.send(200, "text/plain", ok ? "OK" : Update.errorString());
  if (ok) { delay(200); ESP.restart(); }
}
static void handleOtaUpload() {
  HTTPUpload& u = _server.upload();
  if (u.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] start: %s\n", u.filename.c_str());
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    Update.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    Update.end(true);
    Serial.printf("[OTA] done: %u bytes, err=%d\n", u.totalSize, Update.hasError());
  }
}

// ── Public API ────────────────────────────────────────────────────────────────
static void updateLED() {
  static uint32_t _ledMs = 0;
  if (millis() - _ledMs < 1000) return;
  _ledMs = millis();
  bool connected = (WiFi.getMode() == WIFI_STA)
                   ? (WiFi.status() == WL_CONNECTED)
                   : (WiFi.softAPgetStationNum() > 0);
  // neopixelWrite(pin, green, blue, red) — arduino-esp32 built-in, no library needed
  if (connected) neopixelWrite(LED_STATUS_PIN, 0, LED_BRIGHTNESS, 0);
  else           neopixelWrite(LED_STATUS_PIN, 0, 0, LED_BRIGHTNESS);
}

void setupWebUI() {
  Serial.println(F("[W1] LED"));
  neopixelWrite(LED_STATUS_PIN, 0, 0, LED_BRIGHTNESS);  // red — no connection yet
  Serial.println(F("[W2] WiFi begin"));

  bool staOk = false;
  if (strlen(WIFI_STA_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) {
      delay(100);
    }
    staOk = (WiFi.status() == WL_CONNECTED);
  }
  Serial.println(staOk ? F("[W3] STA connected") : F("[W3] STA failed -> AP"));

  if (staOk) {
    Serial.print(F("Web UI (STA): http://"));
    Serial.println(WiFi.localIP());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    Serial.print(F("Web UI (AP): http://"));
    Serial.println(WiFi.softAPIP());
  }

  _server.on("/",        handleRoot);
  _server.on("/state",   handleState);
  _server.on("/set",     handleSet);
  _server.on("/update",  HTTP_POST, handleOtaDone, handleOtaUpload);
  _server.begin();
  _sseServer.begin();
  Serial.println(F("[W4] SSE on port 81"));
}

void handleWebUI() {
  _server.handleClient();

  // Accept a new SSE client (only one at a time)
  if (_sseServer.hasClient()) {
    if (_sseClient) _sseClient.stop();
    _sseClient = _sseServer.accept();
    // Drain HTTP request headers until blank line or 500 ms timeout
    String buf;
    bool done = false;
    uint32_t t = millis();
    while (!done && millis() - t < 500 && _sseClient.connected()) {
      while (!done && _sseClient.available()) {
        buf += (char)_sseClient.read();
        if (buf.endsWith("\r\n\r\n")) done = true;
      }
    }
    _sseClient.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "\r\n");
    _sseClient.print("data: " + stateJSON() + "\r\n\r\n");
  }

  // Push state to SSE client whenever something changed
  if (webDirty) {
    webDirty = false;
    if (_sseClient && _sseClient.connected())
      _sseClient.print("data: " + stateJSON() + "\r\n\r\n");
  }

  updateLED();
}
