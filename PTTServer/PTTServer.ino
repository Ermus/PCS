#include <Arduino.h>
//#include <WiFi.h>
//#include <WiFiUdp.h>
#include <ETH.h>
#include <AudioTools.h>
#include <ESP32SerialCtl.h>
#include <Preferences.h>
#include <ESPping.h>
#include <EncButton.h>
#include "AudioTools/Communication/UDPStream.h"

// ================== НАСТРОЙКИ ==================
#define PTT_PIN              12               // Пин кнопки
#define LED_PIN              2               // Пин светодиода индикации PTT
#define WS_PIN               15
#define BCK_PIN              14
#define RX_DATA_PIN          36

#define DEFAULT_TARGET_IP    "192.168.0.123"
#define DEFAULT_TARGET_PORT  15200

#define ETH_PHY_TYPE         ETH_PHY_LAN8720
#define ETH_PHY_ADDR         1
#define ETH_PHY_POWER        16
#define ETH_PHY_MDC          23
#define ETH_PHY_MDIO         18
#define ETH_CLK_MODE         ETH_CLOCK_GPIO0_IN

#define PING_INTERVAL_MS     3000
// ===============================================

AudioInfo info(48000, 1, 16);
I2SStream i2s;
//WiFiUDP wUdp;
//UDPStream Udp(WiFiUDP); 
audio_tools::UDPStream Udp;
Preferences prefs;
Button pttBtn(PTT_PIN);

// ----- Общее состояние -----
volatile bool eth_connected  = false;
volatile bool clientOnline   = false;
volatile bool pttPressed     = false;
volatile bool isTransmitting = false;

char targetIP[16] = DEFAULT_TARGET_IP;
uint16_t targetPort = DEFAULT_TARGET_PORT;

bool useStaticIP = true;
IPAddress staticIP(192, 168, 0, 122);
IPAddress staticGateway(192, 168, 0, 1);
IPAddress staticSubnet(255, 255, 255, 0);
IPAddress staticDNS(8, 8, 8, 8);

// ----- Хэндлы задач -----
TaskHandle_t audioTaskHandle  = NULL;
TaskHandle_t buttonTaskHandle = NULL;
TaskHandle_t pingTaskHandle   = NULL;
TaskHandle_t cliTaskHandle    = NULL;
TaskHandle_t ledTaskHandle    = NULL;


#include "settings.h"
#include "cli.h"

// ================== ETHERNET EVENT ==================
void WiFiEvent(WiFiEvent_t event)
{
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("ETH Started");
            ETH.setHostname("wt32-eth01-ptt");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("ETH Connected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("ETH MAC: %s, IP: %s\n",
                          ETH.macAddress().c_str(),
                          ETH.localIP().toString().c_str());
            eth_connected = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("ETH Disconnected");
            eth_connected = false;
            clientOnline  = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("ETH Stopped");
            eth_connected = false;
            clientOnline  = false;
            break;
        default:
            break;
    }
}


// ================== ЗАДАЧИ FreeRTOS ==================

// 1. Аудио-задача (ядро 1, высокий приоритет)
void audio_task(void *param)
{
    const int samples_per_frame = 480;
    const int bytes_per_frame   = samples_per_frame * sizeof(int16_t);

    int16_t *samples = (int16_t *)malloc(bytes_per_frame);
    if (!samples) {
        Serial.println("audio_task: malloc failed");
        vTaskDelete(NULL);
    }

    while (true)
    {
        size_t bytes_read = i2s.readBytes((uint8_t *)samples, bytes_per_frame);

        // ICMP reachability can remain false after an Ethernet server reboot
        // even while the UDP receiver is available. Do not gate audio on ping.
        bool should_tx = pttPressed && eth_connected
                         && (bytes_read == bytes_per_frame);

        if (should_tx != isTransmitting) {
            isTransmitting = should_tx;
        }

        if (should_tx) {
            size_t sent = Udp.write((const uint8_t*)samples, bytes_per_frame);
            //Udp.begin(targetIP, targetPort);
            //Udp.write((uint8_t *)samples, bytes_per_frame);
            //wUdp.endPacket();
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// 2. Задача кнопки (EncButton)
void button_task(void *param)
{
    pttBtn.setBtnLevel(LOW);

    bool lastState = false;

    while (true)
    {
        pttBtn.tick();
        bool current = pttBtn.pressing();

        if (current != lastState) {
            lastState  = current;
            pttPressed = current;

            if (current) Serial.println(">>> TX ON");
            else         Serial.println("<<< TX OFF");
        }

        vTaskDelay(pdMS_TO_TICKS(5));     // ~200 Гц
    }
}

// 3. Задача проверки клиента (ping)
void ping_task(void *param)
{
    uint32_t lastPingDiagnostic = 0;

    while (true)
    {
        if (eth_connected) {
            IPAddress targetAddress;
            bool validTarget = targetAddress.fromString(targetIP);
            bool reachable = validTarget && Ping.ping(targetAddress, 1);

            if (reachable != clientOnline) {
                clientOnline = reachable;
                if (clientOnline) {
                    Serial.printf("*** CLIENT ONLINE  %s (%.1f ms) ***\n",
                                  targetIP, Ping.averageTime());
                } else {
                    Serial.printf("*** CLIENT OFFLINE %s ***\n", targetIP);
                }
            }
            if (!reachable && millis() - lastPingDiagnostic >= 10000) {
                lastPingDiagnostic = millis();
                Serial.printf("[PING] no reply from %s; ETH IP=%s\n",
                              targetIP, ETH.localIP().toString().c_str());
            }
        } else {
            if (clientOnline) {
                clientOnline = false;
                Serial.println("*** CLIENT OFFLINE (no ETH) ***");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
    }
}

// 4. Задача CLI
void cli_task(void *param)
{
    while (true)
    {
        esp32SerialCtl.service();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// 5. Задача индикации светодиодом
void led_task(void *param)
{
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    bool lastState = false;

    while (true)
    {
        bool current = isTransmitting;        // индикация по состоянию передачи

        if (current != lastState) {
            lastState = current;
            digitalWrite(LED_PIN, current ? HIGH : LOW);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}


// ================== SETUP & LOOP ==================
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== WT32-ETH01 PTT FreeRTOS ===");

    loadSettings();

    // Регистрация команд
    //esp32SerialCtl.setCommands(kAppCommands, sizeof(kAppCommands) / sizeof(kAppCommands[0]));

    // Ethernet
    WiFi.onEvent(WiFiEvent);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
#else
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
#endif

    if (useStaticIP && staticIP != IPAddress(0, 0, 0, 0)) {
        ETH.config(staticIP, staticGateway, staticSubnet, staticDNS);
        Serial.println("Applying saved static IP...");
    }

    Serial.print("Waiting for Ethernet");
    uint32_t start = millis();
    while (!eth_connected && (millis() - start < 15000)) {
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (eth_connected) {
        Udp.begin(targetIP, targetPort);
        Serial.printf("UDP ready → %s:%u\n", targetIP, targetPort);
    } else {
        Serial.println("Ethernet timeout!");
    }

    // I2S
    AudioLogger::instance().begin(Serial, AudioLogger::Info);

    auto config = i2s.defaultConfig(RX_MODE);
    config.copyFrom(info);
    config.buffer_count     = 4;
    config.buffer_size      = 480;
    config.i2s_format       = I2S_MSB_FORMAT;
    config.bits_per_sample  = 16;
    config.channels         = 1;
    config.channel_format   = I2SChannelSelect::Left;
    config.auto_clear       = true;
    config.pin_ws           = WS_PIN;
    config.pin_bck          = BCK_PIN;
    config.pin_data_rx      = RX_DATA_PIN;
    config.is_master        = true;
    config.use_apll         = false;
    i2s.begin(config);

    // ----- Создание задач FreeRTOS -----
    xTaskCreatePinnedToCore(audio_task,  "audio",  12288, NULL, 5, &audioTaskHandle,  1);
    xTaskCreatePinnedToCore(button_task, "button",  4096, NULL, 3, &buttonTaskHandle, 0);
    xTaskCreatePinnedToCore(ping_task,   "ping",    6144, NULL, 2, &pingTaskHandle,   0);
    xTaskCreatePinnedToCore(cli_task,    "cli",     8192, NULL, 1, &cliTaskHandle,    0);
    xTaskCreatePinnedToCore(led_task,    "led",     2048, NULL, 1, &ledTaskHandle,    0);

    Serial.println("All tasks started");
    Serial.println("Commands: help, status, target, port, ip");
}

void loop()
{
    // Всё выполняется в задачах
    vTaskDelay(pdMS_TO_TICKS(1000));
}
