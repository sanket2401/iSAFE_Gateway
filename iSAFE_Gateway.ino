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
  uint8_t dataBits;   // 7 or 8
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

// Web Security & Control State
bool isLoggedIn = false;
bool manualAlarm = false; // Flag for the UI Ignite Button

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
  preferences.putUChar("dataBits", cfg.dataBits);
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
  String hwMac = WiFi.macAddress();
  preferences.begin("config", true);
  
  // RANDOM MAC FALLBACK LOGIC: If WiFi module is unavailable/broken, generate & store random MAC
  if (hwMac.length() < 10 || hwMac == "00:00:00:00:00:00" || hwMac == "FF:FF:FF:FF:FF:FF") {
      cfg.mac = preferences.getString("randMac", "");
      if (cfg.mac == "") {
          preferences.end();
          preferences.begin("config", false);
          
          uint8_t rm[6];
          for (int i = 0; i < 6; i++) rm[i] = random(256);
          rm[0] = (rm[0] & 0xFC) | 0x02; // Locally administered, unicast bit set
          
          char mBuf[18];
          snprintf(mBuf, sizeof(mBuf), "%02X:%02X:%02X:%02X:%02X:%02X", rm[0], rm[1], rm[2], rm[3], rm[4], rm[5]);
          cfg.mac = String(mBuf);
          preferences.putString("randMac", cfg.mac);
          
          preferences.end();
          preferences.begin("config", true);
      }
  } else {
      cfg.mac = hwMac;
  }
  
  String cleanMac = "";
  for(char c : cfg.mac) if(c != ':') cleanMac += (char)tolower(c);
  String last4 = (cleanMac.length() >= 4) ? cleanMac.substring(cleanMac.length() - 4) : "0000";
  
  String cleanMacUpper = cleanMac;
  cleanMacUpper.toUpperCase();
  String last4Upper = (cleanMacUpper.length() >= 4) ? cleanMacUpper.substring(cleanMacUpper.length() - 4) : "0000";

  cfg.slaveID   = preferences.getUChar("slaveID", 1);
  cfg.baudRate  = preferences.getUInt("baudRate", 115200);
  cfg.dataBits  = preferences.getUChar("dataBits", 8);
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
  if (cfg.baudRate != 9600 && cfg.baudRate != 19200 && cfg.baudRate != 38400 && cfg.baudRate != 57600 && cfg.baudRate != 115200) cfg.baudRate = 115200;
  if (cfg.dataBits < 7 || cfg.dataBits > 8) cfg.dataBits = 8;
  if (cfg.stopBits < 1 || cfg.stopBits > 2) cfg.stopBits = 1;
  if (cfg.retryInterval == 0) cfg.retryInterval = 600;
  if (cfg.retryLimit == 0) cfg.retryLimit = 6;
  if (cfg.wifiMode == 0 && cfg.retryLimit == 6) cfg.wifiMode = 3; 
  
  if (cfg.ssid.length() < 2) cfg.ssid = "iSAFE-NETWORK";
  if (cfg.password.length() < 2) cfg.password = "is@f3@network!@#";
  
  // Strongly enforce correct MAC address in AP and Passwords
  if (cfg.apSSID.length() < 2 || cfg.apSSID.indexOf("0000") > 0 || cfg.apSSID == "d") {
      cfg.apSSID = "iSAFE-" + last4Upper;
  }
  if (cfg.apPassword.length() < 2) cfg.apPassword = "isafe@dm1n";
  
  if (cfg.loginPass.length() < 2 || cfg.loginPass.indexOf("0000") > 0) {
      cfg.loginPass = "isafe@" + last4;
  }
}

// ============================================================
//  MODBUS MAPPING & CALLBACKS
// ============================================================
void configToStatus() {
  cfg.ip = WiFi.localIP();
  IPAddress apIP = WiFi.softAPIP();

  for(int i=0; i<200; i++) statusRegs[i] = 0; 

  statusRegs[0] = cfg.slaveID;
  
  // Map Baud Rate to Status Register (0=9600, 1=19200, 2=38400, 3=57600, 4=115200)
  if (cfg.baudRate == 9600) statusRegs[1] = 0;
  else if (cfg.baudRate == 19200) statusRegs[1] = 1;
  else if (cfg.baudRate == 38400) statusRegs[1] = 2;
  else if (cfg.baudRate == 57600) statusRegs[1] = 3;
  else statusRegs[1] = 4;
  
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

  // Parse custom MAC back to integers for Modbus display
  uint8_t macInts[6] = {0};
  if(cfg.mac.length() == 17) {
      for(int i=0; i<6; i++) {
          macInts[i] = strtoul(cfg.mac.substring(i*3, i*3+2).c_str(), NULL, 16);
      }
  }
  statusRegs[15] = macInts[0];
  statusRegs[16] = macInts[1];
  statusRegs[17] = macInts[2];
  statusRegs[18] = macInts[3];
  statusRegs[19] = macInts[4];
  statusRegs[20] = macInts[5];

  strToRegs(cfg.ssid, &statusRegs[21], 15);      
  strToRegs(cfg.password, &statusRegs[36], 15);  

  statusRegs[51] = cfg.wifiMode;
  statusRegs[52] = cfg.retryInterval;
  statusRegs[53] = cfg.retryLimit;
  
  strToRegs(cfg.apSSID, &statusRegs[54], 15);    
  strToRegs(cfg.apPassword, &statusRegs[69], 15);
  strToRegs(cfg.loginPass, &statusRegs[84], 15); 
  
  // Data bits assigned to register 99 (40100) to safely prevent shifting existing registers
  statusRegs[99] = cfg.dataBits;

  for (int i = 0; i < 200; i++) {
      configRegs[i] = statusRegs[i];
  }
}

uint16_t cbApply(TRegister* reg, uint16_t val) {
  if (val == 1) {  
    for(int i=0; i<109; i++) configRegs[i] = mb_rtu.Hreg(1000 + i); 

    // Clamp values to prevent crashes from invalid Modbus writes
    cfg.slaveID = (configRegs[0] > 0 && configRegs[0] <= 247) ? configRegs[0] : 1;
    
    // Parse Baud Rate from Writable Block
    switch (configRegs[1]) {
      case 0: cfg.baudRate = 9600; break;
      case 1: cfg.baudRate = 19200; break;
      case 2: cfg.baudRate = 38400; break;
      case 3: cfg.baudRate = 57600; break;
      case 4: cfg.baudRate = 115200; break;
      default: cfg.baudRate = 115200; break; 
    }
    
    cfg.stopBits = (configRegs[2] == 1 || configRegs[2] == 2) ? configRegs[2] : 1;
    cfg.parity   = (configRegs[3] <= 2) ? configRegs[3] : 0;
    cfg.liveStatus = (configRegs[4] > 0); 

    cfg.ssid = regsToStr(&configRegs[21], 15);
    cfg.password = regsToStr(&configRegs[36], 15);

    cfg.wifiMode = (configRegs[51] <= 3) ? configRegs[51] : 3;
    cfg.retryInterval = configRegs[52];
    cfg.retryLimit = configRegs[53];
    
    cfg.apSSID = regsToStr(&configRegs[54], 15);
    cfg.apPassword = regsToStr(&configRegs[69], 15);
    cfg.loginPass = regsToStr(&configRegs[84], 15);

    cfg.dataBits = (configRegs[99] == 7 || configRegs[99] == 8) ? configRegs[99] : 8;

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
      body { font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: #eef2f5; color: #333; }
      .wrap { max-width: 750px; margin: 30px auto; padding: 0 15px; }
      
      .page-title { background: #1a252f; color: #fff; padding: 20px; font-size: 1.4em; font-weight: 600; text-align: center; border-radius: 8px 8px 0 0; letter-spacing: 0.5px; position: relative; border-bottom: 4px solid #3498db;}
      .logout-btn { position: absolute; right: 20px; top: 22px; color: #fff; text-decoration: none; font-size: 0.75em; background: rgba(255,255,255,0.15); padding: 5px 12px; border-radius: 4px; transition: 0.3s;}
      .logout-btn:hover { background: #e74c3c; }
      
      .ip-bar { background: #fff; padding: 12px 20px; font-size: 0.95em; color: #2c3e50; font-weight: bold; display: flex; justify-content: space-between; flex-wrap: wrap; box-shadow: 0 2px 5px rgba(0,0,0,0.05); margin-bottom: 20px; border-radius: 0 0 8px 8px; border: 1px solid #dcdcdc; border-top: none;}
      .badge { background: #e8f4f8; padding: 4px 10px; border-radius: 4px; border: 1px solid #bce0ee; color: #007bb5;}
      
      .card { background: #fff; border: 1px solid #dcdcdc; border-radius: 8px; margin-bottom: 20px; box-shadow: 0 2px 8px rgba(0,0,0,0.04); overflow: hidden; border-left: 4px solid #3498db;}
      .card.danger { border-left: 4px solid #e74c3c; }
      
      .sec-hdr { color: #2c3e50; font-weight: 700; font-size: 0.95em; padding: 14px 20px; border-bottom: 1px solid #eee; background: #f8f9fa; text-transform: uppercase; letter-spacing: 0.5px;}
      .row { display: flex; align-items: center; padding: 12px 20px; border-bottom: 1px solid #f3f3f3; }
      .row:last-child { border-bottom: none; }
      .row label { flex: 0 0 220px; font-size: 0.9em; color: #555; font-weight: 600; }
      .row input, .row select { flex: 1; padding: 10px 12px; border: 1px solid #ced4da; border-radius: 4px; font-size: 0.9em; transition: 0.3s; background: #fff;}
      .row input:focus, .row select:focus { border-color: #3498db; outline: none; box-shadow: 0 0 0 2px rgba(52, 152, 219, 0.2);}
      .row input[readonly] { background: #f4f4f4; color: #7f8c8d; border-color: #e0e0e0; }
      .row input[type='checkbox'] { flex: none; width: 18px; height: 18px; cursor: pointer; }
      
      .btn-row { text-align: center; padding: 25px; background: #fff; border-radius: 8px; border: 1px solid #dcdcdc; margin-bottom: 20px; box-shadow: 0 2px 8px rgba(0,0,0,0.04);}
      .btn { background: #2c3e50; color: #fff; padding: 14px 40px; border: none; border-radius: 4px; font-size: 1.05em; font-weight: bold; cursor: pointer; transition: 0.2s; letter-spacing: 0.5px;}
      .btn:hover { background: #1a252f; transform: translateY(-1px);}
      .btn.red { background: #e74c3c; width: 100%; font-size: 1.2em; padding: 16px;}
      .btn.red:hover { background: #c0392b; }
      .btn.green { background: #27ae60; width: 100%; font-size: 1.2em; padding: 16px;}
      .btn.green:hover { background: #2ecc71; }
      
      .footer { text-align: center; color: #95a5a6; font-size: 0.85em; padding: 15px; border-top: 1px solid #ddd; margin-top: 20px;}
      
      /* Login Specific */
      .login-wrap { max-width: 420px; margin: 10vh auto; padding: 0 15px; }
      .login-body { padding: 35px; border-left: none; }
      .lbl { display: block; font-size: 0.9em; font-weight: bold; color: #2c3e50; margin: 15px 0 8px; }
      .login-body input { width: 100%; padding: 12px; border: 1px solid #ced4da; border-radius: 4px; font-size: 1em; margin-bottom: 5px;}
      .err-msg { color: #c0392b; font-size: 0.85em; margin-top: 15px; text-align: center; font-weight: bold; background: #fadbd8; padding: 10px; border-radius: 4px; border: 1px solid #f5b7b1;}
      
      @media (max-width: 600px) { .row { flex-direction: column; align-items: flex-start; } .row label { margin-bottom: 8px; } .row input, .row select { width: 100%; } }
    </style>
  </head><body>
  )rawliteral";
}

// --- LOGIN PAGE ---
String loginPageForm(bool isError) {
  String html = getHtmlHeader("iSAFE - System Login");
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
        <div style='text-align: center; margin-top: 30px;'>
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

  String html = getHtmlHeader("iSAFE - Configuration Panel");
  
  // Header & IP Bar
  html += "<div class='wrap'>";
  html += "<div class='page-title'>iSAFE &ndash; System Configuration";
  html += "<a href='/logout' class='logout-btn'>LOGOUT</a></div>";
  html += "<div class='ip-bar'>";
  html += "<span><span style='color:#7f8c8d'>STA IP:</span> <span class='badge'>" + staIP + "</span></span>";
  html += "<span><span style='color:#7f8c8d'>AP IP:</span> <span class='badge'>" + apIP + "</span></span>";
  html += "</div>";

  // 0. System Controls (IGNITE BUTTON)
  html += "<div class='card danger'><div class='sec-hdr'>Manual Override Controls</div>";
  html += "<div style='padding: 20px;'>";
  html += "<form action='/ignite' method='POST' style='margin:0;'>";
  if (!manualAlarm) {
      html += "<button type='submit' class='btn red'>&#9888; IGNITE ALARM</button>";
      html += "<p style='text-align:center; color:#7f8c8d; font-size:0.85em; margin-top:10px;'>This will manually override sensors and trigger the fire relay.</p>";
  } else {
      html += "<button type='submit' class='btn green'>&#10004; STOP ALARM</button>";
      html += "<p style='text-align:center; color:#e74c3c; font-size:0.85em; font-weight:bold; margin-top:10px;'>ALARM IS CURRENTLY ACTIVE (MANUAL OVERRIDE)</p>";
  }
  html += "</form></div></div>";

  html += "<form action='/save' method='POST'>";

  // 1. Authentication
  html += "<div class='card'><div class='sec-hdr'>Web Panel Security</div>";
  html += "<div class='row'><label>Login Page Password:</label>";
  html += "<input type='password' name='loginPass' placeholder='Leave blank to keep current'></div></div>";

  // 2. WiFi Operation Mode
  html += "<div class='card'><div class='sec-hdr'>Network Operation Mode</div>";
  html += "<div class='row'><label>Active Mode:</label><select name='wifiMode'>";
  html += "<option value='3'" + String(cfg.wifiMode==3?" selected":"") + ">Both AP and Client</option>";
  html += "<option value='2'" + String(cfg.wifiMode==2?" selected":"") + ">Client Mode Only</option>";
  html += "<option value='1'" + String(cfg.wifiMode==1?" selected":"") + ">AP Mode Only</option>";
  html += "<option value='0'" + String(cfg.wifiMode==0?" selected":"") + ">WiFi OFF</option>";
  html += "</select></div></div>";

  // 3. Client Mode Settings
  html += "<div class='card'><div class='sec-hdr'>Station Configuration (Client Mode)</div>";
  html += "<div class='row'><label>Target WiFi Name (SSID):</label><input type='text' name='ssid' value='" + cfg.ssid + "' maxlength='32'></div>";
  html += "<div class='row'><label>Target WiFi Password:</label><input type='text' name='password' value='" + cfg.password + "' maxlength='64'></div>";
  html += "<div class='row'><label>Retry Interval (seconds):</label><input type='number' name='rInt' value='" + String(cfg.retryInterval) + "'></div>";
  html += "<div class='row'><label>Connection Retry Limit:</label><input type='number' name='rLim' value='" + String(cfg.retryLimit) + "'></div></div>";

  // 4. AP Mode Settings
  html += "<div class='card'><div class='sec-hdr'>Access Point Configuration (AP Mode)</div>";
  html += "<div class='row'><label>Broadcast Name (SSID):</label><input type='text' name='apSSID' value='" + cfg.apSSID + "' maxlength='32'></div>";
  html += "<div class='row'><label>Broadcast Password:</label><input type='text' name='apPassword' value='" + cfg.apPassword + "' maxlength='64'></div></div>";

  // 5. Modbus RS485 Settings
  html += "<div class='card'><div class='sec-hdr'>Modbus RS485 & Serial Parameters</div>";
  html += "<div class='row'><label>Modbus Slave ID:</label><input type='number' name='slaveID' value='" + String(cfg.slaveID) + "' min='1' max='247'></div>";
  
  html += "<div class='row'><label>Baud Rate:</label><select name='baudRate'>";
  html += "<option value='9600'" + String(cfg.baudRate==9600?" selected":"") + ">9600 bps</option>";
  html += "<option value='19200'" + String(cfg.baudRate==19200?" selected":"") + ">19200 bps</option>";
  html += "<option value='38400'" + String(cfg.baudRate==38400?" selected":"") + ">38400 bps</option>";
  html += "<option value='57600'" + String(cfg.baudRate==57600?" selected":"") + ">57600 bps</option>";
  html += "<option value='115200'" + String(cfg.baudRate==115200?" selected":"") + ">115200 bps</option>";
  html += "</select></div>";
  
  // ADDED DATA BITS
  html += "<div class='row'><label>Data Bits:</label><select name='dataBits'>";
  html += "<option value='8'" + String(cfg.dataBits==8?" selected":"") + ">8</option>";
  html += "<option value='7'" + String(cfg.dataBits==7?" selected":"") + ">7</option>";
  html += "</select></div>";
  
  html += "<div class='row'><label>Stop Bits:</label><input type='number' name='stopBits' value='" + String(cfg.stopBits) + "' min='1' max='2'></div>";
  
  html += "<div class='row'><label>Parity:</label><select name='parity'>";
  html += "<option value='0'" + String(cfg.parity==0?" selected":"") + ">None</option>";
  html += "<option value='1'" + String(cfg.parity==1?" selected":"") + ">Even</option>";
  html += "<option value='2'" + String(cfg.parity==2?" selected":"") + ">Odd</option>";
  html += "</select></div>";
  
  html += "<div class='row'><label>Enable Live Status Sync:</label><input type='checkbox' name='liveStatus'" + String(cfg.liveStatus?" checked":"") + "></div></div>";

  // 6. Network & Device Readonly Info
  html += "<div class='card'><div class='sec-hdr'>Hardware Identifiers</div>";
  html += "<div class='row'><label>Physical MAC Address:</label><input type='text' value='" + cfg.mac + "' readonly></div>";
  html += "<div class='row'><label>Modbus TCP Port:</label><input type='text' value='502' readonly></div></div>";

  // Footer & Button
  html += "<div class='btn-row'><button type='submit' class='btn'>&#10004; SAVE TO FLASH & APPLY</button></div>";
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

void handleIgnite() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }
  
  // Toggle the manual override flag
  manualAlarm = !manualAlarm;
  
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
  if (server.hasArg("dataBits")) cfg.dataBits  = server.arg("dataBits").toInt(); // ADDED
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
  String successPage = getHtmlHeader("iSAFE - Saving Configuration");
  successPage += R"rawliteral(
  <div class='wrap' style='margin-top: 80px;'>
    <div class='card' style='padding: 50px; text-align: center; border-left: none; border-top: 6px solid #27ae60; border-radius: 8px;'>
      <h2 style='color: #2c3e50; margin-bottom: 20px; font-size: 1.8em;'>&#10004; Configuration Saved</h2>
      <p style='color: #555; font-size: 1.15em;'>The device is writing settings to flash memory and rebooting.</p>
      <p style='color: #888; margin-top: 25px; font-size: 0.95em;'>Please wait 5 seconds before refreshing the page...</p>
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
  server.on("/ignite", HTTP_POST, handleIgnite);
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

  // WAKE UP WIFI DRIVER IMMEDIATELY TO ENSURE TRUE HARDWARE MAC IS READABLE
  WiFi.mode(WIFI_STA);
  delay(50);

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

  // DYNAMIC SERIAL PARSER INCLUDES 7 & 8 DATA BITS
  uint32_t configSer = SERIAL_8N1;
  if (cfg.dataBits == 7) {
      if (cfg.parity == 0 && cfg.stopBits == 1) configSer = SERIAL_7N1;
      else if (cfg.parity == 0 && cfg.stopBits == 2) configSer = SERIAL_7N2;
      else if (cfg.parity == 1 && cfg.stopBits == 1) configSer = SERIAL_7E1;
      else if (cfg.parity == 1 && cfg.stopBits == 2) configSer = SERIAL_7E2;
      else if (cfg.parity == 2 && cfg.stopBits == 1) configSer = SERIAL_7O1;
      else if (cfg.parity == 2 && cfg.stopBits == 2) configSer = SERIAL_7O2;
  } else {
      if (cfg.parity == 0 && cfg.stopBits == 1) configSer = SERIAL_8N1;
      else if (cfg.parity == 0 && cfg.stopBits == 2) configSer = SERIAL_8N2;
      else if (cfg.parity == 1 && cfg.stopBits == 1) configSer = SERIAL_8E1;
      else if (cfg.parity == 1 && cfg.stopBits == 2) configSer = SERIAL_8E2;
      else if (cfg.parity == 2 && cfg.stopBits == 1) configSer = SERIAL_8O1;
      else if (cfg.parity == 2 && cfg.stopBits == 2) configSer = SERIAL_8O2;
  }

  // No Serial logging here, ensuring the code is 100% log-free for Modbus stability
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
  mb_rtu.onSetHreg(1109, cbApply, 1);

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
  
  // RELAY LOGIC: Triggers if Sensor goes HIGH OR Manual Ignite Button is clicked in UI
  digitalWrite(PIN_RELAY, (digitalRead(PIN_SENSOR) == HIGH || manualAlarm) ? HIGH : LOW);
  
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