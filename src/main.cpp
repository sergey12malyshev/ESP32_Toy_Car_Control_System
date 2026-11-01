#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#include <Update.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ===== БАТАРЕЯ (SMBus) =====
#include <Wire.h>
#include <esp_task_wdt.h>


// Глобальная переменная для времени последней команды
static volatile uint32_t lastCommandTime = 0;


// Pin Definitions
#define PIN_FORWARD     18
#define PIN_BACKWARD    5
#define PIN_LEFT        19
#define PIN_RIGHT       23
#define PIN_STOP_RIGHT  15
#define PIN_STOP_LEFT   2
#define PIN_SOUND       4

// PWM Settings
#define PWM_FREQ        20000
#define PWM_RESOLUTION  8
#define PWM_CH_DRIVE    0
#define PWM_CH_STEER    1

#define PWM_MAX_DRIVE    250
#define PWM_MAX_STEER    250

#define PWM_MIN_DRIVE    1
#define PWM_MIN_STEER    1

// ===== БАТАРЕЯ (SMBus) =====
// SMBD (данные) -> D22 -> GPIO22
// SMBC (тактовая) -> D21 -> GPIO21
#define SMBUS_SDA_PIN  21
#define SMBUS_SCL_PIN  22
#define BATTERY_ADDR   0x0B    // Стандартный адрес SMBus-батареи

// Регистры SBS (Smart Battery System)
#define REG_TEMPERATURE      0x08
#define REG_VOLTAGE          0x09
#define REG_CURRENT          0x0A
#define REG_RELATIVE_SOC     0x0D

// Последние прочитанные значения
static volatile uint16_t batteryVoltage_mV   = 0;
static volatile int16_t  batteryCurrent_mA   = 0;
static volatile int16_t  batteryTemperature_C = 0;
static volatile uint8_t  batterySOC_percent  = 0;
static volatile bool     batteryDataValid    = false;

// Мьютекс для доступа к данным батареи (из веб-обработчиков и задачи)
static SemaphoreHandle_t batteryDataMutex = NULL;


// Wi-Fi Access Point
const char* ssid = "ESP32_Car";
const char* password = "12345678";

WebServer server(80);

uint8_t currentDriveSpeedTarget = 185;   // текущая целевая скорость (0-255)
uint8_t currentSteerIntensity = 215;     // интенсивность поворота (0-255)

struct Version
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
};

Version version = {0, 1, 0};

// Low-Level Motor Control
void initPWM() 
{
    ledcSetup(PWM_CH_DRIVE, PWM_FREQ, PWM_RESOLUTION);
    ledcSetup(PWM_CH_STEER, PWM_FREQ, PWM_RESOLUTION);
}

void moveForward(uint8_t speed) 
{
    ledcDetachPin(PIN_BACKWARD);
    ledcAttachPin(PIN_FORWARD, PWM_CH_DRIVE);
    ledcWrite(PWM_CH_DRIVE, speed);
    digitalWrite(PIN_BACKWARD, LOW);
}

void moveBackward(uint8_t speed) 
{
    ledcDetachPin(PIN_FORWARD);
    ledcAttachPin(PIN_BACKWARD, PWM_CH_DRIVE);
    ledcWrite(PWM_CH_DRIVE, speed);
    digitalWrite(PIN_FORWARD, LOW);
}

void softStartForward(uint8_t targetSpeed, uint16_t rampTime_ms) 
{
    if (targetSpeed == 0) 
    {
      return;
    }

    if (targetSpeed > PWM_MAX_DRIVE)
    {
      targetSpeed = (uint8_t) PWM_MAX_DRIVE;
    }

    uint16_t steps = 40;
    uint16_t stepDelay = rampTime_ms / steps;
    for (uint8_t i = 1; i <= steps; i++) 
    {
        uint8_t sp = (uint32_t)targetSpeed * i / steps;
        moveForward(sp);
        delay(stepDelay);
    }
    moveForward(targetSpeed);
}

void softStartBackward(uint8_t targetSpeed, uint16_t rampTime_ms) 
{
    if (targetSpeed == 0) 
    {
        return;
    }f

    if (targetSpeed > PWM_MAX_DRIVE)
    {
      targetSpeed = (uint8_t) PWM_MAX_DRIVE;
    }

    uint16_t steps = 40;
    uint16_t stepDelay = rampTime_ms / steps;
    for (uint8_t i = 1; i <= steps; i++) 
    {
        uint8_t sp = (uint32_t)targetSpeed * i / steps;
        moveBackward(sp);
        delay(stepDelay);
    }
    moveBackward(targetSpeed);
}

void stopMovement() 
{
    ledcWrite(PWM_CH_DRIVE, 0);
}

void turnLeft(uint8_t intensity) 
{
    if (intensity > PWM_MAX_STEER)
    {
      intensity = (uint8_t) PWM_MAX_STEER;
    }

    ledcDetachPin(PIN_RIGHT);
    ledcAttachPin(PIN_LEFT, PWM_CH_STEER);
    ledcWrite(PWM_CH_STEER, intensity);
    digitalWrite(PIN_RIGHT, LOW);
}

void turnRight(uint8_t intensity) 
{
    if (intensity > PWM_MAX_STEER)
    {
      intensity = (uint8_t) PWM_MAX_STEER;
    }

    ledcDetachPin(PIN_LEFT);
    ledcAttachPin(PIN_RIGHT, PWM_CH_STEER);
    ledcWrite(PWM_CH_STEER, intensity);
    digitalWrite(PIN_LEFT, LOW);
}

void straightWheels() 
{
    ledcWrite(PWM_CH_STEER, 0);
}

void stopLightsOn() 
{
    digitalWrite(PIN_STOP_LEFT, HIGH);
    digitalWrite(PIN_STOP_RIGHT, HIGH);
}

void stopLightsOff() 
{
    digitalWrite(PIN_STOP_LEFT, LOW);
    digitalWrite(PIN_STOP_RIGHT, LOW);
}

void soundOn()  { digitalWrite(PIN_SOUND, HIGH); }
void soundOff() { digitalWrite(PIN_SOUND, LOW);  }

void allOff() 
{
    stopMovement();
    straightWheels();
    stopLightsOff();
    soundOff();
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
    <title>ESP32 Car Control</title>
    <style>
        * { user-select: none; touch-action: none; }
        body {
            background: #1a1a2e;
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            font-family: Arial, Helvetica, sans-serif;
            margin: 0;
            padding: 20px;
        }
        .container {
            background: #16213e;
            border-radius: 40px;
            padding: 20px;
            box-shadow: 0 10px 25px rgba(0,0,0,0.5);
            max-width: 500px;
            width: 100%;
        }
        h1 {
            text-align: center;
            color: #e94560;
            margin: 0 0 10px 0;
            font-size: 1.8rem;
        }
        .sub {
            text-align: center;
            color: #aaa;
            margin-bottom: 20px;
            font-size: 0.8rem;
        }
        .dpad {
            display: flex;
            flex-direction: column;
            align-items: center;
            margin-bottom: 20px;
        }
        .dpad-row {
            display: flex;
            justify-content: center;
        }
        .dpad-btn {
            background: #0f3460;
            width: 80px;
            height: 80px;
            margin: 5px;
            border-radius: 20px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 2rem;
            font-weight: bold;
            color: white;
            cursor: pointer;
            transition: 0.1s;
            box-shadow: 0 5px 0 #0a1c2e;
            touch-action: manipulation;
        }
        .dpad-btn:active {
            transform: translateY(2px);
            box-shadow: 0 2px 0 #0a1c2e;
            background: #e94560;
        }
        .stop-btn {
            background: #e94560;
            box-shadow: 0 5px 0 #9e2a3e;
        }
        .slider-container {
            background: #0f3460;
            border-radius: 30px;
            padding: 15px;
            margin: 15px 0;
        }
        .slider-label {
            color: #ddd;
            display: flex;
            justify-content: space-between;
            margin-bottom: 5px;
            font-size: 1rem;
        }
        input[type=range] {
            width: 100%;
            height: 8px;
            -webkit-appearance: none;
            background: #e94560;
            border-radius: 5px;
            outline: none;
        }
        input[type=range]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 25px;
            height: 25px;
            background: white;
            border-radius: 50%;
            cursor: pointer;
        }
        .button-row {
            display: flex;
            justify-content: space-between;
            gap: 10px;
            margin-top: 15px;
        }
        .action-btn {
            background: #0f3460;
            flex: 1;
            text-align: center;
            padding: 12px;
            border-radius: 30px;
            color: white;
            font-weight: bold;
            cursor: pointer;
            transition: 0.1s;
            box-shadow: 0 3px 0 #0a1c2e;
            font-size: 0.9rem;
        }
        .action-btn:active {
            transform: translateY(2px);
            box-shadow: 0 1px 0 #0a1c2e;
        }
        .status {
            text-align: center;
            margin-top: 15px;
            color: #aaa;
            font-size: 0.8rem;
        }
        .battery-panel {
            background: #0f3460;
            border-radius: 20px;
            padding: 15px;
            margin: 15px 0;
            color: #ddd;
            font-size: 0.95rem;
        }
        .battery-panel h3 {
            margin: 0 0 10px 0;
            color: #e94560;
            font-size: 1rem;
            text-align: center;
        }
        .battery-row {
            display: flex;
            justify-content: space-between;
            margin: 4px 0;
        }
        .battery-value { color: #4ade80; font-weight: bold; }
        .bar-wrap {
            background: #0a1c2e;
            border-radius: 10px;
            height: 14px;
            margin-top: 8px;
            overflow: hidden;
        }
        .bar-fill {
            height: 100%;
            background: linear-gradient(90deg, #e94560, #4ade80);
            width: 0%;
            transition: width 0.4s;
        }
        @media (max-width: 480px) {
            .dpad-btn { width: 70px; height: 70px; font-size: 1.6rem; }
        }
    </style>
</head>
<body>
<div class="container">
    <h1>ESP32 CAR</h1>
    <div class="sub">Wi-Fi Remote Control</div>

    <div class="dpad">
        <div class="dpad-row">
            <div class="dpad-btn" id="btn-up">UP</div>
        </div>
        <div class="dpad-row">
            <div class="dpad-btn" id="btn-left">L</div>
            <div class="dpad-btn stop-btn" id="btn-stop">STOP</div>
            <div class="dpad-btn" id="btn-right">R</div>
        </div>
        <div class="dpad-row">
            <div class="dpad-btn" id="btn-down">DN</div>
        </div>
    </div>

    <div class="slider-container">
        <div class="slider-label"><span>Max Speed</span><span id="speedValue">185</span></div>
        <input type="range" id="speedSlider" min="100" max="250" value="185">
    </div>
    <div class="slider-container">
        <div class="slider-label"><span>Steer Intensity</span><span id="steerValue">215</span></div>
        <input type="range" id="steerSlider" min="100" max="250" value="215">
    </div>

    <div class="button-row">
        <div class="action-btn" id="btn-sound">HORN</div>
        <div class="action-btn" id="btn-lights-on">LIGHTS ON</div>
        <div class="action-btn" id="btn-lights-off">LIGHTS OFF</div>
    </div>

    <div class="battery-panel">
        <h3>BATTERY (RRC2054)</h3>
        <div class="battery-row"><span>Voltage:</span><span class="battery-value" id="bat-v">-- mV</span></div>
        <div class="battery-row"><span>Current:</span><span class="battery-value" id="bat-i">-- mA</span></div>
        <div class="battery-row"><span>Temperature:</span><span class="battery-value" id="bat-t">-- &deg;C</span></div>
        <div class="battery-row"><span>Charge:</span><span class="battery-value" id="bat-soc">-- %</span></div>
        <div class="bar-wrap"><div class="bar-fill" id="bat-bar"></div></div>
    </div>

    <div class="status" id="status">Ready</div>
</div>

<script>
    async function sendCmd(cmd, value=null) {
        let url = '/cmd?q=' + cmd;
        if (value !== null) url += '&v=' + value;
        try {
            let resp = await fetch(url);
            let txt = await resp.text();
            document.getElementById('status').innerText = 'Command: ' + cmd + (value!==null? ' '+value : '');
        } catch(e) { console.log(e); }
    }

    const up = document.getElementById('btn-up');
    const down = document.getElementById('btn-down');
    const left = document.getElementById('btn-left');
    const right = document.getElementById('btn-right');
    const stopBtn = document.getElementById('btn-stop');

    up.addEventListener('mousedown', () => sendCmd('forward'));
    up.addEventListener('mouseup', () => sendCmd('stop'));
    up.addEventListener('touchstart', (e) => { e.preventDefault(); sendCmd('forward'); });
    up.addEventListener('touchend', (e) => { e.preventDefault(); sendCmd('stop'); });
    
    down.addEventListener('mousedown', () => sendCmd('backward'));
    down.addEventListener('mouseup', () => sendCmd('stop'));
    down.addEventListener('touchstart', (e) => { e.preventDefault(); sendCmd('backward'); });
    down.addEventListener('touchend', (e) => { e.preventDefault(); sendCmd('stop'); });
    
    left.addEventListener('mousedown', () => sendCmd('left'));
    left.addEventListener('mouseup', () => sendCmd('straight'));
    left.addEventListener('touchstart', (e) => { e.preventDefault(); sendCmd('left'); });
    left.addEventListener('touchend', (e) => { e.preventDefault(); sendCmd('straight'); });
    
    right.addEventListener('mousedown', () => sendCmd('right'));
    right.addEventListener('mouseup', () => sendCmd('straight'));
    right.addEventListener('touchstart', (e) => { e.preventDefault(); sendCmd('right'); });
    right.addEventListener('touchend', (e) => { e.preventDefault(); sendCmd('straight'); });
    
    stopBtn.addEventListener('click', () => sendCmd('stop'));
    stopBtn.addEventListener('touchstart', (e) => { e.preventDefault(); sendCmd('stop'); });
    
    document.getElementById('btn-sound').addEventListener('click', () => sendCmd('sound'));
    document.getElementById('btn-lights-on').addEventListener('click', () => sendCmd('lights_on'));
    document.getElementById('btn-lights-off').addEventListener('click', () => sendCmd('lights_off'));
    
    const speedSlider = document.getElementById('speedSlider');
    const steerSlider = document.getElementById('steerSlider');
    const speedValue = document.getElementById('speedValue');
    const steerValue = document.getElementById('steerValue');
    
    speedSlider.addEventListener('input', () => {
        let val = speedSlider.value;
        speedValue.innerText = val;
        sendCmd('speed', val);
    });
    steerSlider.addEventListener('input', () => {
        let val = steerSlider.value;
        steerValue.innerText = val;
        sendCmd('steer_intensity', val);
    });

    // ===== Обновление данных батареи =====
    async function updateBattery() {
        try {
            const r = await fetch('/battery');
            const d = await r.json();
            document.getElementById('bat-v').innerText = d.voltage_mv + ' mV';
            document.getElementById('bat-i').innerText = d.current_ma + ' mA';
            document.getElementById('bat-t').innerText = d.temperature_c + ' \u00B0C';
            document.getElementById('bat-soc').innerText = d.soc + ' %';
            document.getElementById('bat-bar').style.width = d.soc + '%';
        } catch(e) { /* нет связи — просто пропускаем */ }
    }
    setInterval(updateBattery, 2000);
    window.addEventListener('load', updateBattery);
    
    window.addEventListener('beforeunload', () => sendCmd('stop'));
</script>
</body>
</html>
)rawliteral";

// Web Server Handlers
void handleRoot() {
    server.send(200, "text/html", index_html);
}

// ===== БАТАРЕЯ: HTTP JSON endpoint =====
void handleBattery() {
    uint16_t v; int16_t i, t; uint8_t soc; bool ok;
    if (xSemaphoreTake(batteryDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        v   = batteryVoltage_mV;
        i   = batteryCurrent_mA;
        t   = batteryTemperature_C;
        soc = batterySOC_percent;
        ok  = batteryDataValid;
        xSemaphoreGive(batteryDataMutex);
    } else {
        v = 0; i = 0; t = 0; soc = 0; ok = false;
    }

    char json[160];
    snprintf(json, sizeof(json),
             "{\"voltage_mv\":%u,\"current_ma\":%d,\"temperature_c\":%d,"
             "\"soc\":%u,\"valid\":%s}",
             v, i, t, soc, ok ? "true" : "false");
    server.send(200, "application/json", json);
}

void handleCmd() {

    lastCommandTime = millis();

    if (!server.hasArg("q")) {
        server.send(400, "text/plain", "Missing command");
        return;
    }
    String cmd = server.arg("q");
    if (cmd == "forward") {
        softStartForward(currentDriveSpeedTarget, 500); // плавный разгон 500 мс
        server.send(200, "text/plain", "OK");
    } 
    else if (cmd == "backward") {
        softStartBackward(currentDriveSpeedTarget, 500);
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "stop") {
        stopMovement();
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "left") {
        turnLeft(currentSteerIntensity);
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "right") {
        turnRight(currentSteerIntensity);
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "straight") {
        straightWheels();
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "sound") {
        soundOn();
        delay(265);
        soundOff();
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "lights_on") {
        stopLightsOn();
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "lights_off") {
        stopLightsOff();
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "speed") {
        if (server.hasArg("v")) {
            currentDriveSpeedTarget = constrain(server.arg("v").toInt(), 100, 250);
        }
        server.send(200, "text/plain", "OK");
    }
    else if (cmd == "steer_intensity") {
        if (server.hasArg("v")) {
            currentSteerIntensity = constrain(server.arg("v").toInt(), 100, 250);
        }
        server.send(200, "text/plain", "OK");
    }
    else {
        server.send(400, "text/plain", "Unknown command");
    }
}

void initWiFiAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
}

// ===== БАТАРЕЯ: чтение слова SMBus =====
bool smbusReadWord(uint8_t reg, uint16_t &value) {
    Wire.beginTransmission(BATTERY_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((uint8_t)BATTERY_ADDR, (uint8_t)2) != 2) {
        return false;
    }
    uint8_t low  = Wire.read();
    uint8_t high = Wire.read();
    value = ((uint16_t)high << 8) | low;   // SMBus: сначала младший байт
    return true;
}

// ===== БАТАРЕЯ: задача FreeRTOS =====
void batteryTask(void * parameter) {
    const TickType_t xFrequency = pdMS_TO_TICKS(2000); // опрос каждые 2 с
    TickType_t xLastWakeTime = xTaskGetTickCount();
    uint32_t consecutiveErrors = 0;

    for (;;) {
        uint16_t raw;
        uint16_t v_mV = 0;
        int16_t  i_mA = 0;
        int16_t  t_C  = 0;
        uint8_t  soc  = 0;
        bool     anyOk = false;

        // Напряжение (мВ)
        if (smbusReadWord(REG_VOLTAGE, raw)) {
            v_mV = raw;
            anyOk = true;
        }

        // Ток (мА, знаковый)
        if (smbusReadWord(REG_CURRENT, raw)) {
            i_mA = (int16_t)raw;
            anyOk = true;
        }

        // Температура (0.1 K -> °C)
        if (smbusReadWord(REG_TEMPERATURE, raw)) {
            t_C = (int16_t)(raw / 10) - 273;
            anyOk = true;
        }

        // Относительный заряд RSOC (0..100 %)
        if (smbusReadWord(REG_RELATIVE_SOC, raw)) {
            soc = (uint8_t)(raw > 100 ? 100 : raw);
            anyOk = true;
        }

        if (xSemaphoreTake(batteryDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (anyOk) {
                batteryVoltage_mV    = v_mV;
                batteryCurrent_mA    = i_mA;
                batteryTemperature_C = t_C;
                batterySOC_percent   = soc;
                batteryDataValid     = true;
            } else {
                batteryDataValid     = false;
            }
            xSemaphoreGive(batteryDataMutex);
        }

        if (anyOk) {
            consecutiveErrors = 0;
            Serial.printf("Battery: %u mV, %d mA, %d C, %u %%\n",
                          v_mV, i_mA, t_C, soc);
        } else {
            if (consecutiveErrors < 5) {
                Serial.println("Battery: SMBus read error");
            }
            consecutiveErrors++;
        }

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void safetyTask(void * parameter) {

    const uint32_t COMMAND_TIMEOUT = 30000; // 30 секунд Таймаут бездействия (в миллисекундах)

    vTaskDelay(10000 / portTICK_PERIOD_MS);

    for (;;) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        
        // Количество подключённых клиентов
        int clients = WiFi.softAPgetStationNum();
        // Время с последней команды
        uint32_t now = (uint32_t) millis();
        uint32_t elapsed = now - lastCommandTime;
        
        if (clients == 0 || elapsed > COMMAND_TIMEOUT) {
            // Аварийная остановка (idempotent, можно вызывать много раз)
            allOff();

            soundOn();
            delay(100);
            Serial.printf("Not connect!");
            soundOff();
            stopLightsOn();
        }
    }
}

void setup() {
    Serial.begin(115200);
    initPWM();
    pinMode(PIN_FORWARD, OUTPUT);
    pinMode(PIN_BACKWARD, OUTPUT);
    pinMode(PIN_LEFT, OUTPUT);
    pinMode(PIN_RIGHT, OUTPUT);
    pinMode(PIN_STOP_LEFT, OUTPUT);
    pinMode(PIN_STOP_RIGHT, OUTPUT);
    pinMode(PIN_SOUND, OUTPUT);
    allOff();

    soundOn();
    delay(100);
    Serial.printf("Firmware version: %d.%d.%d\n", version.major, version.minor, version.patch);

    soundOff();
    delay(1000);

    // ===== БАТАРЕЯ: инициализация I2C / SMBus =====
    // Внешние подтяжки 1.5 кОм уже установлены, поэтому внутренние не включаем
    pinMode(SMBUS_SDA_PIN, INPUT_PULLUP);
    pinMode(SMBUS_SCL_PIN, INPUT_PULLUP);
    Wire.begin(SMBUS_SDA_PIN, SMBUS_SCL_PIN);
    Wire.setClock(100000);   // SMBus: 100 кГц
    Serial.println("SMBus initialized (SDA=GPIO22/D22, SCL=GPIO21/D21)");

    // Мьютекс для доступа к данным батареи
    batteryDataMutex = xSemaphoreCreateMutex();

    // Задача чтения батареи — на ядре 0
    xTaskCreatePinnedToCore(
        batteryTask,
        "BatteryTask",
        4096,
        NULL,
        1,
        NULL,
        0
    );
    Serial.println("Battery task started");

    initWiFiAP();
    server.on("/", handleRoot);
    server.on("/cmd", handleCmd);
    server.on("/battery", handleBattery);   // <-- новый endpoint

    lastCommandTime = millis();

    // OTA Update
    // Страница с формой для загрузки (GET)
    server.on("/update", HTTP_GET, []() {
        server.send(200, "text/html",
            "<!DOCTYPE html>"
            "<html><head><meta charset='UTF-8'><title>OTA Update</title>"
            "<style>body{background:#1a1a2e;color:white;font-family:Arial;padding:20px;}</style>"
            "</head><body>"
            "<h2>ESP32 Car - Firmware Update</h2>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin'>"
            "<input type='submit' value='Upload and Update'>"
            "</form>"
            "</body></html>"
        );
    });

    // Обработка POST-запроса с файлом
    server.on("/update", HTTP_POST, []() {
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u bytes\nRebooting...\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });

    server.begin();
    Serial.println("HTTP server started");

    // Создаём задачу безопасности на втором ядре (ядро 1)
    xTaskCreatePinnedToCore(
        safetyTask,      // функция задачи
        "SafetyTask",    // имя
        2048,            // размер стека
        NULL,            // параметр
        1,               // приоритет (низкий)
        NULL,            // handle
        1                // ядро (1)
    );
    Serial.println("Safety task started");
}


void loop() {
    server.handleClient();
    delay(10);
}