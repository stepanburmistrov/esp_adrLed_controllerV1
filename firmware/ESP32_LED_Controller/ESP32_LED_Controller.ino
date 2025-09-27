/****************************************************
 * ESP32 LED Controller (WS2812B + FastLED)
 * - Web UI: HSV color wheel (canvas) + brightness + ON/OFF
 * - HTTP GET API
 * - Telegram Bot in separate FreeRTOS task (no UI lag)
 * - Inline menu (/menu) + inline palette; setMyCommands (/setup_menu)
 * - NVS (WiFi/mDNS/LED count/brightness/power/token/chatIDs/lastColor)
 * - UART CLI (SET/GET/DEL/CHAT) without reflashing
 * Safe init order: start services only after WiFi got IP
 * Tested with ESP32 Arduino Core 2.x (WiFi events ARDUINO_EVENT_*)
 ****************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>            // <-- for setMyCommands
#include <FastLED.h>
#include <UniversalTelegramBot.h>  // requires ArduinoJson v6

// =================== USER DEFAULTS ===================
#define LED_PIN            5          // Data pin for WS2812B
#define DEFAULT_LED_COUNT  1          // Default if not set via UART
#define COLOR_ORDER        GRB
#define LED_TYPE           WS2812B
#define SERIAL_BAUD        115200
#define MAX_BRIGHTNESS     255

const unsigned long NET_STATUS_INTERVAL = 5000;    // 5s

// =================== GLOBALS ===================
Preferences prefs;                     // NVS namespace "cfg"
WebServer server(80);
WiFiClientSecure secured_client;       // TLS for Telegram
UniversalTelegramBot* bot = nullptr;

String wifi_ssid, wifi_pass, hostname, bot_token;
String chat_ids_csv;                   // CSV: "123,456"
uint16_t led_count = DEFAULT_LED_COUNT;
uint8_t brightness = 128;
bool powerOn = true;                   // ON/OFF
uint32_t lastColor = 0xFFFFFF;         // 0xRRGGBB

CRGB* leds = nullptr;

unsigned long lastNetStatus = 0;

volatile bool wifiConnected = false;
bool servicesStarted = false;

// Telegram task
TaskHandle_t tgTaskHandle = nullptr;

// =================== UTILS ===================
static uint8_t hexToByte(const char* p) {
  char buf[3] = {p[0], p[1], '\0'};
  return (uint8_t) strtoul(buf, nullptr, 16);
}

static void colorFromHex(const String& hex, uint8_t &r, uint8_t &g, uint8_t &b) {
  String h = hex; if (h.startsWith("#")) h.remove(0,1);
  if (h.length() < 6) { r=g=b=0; return; }
  r = hexToByte(h.substring(0,2).c_str());
  g = hexToByte(h.substring(2,4).c_str());
  b = hexToByte(h.substring(4,6).c_str());
}

void setColorHex(const String& hex){
  uint8_t r,g,b; colorFromHex(hex, r, g, b);
  // setColorRGB объявлена ниже
  extern void setColorRGB(uint8_t r, uint8_t g, uint8_t b);
  setColorRGB(r,g,b);
}

static String ipToStr(IPAddress ip){
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buf);
}

const char* wlStr(wl_status_t s){
  switch(s){
    case WL_IDLE_STATUS:    return "IDLE";
    case WL_NO_SSID_AVAIL:  return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED:      return "CONNECTED";
    case WL_CONNECT_FAILED: return "FAILED";
    case WL_CONNECTION_LOST:return "LOST";
    case WL_DISCONNECTED:   return "DISCONNECTED";
    default:                return "UNKNOWN";
  }
}

bool isChatAllowed(const String& chat_id){
  if (chat_ids_csv.length() == 0) return true; // public
  String csv = "," + chat_ids_csv + ",";
  String key = "," + chat_id + ",";
  return csv.indexOf(key) >= 0;
}

// =================== NVS ===================
void loadPrefs(){
  prefs.begin("cfg", true);
  wifi_ssid    = prefs.getString("wifi_ssid", "");
  wifi_pass    = prefs.getString("wifi_pass", "");
  hostname     = prefs.getString("hostname", "ledcontroller");
  bot_token    = prefs.getString("bot_token", "");
  chat_ids_csv = prefs.getString("chat_ids", "");
  led_count    = prefs.getUShort("led_count", DEFAULT_LED_COUNT);
  brightness   = prefs.getUChar("brightness", 128);
  powerOn      = prefs.getBool("power", true);
  lastColor    = prefs.getUInt("last_color", 0xFFFFFF);
  prefs.end();
}
void savePref(const char* key, const String& val){ prefs.begin("cfg", false); prefs.putString(key, val); prefs.end(); }
void savePrefUShort(const char* key, uint16_t v){  prefs.begin("cfg", false); prefs.putUShort(key, v); prefs.end(); }
void savePrefUChar(const char* key, uint8_t v){    prefs.begin("cfg", false); prefs.putUChar(key, v); prefs.end(); }
void savePrefBool(const char* key, bool v){        prefs.begin("cfg", false); prefs.putBool(key, v); prefs.end(); }
void savePrefUInt(const char* key, uint32_t v){    prefs.begin("cfg", false); prefs.putUInt(key, v); prefs.end(); }

// =================== FastLED ===================
void applyLeds(){
  uint8_t r = (lastColor >> 16) & 0xFF;
  uint8_t g = (lastColor >> 8)  & 0xFF;
  uint8_t b = (lastColor)       & 0xFF;
  if (!leds) return;
  if (powerOn) fill_solid(leds, led_count, CRGB(r,g,b));
  else         fill_solid(leds, led_count, CRGB::Black);
  FastLED.setBrightness(min<uint8_t>(brightness, MAX_BRIGHTNESS));
  FastLED.show();
}

bool initLeds(){
  if (leds){ delete[] leds; leds = nullptr; }
  if (led_count == 0) led_count = 1;
  leds = new(std::nothrow) CRGB[led_count];
  if (!leds) return false;
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, led_count);
  applyLeds();
  return true;
}

// =================== Wi-Fi/mDNS (Core 2.x events) ===================
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnected = false;
      servicesStarted = false;   // restart services after reconnect
      WiFi.reconnect();
      break;
    default:
      break;
  }
}

void startMDNS(){
  if (hostname.length()==0) hostname = "ledcontroller";
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(hostname.c_str())){
      MDNS.addService("http", "tcp", 80);
    }
  }
}

void connectWiFi(){
  if (wifi_ssid.isEmpty()) return; // not configured
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
}

// =================== WEB (HSV wheel) ===================
const char* INDEX_HTML = R"HTML(
<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>LED Controller</title>
<style>
  body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;padding:16px;background:#0b0f14;color:#e6edf3}
  .card{max-width:720px;margin:0 auto;background:#111827;border-radius:16px;padding:20px;box-shadow:0 10px 30px rgba(0,0,0,.35)}
  h1{font-size:22px;margin:0 0 8px}
  .row{display:flex;gap:16px;flex-wrap:wrap}
  .col{flex:1 1 260px}
  button{border:0;border-radius:12px;padding:12px 16px;font-weight:600;cursor:pointer}
  .on{background:#10b981;color:#032519}
  .off{background:#ef4444;color:#2d0b0b}
  .panel{background:#0f172a;border-radius:12px;padding:12px}
  input[type="range"]{width:100%}
  .muted{color:#9ca3af;font-size:12px}
  #wheel{width:100%;max-width:240px;height:auto;border-radius:12px;touch-action:none}
</style>
</head>
<body>
<div class="card">
  <h1>ESP32 LED Controller</h1>
  <p class="muted">mDNS: <span id="host">—</span> · IP: <span id="ip">—</span></p>
  <div class="row">
    <div class="col">
      <div class="panel">
        <button id="toggle" class="on">Включить</button>
      </div>
      <div class="panel">
        <label>Цвет (колесо)</label>
        <canvas id="wheel" width="240" height="240"></canvas>
      </div>
    </div>
    <div class="col">
      <div class="panel">
        <label>Яркость: <span id="bval">128</span></label>
        <input id="bright" type="range" min="0" max="255" value="128" />
      </div>
      <div class="panel">
        <pre id="state" style="white-space:pre-wrap"></pre>
      </div>
    </div>
  </div>
</div>
<script>
async function api(path){ const r = await fetch(path); return r.ok ? r.json() : null; }
function hex2(n){ return n.toString(16).padStart(2,'0'); }

async function refresh(){
  const s = await api('/api/state');
  if(!s) return;
  document.getElementById('host').textContent = s.hostname + '.local';
  document.getElementById('ip').textContent = s.ip;
  document.getElementById('toggle').textContent = s.power? 'Выключить' : 'Включить';
  document.getElementById('toggle').className = s.power? 'off' : 'on';
  document.getElementById('bright').value = s.brightness;
  document.getElementById('bval').textContent = s.brightness;
  document.getElementById('state').textContent = JSON.stringify(s, null, 2);
}

const t = document.getElementById('toggle');
t.addEventListener('click', async ()=>{ await fetch('/api/toggle'); refresh(); });

// ======== HSV wheel on canvas ========
const wheel = document.getElementById('wheel');
const ctx = wheel.getContext('2d');
const W = wheel.width, H = wheel.height, R = 110, rInner = 0;

function hsv2rgb(h,s,v){
  let f = (n,k=(n+h/60)%6)=> v - v*s*Math.max(Math.min(k,4-k,1),0);
  return { r:Math.round(f(5)*255), g:Math.round(f(3)*255), b:Math.round(f(1)*255) };
}
function drawWheel(){
  const cx = W/2, cy = H/2;
  const img = ctx.createImageData(W,H);
  for (let y=0;y<H;y++){
    for (let x=0;x<W;x++){
      const dx = x-cx, dy = y-cy;
      const dist = Math.hypot(dx,dy);
      const i = (y*W + x)*4;
      if (dist<=R && dist>=rInner){
        let hue = Math.atan2(dy,dx)*180/Math.PI; if (hue<0) hue+=360;
        const sat = dist/R; const val = 1.0;
        const {r,g,b} = hsv2rgb(hue, sat, val);
        img.data[i]=r; img.data[i+1]=g; img.data[i+2]=b; img.data[i+3]=255;
      } else {
        img.data[i]=17; img.data[i+1]=24; img.data[i+2]=39; img.data[i+3]=255;
      }
    }
  }
  ctx.putImageData(img,0,0);
}
drawWheel();

let sendDebounce;
function sendColorAt(px,py){
  const rect = wheel.getBoundingClientRect();
  const scaleX = W / rect.width;
  const scaleY = H / rect.height;
  const x = (px - rect.left) * scaleX;
  const y = (py - rect.top)  * scaleY;

  const cx = W/2, cy = H/2;
  const dx = x-cx, dy = y-cy;
  let dist = Math.hypot(dx,dy);
  if (dist>R) dist=R; if (dist<rInner) dist=rInner;

  let hue = Math.atan2(dy,dx)*180/Math.PI; if (hue<0) hue+=360;
  const sat = dist/R; const val = 1.0;
  const {r,g,b} = hsv2rgb(hue, sat, val);
  const hex = hex2(r)+hex2(g)+hex2(b);

  clearTimeout(sendDebounce);
  sendDebounce = setTimeout(()=>{ fetch('/api/color?hex='+hex); }, 20);
}

let dragging=false;
function onPointer(e){
  if(!dragging) return;
  const p = (e.touches && e.touches[0]) || e;
  sendColorAt(p.clientX, p.clientY);
}
wheel.addEventListener('pointerdown', e=>{ dragging=true; onPointer(e); });
wheel.addEventListener('pointermove', onPointer);
window.addEventListener('pointerup', ()=> dragging=false);
wheel.addEventListener('touchstart', e=>{ dragging=true; onPointer(e); }, {passive:true});
wheel.addEventListener('touchmove', onPointer, {passive:true});
window.addEventListener('touchend', ()=> dragging=false);

// Brightness
const b = document.getElementById('bright');
let brightDebounce;
b.addEventListener('input', ()=>{
  document.getElementById('bval').textContent = b.value;
  clearTimeout(brightDebounce);
  brightDebounce = setTimeout(()=>{ fetch('/api/brightness?value='+b.value); }, 30);
});

refresh();
setInterval(refresh, 1500);
</script>
</body>
</html>
)HTML";

void handleIndex(){ server.send(200, "text/html", INDEX_HTML); }

void handleState(){
  String json = "{";
  json += "\"hostname\":\"" + hostname + "\",";
  json += "\"ip\":\"" + ipToStr(WiFi.localIP()) + "\",";
  json += String("\"power\":") + (powerOn?"true":"false") + ",";
  json += String("\"brightness\":") + brightness + ",";
  uint8_t r=(lastColor>>16)&0xFF, g=(lastColor>>8)&0xFF, b=lastColor&0xFF;
  json += "\"color\":{\"r\":" + String(r) + ",\"g\":" + String(g) + ",\"b\":" + String(b) + "}}";
  server.send(200, "application/json", json);
}

void handleToggle(){ powerOn = !powerOn; savePrefBool("power", powerOn); applyLeds(); handleState(); }
void handleOn(){ powerOn = true; savePrefBool("power", powerOn); applyLeds(); server.send(200, "text/plain", "OK"); }
void handleOff(){ powerOn = false; savePrefBool("power", powerOn); applyLeds(); server.send(200, "text/plain", "OK"); }

void setColorRGB(uint8_t r, uint8_t g, uint8_t b){ lastColor = ((uint32_t)r<<16) | ((uint32_t)g<<8) | b; savePrefUInt("last_color", lastColor); applyLeds(); }
void setBrightness(uint8_t v){ brightness = v; savePrefUChar("brightness", brightness); applyLeds(); }

void handleApiColor(){
  if (server.hasArg("hex")){
    String h = server.arg("hex"); uint8_t r,g,b; colorFromHex(h,r,g,b); setColorRGB(r,g,b);
    server.send(200, "text/plain", "OK"); return;
  }
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")){
    uint8_t r = (uint8_t) server.arg("r").toInt();
    uint8_t g = (uint8_t) server.arg("g").toInt();
    uint8_t b = (uint8_t) server.arg("b").toInt();
    setColorRGB(r,g,b); server.send(200, "text/plain", "OK"); return;
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleApiBrightness(){
  if (!server.hasArg("value")){ server.send(400, "text/plain", "Bad Request"); return; }
  int v = server.arg("value").toInt();
  v = constrain(v, 0, 255);
  setBrightness((uint8_t)v);
  server.send(200, "text/plain", "OK");
}

void handleSetCombo(){
  if (server.hasArg("on")){ powerOn = server.arg("on").toInt() != 0; savePrefBool("power", powerOn); }
  if (server.hasArg("brightness")){ int v = constrain(server.arg("brightness").toInt(),0,255); setBrightness((uint8_t)v); }
  if (server.hasArg("color")){ String h = server.arg("color"); uint8_t r,g,b; colorFromHex(h,r,g,b); setColorRGB(r,g,b); }
  applyLeds();
  server.send(200, "text/plain", "OK");
}

void setupWeb(){
  server.on("/", handleIndex);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/toggle", HTTP_GET, handleToggle);
  server.on("/api/color", HTTP_GET, handleApiColor);
  server.on("/api/brightness", HTTP_GET, handleApiBrightness);
  server.on("/set", HTTP_GET, handleSetCombo);
  server.on("/on", HTTP_GET, handleOn);
  server.on("/off", HTTP_GET, handleOff);
  server.begin();
}

// =================== Telegram: inline menu + palette + setMyCommands ===================

// setMyCommands: системное меню команд Telegram
bool tgSetCommands(){
  if (bot_token.isEmpty()) return false;
  String url = "https://api.telegram.org/bot" + bot_token + "/setMyCommands";
  String body =
    "{\"commands\":["
      "{\"command\":\"start\",\"description\":\"Старт / помощь\"},"
      "{\"command\":\"menu\",\"description\":\"Открыть меню\"},"
      "{\"command\":\"palette\",\"description\":\"Палитра цветов\"},"
      "{\"command\":\"on\",\"description\":\"Включить ленту\"},"
      "{\"command\":\"off\",\"description\":\"Выключить ленту\"},"
      "{\"command\":\"state\",\"description\":\"Показать состояние\"},"
      "{\"command\":\"bright\",\"description\":\"Яркость 0..255\"}"
    "]}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return (code > 0);
}

// Inline главное меню
void sendInlineMainMenu(const String& chat_id){
  String kb =
    "["
      "[{\"text\":\"▶ Включить\",\"callback_data\":\"a:on\"},"
       "{\"text\":\"⏹ Выключить\",\"callback_data\":\"a:off\"}],"
      "[{\"text\":\"⬆ Ярче\",\"callback_data\":\"a:b+\"},"
       "{\"text\":\"⬇ Тусклее\",\"callback_data\":\"a:b-\"}],"
      "[{\"text\":\"🌈 Палитра\",\"callback_data\":\"a:pal\"},"
       "{\"text\":\"ℹ Состояние\",\"callback_data\":\"a:state\"}]"
    "]";
  bot->sendMessageWithInlineKeyboard(chat_id, "Меню управления", "", kb);
}

// Inline палитра (2 страницы, по 8 цветов)
void sendInlinePalette(const String& chat_id, uint8_t page = 0){
  static const char* cols[][8] = {
    { "#FF0000","#00FF00","#0000FF","#FFFF00","#FF00FF","#00FFFF","#FFFFFF","#000000" },
    { "#FF8000","#80FF00","#00FF80","#0080FF","#8000FF","#FF0080","#808080","#202020" }
  };
  if (page > 1) page = 0;

  String rows = "[";
  for (int i = 0; i < 2; i++) {
    rows += "[";
    for (int j = 0; j < 4; j++) {
      const char* hex = cols[page][i*4 + j];
      rows += "{\"text\":\"";
      rows += hex;
      rows += "\",\"callback_data\":\"c:";
      rows += hex;
      rows += "\"}";
      if (j < 3) rows += ",";
    }
    rows += "]";
    if (i < 1) rows += ",";
  }
  rows += ",[{\"text\":\"◀\",\"callback_data\":\"p:prev\"},"
          "{\"text\":\"Закрыть\",\"callback_data\":\"p:close\"},"
          "{\"text\":\"▶\",\"callback_data\":\"p:next\"}]]";

  bot->sendMessageWithInlineKeyboard(
    chat_id,
    "Выбери цвет (стр. " + String(page + 1) + ")",
    "",
    rows
  );
}

void handleTgMessage(size_t idx){
  if (!bot) return;
  auto &msg = bot->messages[idx];

  // 1) Callback (inline)
  if (msg.type == "callback_query") {
    String cqid = msg.query_id;
    String data = msg.text;       // callback_data
    String from_chat = msg.chat_id;

    // Главное меню
    if (data == "a:on"){
      powerOn = true; savePrefBool("power", powerOn); applyLeds();
      bot->answerCallbackQuery(cqid, "Включено");
      bot->sendMessage(from_chat, "LED: ON", "");
      return;
    }
    if (data == "a:off"){
      powerOn = false; savePrefBool("power", powerOn); applyLeds();
      bot->answerCallbackQuery(cqid, "Выключено");
      bot->sendMessage(from_chat, "LED: OFF", "");
      return;
    }
    if (data == "a:b+"){
      int v = min<int>(brightness + 16, 255); setBrightness((uint8_t)v);
      bot->answerCallbackQuery(cqid, "Яркость: " + String(v));
      return;
    }
    if (data == "a:b-"){
      int v = max<int>(brightness - 16, 0); setBrightness((uint8_t)v);
      bot->answerCallbackQuery(cqid, "Яркость: " + String(v));
      return;
    }
    if (data == "a:pal"){
      bot->answerCallbackQuery(cqid, "Палитра");
      sendInlinePalette(from_chat, 0);
      return;
    }
    if (data == "a:state"){
      bot->answerCallbackQuery(cqid, "Состояние");
      char buf[128];
      snprintf(buf, sizeof(buf), "Power:%s\nBrightness:%u\nColor:#%06X",
              powerOn?"ON":"OFF", brightness, lastColor);
      bot->sendMessage(from_chat, buf, "");
      return;
    }

    // Палитра
    if (data.startsWith("c:")) {
      String hex = data.substring(2);
      setColorHex(hex);
      bot->answerCallbackQuery(cqid, "Цвет " + hex, false);
      return;
    } else if (data == "p:close") {
      bot->answerCallbackQuery(cqid, "Закрыто", false);
      bot->sendMessage(from_chat, "Палитра закрыта", "");
      return;
    } else if (data == "p:next") {
      bot->answerCallbackQuery(cqid, "Следующая", false);
      sendInlinePalette(from_chat, 1);
      return;
    } else if (data == "p:prev") {
      bot->answerCallbackQuery(cqid, "Предыдущая", false);
      sendInlinePalette(from_chat, 0);
      return;
    }
    return;
  }

  // 2) Обычный текст
  String chat_id = msg.chat_id;
  if (!isChatAllowed(chat_id)) return;

  String text = msg.text; text.trim();

  if (text.equalsIgnoreCase("/start")){
    bot->sendMessage(chat_id, "Привет! Доступные команды: /menu, /palette, /on, /off, /bright 0..255, /state, /setup_menu", "");
    return;
  }
  if (text.equalsIgnoreCase("/menu")){
    sendInlineMainMenu(chat_id);
    return;
  }
  if (text.equalsIgnoreCase("/palette")){
    sendInlinePalette(chat_id, 0);
    return;
  }
  if (text.equalsIgnoreCase("/setup_menu")){
    bool ok = tgSetCommands();
    bot->sendMessage(chat_id, ok ? "Меню команд обновлено ✅" : "Не удалось обновить меню ❌", "");
    return;
  }

  if (text.equalsIgnoreCase("/on")){
    powerOn = true; savePrefBool("power", powerOn); applyLeds();
    bot->sendMessage(chat_id, "LED: ON", "");
  } else if (text.equalsIgnoreCase("/off")){
    powerOn = false; savePrefBool("power", powerOn); applyLeds();
    bot->sendMessage(chat_id, "LED: OFF", "");
  } else if (text.startsWith("/color")){
    int sp = text.indexOf(' ');
    if (sp>0){ String arg = text.substring(sp+1); arg.trim();
      if (arg.startsWith("#")){
        setColorHex(arg);
      } else {
        int r,g,b; sscanf(arg.c_str(), "%d %d %d", &r, &g, &b);
        setColorRGB((uint8_t)r,(uint8_t)g,(uint8_t)b);
      }
      bot->sendMessage(chat_id, "Color updated", "");
    }
  } else if (text.startsWith("/bright")){
    int v = text.substring(text.indexOf(' ')+1).toInt();
    v = constrain(v,0,255); setBrightness((uint8_t)v);
    bot->sendMessage(chat_id, String("Brightness ")+v, "");
  } else if (text.equalsIgnoreCase("/state")){
    char buf[128];
    snprintf(buf, sizeof(buf), "Power:%s\nBrightness:%u\nColor:#%06X",
             powerOn?"ON":"OFF", brightness, lastColor);
    bot->sendMessage(chat_id, buf, "");
  } else {
    bot->sendMessage(chat_id,
      "Команды: /menu, /palette, /on, /off, /color R G B | #RRGGBB, /bright 0..255, /state, /setup_menu", "");
  }
}

void tgTask(void *){
  for(;;){
    if (wifiConnected && bot){
      int n = bot->getUpdates(bot->last_message_received + 1);
      for (int i=0; i<n; i++) handleTgMessage((size_t)i);
    }
    vTaskDelay(pdMS_TO_TICKS(1200)); // ~1.2 s
  }
}

void setupTelegram(){
  if (bot_token.length()==0) return; // disabled
  secured_client.setInsecure();      // NOTE: for production, set root CA
  secured_client.setTimeout(3000);   // faster timeouts
  bot = new UniversalTelegramBot(bot_token, secured_client);
  tgSetCommands();                   // установить /-меню
  if (!tgTaskHandle){
    xTaskCreatePinnedToCore(tgTask, "tgTask", 8192, nullptr, 1, &tgTaskHandle, 0); // core 0
  }
}

// =================== UART CLI ===================
String readLine(){
  static String acc;
  while (Serial.available()){
    char c = (char)Serial.read();
    if (c=='\r') continue;
    if (c=='\n'){ String out = acc; acc=""; return out; }
    acc += c;
  }
  return String();
}

void printHelp(){
  Serial.println(F("Commands: GET <KEY|ALL>, SET <KEY> <VAL>, DEL KEY <KEY>, CHAT ADD|DEL <ID>, CHAT LIST, SAVE, REBOOT"));
  Serial.println(F("Keys: WIFI_SSID, WIFI_PASS, HOSTNAME, LED_COUNT, BRIGHTNESS, POWER, TOKEN"));
}

void handleCli(const String& line){
  if (line.length()==0) return;
  String cmd = line; cmd.trim();
  cmd.replace("\t"," ");
  int sp = cmd.indexOf(' ');
  String op = sp>=0 ? cmd.substring(0, sp) : cmd; op.toUpperCase();
  String rest = sp>=0 ? cmd.substring(sp+1) : ""; rest.trim();

  if (op=="GET"){
    if (rest.equalsIgnoreCase("ALL")){
      Serial.printf("WIFI_SSID=%s\n", wifi_ssid.c_str());
      Serial.printf("WIFI_PASS=%s\n", wifi_pass.c_str());
      Serial.printf("HOSTNAME=%s\n", hostname.c_str());
      Serial.printf("LED_COUNT=%u\n", led_count);
      Serial.printf("BRIGHTNESS=%u\n", brightness);
      Serial.printf("POWER=%u\n", powerOn);
      Serial.printf("TOKEN=%s\n", bot_token.c_str());
      Serial.printf("CHAT_IDS=%s\n", chat_ids_csv.c_str());
    } else if (rest.length()){
      String k = rest; k.toUpperCase();
      if (k=="WIFI_SSID") Serial.println(wifi_ssid);
      else if (k=="WIFI_PASS") Serial.println(wifi_pass);
      else if (k=="HOSTNAME") Serial.println(hostname);
      else if (k=="LED_COUNT") Serial.println(led_count);
      else if (k=="BRIGHTNESS") Serial.println(brightness);
      else if (k=="POWER") Serial.println((int)powerOn);
      else if (k=="TOKEN") Serial.println(bot_token);
      else if (k=="CHAT_IDS") Serial.println(chat_ids_csv);
      else Serial.println(F("Unknown key"));
    } else printHelp();
  }
  else if (op=="SET"){
    int sp2 = rest.indexOf(' ');
    if (sp2<0){ printHelp(); return; }
    String key = rest.substring(0, sp2); String val = rest.substring(sp2+1);
    String K = key; K.toUpperCase();
    if (K=="WIFI_SSID"){ wifi_ssid = val; savePref("wifi_ssid", wifi_ssid); Serial.println(F("OK")); }
    else if (K=="WIFI_PASS"){ wifi_pass = val; savePref("wifi_pass", wifi_pass); Serial.println(F("OK")); }
    else if (K=="HOSTNAME"){ hostname = val; savePref("hostname", hostname); Serial.println(F("OK")); }
    else if (K=="LED_COUNT"){ led_count = (uint16_t) val.toInt(); savePrefUShort("led_count", led_count); initLeds(); Serial.println(F("OK")); }
    else if (K=="BRIGHTNESS"){ int v=constrain(val.toInt(),0,255); setBrightness((uint8_t)v); Serial.println(F("OK")); }
    else if (K=="POWER"){ powerOn = (val.toInt()!=0); savePrefBool("power", powerOn); applyLeds(); Serial.println(F("OK")); }
    else if (K=="TOKEN"){ bot_token = val; savePref("bot_token", bot_token); setupTelegram(); Serial.println(F("OK")); }
    else Serial.println(F("Unknown key"));
  }
  else if (op=="DEL"){
    if (rest.startsWith("KEY ")){
      String key = rest.substring(4); key.trim();
      prefs.begin("cfg", false); prefs.remove(key.c_str()); prefs.end();
      Serial.println(F("OK"));
    } else printHelp();
  }
  else if (op=="CHAT"){
    if (rest.startsWith("ADD ")){
      String id = rest.substring(4); id.trim();
      if (chat_ids_csv.length()==0) chat_ids_csv = id; else chat_ids_csv += "," + id;
      savePref("chat_ids", chat_ids_csv); Serial.println(F("OK"));
    } else if (rest.startsWith("DEL ")){
      String id = rest.substring(4); id.trim();
      String csv = "," + chat_ids_csv + ","; String key = ","+id+",";
      csv.replace(key, ",");
      if (csv.length()>=2) chat_ids_csv = csv.substring(1, csv.length()-1); else chat_ids_csv="";
      savePref("chat_ids", chat_ids_csv); Serial.println(F("OK"));
    } else if (rest.equalsIgnoreCase("LIST")){
      Serial.println(chat_ids_csv);
    } else printHelp();
  }
  else if (op=="SAVE"){
    WiFi.disconnect(true);
    connectWiFi();
    Serial.println(F("Reconnecting WiFi..."));
  }
  else if (op=="REBOOT"){
    Serial.println(F("Rebooting...")); delay(200); ESP.restart();
  }
  else {
    printHelp();
  }
}

// =================== SERVICE STARTER ===================
void startNetworkServicesOnce(){
  if (!wifiConnected || servicesStarted) return;

  startMDNS();

  static bool webInited = false;
  if (!webInited){ setupWeb(); webInited = true; }

  setupTelegram();

  servicesStarted = true;
  Serial.printf("[NET] %s ip=%s host=%s.local\n",
                wlStr(WiFi.status()), ipToStr(WiFi.localIP()).c_str(), hostname.c_str());
}

// =================== SETUP/LOOP ===================
void setup(){
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\n[BOOT]");

  loadPrefs();
  initLeds();

  WiFi.onEvent(onWiFiEvent); // Core 2.x callback signature
  connectWiFi();             // Services start after GOT_IP
}

void loop(){
  startNetworkServicesOnce();

  server.handleClient();

  if (millis() - lastNetStatus > NET_STATUS_INTERVAL){
    lastNetStatus = millis();
    Serial.printf("[NET] %s ip=%s host=%s.local\n",
                  wlStr(WiFi.status()), ipToStr(WiFi.localIP()).c_str(), hostname.c_str());
  }

  // UART
  String line = readLine();
  if (line.length()) handleCli(line);
}
