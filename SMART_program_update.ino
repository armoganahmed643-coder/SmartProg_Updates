#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>

// ==========================================
// FIRMWARE VERSION
// ==========================================
#define FIRMWARE_VERSION "V2.1"

// ==========================================
// WIFI & SERVER
// ==========================================
const char *ssid = "SmartProgrammer";
const char *password = "12345678";
ESP8266WebServer server(80);

// ==========================================
// HARDWARE PINOUT (BOMBAY PCB)
// ==========================================
#define PIN_D1 5   // SDA (24xx) & OLED SDA
#define PIN_D2 4   // SCL (24xx) & OLED SCL
#define PIN_D3 0   // ORG (93xx)
#define PIN_D5 14  // SK (93xx)  | A2 (24xx)
#define PIN_D6 12  // CS (93xx)  | A0 (24xx)
#define PIN_D7 13  // DI (93xx)  | A1 (24xx)
#define PIN_D8 15  // DO (93xx)  | WP (24xx)
#define STATUS_LED 16 // D0
#define BATTERY_PIN A0

// ==========================================
// OLED DISPLAY SETTINGS
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ==========================================
// GLOBAL STATE VARIABLES
// ==========================================
const uint8_t EEPROM_I2C_ADDRESS = 0x50;
uint16_t currentAddress = 0x0000;
bool eepromReadComplete = false;      
bool isActivated = false;

String mainMode = "EEPROM Programmer";
String eepromFamily = "24xx"; 
String eepromMode = "Read";
String eepromSize = "24C04";
String eepromOrg = "x16";     

bool MODE_16BIT = false;
uint8_t ADDRESS_BITS = 6;
bool isOperating = false;             

String currentAction = "READY";
int progressPct = 0;
uint32_t totalBytesActive = 2048;
unsigned long successTimer = 0;

// ==========================================
// OLED & BATTERY FUNCTIONS
// ==========================================
int getBatteryPercentage() {
    int analogValue = analogRead(BATTERY_PIN);
    int percentage = map(analogValue, 775, 992, 0, 100);
    if(percentage > 100) percentage = 100;
    if(percentage < 0) percentage = 0;
    return percentage;
}

void drawClassyHeader(String mode) {
    display.fillRect(0, 0, 128, 13, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(3, 3);
    display.print("BOMBAY PCB"); 
    
    int bat = getBatteryPercentage();
    display.setCursor(82, 3);
    display.print(bat); display.print("%");
    
    display.drawRect(112, 3, 12, 7, SSD1306_BLACK);
    display.fillRect(124, 5, 2, 3, SSD1306_BLACK);
    int batWidth = map(bat, 0, 100, 0, 10);
    display.fillRect(113, 4, batWidth, 5, SSD1306_BLACK);
    display.setTextColor(SSD1306_WHITE);
}

void updateOLED() {
    display.clearDisplay();
    
    int dotCount = (millis() / 500) % 4; 
    String dots = "";
    for(int i = 0; i < dotCount; i++) dots += ".";

    if(currentAction == "READY") {
        drawClassyHeader("RDY");
        display.drawLine(0, 14, 128, 14, SSD1306_WHITE);
        
        display.setTextSize(1); display.setCursor(4, 18); display.print("TARGET CHIP:");
        display.setTextSize(2); display.setCursor(4, 28); 
        
        String dispSize = eepromSize;
        dispSize.replace("_", "C"); // 93_56 -> 93C56
        display.print(dispSize);
        
        display.drawLine(85, 14, 85, 48, SSD1306_WHITE);
        display.setTextSize(1); display.setCursor(90, 20); display.print("MODE:");
        
        display.setCursor(90, 32); 
        if(eepromFamily == "93xx") {
            display.print("93xx\n");
            display.setCursor(90, 42);
            display.print(eepromOrg); // Show x16 or x8
        } else {
            display.print("24xx\n");
            display.setCursor(90, 42);
            display.print("I2C");
        }
        
        display.drawLine(0, 49, 128, 49, SSD1306_WHITE);
        
        display.setCursor(4, 53);
        int footerCycle = (millis() / 3000) % 3;
        if(footerCycle == 0) display.print("IP: 192.168.4.1");
        else if(footerCycle == 1) display.print(isActivated ? "SEC: SYSTEM SECURED" : "SEC: UNREGISTERED");
        else { display.print("FIRMWARE: "); display.print(FIRMWARE_VERSION); }
    } 
    else if(currentAction == "READING" || currentAction == "WRITING") {
        drawClassyHeader("BSY");
        display.drawLine(0, 14, 128, 14, SSD1306_WHITE);
        display.setTextSize(1); display.setCursor(4, 18); display.print(currentAction + dots); 
        display.setCursor(4, 30); display.printf("0x%04X / 0x%04X", currentAddress, totalBytesActive);
        display.drawRect(4, 43, 120, 8, SSD1306_WHITE);
        int fillWidth = map(progressPct, 0, 100, 0, 116);
        display.fillRect(6, 45, fillWidth, 4, SSD1306_WHITE);
        display.setCursor(54, 54); display.print(progressPct); display.print("%");
    }
    else if(currentAction == "SUCCESS") {
        drawClassyHeader("OK");
        display.setTextSize(2); display.setCursor(20, 26); display.print("SUCCESS!");
    }
    else if(currentAction == "UPDATING" || currentAction == "WIFI FAIL" || currentAction == "ERROR") {
        display.fillRect(0, 0, 128, 64, SSD1306_WHITE); 
        display.setTextColor(SSD1306_BLACK);
        display.setTextSize(2); display.setCursor(15, 10); display.print("OTA MODE");
        display.setTextSize(1); display.setCursor(10, 35); display.print(currentAction);
        display.setCursor(10, 50); display.print(dots);
        display.setTextColor(SSD1306_WHITE);
    }
    display.display();
}

// ==========================================
// STATUS LED & MULTIPLEXER
// ==========================================
void initStatusLED() { pinMode(STATUS_LED, OUTPUT); digitalWrite(STATUS_LED, HIGH); }
void ledFlicker() { digitalWrite(STATUS_LED, !digitalRead(STATUS_LED)); }

void updateStatusLED() {
    if (isOperating) return; 
    unsigned long currentMillis = millis();
    if (!isActivated) digitalWrite(STATUS_LED, (currentMillis / 1000) % 2 == 0 ? LOW : HIGH);
    else if (WiFi.softAPgetStationNum() == 0) {
        int cycle = currentMillis % 1500;
        if (cycle < 100 || (cycle > 200 && cycle < 300)) digitalWrite(STATUS_LED, LOW);
        else digitalWrite(STATUS_LED, HIGH);
    } 
    else digitalWrite(STATUS_LED, LOW);
}

void setupHardwarePins() {
    if (eepromFamily == "93xx") {
        pinMode(PIN_D6, OUTPUT); pinMode(PIN_D5, OUTPUT); pinMode(PIN_D7, OUTPUT); pinMode(PIN_D8, INPUT);
        digitalWrite(PIN_D6, LOW); digitalWrite(PIN_D5, LOW);
        pinMode(PIN_D3, OUTPUT); 
        if (eepromOrg == "x16") { MODE_16BIT = true; digitalWrite(PIN_D3, HIGH); }
        else { MODE_16BIT = false; digitalWrite(PIN_D3, LOW); }
    } else {
        pinMode(PIN_D6, OUTPUT); pinMode(PIN_D7, OUTPUT); pinMode(PIN_D5, OUTPUT); pinMode(PIN_D8, OUTPUT);
        digitalWrite(PIN_D6, LOW); digitalWrite(PIN_D7, LOW); digitalWrite(PIN_D5, LOW); digitalWrite(PIN_D8, LOW); 
    }
}

// ==========================================
// 93XX MICROWIRE LOGIC
// ==========================================
void MW_Delay() { delayMicroseconds(5); }
void MW_Start() { digitalWrite(PIN_D6, HIGH); MW_Delay(); }
void MW_Stop() { digitalWrite(PIN_D6, LOW); MW_Delay(); }
void MW_SendBit(bool bitData) { digitalWrite(PIN_D7, bitData); digitalWrite(PIN_D5, HIGH); MW_Delay(); digitalWrite(PIN_D5, LOW); MW_Delay(); }
bool MW_ReadBit() { digitalWrite(PIN_D5, HIGH); MW_Delay(); bool val = digitalRead(PIN_D8); digitalWrite(PIN_D5, LOW); MW_Delay(); return val; }
void MW_SendBits(uint16_t data, uint8_t bits) { for(int i = bits - 1; i >= 0; i--) MW_SendBit((data >> i) & 1); }
uint16_t MW_ReadBits(uint8_t bits) { uint16_t value = 0; for(int i = 0; i < bits; i++) { value <<= 1; if(MW_ReadBit()) value |= 1; } return value; }

int getMWAddressBits() {
    bool is16 = MODE_16BIT;
    if (eepromSize == "93_46") return is16 ? 6 : 7;
    if (eepromSize == "93_56" || eepromSize == "93_66") return is16 ? 8 : 9;
    if (eepromSize == "93_76" || eepromSize == "93_86") return is16 ? 10 : 11;
    return 8;
}

void EEPROM_EWEN() { MW_Start(); MW_SendBit(1); MW_SendBits(0b00, 2); MW_SendBits(0b11, 2); MW_SendBits(0, getMWAddressBits()); MW_Stop(); delay(5); }
void EEPROM_EWDS() { MW_Start(); MW_SendBit(1); MW_SendBits(0b00, 2); MW_SendBits(0b00, 2); MW_SendBits(0, getMWAddressBits()); MW_Stop(); delay(5); }

uint16_t EEPROM_Read_93xx(uint16_t addr) {
    uint16_t data = 0; MW_Start(); MW_SendBit(1); MW_SendBits(0b10, 2); MW_SendBits(addr, getMWAddressBits());
    if(MODE_16BIT) data = MW_ReadBits(16); else data = MW_ReadBits(8);
    MW_Stop(); return data;
}

bool EEPROM_Write_93xx(uint16_t addr, uint16_t data) {
    EEPROM_EWEN(); MW_Start(); MW_SendBit(1); MW_SendBits(0b01, 2); MW_SendBits(addr, getMWAddressBits());
    if(MODE_16BIT) MW_SendBits(data, 16); else MW_SendBits(data, 8);
    MW_Stop(); delay(10);
    uint16_t verify = EEPROM_Read_93xx(addr); EEPROM_EWDS(); return (verify == data);
}

bool write93XXBlock(uint16_t startAddress, const uint8_t *data, size_t length) {
    setupHardwarePins(); isOperating = true; int step = MODE_16BIT ? 2 : 1;
    uint16_t logicalAddr = MODE_16BIT ? (startAddress / 2) : startAddress;
    for (size_t i = 0; i < length; i += step) {
        ledFlicker(); yield();
        uint16_t wordData = data[i]; if (MODE_16BIT && i + 1 < length) wordData |= (data[i+1] << 8); 
        if(!EEPROM_Write_93xx(logicalAddr, wordData)) { isOperating = false; return false; }
        logicalAddr++;
    }
    isOperating = false; return true;
}

bool read93XXBlock(uint16_t startAddress, uint8_t *data, size_t length) {
    setupHardwarePins(); isOperating = true; int step = MODE_16BIT ? 2 : 1;
    uint16_t logicalAddr = MODE_16BIT ? (startAddress / 2) : startAddress;
    for (size_t i = 0; i < length; i += step) {
        ledFlicker(); yield();
        uint16_t wordData = EEPROM_Read_93xx(logicalAddr);
        data[i] = wordData & 0xFF; if (MODE_16BIT && i + 1 < length) data[i+1] = (wordData >> 8) & 0xFF; 
        logicalAddr++;
    }
    isOperating = false; return true;
}

// ==========================================
// 24XX I2C LOGIC
// ==========================================
size_t getEEPROMPageSize() {
    if (eepromSize == "24C01" || eepromSize == "24C02") return 8; else if (eepromSize == "24C04" || eepromSize == "24C08" || eepromSize == "24C16") return 16;
    else if (eepromSize == "24C32" || eepromSize == "24C64") return 32; else if (eepromSize == "24C128" || eepromSize == "24C256") return 64; return 16;  
}

size_t getMaxAddress() {
    if (eepromSize == "24C01") return 128; else if (eepromSize == "24C02") return 256; else if (eepromSize == "24C04") return 512;   
    else if (eepromSize == "24C08") return 1024; else if (eepromSize == "24C16") return 2048; else if (eepromSize == "24C32") return 4096;  
    else if (eepromSize == "24C64") return 8192; else if (eepromSize == "24C128") return 16384; else if (eepromSize == "24C256") return 32768; else if (eepromSize == "24C512") return 65536; return 512;  
}

int getTotalSizeGlobal() {
    if (eepromFamily == "93xx") {
        if (eepromSize == "93_46") return 128; if (eepromSize == "93_56") return 256;
        if (eepromSize == "93_66") return 512; if (eepromSize == "93_76") return 1024; return 2048;
    } return getMaxAddress();
}

bool is16BitAddress() { return (eepromSize == "24C32" || eepromSize == "24C64" || eepromSize == "24C128" || eepromSize == "24C256" || eepromSize == "24C512"); }
uint8_t getDeviceAddress(uint16_t address) {
    if (eepromSize == "24C16" || eepromSize == "24C08") return EEPROM_I2C_ADDRESS | ((address >> 8) & 0x07);
    else if (is16BitAddress()) return EEPROM_I2C_ADDRESS; else return EEPROM_I2C_ADDRESS | ((address >> 8) & 0x01);
}

bool writeEEPROMPage(uint16_t address, const uint8_t *data, size_t length) {
    setupHardwarePins(); isOperating = true; 
    uint8_t deviceAddress = getDeviceAddress(address);
    Wire.beginTransmission(deviceAddress);
    if (is16BitAddress()) Wire.write((address >> 8) & 0xFF);  
    Wire.write(address & 0xFF);  
    for (size_t i = 0; i < length; i++) Wire.write(data[i]);
    if (Wire.endTransmission() == 0) { delay(10); isOperating = false; return true; } 
    isOperating = false; return false;
}

bool readEEPROMPage(uint16_t address, uint8_t *data, size_t length) {
    setupHardwarePins(); isOperating = true; uint8_t deviceAddress = getDeviceAddress(address);
    Wire.beginTransmission(deviceAddress);
    if (is16BitAddress()) Wire.write((address >> 8) & 0xFF); 
    Wire.write(address & 0xFF);  
    if (Wire.endTransmission() != 0) { isOperating = false; return false; }
    Wire.requestFrom(deviceAddress, length);
    for (size_t i = 0; i < length; i++) {
        if (Wire.available()) data[i] = Wire.read(); else { isOperating = false; return false; }
    }
    isOperating = false; return true;
}

// ==========================================
// WEB UI HTML PAGES
// ==========================================
const char activation_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Black Box Pro | Activation</title>
    <style>
        body { background-color: #0f1115; color: #00d2ff; font-family: monospace; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .box { background: #1a1d24; padding: 30px; border-radius: 8px; border: 1px solid #333842; text-align: center; width: 320px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
        input { width: 90%; padding: 12px; margin: 15px 0; background: #000; border: 1px solid #00d2ff; color: #fff; font-size: 16px; text-align: center; letter-spacing: 1px; }
        input:focus { outline: none; border-color: #10b981; }
        button { background: #00d2ff; color: #000; font-weight: bold; padding: 12px; width: 100%; border: none; cursor: pointer; font-size: 16px; margin-bottom: 15px; text-transform: uppercase; letter-spacing: 1px; transition: 0.2s;}
        button:hover { background: #00a8cc; }
        .device-id { font-size: 26px; color: #10b981; font-weight: bold; word-break: break-all; margin-bottom: 10px; border: 1px dashed #10b981; padding: 10px; background: #0b0c10;}
        h2 { margin-top: 0; letter-spacing: 2px; }
    </style>
</head>
<body>
    <div class="box">
        <h2>SYSTEM LOCKED</h2>
        <p style="color: #888; font-size: 12px; line-height: 1.5;">Provide this Hardware ID to unlock.</p>
        <div style="color: #888; font-size: 12px; margin-top: 15px;">HARDWARE ID:</div>
        <div class="device-id">{DEVICE_ID}</div>
        <input type="number" id="key" placeholder="Enter License Key">
        <button onclick="activate()">Unlock Programmer</button>
    </div>
    <script>
        function activate() {
            let key = document.getElementById('key').value;
            if(!key) return alert("Please Enter License Key!");
            fetch('/activate', { method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'key='+key })
            .then(res => res.text()).then(data => {
                if(data === "OK") { alert("Activation Successful! Loading System..."); location.reload(); }
                else { alert("Invalid License Key!"); }
            });
        }
    </script>
</body>
</html>
)rawliteral";

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Smart Programmer | Pro UI</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Cinzel:wght@700&family=Roboto+Mono:wght@400;500&family=Inter:wght@400;600;700&display=swap');
        :root { --bg-color: #121212; --panel-bg: #1e1e1e; --border-color: #333333; --primary: #d4af37; --primary-hover: #b48600; --text-main: #e0e0e0; --text-muted: #888888; --btn-bg: #2d2d2d; --danger: #dc3545; --success: #28a745; --hex-bg: #0a0a0a; }
        * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; } 
        body { background-color: var(--bg-color); color: var(--text-main); font-family: 'Inter', sans-serif; overflow-x: hidden; }
        .container { max-width: 1200px; margin: 0 auto; padding: 15px; display: grid; grid-template-columns: 1fr; gap: 15px; }
        @media (min-width: 900px) { .container { grid-template-columns: 320px 1fr; padding: 25px; } }
        .panel { background: var(--panel-bg); border: 1px solid var(--border-color); border-radius: 8px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
        .v-oled-container { background: #000; border: 3px solid #333; border-radius: 6px; width: 100%; margin: 15px 0; overflow: hidden; font-family: 'Courier New', monospace; box-shadow: 0 0 15px rgba(0, 255, 255, 0.2); transition: all 0.3s ease; }
        .v-oled-header { background: #00ffff; color: #000; padding: 5px 8px; font-weight: bold; display: flex; justify-content: space-between; font-size: 13px;}
        .v-oled-body { padding: 15px 10px; font-size: 15px; color: #00ffff; min-height: 85px; text-align: left; line-height: 1.5; text-shadow: 0 0 5px rgba(0,255,255,0.5); }
        .v-oled-footer { background: #111; color: #00ffff; padding: 5px 8px; font-size: 11px; border-top: 1px solid #00ffff; }
        h1.brand-title { font-family: 'Cinzel', serif; font-size: 26px; font-weight: 700; text-align: center; color: var(--primary); padding-bottom: 5px; letter-spacing: 1px; }
        h3 { font-size: 14px; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-muted); margin-bottom: 12px; font-weight: 700; }
        button { background: var(--btn-bg); color: var(--primary); border: 1px solid var(--border-color); padding: 14px 16px; font-family: 'Inter', sans-serif; font-size: 14px; font-weight: 600; border-radius: 6px; cursor: pointer; width: 100%; margin-bottom: 10px; transition: transform 0.1s ease, background 0.3s ease, color 0.3s ease; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }
        button:active { transform: scale(0.96); box-shadow: 0 1px 2px rgba(0,0,0,0.2); } 
        button:hover { background: var(--primary); color: #fff; border-color: var(--primary); }
        .btn-danger { color: var(--danger); } .btn-danger:hover { background: var(--danger); color: #fff; border-color: var(--danger); }
        .btn-primary-solid { background: var(--primary); color: #000; border-color: var(--primary); } .btn-primary-solid:hover { background: var(--primary-hover); }
        .action-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 10px; } .action-grid-full { grid-column: span 2; }
        input, select { width: 100%; padding: 12px; margin-bottom: 15px; background: var(--bg-color); border: 1px solid var(--border-color); color: var(--text-main); font-family: 'Inter', sans-serif; font-size: 15px; border-radius: 6px; appearance: none; }
        select:focus, input:focus { border-color: var(--primary); outline: none; }
        .hex-viewer { font-family: 'Roboto Mono', monospace; font-size: 13px; background: var(--hex-bg); padding: 15px; border-radius: 6px; height: 350px; overflow-y: auto; border: 1px solid var(--border-color); white-space: pre-wrap; line-height: 1.6; margin-bottom: 15px; }
        .hex-addr { color: var(--primary); font-weight: 700;} .hex-data { color: var(--text-main); } .hex-ascii { color: var(--text-muted); }
        .progress-container { width: 100%; background: var(--bg-color); border-radius: 4px; margin-bottom: 20px; overflow: hidden; border: 1px solid var(--border-color); height: 12px;}
        .progress-bar { height: 100%; background: var(--primary); width: 0%; transition: width 0.4s cubic-bezier(0.4, 0, 0.2, 1); box-shadow: 0 0 10px rgba(212, 175, 55, 0.5); }
        .file-manager { display: flex; gap: 10px; margin-top: 15px; flex-wrap: wrap; }
        .file-manager button { flex: 1; margin-bottom: 0; } .file-manager input[type="text"] { flex: 2; margin-bottom: 0; }
        .toggle-wrap { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; font-size: 14px; font-weight: 600;}
        .switch { position: relative; display: inline-block; width: 38px; height: 22px; } .switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: var(--border-color); transition: .3s; border-radius: 22px; }
        .slider:before { position: absolute; content: ""; height: 16px; width: 16px; left: 3px; bottom: 3px; background-color: #fff; transition: .3s; border-radius: 50%; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }
        input:checked + .slider { background-color: var(--primary); } input:checked + .slider:before { transform: translateX(16px); }

        .modal { display: none; position: fixed; z-index: 1000; left: 0; top: 0; width: 100%; height: 100%; background-color: rgba(0,0,0,0.8); backdrop-filter: blur(4px); }
        .modal-content { background-color: var(--panel-bg); margin: 15% auto; padding: 25px; border: 1px solid var(--primary); border-radius: 8px; width: 90%; max-width: 500px; color: var(--text-main); box-shadow: 0 0 25px rgba(212, 175, 55, 0.3); position: relative; }
        .close-btn { color: var(--danger); position: absolute; right: 20px; top: 15px; font-size: 28px; font-weight: bold; cursor: pointer; transition: 0.2s;}
        .close-btn:hover { color: #fff; }
        .text-badge { background: #111; border: 1px solid var(--primary); padding: 6px 12px; margin: 5px; border-radius: 4px; display: inline-block; color: var(--primary); font-weight: bold; font-family: 'Roboto Mono', monospace; letter-spacing: 1px; box-shadow: 0 2px 4px rgba(0,0,0,0.5);}
    </style>
</head>
<body>
    <div class="container">
        <aside>
            <div class="panel">
                <h1 class="brand-title">BOMBAY PCB</h1>
                <div class="v-oled-container">
                    <div class="v-oled-header"><span>SMART PROG V2.1</span><span id="v-bat">100%</span></div>
                    <div class="v-oled-body" id="v-body">LOADING...</div>
                    <div class="v-oled-footer" id="v-foot">SYSTEM READY</div>
                </div>
            </div>
            <div class="panel">
                <h3>Target Configuration</h3>
                <select id="eepromFamily" onchange="updateChipOptions()">
                    <option value="24xx" selected>24xx (Standard I2C)</option>
                    <option value="93xx">93xx (Microwire)</option>
                </select>
                <select id="eepromSize" onchange="pushConfig()"></select>
                <select id="eepromOrg" style="display:none;" onchange="pushConfig()">
                    <option value="x16">x16 (16-Bit Word Mode)</option>
                    <option value="x8">x8 (8-Bit Byte Mode)</option>
                </select>
                <div class="action-grid" style="margin-top: 10px; margin-bottom: 0;">
                    <button onclick="detectChip()" id="btnDetect24" class="btn-primary-solid action-grid-full">Auto-Detect 24xx IC</button>
                    <button onclick="detect93xx()" id="btnDetect93" class="btn-primary-solid action-grid-full" style="display:none; margin-bottom: 0;">Auto-Select 93xx IC</button>
                </div>
                <div id="chip-info" style="font-size: 12px; color: var(--primary); margin-top: 8px; font-weight:600; min-height: 18px; text-align: center;"></div>
            </div>

            <button onclick="toggleOTA()" style="background: transparent; color: var(--text-muted); border: 1px dashed var(--border-color); margin-top: 10px;">⚙️ Advanced: Show/Hide OTA Update</button>
            <div class="panel" id="otaPanel" style="margin-top: 10px; border: 1px solid var(--danger); display: none;">
                <h3 style="color: var(--danger);">Auto Software Update</h3>
                <p style="font-size:12px; color:var(--text-muted); margin-bottom:10px;">
                    Current Firmware: <b style="color: var(--primary);">V2.1</b><br>
                    Enter Wi-Fi details below to download & install new updates automatically.
                </p>
                <input type="text" id="clientSsid" placeholder="Wi-Fi Name (SSID)">
                <input type="password" id="clientPass" placeholder="Wi-Fi Password">
                <button onclick="startCloudUpdate()" class="btn-danger">Check & Install Update</button>
            </div>
        </aside>
        <main>
            <div class="panel">
                <h3>Memory Operations</h3>
                <div class="progress-container"><div id="progressBar" class="progress-bar"></div></div>
                <div class="action-grid">
                    <button onclick="readEEPROM()" class="btn-primary-solid">Read Chip</button>
                    <button onclick="writeEEPROM()" style="color:var(--primary); border-color:var(--primary);">Write Chip</button>
                    <button class="btn-danger action-grid-full" onclick="eraseEEPROM()">Format (0xFF)</button>
                </div>
                <div class="file-manager">
                    <button onclick="document.getElementById('fileInput').click()">Load BIN</button>
                    <input type="file" id="fileInput" style="display:none" accept=".bin,.hex" onchange="handleFileUpload(event)">
                    <input type="text" id="saveFileName" placeholder="Filename (e.g. LG_Main)">
                    <button onclick="downloadDump()" style="color:var(--primary); border-color:var(--primary);">Save BIN</button>
                </div>
            </div>
            <div class="panel">
                <div class="toggle-wrap">
                    <span style="font-size: 14px; text-transform: uppercase; letter-spacing: 1px; color: var(--text-muted);">Hexadecimal Viewer</span>
                    <div style="display:flex; align-items:center; gap:8px;">
                        <button onclick="extractInfo()" style="width: auto; padding: 8px 12px; margin: 0; background: var(--primary); color: #000; border: none; font-size: 12px;">🔍 Extract BIN Info</button>
                        <span style="margin-left: 10px;">ASCII View</span>
                        <label class="switch"><input type="checkbox" id="asciiToggle" checked onchange="updateHexView()"><span class="slider"></span></label>
                    </div>
                </div>
                <div id="hexViewer" class="hex-viewer">System Ready. Select EEPROM size and Read...</div>
            </div>
        </main>
    </div>

    <div id="infoModal" class="modal">
      <div class="modal-content">
        <span class="close-btn" onclick="closeModal()">&times;</span>
        <h3 style="color: var(--primary); border-bottom: 1px solid #333; padding-bottom: 10px; margin-bottom: 15px;">🔍 Extracted Data</h3>
        <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 15px;">Possible Model Numbers, Compressor IDs, and Data Strings found in the binary file:</p>
        <div id="modalText" style="line-height: 2.2; max-height: 300px; overflow-y: auto;"></div>
      </div>
    </div>

    <script>
        let hexBuffer = new Uint8Array(0); let showAscii = true;
        
        setInterval(() => {
            fetch('/api/status').then(res => res.json()).then(data => {
                document.getElementById('v-bat').innerText = data.bat + '%';
                let bodyHTML = '';
                
                let dots = "";
                let dotCount = Math.floor(Date.now() / 500) % 4;
                for(let i=0; i<dotCount; i++) dots += ".";

                if(data.act === 'READY') {
                    let dName = data.chip.replace("_", "C");
                    bodyHTML = `TARGET CHIP:<br><b style="font-size:22px; color:#fff;">${dName}</b><br>MODE: ${data.fam}`;
                    document.getElementById('v-foot').innerText = data.lic ? "SEC: LOCKED & SECURE" : "SEC: UNREGISTERED";
                } else if(data.act === 'READING' || data.act === 'WRITING') {
                    let hexAddr = "0x" + data.addr.toString(16).toUpperCase().padStart(4, '0');
                    let hexTot = "0x" + data.total.toString(16).toUpperCase().padStart(4, '0');
                    bodyHTML = `<b>${data.act}${dots}</b><br><br>ADDR: ${hexAddr} / ${hexTot}<br>
                    <div style="width:100%; border:1px solid #00ffff; margin-top:5px;"><div style="height:6px; background:#00ffff; width:${data.pct}%"></div></div>`;
                    document.getElementById('v-foot').innerText = `${data.pct}% COMPLETED`;
                } else if(data.act === 'SUCCESS') {
                    bodyHTML = `<br><center><b style="font-size:24px; color:#0f0;">SUCCESS!</b></center>`;
                    document.getElementById('v-foot').innerText = `PROCESS COMPLETED`;
                } else {
                    bodyHTML = `<br><center><b style="font-size:18px; color:#f00;">${data.act}${dots}</b></center>`;
                }
                document.getElementById('v-body').innerHTML = bodyHTML;
            }).catch(e=>{});
        }, 500);

        function toggleOTA() {
            let p = document.getElementById('otaPanel');
            p.style.display = (p.style.display === 'none') ? 'block' : 'none';
        }

        function extractInfo() {
            if(hexBuffer.length === 0) return alert("Buffer is empty! Please Read Chip or Load BIN file first.");
            let extractedText = ""; let currentStr = "";
            for(let i = 0; i < hexBuffer.length; i++) {
                let val = hexBuffer[i];
                if((val >= 48 && val <= 57) || (val >= 65 && val <= 90) || (val >= 97 && val <= 122) || val === 45 || val === 46) {
                    currentStr += String.fromCharCode(val);
                } else {
                    if(currentStr.length >= 5) { extractedText += `<span class="text-badge">${currentStr}</span> `; }
                    currentStr = "";
                }
            }
            if(currentStr.length >= 5) extractedText += `<span class="text-badge">${currentStr}</span>`;
            document.getElementById('modalText').innerHTML = extractedText || "<span style='color:var(--text-muted);'>No valid model or compressor info found.</span>";
            document.getElementById('infoModal').style.display = 'block';
        }

        function closeModal() { document.getElementById('infoModal').style.display = 'none'; }

        function startCloudUpdate() {
            let ssid = document.getElementById('clientSsid').value;
            let pass = document.getElementById('clientPass').value;
            if(!ssid) return alert("Please enter Wi-Fi Name!");
            if(confirm("Device will connect to internet and check for updates. Do not turn off power! Proceed?")) {
                fetch('/start_ota', {
                    method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                    body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
                }).then(res => res.text()).then(msg => alert(msg));
            }
        }

        const chipOptions = {
            "24xx": [{val: "24C01", text: "24C01"}, {val: "24C02", text: "24C02"}, {val: "24C04", text: "24C04"}, {val: "24C08", text: "24C08"}, {val: "24C16", text: "24C16"}, {val: "24C32", text: "24C32"}, {val: "24C64", text: "24C64"}, {val: "24C128", text: "24C128"}, {val: "24C256", text: "24C256"}, {val: "24C512", text: "24C512"}],
            "93xx": [{val: "93_46", text: "93C46"}, {val: "93_56", text: "93C56"}, {val: "93_66", text: "93C66"}, {val: "93_76", text: "93C76"}, {val: "93_86", text: "93C86"}]
        };
        const sizeMap = { '24C01': 128, '24C02': 256, '24C04': 512, '24C08': 1024, '24C16': 2048, '24C32': 4096, '24C64': 8192, '24C128': 16384, '24C256': 32768, '24C512': 65536, '93_46': 128, '93_56': 256, '93_66': 512, '93_76': 1024, '93_86': 2048 };

        function updateHexView() { showAscii = document.getElementById('asciiToggle').checked; if(hexBuffer.length > 0) renderHexFast(hexBuffer); }

        function updateChipOptions() {
            let fam = document.getElementById('eepromFamily').value;
            let sizeSel = document.getElementById('eepromSize'); sizeSel.innerHTML = '';
            chipOptions[fam].forEach(opt => { let el = document.createElement('option'); el.value = opt.val; el.innerText = opt.text; sizeSel.appendChild(el); });
            if(fam === '24xx') {
                document.getElementById('eepromSize').value = '24C04';
                document.getElementById('btnDetect24').style.display = 'block'; document.getElementById('btnDetect93').style.display = 'none'; document.getElementById('eepromOrg').style.display = 'none';
            } else {
                document.getElementById('eepromSize').value = '93_56'; 
                document.getElementById('btnDetect24').style.display = 'none'; document.getElementById('btnDetect93').style.display = 'block'; document.getElementById('eepromOrg').style.display = 'block'; document.getElementById('eepromOrg').value = 'x16';
            }
            document.getElementById('chip-info').innerText = ""; pushConfig();
        }

        async function pushConfig() {
            let payload = { main_mode: "EEPROM", eeprom_mode: "Read", eeprom_family: document.getElementById('eepromFamily').value, eeprom_size: document.getElementById('eepromSize').value, eeprom_org: document.getElementById('eepromOrg').value };
            await fetch('/detail_data', { method: 'POST', body: JSON.stringify(payload) });
        }
        
        async function detectChip() {
            let info = document.getElementById('chip-info'); info.innerHTML = "Scanning..."; 
            try { 
                let res = await fetch('/detect_chip'); let data = await res.json();
                if(data.found) { 
                    let dName = data.chip;
                    if(dName === "24C02") dName = "24C02 (Or 32/64/256)";
                    info.innerHTML = `Identified: ${dName}`; 
                    document.getElementById('eepromSize').value = data.chip; 
                    await pushConfig(); 
                } else { info.innerHTML = "No 24xx chip found."; } 
            } catch(e) {}
        }

        async function detect93xx() {
            let info = document.getElementById('chip-info'); info.innerHTML = "Analyzing..."; 
            try { 
                let res = await fetch('/detect_93xx'); let data = await res.json();
                if(data.found) { 
                    info.innerHTML = `Auto-Detected: ${data.chip.replace("_", "C")}`; 
                    document.getElementById('eepromSize').value = data.chip; 
                    await pushConfig(); 
                } else { info.innerHTML = "Chip not responding."; } 
            } catch(e) {}
        }
        updateChipOptions();

        async function readEEPROM() {
            await pushConfig(); let totalSize = sizeMap[document.getElementById('eepromSize').value]; hexBuffer = new Uint8Array(totalSize); let chunkSize = 128; document.getElementById('progressBar').style.width = '0%';
            for(let addr = 0; addr < totalSize; addr += chunkSize) {
                let len = Math.min(chunkSize, totalSize - addr); let res = await fetch(`/api_web_read?addr=${addr}&len=${len}`);
                if(!res.ok) { alert("Read failed."); break; } let data = await res.json();
                for(let i=0; i<data.data.length; i++) { hexBuffer[addr + i] = data.data[i]; }
                document.getElementById('progressBar').style.width = (((addr + len) / totalSize) * 100) + '%';
            }
            fetch('/api/set_status?s=SUCCESS'); renderHexFast(hexBuffer); 
        }

        async function writeEEPROM() {
            if (hexBuffer.length === 0) return alert('Buffer empty.'); await pushConfig(); let totalSize = sizeMap[document.getElementById('eepromSize').value];
            document.getElementById('progressBar').style.width = '0%'; let chunkSize = 64; 
            for (let addr = 0; addr < totalSize; addr += chunkSize) {
                let chunk = hexBuffer.slice(addr, addr + chunkSize); if (chunk.length === 0) break;
                let res = await fetch('/api_web_write', { method: 'POST', body: JSON.stringify({ addr: addr, data: Array.from(chunk) }) });
                if (!res.ok) { alert('Write error.'); break; } document.getElementById('progressBar').style.width = (((addr + chunk.length) / totalSize) * 100) + '%';
            }
            fetch('/api/set_status?s=SUCCESS'); alert('Write Successful!');
        }

        async function eraseEEPROM() {
            if (!confirm('WARNING: Erasing chip to 0xFF?')) return;
            let totalSize = sizeMap[document.getElementById('eepromSize').value]; hexBuffer = new Uint8Array(totalSize).fill(0xFF); renderHexFast(hexBuffer); await writeEEPROM();
        }

        function renderHexFast(buffer) {
            let lines = [];
            if(showAscii) lines.push(`Address  | Hex Data                                         | ASCII\n-------------------------------------------------------------------`);
            else lines.push(`Address  | Hex Data\n-----------------------------------------------------------`);
            for (let i = 0; i < buffer.length; i += 16) {
                let addr = i.toString(16).padStart(4, '0').toUpperCase(); let hexArr = []; let asciiLine = "";
                for (let j = 0; j < 16; j++) {
                    if (i + j < buffer.length) { 
                        let val = buffer[i+j]; hexArr.push(val.toString(16).padStart(2, '0').toUpperCase()); 
                        asciiLine += (val >= 32 && val <= 126) ? String.fromCharCode(val) : '.'; 
                    } else { hexArr.push("  "); asciiLine += ' '; }
                }
                let hexStr = hexArr.join(' ');
                if(showAscii) { let safeAscii = asciiLine.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); lines.push(`<span class="hex-addr">${addr}</span>     | <span class="hex-data">${hexStr}</span> | <span class="hex-ascii">${safeAscii}</span>`); } 
                else { lines.push(`<span class="hex-addr">${addr}</span>     | <span class="hex-data">${hexStr}</span>`); }
            }
            document.getElementById('hexViewer').innerHTML = lines.join('\n');
        }

        function handleFileUpload(event) {
            const file = event.target.files[0]; if (!file) return;
            let rawName = file.name; if(rawName.endsWith('.bin') || rawName.endsWith('.hex')) rawName = rawName.substring(0, rawName.lastIndexOf('.'));
            document.getElementById('saveFileName').value = rawName;
            const reader = new FileReader(); reader.onload = function(e) { hexBuffer = new Uint8Array(e.target.result); renderHexFast(hexBuffer); alert('File Loaded.'); }; reader.readAsArrayBuffer(file);
        }

        function downloadDump() {
            if(hexBuffer.length === 0) return; let customName = document.getElementById('saveFileName').value.trim();
            if (customName === "") customName = document.getElementById('eepromSize').value + "_dump";
            if(!customName.toLowerCase().endsWith('.bin')) customName += ".bin";
            const blob = new Blob([hexBuffer], { type: 'application/octet-stream' });
            const url = URL.createObjectURL(blob); const a = document.createElement('a'); a.href = url; a.download = customName; a.click(); URL.revokeObjectURL(url);
        }
    </script>
</body>
</html>
)rawliteral";

// ==========================================
// CLOUD OTA UPDATE LOGIC
// ==========================================
void handleCloudOTA() {
    if (!isActivated) return server.send(403, "text/plain", "Locked");
    String clientSSID = server.arg("ssid");
    String clientPASS = server.arg("pass");

    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    // YAHAN APNA GITHUB RAW LINK DAALEIN (update.bin ka direct link)
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    String firmwareURL = "https://raw.githubusercontent.com/YourUsername/SmartProg-Updates/main/update.bin";

    server.send(200, "text/plain", "Updating Firmware... Please wait 2-3 minutes. DO NOT POWER OFF! Device will restart automatically.");
    
    currentAction = "UPDATING"; updateOLED();

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(clientSSID.c_str(), clientPASS.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500); attempts++; ledFlicker();
    }

    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure(); // GitHub HTTPS ke liye zaruri hai
        t_httpUpdate_return ret = ESPhttpUpdate.update(client, firmwareURL);

        if (ret == HTTP_UPDATE_FAILED) { currentAction = "ERROR"; updateOLED(); delay(4000); }
    } else {
        currentAction = "WIFI FAIL"; updateOLED(); delay(4000);
    }
    
    WiFi.mode(WIFI_AP);
    currentAction = "READY"; updateOLED();
}

// ==========================================
// API & WEB HANDLERS
// ==========================================
void handleDetailDataRequest() {
    if (!isActivated) return server.send(403, "text/plain", "Locked");
    String payload = server.arg("plain"); StaticJsonDocument<256> jsonDoc; deserializeJson(jsonDoc, payload);
    mainMode = jsonDoc["main_mode"] | ""; eepromMode = jsonDoc["eeprom_mode"] | "";
    if(jsonDoc.containsKey("eeprom_family")) eepromFamily = jsonDoc["eeprom_family"].as<String>();
    if(jsonDoc.containsKey("eeprom_org")) eepromOrg = jsonDoc["eeprom_org"].as<String>();
    eepromSize = jsonDoc["eeprom_size"] | "";
    setupHardwarePins(); updateOLED(); server.send(200, "text/plain", "OK");
}

void handleWebApiRead() {
    if (!isActivated) return server.send(403, "application/json", "{\"error\": \"Locked\"}");
    uint16_t startAddr = server.arg("addr").toInt(); uint16_t len = server.arg("len").toInt();
    if(len > 256) len = 256; 
    
    totalBytesActive = getTotalSizeGlobal(); currentAction = "READING"; currentAddress = startAddr;
    progressPct = ((startAddr + len) * 100) / totalBytesActive; updateOLED();
    
    uint8_t buffer[256]; bool success = false;
    if (eepromFamily == "93xx") success = read93XXBlock(startAddr, buffer, len);
    else {
        size_t bytesRead = 0; success = true;
        while(bytesRead < len) {
            ledFlicker(); yield(); size_t pageSz = getEEPROMPageSize(); 
            uint16_t currentWriteAddr = startAddr + bytesRead;
            size_t offset = currentWriteAddr % pageSz; 
            size_t toRead = pageSz - offset; 
            if(toRead > (len - bytesRead)) toRead = len - bytesRead;
            
            if(!readEEPROMPage(currentWriteAddr, &buffer[bytesRead], toRead)) { success = false; break; }
            bytesRead += toRead;
        }
    }
    
    if(success) {
        DynamicJsonDocument doc(1024); JsonArray arr = doc.createNestedArray("data");
        for (size_t i = 0; i < len; i++) arr.add(buffer[i]);
        String res; serializeJson(doc, res); server.send(200, "application/json", res);
    } else { server.send(500, "application/json", "{\"error\": \"Read failed\"}"); }
}

void handleWebApiWrite() {
    if (!isActivated) return server.send(403, "text/plain", "Locked");
    DynamicJsonDocument doc(1024); deserializeJson(doc, server.arg("plain"));
    uint16_t startAddr = doc["addr"].as<uint16_t>(); JsonArray dataArr = doc["data"].as<JsonArray>(); 
    size_t len = dataArr.size();
    
    totalBytesActive = getTotalSizeGlobal(); currentAction = "WRITING"; currentAddress = startAddr;
    progressPct = ((startAddr + len) * 100) / totalBytesActive; updateOLED();
    
    uint8_t bytes[256]; for (size_t i = 0; i < len; i++) bytes[i] = dataArr[i].as<uint8_t>();
    bool success = false;
    
    if (eepromFamily == "93xx") success = write93XXBlock(startAddr, bytes, len);
    else {
        size_t bytesWritten = 0; success = true;
        while(bytesWritten < len) {
            ledFlicker(); yield(); size_t pageSz = getEEPROMPageSize(); 
            uint16_t currentWriteAddr = startAddr + bytesWritten;
            
            size_t offset = currentWriteAddr % pageSz;
            size_t toWrite = pageSz - offset;
            if(toWrite > (len - bytesWritten)) toWrite = len - bytesWritten;
            
            if(!writeEEPROMPage(currentWriteAddr, &bytes[bytesWritten], toWrite)) { success = false; break; }
            bytesWritten += toWrite;
        }
    }
    if(success) server.send(200, "text/plain", "OK"); else server.send(500, "text/plain", "Fail");
}

void handleDetectChip() {
    setupHardwarePins(); 
    delay(10); 
    
    StaticJsonDocument<256> doc; bool respond[8]; int count = 0;
    for(int i=0; i<8; i++) { Wire.beginTransmission(0x50 + i); respond[i] = (Wire.endTransmission() == 0); if(respond[i]) count++; }
    
    if (count > 0) {
        doc["found"] = true; String chipName = "24C02"; 
        if(count == 8) chipName = "24C16"; else if(count == 4 && respond[0] && respond[1] && respond[2] && respond[3]) chipName = "24C08";
        else if(count == 2 && respond[0] && respond[1]) chipName = "24C04"; else if(count == 1 && respond[0]) chipName = "24C02"; 
        doc["chip"] = chipName;
    } else { doc["found"] = false; }
    String res; serializeJson(doc, res); server.send(200, "application/json", res);
}

void handleDetect93xx() {
    String originalSize = eepromSize; 
    String originalOrg = eepromOrg;

    eepromOrg = "x16"; 
    setupHardwarePins(); 
    delay(10);
    
    StaticJsonDocument<256> doc;
    
    eepromSize = "93_46"; 
    uint16_t a = EEPROM_Read_93xx(0x00); uint16_t b = EEPROM_Read_93xx(0x40);
    if(a == b && a != 0xFFFF && a != 0x0000) { doc["found"] = true; doc["chip"] = "93_46"; } 
    else {
        eepromSize = "93_56"; 
        a = EEPROM_Read_93xx(0x00); b = EEPROM_Read_93xx(0x80);
        if(a == b && a != 0xFFFF && a != 0x0000) { doc["found"] = true; doc["chip"] = "93_56"; } 
        else {
            eepromSize = "93_76"; 
            a = EEPROM_Read_93xx(0x00); b = EEPROM_Read_93xx(0x200);
            if(a == b && a != 0xFFFF && a != 0x0000) { doc["found"] = true; doc["chip"] = "93_76"; } 
            else { doc["found"] = false; } 
        }
    }
    
    eepromSize = originalSize;
    eepromOrg = originalOrg;
    setupHardwarePins();
    
    String res; serializeJson(doc, res); server.send(200, "application/json", res);
}

void handleStatusSync() {
    String json = "{";
    json += "\"act\":\"" + currentAction + "\",\"chip\":\"" + eepromSize + "\",\"fam\":\"" + eepromFamily + "\",";
    json += "\"bat\":" + String(getBatteryPercentage()) + ",\"lic\":" + String(isActivated ? "true" : "false") + ",";
    json += "\"addr\":" + String(currentAddress) + ",\"total\":" + String(totalBytesActive) + ",\"pct\":" + String(progressPct);
    json += "}";
    server.send(200, "application/json", json);
}

void handleSetStatus() {
    String st = server.arg("s");
    if(st == "SUCCESS") { currentAction = "SUCCESS"; updateOLED(); successTimer = millis(); }
    server.send(200, "text/plain", "OK");
}

void handleActivationPOST() {
    uint32_t enteredKey = strtoul(server.arg("key").c_str(), NULL, 10);
    uint32_t correctKey = ((ESP.getChipId() ^ 0x8F3B9A2C) << 3) ^ 0x7860;
    if (enteredKey == correctKey) { EEPROM.write(0, 11); EEPROM.write(1, 22); EEPROM.commit(); isActivated = true; server.send(200, "text/plain", "OK"); } 
    else { server.send(403, "text/plain", "FAIL"); }
}

void handleRoot() {
    if (isActivated) { server.send(200, "text/html", index_html); } 
    else { String page = FPSTR(activation_html); page.replace("{DEVICE_ID}", String(ESP.getChipId())); server.send(200, "text/html", page); }
}

// ==========================================
// SETUP & MAIN LOOP
// ==========================================
void setup() {
    Serial.begin(115200);
    initStatusLED(); 
    
    Wire.begin(PIN_D1, PIN_D2);  
    setupHardwarePins(); 

    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Init Failed");
    
    EEPROM.begin(512);
    if(EEPROM.read(0) == 11 && EEPROM.read(1) == 22) isActivated = true;

    WiFi.softAP(ssid, password);
    
    server.on("/", HTTP_GET, handleRoot);
    server.on("/activate", HTTP_POST, handleActivationPOST);
    server.on("/detail_data", HTTP_POST, handleDetailDataRequest);
    server.on("/api_web_read", HTTP_GET, handleWebApiRead);
    server.on("/api_web_write", HTTP_POST, handleWebApiWrite);
    server.on("/detect_chip", HTTP_GET, handleDetectChip);
    server.on("/detect_93xx", HTTP_GET, handleDetect93xx);
    server.on("/api/status", HTTP_GET, handleStatusSync);
    server.on("/api/set_status", HTTP_GET, handleSetStatus);
    server.on("/start_ota", HTTP_POST, handleCloudOTA);

    server.begin();
    updateOLED(); 
}

void loop() {
    server.handleClient();
    updateStatusLED(); 
    if(currentAction == "SUCCESS" && millis() - successTimer > 2500) { currentAction = "READY"; updateOLED(); }
}
