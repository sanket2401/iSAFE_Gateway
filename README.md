# iSAFE - Industrial WiFi & Modbus Gateway

**Firmware Version:** 1.0-20260126a1  
**Copyright:** © 2025, iSAFE  

iSAFE is a robust, ESP32-based industrial fire detection and data communication gateway. It acts as a seamless bridge between **Modbus RTU (RS485)** and **Modbus TCP (WiFi)**. Designed for extreme reliability, it features non-blocking WiFi reconnection, automatic flash memory repair, a manual alarm override, and an industrial-grade secure Web Configuration UI.

---

## 🌟 Key Features
* **Dual Modbus Integration:** Operates Modbus RTU and Modbus TCP simultaneously. Data written to one is instantly mirrored to the other.
* **100% Crash-Proof Modbus Mapping:** Advanced memory mapping prevents `Exception 2` errors during large block reads (supports up to 110-register batch requests).
* **Auto-Healing & Random MAC Fallback:** Automatically detects corrupted NVS flash sectors and restores factory defaults. If the WiFi chip is damaged, it generates a stable, random MAC to prevent system lockout.
* **Industrial Web UI:** Responsive, SCADA-styled configuration portal with secure login and a manual "Ignite Alarm" override button.
* **Non-Blocking Architecture:** WiFi drops or reconnections happen silently in the background without freezing the Modbus polling or relay logic.
* **Four Deployment Modes:**
  1. Fully automated, wired communication (4 wires).
  2. Fully automated, wireless communication.
  3. Self-operating mode (battery/2 wires).
  4. Manual mode.

---

## 🔌 Hardware Wiring & Pinout
* **Data & Power (Location A):**
  * `GPIO 16`: RS485 RX
  * `GPIO 17`: RS485 TX
  * `VCC`: Module Power
  * `GND`: Ground
  * *(Note: Designed for Auto-TX RS485 chips. No `DE/RE` pin required).*
* **Relay & Sensor Control (Location B):**
  * `GPIO 4`: Relay Output (Fire detection / manual trigger)
  * `GPIO 5`: External Sensor / Manual trigger input

---

## 💻 Web User Interface

The device hosts a built-in secure web server. By default, it broadcasts an Access Point (AP).
* **Default AP SSID:** `iSAFE-[Last 4 characters of MAC]`
* **Default AP Password:** `isafe@dm1n`
* **Default Gateway IP:** `192.168.4.1`

### Security & Login
Access to the configuration panel is restricted by a login portal.
* **Default Username:** `isafeadmin`
* **Default Password:** `isafe@[Last 4 characters of MAC]` *(e.g., if MAC ends in 09:88, password is `isafe@0988`)*

![iSAFE Login Page](login.png)

### Configuration Panel
Once logged in, you can configure Network modes (AP, Client, Both, Off), Modbus Serial settings (Baud rate, Data bits, Parity, Stop bits), trigger the Manual Alarm, and view live IP/MAC addresses. 

![iSAFE Configuration Panel](iSAFEConfigurationPanel.jpeg)

---

## 🧮 Modbus Architecture & Register Map

The Modbus memory is strictly divided into two distinct blocks to ensure data integrity:
1. **Read-Only Status Block (`40001 - 40200`):** Contains live hardware states, current IPs, and active configurations. Cannot be overwritten.
2. **Read/Write Config Block (`41001 - 41200`):** Used to stage new configurations in RAM. 
3. **Trigger (`41110`):** Writing a `1` to this register forces the ESP32 to save the `41000` block to permanent flash memory and reboot.

### String (Text) Data Encoding
To prevent Endianness (byte-swapping) issues between different PLCs and Master software, the iSAFE firmware stores exactly **ONE character per 16-bit register**. E.g., if Register 40022 reads `105`, that is the ASCII code for the letter 'i'. Unused spaces are populated with `0`.

### Complete Register Map

| Read-Only (Live) | Read/Write (Setup) | Parameter Name | Value Format / Description |
| :--- | :--- | :--- | :--- |
| **40001** | **41001** | **Slave ID** | `1` to `247`. Default: `1`. |
| **40002** | **41002** | **Baud Rate Code** | `0`=9600, `1`=19200, `2`=38400, `3`=57600, `4`=115200 (Default) |
| **40003** | **41003** | **Stop Bits** | `1` or `2`. Default: `1`. |
| **40004** | **41004** | **Parity** | `0`=None (Default), `1`=Even, `2`=Odd |
| **40005** | **41005** | **Live Status** | `0` (Inactive) or `1` (Active) |
| **40006 - 40009** | *Read Only* | **Station IP Address** | 4 Registers (e.g. `192`, `168`, `1`, `100`) |
| **40010** | *Read Only* | **Station Port** | `502` |
| **40011 - 40014** | *Read Only* | **AP IP Address** | 4 Registers (e.g. `192`, `168`, `4`, `1`) |
| **40015** | *Read Only* | **AP Port** | `502` |
| **40016 - 40021** | *Read Only* | **MAC Address** | 6 Registers (e.g. `236`, `218`, `59`...) |
| **40022 - 40036** | **41022 - 41036** | **Client SSID** | 15 Registers (ASCII: 1 letter per reg) |
| **40037 - 40051** | **41037 - 41051** | **Client Password** | 15 Registers (ASCII: 1 letter per reg) |
| **40052** | **41052** | **WiFi Mode** | `0`=Off, `1`=AP, `2`=Client, `3`=Both |
| **40053** | **41053** | **Retry Interval** | Seconds (e.g. `600`) |
| **40054** | **41054** | **Retry Limit** | Attempts (e.g. `6`) |
| **40055 - 40069** | **41055 - 41069** | **AP SSID** | 15 Registers (ASCII: 1 letter per reg) |
| **40070 - 40084** | **41070 - 41084** | **AP Password** | 15 Registers (ASCII: 1 letter per reg) |
| **40085 - 40099** | **41085 - 41099** | **Web Login Pass** | 15 Registers (ASCII: 1 letter per reg) |
| **40100** | **41100** | **Data Bits** | `7` or `8`. Default: `8`. |
| **40101 - 40109** | **41101 - 41109** | **Reserved Buffer** | Empty space to separate data from the trigger. |
| *Read Only* | **41110** | **Commit & Reboot** | Write `1` here to Save all settings to Flash! |

---

## ⚙️ System Logic & Architecture Breakdown

This firmware utilizes advanced C++ logic to ensure 100% industrial uptime and prevent device lockups.

### 1. Dual-Modbus Mirroring Engine
The device initializes two completely separate Modbus libraries: `ModbusRTU` (assigned to Serial pins 16/17) and `ModbusTCP` (assigned to WiFi Port 502). By default, these two systems do not talk to each other. To solve this, the firmware uses a **Mirroring Callback System**.
Every writable register (41001-41100) has an attached callback function. If a Master writes to the RTU register, the callback instantly grabs that value and silently pushes it to the TCP register bank (and vice versa). A boolean flag (`isMirroring`) ensures an infinite loop does not occur while they sync.

### 2. The "Commit Trigger" Staging System
Writing directly to flash memory on an ESP32 takes time. If a Modbus master tries to write 15 characters of an SSID sequentially and the device saves to flash after every single letter, the device will crash or timeout.
To solve this, the `41000` block acts strictly as RAM (Random Access Memory). The Master can write data as slowly or as quickly as it wants without interrupting the device. The data is only converted, checked, and burned into the permanent NVS Flash memory when the Master writes a `1` to register **41110**. This acts as a hardware "Save & Reboot" switch.

### 3. Non-Blocking WiFi State Machine
Standard Arduino WiFi code uses a `while(WiFi.status() != WL_CONNECTED)` loop. In an industrial environment, if the router turns off, this standard loop will freeze the entire processor, meaning the Fire Relay and Modbus RS485 will stop working.
The iSAFE firmware uses a **Switch-Case State Machine** (`wState`) attached to a `millis()` timer. It checks the WiFi status for just 1 microsecond during each loop. If the connection fails, it increments a counter. If it hits the retry limit (e.g., 6 times), it switches to a `WS_LONG_WAIT` state and completely suspends WiFi connection attempts for exactly 1 hour. During this entire process, the core loop continues running at maximum speed.

### 4. Auto-Healing Memory & Random MAC Fallback
If a power surge interrupts a flash-write cycle, the ESP32 memory can become corrupted. 
The `loadConfig()` function includes an **Auto-Repair Module**. As soon as variables are pulled from flash, they are clamped to mathematical limits (e.g., if Parity > 2, force it to 0). Furthermore, if the physical WiFi antenna is damaged and returns a MAC address of `00:00:00:00:00:00`, the system will use an internal randomizer to generate a stable, locally-administered MAC address, save it to flash, and use it to construct the default Passwords so you are never locked out of the device.

### 5. AP Auto-Timeout Logic
If the device is in "Both AP and Client" mode (Code 3), the Access Point must disable itself to save power and security bandwidth after 20 minutes. The system records `apStartMillis` at boot. The main loop constantly compares the current time against this marker. Once the difference exceeds `1,200,000 milliseconds` (20 minutes), the firmware executes `WiFi.softAPdisconnect(true)`, killing the broadcast network while leaving the Client network flawlessly intact.

### 6. Manual Ignite Override & Relay Control
The relay logic is executed on every single loop iteration via a highly optimized ternary operator: 
`digitalWrite(PIN_RELAY, (digitalRead(PIN_SENSOR) == HIGH || manualAlarm) ? HIGH : LOW);`
This means the Fire Relay will instantly activate if the physical sensor pin goes HIGH **OR** if the virtual `manualAlarm` variable is triggered via the Web UI "Ignite Alarm" button. The relay cannot be blocked by any web loading or Modbus querying.

---

## 🛠️ Testing & Automation
You can fully test and interact with this gateway using the included Desktop testing tools.

* **Modbus Diagnostic Dashboard (PyQt6):** A full visual UI tool (`modbus_tool.py`) allowing one-click block reading, direct ASCII translation, and writing.
* **CLI Automated Test (`test.py`):** An automated python script that connects to the COM port, safely pulls all 110 live registers in one batch, decodes the text, and prints a formatted terminal report.

**Required Libraries for Testing:**
```bash
pip install pymodbus pyserial PyQt6
