# Electric Vehicle Control System (EN)

Firmware for controlling an electric vehicle based on ESP32.
The device creates its own Wi-Fi access point with a web interface, controls motors via PWM, reads RRC2054 battery telemetry using the SMBus protocol, and supports OTA firmware updates over the air.

## 🛠️ Toolkit

- PlatformIO (Core 6.1.11, Home 3.4.4)
- VS Code + PlatformIO IDE extension
- Framework: Arduino (for ESP32)
- Board: ESP32_DevKitC_V4

### Build and upload

```bash
# Build
pio run

# Upload via USB
pio run -t upload

# Port monitor (115200 baud)
pio device monitor
```

### 🔄 OTA update

1. Connect to Wi-Fi ESP32_Car (see password below).
2. Open in the browser http://192.168.4.1/update
3. Select the .bin file (is built automatically in .pio/build/<env>/firmware.bin) and upload it.
4. ESP32 will reboot automatically.

## Connecting to Wi-Fi

ESP32 operates in access point (AP) mode:

| Parameter | Value |
|---|---|
| SSID | ESP32_Car |
| Password | 12345678 |
| ESP32 IP address | 192.168.4.1 |
| Web server port | 80 |

After connecting, open the following in your browser:

| URL | Purpose |
|---|---|
| http://192.168.4.1/ | Main control interface |
| http://192.168.4.1/battery | JSON with battery data |
| http://192.168.4.1/update | OTA update page |

## Pinout

### Motors (PWM, 20 kHz, 8 bit)

| Function | GPIO | Note |
|---|---|---|
| Forward | 18 | PWM channel 0 (drive) |
| Reverse | 5 | PWM channel 0 (drive) |
| Left | 19 | PWM channel 1 (steer) |
| Right | 23 | PWM channel 1 (steer) |

### Light and sound

| Function | GPIO |
|---|---|
| Left brake light | 2 |
| Right brake light | 15 |
| Beeper (horn) | 4 |

### RRC2054 battery (SMBus / I2C)

| Line | GPIO | ESP32 Board |
|---|---|---|
| SMBD (SDA) | 21 | D21 |
| SMBC (SCL) | 22 | D22 |
| Device address | 0x0B | standard SBS |
| Pull-up | external, 1.5 kΩ | to both lines to +3.3 V |
| Bus frequency | 100 kHz | Wire.setClock(100000) |

External 1.5 kΩ pull-ups are mandatory — without them, SMBus communication with the battery will be unstable or will not work at all.

## Control

### Web interface

- D-pad (UP / DOWN / L / R / STOP) — movement and rotation.
- Sliders:
  - Max Speed — target speed (100–250)
  - Steer Intensity — steering intensity (100–250)
- Buttons: HORN, LIGHTS ON, LIGHTS OFF.
- Battery panel: voltage, temperature, state of charge (SOC) in real time.

### HTTP API (endpoint /cmd)

All commands are executed via GET /cmd?q=<command>[&v=<value>].

| Command | Parameter v | Action |
|---|---|---|
| forward | — | Smooth forward start (500 ms ramp) |
| backward | — | Smooth backward start |
| stop | — | Stop |
| left / right | — | Turn the wheels |
| straight | — | Wheels straight |
| sound | — | Beep (265 ms) |
| lights_on / lights_off | — | Brake lights on/off |
| speed | 100–250 | Set target speed |
| steer_intensity | 100–250 | Set steering intensity |


## RRC2054 battery (SMBus)

The Smart Battery System (SBS) standard is used. The batteryTask on core 0 polls the registers every 2 seconds with a 25 ms pause between transactions.

| Parameter | Register | Format | Conversion |
|---|---|---|---|
| Temperature | 0x08 | uint16, 0.1 K | C = raw/10 - 273 |
| Voltage | 0x09 | uint16, mV | raw |
| Current | 0x0A | int16, mA | (int16_t)raw |
| Relative SOC | 0x0D | uint16, % | raw (0–100) |

If register 0x0A returns zero — try 0x0B (AverageCurrent), or vice versa, depending on the specific RRC2054 instance.

## Safety

The firmware runs the safetyTask task (on core 1), which automatically performs an emergency stop and beeps if:

- there are no Wi-Fi clients connected to the ESP32, or
- more than 30 seconds have passed since the last command.

This protects the vehicle from "running away" when communication with the remote control is lost.

## Project structure

```
.
├── platformio.ini
├── src/
│   └── main.cpp           # firmware (everything in one file)
├── include/
├── lib/
├── test/
└── README.md
```

### Main firmware components

| Function / task | Purpose |
|---|---|
| initPWM() | PWM channel configuration (20 kHz, 8 bits) |
| moveForward() / moveBackward() | Drive control |
| turnLeft() / turnRight() / straightWheels() | Steering control |
| softStartForward() / softStartBackward() | Smooth acceleration (40 steps) |
| allOff() | Complete shutdown of everything |
| handleCmd() | Handling HTTP commands |
| handleBattery() | JSON with battery telemetry |
| smbusReadWord() | Low-level SMBus word reading |
| batteryTask() | FreeRTOS battery polling task (core 0, 2 s) |
| safetyTask() | FreeRTOS emergency stop task (core 1, 5 s) |

## Default settings

| Parameter | Value |
|---|---|
| Target speed | 185 |
| Steering intensity | 215 |
| PWM frequency | 20 kHz |
| PWM resolution | 8 bits (0–255) |
| Command timeout | 30,000 ms |
| Firmware version | 0.1.0 |

## Known issues and solutions

### Battery current is zero

1. Make sure REG_CURRENT is set to 0x0A (not 0x0B).
2. Make sure a load is connected to the battery — with no load the current is near zero.
3. Check that SDA/SCL contacts are reliable and that 1.5 kΩ pull-ups are present.

### VS Code terminal does not start

The error Path to shell executable "...platformio.exe" does not exist is fixed by explicitly specifying the path to platformio.exe in VS Code settings:

```json
"platformio-ide.useBuiltinPIOCore": false,
"platformio-ide.customPATH": "C:/Users/<user>/.platformio/penv/Scripts"
```

### Battery data is not updating in the web interface

Open the browser console (F12): if TypeError: Cannot set properties of null occurs in updateBattery(), an element referenced by JS has been removed or renamed in HTML. Use the safe setText() helper (see the "Web interface" section in the code).

## Useful links

- PlatformIO + ESP32: https://docs.platformio.org/en/latest/platforms/espressif32.html
- SBS — Smart Battery System Specification: https://www.sbs-forum.org/
- RRC2054 datasheet (RRC power solutions): https://www.rrc-ps.com/
- ESP32 Arduino Core — WebServer: https://github.com/espressif/arduino-esp32/tree/master/libraries/WebServer

## Version history

| Version | Changes |
|---|---|
| 0.1.0 | Basic version: movement, light, sound, web interface, OTA |
| 0.1.x | Added the task of reading the RRC2054 battery via SMBus |


# Система управления электротранспортом (РУС)

ПО для управления электрическим транспортным средством на базе ESP32.
Устройство поднимает собственную Wi-Fi точку доступа с веб-интерфейсом, управляет
моторами посредством ШИМ, читает данные аккумулятора RRC2054 по протоколу SMBus
и поддерживает OTA-обновление прошивки по воздуху.

## 🛠️ Инструментарий

- PlatformIO (Core 6.1.11, Home 3.4.4)
- VS Code + расширение PlatformIO IDE
- Framework: Arduino (для ESP32)
- Плата: ESP32_DevKitC_V4

### Сборка и прошивка

```bash
# Сборка
pio run

# Прошивка по USB
pio run -t upload

# Монитор порта (115200 бод)
pio device monitor
```

### 🔄 OTA-обновление

1. Подключитесь к Wi-Fi ESP32_Car (пароль — см. ниже).
2. Откройте в браузере http://192.168.4.1/update
3. Выберите .bin-файл (собирается автоматически в .pio/build/<env>/firmware.bin) и загрузите.
4. ESP32 перезагрузится автоматически.

## Подключение к Wi-Fi

ESP32 работает в режиме точки доступа (AP):

| Параметр | Значение |
|---|---|
| SSID | ESP32_Car |
| Пароль | 12345678 |
| IP-адрес ESP32 | 192.168.4.1 |
| Порт веб-сервера | 80 |

После подключения откройте в браузере:

| URL | Назначение |
|---|---|
| http://192.168.4.1/ | Основной интерфейс управления |
| http://192.168.4.1/battery | JSON с данными аккумулятора |
| http://192.168.4.1/update | Страница OTA-обновления |

## Распиновка

### Моторы (PWM, 20 кГц, 8 бит)

| Функция | GPIO | Примечание |
|---|---|---|
| Вперёд | 18 | PWM-канал 0 (drive) |
| Назад | 5 | PWM-канал 0 (drive) |
| Влево | 19 | PWM-канал 1 (steer) |
| Вправо | 23 | PWM-канал 1 (steer) |

### Свет и звук

| Функция | GPIO |
|---|---|
| Стоп-сигнал левый | 2 |
| Стоп-сигнал правый | 15 |
| Звуковой сигнал (horn) | 4 |

### Аккумулятор RRC2054 (SMBus / I2C)

| Линия | GPIO | Плата ESP32 |
|---|---|---|
| SMBD (SDA) | 21 | D21 |
| SMBC (SCL) | 22 | D22 |
| Адрес устройства | 0x0B | стандартный SBS |
| Подтяжка | внешняя, 1.5 кОм | на обе линии к +3.3 В |
| Частота шины | 100 кГц | Wire.setClock(100000) |

Внешние подтяжки 1.5 кОм обязательны — без них SMBus-обмен с аккумулятором
будет нестабильным или вообще не заработает.

## Управление

### Веб-интерфейс

- D-pad (UP / DOWN / L / R / STOP) — движение и поворот.
- Слайдеры:
  - Max Speed — целевая скорость (100–250)
  - Steer Intensity — интенсивность поворота (100–250)
- Кнопки: HORN, LIGHTS ON, LIGHTS OFF.
- Панель батареи: напряжение, температура, уровень заряда (SOC) в реальном времени.

### HTTP API (endpoint /cmd)

Все команды выполняются через GET /cmd?q=<команда>[&v=<значение>].

| Команда | Параметр v | Действие |
|---|---|---|
| forward | — | Плавный старт вперёд (500 мс рампы) |
| backward | — | Плавный старт назад |
| stop | — | Стоп |
| left / right | — | Поворот колёс |
| straight | — | Колёса прямо |
| sound | — | Бип (265 мс) |
| lights_on / lights_off | — | Стоп-сигналы вкл/выкл |
| speed | 100–250 | Установить целевую скорость |
| steer_intensity | 100–250 | Установить интенсивность руления |



## Безопасность

В прошивке работает задача safetyTask (на ядре 1), которая автоматически
выполняет аварийную остановку и подаёт звуковой сигнал, если:

- к ESP32 не подключено ни одного клиента Wi-Fi, или
- прошло более 30 секунд с момента последней команды.

Это защищает транспортное средство от «убегания» при потере связи с пультом.

## Структура проекта

```
.
├── platformio.ini
├── src/
│   └── main.cpp           # прошивка (всё в одном файле)
├── include/
├── lib/
├── test/
└── README.md
```

### Основные компоненты прошивки

| Функция / задача | Назначение |
|---|---|
| initPWM() | Настройка ШИМ-каналов (20 кГц, 8 бит) |
| moveForward() / moveBackward() | Управление приводом |
| turnLeft() / turnRight() / straightWheels() | Управление рулением |
| softStartForward() / softStartBackward() | Плавный разгон (40 шагов) |
| allOff() | Полное отключение всего |
| handleCmd() | Обработка HTTP-команд |
| handleBattery() | JSON с телеметрией батареи |
| smbusReadWord() | Низкоуровневое чтение слова SMBus |
| batteryTask() | FreeRTOS-задача опроса батареи (ядро 0, 2 с) |
| safetyTask() | FreeRTOS-задача аварийной остановки (ядро 1, 5 с) |

## Настройки по умолчанию

| Параметр | Значение |
|---|---|
| Целевая скорость | 185 |
| Интенсивность руления | 215 |
| Частота ШИМ | 20 кГц |
| Разрешение ШИМ | 8 бит (0–255) |
| Таймаут команд | 30 000 мс |
| Версия прошивки | 0.1.0 |

## Полезные ссылки

- PlatformIO + ESP32: https://docs.platformio.org/en/latest/platforms/espressif32.html
- SBS — Smart Battery System Specification: https://www.sbs-forum.org/
- RRC2054 datasheet (RRC power solutions): https://www.rrc-ps.com/
- ESP32 Arduino Core — WebServer: https://github.com/espressif/arduino-esp32/tree/master/libraries/WebServer

## История версий

| Версия | Изменения |
|---|---|
| 0.1.0 | Базовая версия: движение, свет, звук, веб-интерфейс, OTA |
| 0.1.x | Добавлена задача чтения аккумулятора RRC2054 по SMBus |