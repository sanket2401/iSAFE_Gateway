/**
 * ============================================================
 * iSAFE - Industrial WiFi & Modbus Gateway
 * Firmware Version: 1.0-20260126a1
 * © Copyright 2025, iSAFE
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ModbusRTU.h>
#include <ModbusTCP.h>
#include <WebServer.h> 

// ---------------- CONSTANTS ----------------
#define FW_VERSION "1.0-20260126a1"

// ---------------- HARDWARE PINS ----------------
#define PIN_RELAY      4
#define PIN_SENSOR     5
// PIN_RS485_DE is intentionally removed to match your auto-TX hardware

// ---------------- CONFIG STRUCT ----------------
struct Config {
  uint8_t slaveID;
  uint32_t baudRate;
  uint8_t stopBits;
  uint8_t parity;     // 0=None, 1=Even, 2=Odd
  bool liveStatus;

  uint8_t wifiMode;       
  uint16_t retryInterval; 
  uint8_t retryLimit;

  String ssid;            // Client SSID
  String password;        // Client Password
  String apSSID;          // AP SSID
  String apPassword;      // AP Password
  String loginPass;       // Web UI Password
  
  bool apMode;

  IPAddress ip, gateway, subnet;
  String mac;
} cfg;

// ---------------- REGISTER ARRAYS ----------------
uint16_t statusRegs[200];   // 40001 to 40200 (Addresses 0-199)
uint16_t configRegs[200];   // 41001 to 41200 (Addresses 1000-1199)

// ---------------- GLOBAL STATE ----------------
Preferences preferences;
ModbusRTU mb_rtu;
ModbusTCP mb_tcp;
WebServer server(80);

unsigned long apStartMillis = 0;
bool apActive = false;
bool tcpStarted = false;

// Web Security State
bool isLoggedIn = false;

// Safe Reboot Flags
bool pendingRestart = false;
unsigned long restartTime = 0;

// Dual-Modbus Mirroring Lock
bool isMirroring = false;

// WiFi Non-Blocking State Machine
enum WiFiState { WS_IDLE, WS_CONNECTING, WS_CONNECTED, WS_WAIT_RETRY, WS_LONG_WAIT };
WiFiState wState = WS_IDLE;
unsigned long wTimer = 0;
int retryCount = 0;

// ============================================================
//  UTILITIES (1 Char Per Register for clear ASCII display)
// ============================================================
void strToRegs(String s, uint16_t *regs, int maxRegs) {
  for (int i = 0; i < maxRegs; i++) regs[i] = 0;
  for (int i = 0; i < s.length() && i < maxRegs; i++) {
    regs[i] = (uint16_t)s[i]; 
  }
}

String regsToStr(uint16_t *regs, int maxRegs) {
  String s = "";
  for (int i = 0; i < maxRegs; i++) {
    if (regs[i] == 0) break; 
    if (regs[i] >= 32 && regs[i] <= 126) { // Valid text characters only
      s += (char)regs[i];
    }
  }
  return s;
}

// ============================================================
//  PERSISTENT STORAGE (Save & Load)
// ============================================================
void saveConfig() {
  preferences.begin("config", false);
  preferences.putUChar("slaveID", cfg.slaveID);
  preferences.putUInt("baudRate", cfg.baudRate);
  preferences.putUChar("stopBits", cfg.stopBits);
  preferences.putUChar("parity", cfg.parity);
  preferences.putBool("liveStatus", cfg.liveStatus);
  
  preferences.putUChar("wifiMode", cfg.wifiMode);
  preferences.putUShort("retryInt", cfg.retryInterval);
  preferences.putUChar("retryLim", cfg.retryLimit);
  
  preferences.putString("ssid", cfg.ssid);
  preferences.putString("password", cfg.password);
  preferences.putString("apSSID", cfg.apSSID);
  preferences.putString("apPass", cfg.apPassword);
  preferences.putString("loginPass", cfg.loginPass);
  
  preferences.putBool("apMode", cfg.apMode);
  preferences.end();
}

void loadConfig() {
  cfg.mac = WiFi.macAddress();
  
  String cleanMac = "";
  for(char c : cfg.mac) if(c != ':') cleanMac += (char)tolower(c);
  String last4 = (cleanMac.length() >= 4) ? cleanMac.substring(cleanMac.length() - 4) : "0000";
  
  String cleanMacUpper = cleanMac;
  cleanMacUpper.toUpperCase();
  String last4Upper = (cleanMacUpper.length() >= 4) ? cleanMacUpper.substring(cleanMacUpper.length() - 4) : "0000";

  preferences.begin("config", true);
  cfg.slaveID   = preferences.getUChar("slaveID", 1);
  cfg.baudRate  = preferences.getUInt("baudRate", 115200);
  cfg.stopBits  = preferences.getUChar("stopBits", 1);
  cfg.parity    = preferences.getUChar("parity", 0);
  cfg.liveStatus= preferences.getBool("liveStatus", true);
  
  cfg.wifiMode  = preferences.getUChar("wifiMode", 3); 
  cfg.retryInterval = preferences.getUShort("retryInt", 600);
  cfg.retryLimit = preferences.getUChar("retryLim", 6);
  
  cfg.ssid      = preferences.getString("ssid", "iSAFE-NETWORK");
  cfg.password  = preferences.getString("password", "is@f3@network!@#");
  cfg.apSSID    = preferences.getString("apSSID", "iSAFE-" + last4Upper);
  cfg.apPassword = preferences.getString("apPass", "isafe@dm1n");
  cfg.loginPass = preferences.getString("loginPass", "isafe@" + last4);
  
  cfg.apMode    = preferences.getBool("apMode", true);
  preferences.end();

  // AUTO-REPAIR: Heals corrupted memory defaults automatically
  if (cfg.slaveID == 0 || cfg.slaveID > 247) cfg.slaveID = 1;
  if (cfg.baudRate != 9600 && cfg.baudRate != 19200 && cfg.baudRate != 115200) cfg.baudRate = 115200;
  if (cfg.stopBits < 1 || cfg.stopBits > 2) cfg.stopBits = 1;
  if (cfg.retryInterval == 0) cfg.retryInterval = 600;
  if (cfg.retryLimit == 0) cfg.retryLimit = 6;
  if (cfg.wifiMode == 0 && cfg.retryLimit == 6) cfg.wifiMode = 3; 
  
  if (cfg.ssid.length() < 2) cfg.ssid = "iSAFE-NETWORK";
  if (cfg.password.length() < 2) cfg.password = "is@f3@network!@#";
  if (cfg.apSSID.length() < 2 || cfg.apSSID == "d") cfg.apSSID = "iSAFE-" + last4Upper;
  if (cfg.apPassword.length() < 2) cfg.apPassword = "isafe@dm1n";
  if (cfg.loginPass.length() < 2) cfg.loginPass = "isafe@" + last4;
}

// ============================================================
//  MODBUS MAPPING & CALLBACKS
// ============================================================
void configToStatus() {
  cfg.ip = WiFi.localIP();
  IPAddress apIP = WiFi.softAPIP();

  for(int i=0; i<200; i++) statusRegs[i] = 0; 

  statusRegs[0] = cfg.slaveID;
  statusRegs[1] = (cfg.baudRate == 9600 ? 0 : (cfg.baudRate == 19200 ? 1 : 2)); 
  statusRegs[2] = cfg.stopBits;
  statusRegs[3] = cfg.parity;
  statusRegs[4] = cfg.liveStatus;

  statusRegs[5] = cfg.ip[0];
  statusRegs[6] = cfg.ip[1];
  statusRegs[7] = cfg.ip[2];
  statusRegs[8] = cfg.ip[3];
  statusRegs[9] = 502; 

  statusRegs[10] = apIP[0];
  statusRegs[11] = apIP[1];
  statusRegs[12] = apIP[2];
  statusRegs[13] = apIP[3];
  statusRegs[14] = 502; 

  uint8_t mac[6];
  WiFi.macAddress(mac);
  statusRegs[15] = mac[0];
  statusRegs[16] = mac[1];
  statusRegs[17] = mac[2];
  statusRegs[18] = mac[3];
  statusRegs[19] = mac[4];
  statusRegs[20] = mac[5];

  strToRegs(cfg.ssid, &statusRegs[21], 15);      
  strToRegs(cfg.password, &statusRegs[36], 15);  

  statusRegs[51] = cfg.wifiMode;
  statusRegs[52] = cfg.retryInterval;
  statusRegs[53] = cfg.retryLimit;
  
  strToRegs(cfg.apSSID, &statusRegs[54], 15);    
  strToRegs(cfg.apPassword, &statusRegs[69], 15);
  strToRegs(cfg.loginPass, &statusRegs[84], 15); 
  
  for (int i = 0; i < 200; i++) {
      configRegs[i] = statusRegs[i];
  }
}

uint16_t cbApply(TRegister* reg, uint16_t val) {
  if (val == 1) {  
    for(int i=0; i<109; i++) configRegs[i] = mb_rtu.Hreg(1000 + i); 

    cfg.slaveID = configRegs[0];
    switch (configRegs[1]) {
      case 0: cfg.baudRate = 9600; break;
      case 1: cfg.baudRate = 19200; break;
      case 2: cfg.baudRate = 115200; break;
      default: cfg.baudRate = 115200; break; 
    }
    cfg.stopBits = configRegs[2];
    cfg.parity   = configRegs[3];
    cfg.liveStatus = configRegs[4];

    cfg.ssid = regsToStr(&configRegs[21], 15);
    cfg.password = regsToStr(&configRegs[36], 15);

    cfg.wifiMode = configRegs[51];
    cfg.retryInterval = configRegs[52];
    cfg.retryLimit = configRegs[53];
    
    cfg.apSSID = regsToStr(&configRegs[54], 15);
    cfg.apPassword = regsToStr(&configRegs[69], 15);
    cfg.loginPass = regsToStr(&configRegs[84], 15);

    if (cfg.slaveID == 0 || cfg.slaveID > 247) cfg.slaveID = 1;

    saveConfig();
    
    pendingRestart = true;
    restartTime = millis() + 1000;
    
    mb_rtu.Hreg(109, 0); mb_rtu.Hreg(1109, 0);
    mb_tcp.Hreg(109, 0); mb_tcp.Hreg(1109, 0);
    return 0; 
  }
  return val;
}

uint16_t onReadOnlyRtu(TRegister* reg, uint16_t val) { return reg->value; }
uint16_t onReadOnlyTcp(TRegister* reg, uint16_t val) { return reg->value; }

uint16_t onWriteRtu(TRegister* reg, uint16_t val) {
  if(isMirroring) return val;
  isMirroring = true;
  mb_tcp.Hreg(reg->address.address, val);
  isMirroring = false;
  return val;
}

uint16_t onWriteTcp(TRegister* reg, uint16_t val) {
  if(isMirroring) return val;
  isMirroring = true;
  mb_rtu.Hreg(reg->address.address, val);
  isMirroring = false;
  return val;
}

// ============================================================
//  WEB SERVER (Industrial UI & Login)
// ============================================================

// --- SHARED CSS HEADER ---
String getHtmlHeader(String title) {
  return R"rawliteral(
  <!DOCTYPE html><html lang='en'>
  <head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width,initial-scale=1'>
    <title>)rawliteral" + title + R"rawliteral(</title>
    <style>
      * { box-sizing: border-box; margin: 0; padding: 0; }
      body { font-family: 'Segoe UI', Arial, sans-serif; background: #eef2f5; color: #333; }
      .wrap { max-width: 700px; margin: 30px auto; padding: 0 15px; }
      
      .page-title { background: #1a3a6b; color: #fff; padding: 18px 20px; font-size: 1.3em; font-weight: bold; text-align: center; border-radius: 6px 6px 0 0; letter-spacing: 0.5px; position: relative;}
      .logout-btn { position: absolute; right: 20px; top: 18px; color: #fff; text-decoration: none; font-size: 0.8em; background: rgba(255,255,255,0.2); padding: 4px 10px; border-radius: 4px; }
      .logout-btn:hover { background: rgba(255,255,255,0.4); }
      
      .ip-bar { background: #d0daf5; border-bottom: 3px solid #1a3a6b; padding: 10px 18px; font-size: 0.95em; color: #1a3a6b; font-weight: bold; display: flex; justify-content: space-between; flex-wrap: wrap;}
      
      .card { background: #fff; border: 1px solid #dcdcdc; border-radius: 0 0 6px 6px; margin-bottom: 18px; box-shadow: 0 2px 5px rgba(0,0,0,0.02); }
      .card + .card { border-radius: 6px; }
      
      .sec-hdr { color: #1a3a6b; font-weight: bold; font-size: 0.95em; padding: 12px 18px; border-bottom: 1px solid #eee; background: #f8f9fa; }
      .row { display: flex; align-items: center; padding: 10px 18px; border-bottom: 1px solid #f3f3f3; }
      .row:last-child { border-bottom: none; }
      .row label { flex: 0 0 220px; font-size: 0.9em; color: #555; font-weight: 600; }
      .row input, .row select { flex: 1; padding: 8px 10px; border: 1px solid #ccc; border-radius: 4px; font-size: 0.9em; transition: border 0.2s; }
      .row input:focus, .row select:focus { border-color: #1a3a6b; outline: none; }
      .row input[readonly] { background: #f4f4f4; color: #666; border-color: #e0e0e0; }
      .row input[type='checkbox'] { flex: none; width: 18px; height: 18px; margin-left: 5px; cursor: pointer; }
      
      .btn-row { text-align: center; padding: 20px; background: #fff; border-radius: 6px; border: 1px solid #dcdcdc; margin-bottom: 20px;}
      .btn { background: #1a3a6b; color: #fff; padding: 12px 50px; border: none; border-radius: 4px; font-size: 1.05em; font-weight: bold; cursor: pointer; transition: background 0.2s; }
      .btn:hover { background: #0e2550; }
      
      .footer { text-align: center; color: #777; font-size: 0.85em; padding: 15px; border-top: 1px solid #ddd; margin-top: 20px;}
      
      /* Login Specific */
      .login-wrap { max-width: 400px; margin: 80px auto; padding: 0 15px; }
      .login-body { padding: 30px; }
      .lbl { display: block; font-size: 0.9em; font-weight: bold; color: #444; margin: 15px 0 5px; }
      .login-body input { width: 100%; padding: 10px; border: 1px solid #ccc; border-radius: 4px; font-size: 1em; }
      .err-msg { color: #d32f2f; font-size: 0.85em; margin-top: 15px; text-align: center; font-weight: bold; background: #fdeaea; padding: 8px; border-radius: 4px; border: 1px solid #f5c6c6;}
      
      @media (max-width: 600px) { .row { flex-direction: column; align-items: flex-start; } .row label { margin-bottom: 6px; } .row input, .row select { width: 100%; } }
    </style>
  </head><body>
  )rawliteral";
}

// --- LOGIN PAGE ---
String loginPageForm(bool isError) {
  String html = getHtmlHeader("iSAFE - Login");
  html += R"rawliteral(
  <div class='login-wrap'>
    <div class='page-title'>iSAFE &ndash; System Access</div>
    <div class='card login-body'>
      <form action='/login' method='POST'>
        <span class='lbl'>Username:</span>
        <input type='text' name='u' value='isafeadmin' readonly>
        <span class='lbl'>Password:</span>
        <input type='password' name='p' autofocus>
  )rawliteral";
  
  if (isError) {
    html += "<div class='err-msg'>&#9888; Invalid Password. Please try again.</div>";
  }
  
  html += R"rawliteral(
        <div style='text-align: center; margin-top: 25px;'>
          <button class='btn' type='submit' style='width: 100%;'>Secure Login</button>
        </div>
      </form>
    </div>
  </div></body></html>
  )rawliteral";
  return html;
}

// --- MAIN CONFIG PAGE ---
String configPageForm() {
  String staIP = WiFi.localIP().toString();
  String apIP  = WiFi.softAPIP().toString();
  String mac   = WiFi.macAddress();

  String html = getHtmlHeader("iSAFE - Configuration");
  
  // Header & IP Bar
  html += "<div class='wrap'>";
  html += "<div class='page-title'>iSAFE &ndash; WiFi Configuration";
  html += "<a href='/logout' class='logout-btn'>Logout</a></div>";
  html += "<div class='card'><div class='ip-bar'>";
  html += "<span>&#127760; Station IP: " + staIP + "</span>";
  html += "<span>&#128246; AP IP: " + apIP + "</span>";
  html += "</div></div>";

  html += "<form action='/save' method='POST'>";

  // 1. Authentication
  html += "<div class='card'><div class='sec-hdr'>Authentication</div>";
  html += "<div class='row'><label>Login Page Password:</label>";
  html += "<input type='password' name='loginPass' placeholder='Leave blank to keep current'></div></div>";

  // 2. WiFi Operation Mode
  html += "<div class='card'><div class='sec-hdr'>WiFi Operation Mode</div>";
  html += "<div class='row'><label>Mode:</label><select name='wifiMode'>";
  html += "<option value='3'" + String(cfg.wifiMode==3?" selected":"") + ">Both AP and Client</option>";
  html += "<option value='2'" + String(cfg.wifiMode==2?" selected":"") + ">Client Mode</option>";
  html += "<option value='1'" + String(cfg.wifiMode==1?" selected":"") + ">AP Mode</option>";
  html += "<option value='0'" + String(cfg.wifiMode==0?" selected":"") + ">OFF</option>";
  html += "</select></div></div>";

  // 3. Client Mode Settings
  html += "<div class='card'><div class='sec-hdr'>Client Mode Settings</div>";
  html += "<div class='row'><label>WiFi Name (SSID):</label><input type='text' name='ssid' value='" + cfg.ssid + "' maxlength='32'></div>";
  html += "<div class='row'><label>WiFi Password:</label><input type='text' name='password' value='" + cfg.password + "' maxlength='64'></div>";
  html += "<div class='row'><label>Retry Interval (seconds):</label><input type='number' name='rInt' value='" + String(cfg.retryInterval) + "'></div>";
  html += "<div class='row'><label>Retry Limit:</label><input type='number' name='rLim' value='" + String(cfg.retryLimit) + "'></div></div>";

  // 4. AP Mode Settings
  html += "<div class='card'><div class='sec-hdr'>AP Mode Settings</div>";
  html += "<div class='row'><label>AP WiFi Name (SSID):</label><input type='text' name='apSSID' value='" + cfg.apSSID + "' maxlength='32'></div>";
  html += "<div class='row'><label>AP WiFi Password:</label><input type='text' name='apPassword' value='" + cfg.apPassword + "' maxlength='64'></div></div>";

  // 5. Modbus RS485 Settings
  html += "<div class='card'><div class='sec-hdr'>Modbus & Serial Settings</div>";
  html += "<div class='row'><label>Slave ID:</label><input type='number' name='slaveID' value='" + String(cfg.slaveID) + "' min='1' max='247'></div>";
  
  html += "<div class='row'><label>Baud Rate:</label><select name='baudRate'>";
  html += "<option value='9600'" + String(cfg.baudRate==9600?" selected":"") + ">9600</option>";
  html += "<option value='19200'" + String(cfg.baudRate==19200?" selected":"") + ">19200</option>";
  html += "<option value='115200'" + String(cfg.baudRate==115200?" selected":"") + ">115200</option>";
  html += "</select></div>";
  
  html += "<div class='row'><label>Stop Bits:</label><input type='number' name='stopBits' value='" + String(cfg.stopBits) + "'></div>";
  
  html += "<div class='row'><label>Parity:</label><select name='parity'>";
  html += "<option value='0'" + String(cfg.parity==0?" selected":"") + ">None</option>";
  html += "<option value='1'" + String(cfg.parity==1?" selected":"") + ">Even</option>";
  html += "<option value='2'" + String(cfg.parity==2?" selected":"") + ">Odd</option>";
  html += "</select></div>";
  
  html += "<div class='row'><label>Live Status:</label><input type='checkbox' name='liveStatus'" + String(cfg.liveStatus?" checked":"") + "></div></div>";

  // 6. Network & Device Readonly Info
  html += "<div class='card'><div class='sec-hdr'>Network & Device Details</div>";
  html += "<div class='row'><label>MAC Address:</label><input type='text' value='" + mac + "' readonly></div>";
  html += "<div class='row'><label>Modbus TCP Port:</label><input type='text' value='502' readonly></div></div>";

  // Footer & Button
  html += "<div class='btn-row'><button type='submit' class='btn'>Save & Apply</button></div>";
  html += "</form>";
  
  html += "<div class='footer'><hr>";
  html += "<center>Firmware " FW_VERSION "<br>&copy; Copyright 2025, iSAFE</center>";
  html += "</div></div></body></html>";
  
  return html;
}

// --- HTTP HANDLERS ---
void handleRoot() {
  if (!isLoggedIn) {
    server.send(200, "text/html", loginPageForm(false));
  } else {
    server.send(200, "text/html", configPageForm());
  }
}

void handleLogin() {
  if (server.hasArg("u") && server.hasArg("p")) {
    if (server.arg("u") == "isafeadmin" && server.arg("p") == cfg.loginPass) {
      isLoggedIn = true;
      server.sendHeader("Location", "/");
      server.send(302);
      return;
    }
  }
  server.send(200, "text/html", loginPageForm(true));
}

void handleLogout() {
  isLoggedIn = false;
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSave() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }

  if (server.hasArg("slaveID")) cfg.slaveID   = server.arg("slaveID").toInt();
  if (server.hasArg("baudRate")) cfg.baudRate  = server.arg("baudRate").toInt();
  if (server.hasArg("stopBits")) cfg.stopBits  = server.arg("stopBits").toInt();
  if (server.hasArg("parity")) cfg.parity    = server.arg("parity").toInt();
  
  cfg.liveStatus= server.hasArg("liveStatus"); 
  
  if (server.hasArg("wifiMode")) cfg.wifiMode  = server.arg("wifiMode").toInt();
  if (server.hasArg("rInt")) cfg.retryInterval = server.arg("rInt").toInt();
  if (server.hasArg("rLim")) cfg.retryLimit = server.arg("rLim").toInt();
  
  if (server.hasArg("ssid")) cfg.ssid = server.arg("ssid");
  if (server.hasArg("password")) cfg.password = server.arg("password");
  if (server.hasArg("apSSID")) cfg.apSSID = server.arg("apSSID");
  if (server.hasArg("apPassword")) cfg.apPassword = server.arg("apPassword");

  // Only update login password if a new value was typed in the box
  if (server.hasArg("loginPass")) {
      String newPass = server.arg("loginPass");
      if (newPass.length() > 0) {
          cfg.loginPass = newPass;
      }
  }

  if (cfg.slaveID == 0 || cfg.slaveID > 247) cfg.slaveID = 1;

  saveConfig();
  
  // Industrial Styled Success Page
  String successPage = getHtmlHeader("iSAFE - Saved");
  successPage += R"rawliteral(
  <div class='wrap' style='margin-top: 80px;'>
    <div class='card' style='padding: 40px; text-align: center; border-top: 5px solid #1a3a6b; border-radius: 6px;'>
      <h2 style='color: #1a3a6b; margin-bottom: 15px;'>Configuration Saved!</h2>
      <p style='color: #555; font-size: 1.1em;'>The device is saving changes to flash memory and rebooting.</p>
      <p style='color: #888; margin-top: 20px; font-size: 0.9em;'>Please wait a few seconds before reconnecting...</p>
    </div>
  </div></body></html>
  )rawliteral";

  server.send(200, "text/html", successPage);
  
  pendingRestart = true;
  restartTime = millis() + 1000;
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// ============================================================
//  WIFI CONNECTIVITY LOOP
// ============================================================
void handleWiFiClient() {
  if(cfg.wifiMode != 2 && cfg.wifiMode != 3) return;

  switch(wState) {
    case WS_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        wState = WS_CONNECTED;
        retryCount = 0;
        
        cfg.ip = WiFi.localIP();
        for(int i = 0; i < 4; i++) {
            statusRegs[5 + i] = (uint16_t)(cfg.ip[i]);
            configRegs[5 + i] = (uint16_t)(cfg.ip[i]); 
            mb_rtu.Hreg(5 + i, statusRegs[5 + i]);
            mb_tcp.Hreg(5 + i, statusRegs[5 + i]);
            mb_rtu.Hreg(1005 + i, configRegs[5 + i]);
            mb_tcp.Hreg(1005 + i, configRegs[5 + i]);
        }
      } 
      else if (millis() - wTimer > 30000UL) { 
        WiFi.disconnect();
        retryCount++;
        if (retryCount >= cfg.retryLimit) {
          wState = WS_LONG_WAIT;
          wTimer = millis();
        } else {
          wState = WS_WAIT_RETRY;
          wTimer = millis();
        }
      }
      break;
      
    case WS_WAIT_RETRY:
      if (millis() - wTimer > (cfg.retryInterval * 1000UL)) {
        WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
        wState = WS_CONNECTING;
        wTimer = millis();
      }
      break;
      
    case WS_LONG_WAIT:
      if (millis() - wTimer > (60UL * 60UL * 1000UL)) { 
        retryCount = 0;
        WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
        wState = WS_CONNECTING;
        wTimer = millis();
      }
      break;
      
    case WS_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        wState = WS_CONNECTING; 
        wTimer = millis();
        WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
      }
      break;
      
    default: break;
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_SENSOR, INPUT);
  digitalWrite(PIN_RELAY, LOW);

  loadConfig(); 
  
  if(cfg.wifiMode != 0) {
      WiFi.mode(WIFI_AP_STA);
      if(cfg.wifiMode == 1 || cfg.wifiMode == 3) {
          WiFi.softAP(cfg.apSSID.c_str(), cfg.apPassword.c_str());
          apActive = true;
          apStartMillis = millis();
      }
  }

  configToStatus();

  uint32_t configSer = SERIAL_8N1;
  if(cfg.parity == 1 && cfg.stopBits == 1) configSer = SERIAL_8E1;
  if(cfg.parity == 2 && cfg.stopBits == 1) configSer = SERIAL_8O1;
  if(cfg.parity == 0 && cfg.stopBits == 2) configSer = SERIAL_8N2;

  #define MODBUS Serial
  Serial.begin(cfg.baudRate, configSer);
  mb_rtu.begin(&Serial); 
  mb_rtu.slave(cfg.slaveID);

  for (int i=0; i<200; i++) {
      mb_rtu.addHreg(i, statusRegs[i]);
      mb_tcp.addHreg(i, statusRegs[i]);
      mb_rtu.addHreg(1000+i, configRegs[i]);
      mb_tcp.addHreg(1000+i, configRegs[i]);

      if (i != 109) {
          mb_rtu.onSetHreg(i, onReadOnlyRtu); 
          mb_tcp.onSetHreg(i, onReadOnlyTcp);
          mb_rtu.onSetHreg(1000+i, onWriteRtu);
          mb_tcp.onSetHreg(1000+i, onWriteTcp);
      }
  }

  mb_rtu.onSetHreg(109, cbApply, 1);
  mb_rtu.onSetHreg(109, cbApply, 1);
  mb_rtu.onSetHreg(1109, cbApply, 1);
  mb_tcp.onSetHreg(1109, cbApply, 1);

  if(cfg.wifiMode == 2 || cfg.wifiMode == 3) {
      WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
      wState = WS_CONNECTING;
      wTimer = millis();
      retryCount = 0;
  }

  mb_tcp.server();
  tcpStarted = true;
  setupWebServer();
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  server.handleClient();
  
  mb_rtu.task();
  if(tcpStarted) mb_tcp.task();
  
  digitalWrite(PIN_RELAY, (digitalRead(PIN_SENSOR) == HIGH) ? HIGH : LOW);
  
  handleWiFiClient();
  
  if(apActive && cfg.wifiMode == 3 && (millis() - apStartMillis > 20UL * 60UL * 1000UL)) {
      WiFi.softAPdisconnect(true);
      apActive = false;
  }

  if (pendingRestart && millis() > restartTime) {
      ESP.restart();
  }
  
  delay(10);
}