#define __FREERTOS 1
#define NO_WIFI
#include <Arduino.h>
#include <Ethernet.h>
#include <AudioTools.h>
#include <SPI.h>

#include <EEPROM.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <ctype.h>

// ================== НАСТРОЙКИ ==================

#define DEFAULT_UDP_PORT      15200
#define EEPROM_SIZE           64
#define EEPROM_MAGIC          0xA5
#define EE_MAGIC              0
#define EE_PORT               1
#define EE_USE_STATIC         3
#define EE_IP                 4
#define EE_GW                 8
#define EE_SN                 12
#define EE_DNS                16

#define W5500_CS              17
#define W5500_RST             20

#define WS_PIN               3
#define BCK_PIN              2
#define RX_DATA_PIN          4

constexpr uint32_t SAMPLE_RATE = 48000;
constexpr uint32_t CHANNELS = 1;
constexpr uint32_t SAMPLES_PER_FRAME = 480;
constexpr uint32_t BYTES_PER_SAMPLE = 2;
constexpr uint32_t BYTES_PER_FRAME = 960;

#define SAMPLE_RATE           48000
#define BITS_PER_SAMPLE       16
#define CHANNELS              1

#define UDP_BUFFER_SIZE       960
#define AUDIO_QUEUE_DEPTH     4
#define CLI_BUF_SIZE          80
// ===============================================

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xEE };

EthernetUDP Udp;
AudioInfo info(SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);
//I2S i2s(OUTPUT);
I2SStream i2s;
audio_tools::HighPassFilter<float> highPass(300.0f, SAMPLE_RATE);
audio_tools::Compressor compressor(SAMPLE_RATE, 30, 100, 10, 35, 0.5f);

uint16_t udpPort = DEFAULT_UDP_PORT;
bool useStaticIP = true;
IPAddress staticIP(192, 168, 0, 123);
IPAddress staticGW(192, 168, 0, 1);
IPAddress staticSN(255, 255, 255, 0);
IPAddress staticDNS(8, 8, 8, 8);

volatile bool ethReady       = false;
volatile bool receiving      = false;
volatile uint32_t lastRxTime = 0;
volatile uint32_t rxPackets  = 0;
volatile uint32_t rxBytes    = 0;

struct AudioPacket {
    uint8_t data[UDP_BUFFER_SIZE];
    size_t len;
};
QueueHandle_t audioQueue;
volatile uint32_t droppedAudioPackets = 0;

char cliBuf[CLI_BUF_SIZE];
uint8_t cliIndex = 0;

TaskHandle_t netTaskHandle   = NULL;
TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t cliTaskHandle   = NULL;

// ================== ВСПОМОГАТЕЛЬНЫЕ ==================
void strToLower(char *s) {
    while (*s) { *s = tolower(*s); s++; }
}

void writeIP(int addr, IPAddress ip) {
    for (int i = 0; i < 4; i++) EEPROM.write(addr + i, ip[i]);
}

IPAddress readIP(int addr) {
    return IPAddress(EEPROM.read(addr), EEPROM.read(addr + 1),
                     EEPROM.read(addr + 2), EEPROM.read(addr + 3));
}

void loadSettings() {
    EEPROM.begin(EEPROM_SIZE);
    if (EEPROM.read(EE_MAGIC) != EEPROM_MAGIC) {
        Serial.println(F("[EEPROM] empty → defaults"));
        return;
    }

    uint16_t port = EEPROM.read(EE_PORT) | (EEPROM.read(EE_PORT + 1) << 8);
    if (port >= 1 && port <= 65535) udpPort = port;

    useStaticIP = EEPROM.read(EE_USE_STATIC);
    if (useStaticIP) {
        staticIP  = readIP(EE_IP);
        staticGW  = readIP(EE_GW);
        staticSN  = readIP(EE_SN);
        staticDNS = readIP(EE_DNS);
    }
    Serial.println(F("[EEPROM] settings loaded"));
}

void saveSettings() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(EE_MAGIC, EEPROM_MAGIC);
    EEPROM.write(EE_PORT, udpPort & 0xFF);
    EEPROM.write(EE_PORT + 1, (udpPort >> 8) & 0xFF);
    EEPROM.write(EE_USE_STATIC, useStaticIP ? 1 : 0);
    writeIP(EE_IP,  staticIP);
    writeIP(EE_GW,  staticGW);
    writeIP(EE_SN,  staticSN);
    writeIP(EE_DNS, staticDNS);
    EEPROM.commit();
    Serial.println(F("[OK] settings saved"));
}

void applyNetwork() {
    Udp.stop();
    if (useStaticIP) {
        Ethernet.begin(mac, staticIP, staticDNS, staticGW, staticSN);
    } else {
        Ethernet.begin(mac);
    }
    delay(700);
    Udp.begin(udpPort);
    Serial.print(F("[NET] IP = "));
    Serial.println(Ethernet.localIP());
}

void printPrompt() {
    Serial.print(F("rx> "));
}

// ================== КОМАНДЫ CLI ==================
void cmdHelp() {
    Serial.println(F("Available commands:"));
    Serial.println(F("  help, ?                 Show this help"));
    Serial.println(F("  status, st              General status"));
    Serial.println(F("  stats, stat             RX statistics"));
    Serial.println(F("  ip                      Show IP info"));
    Serial.println(F("  ip dhcp                 Switch to DHCP (auto-save)"));
    Serial.println(F("  ip static <ip> <gw> <sn> [dns]  (auto-save)"));
    Serial.println(F("  port                    Show UDP port"));
    Serial.println(F("  port <num>              Set UDP port (auto-save)"));
    Serial.println(F("  save                    Force save"));
    Serial.println(F("  clear, cls              Clear screen"));
    Serial.println(F("  reboot                  Restart board"));
}

void cmdStatus() {
    Serial.println(F("──────── Status ────────"));
    Serial.print(F(" Mode       : ")); Serial.println(useStaticIP ? F("Static") : F("DHCP"));
    Serial.print(F(" IP         : ")); Serial.println(Ethernet.localIP());
    Serial.print(F(" Gateway    : ")); Serial.println(Ethernet.gatewayIP());
    Serial.print(F(" Subnet     : ")); Serial.println(Ethernet.subnetMask());
    Serial.print(F(" UDP port   : ")); Serial.println(udpPort);
    Serial.print(F(" Receiving  : ")); Serial.println(receiving ? F("YES") : F("no"));
    Serial.print(F(" Uptime     : ")); Serial.print(millis() / 1000); Serial.println(F(" s"));
    Serial.println(F("────────────────────────"));
}

void cmdStats() {
    Serial.println(F("────── Statistics ──────"));
    Serial.print(F(" Packets    : ")); Serial.println(rxPackets);
    Serial.print(F(" Bytes      : ")); Serial.println(rxBytes);
    Serial.print(F(" Dropped    : ")); Serial.println(droppedAudioPackets);
    if (millis() > 2000) {
        uint32_t bps = (rxBytes * 8) / (millis() / 1000);
        Serial.print(F(" Avg rate   : ")); Serial.print(bps); Serial.println(F(" bit/s"));
    }
    Serial.println(F("────────────────────────"));
}

void cmdIpShow() {
    Serial.print(F("Mode : ")); Serial.println(useStaticIP ? F("Static") : F("DHCP"));
    Serial.print(F("IP   : ")); Serial.println(Ethernet.localIP());
    Serial.print(F("GW   : ")); Serial.println(Ethernet.gatewayIP());
    Serial.print(F("Mask : ")); Serial.println(Ethernet.subnetMask());
}

void cmdIpDhcp() {
    useStaticIP = false;
    applyNetwork();
    saveSettings();
    Serial.println(F("[OK] DHCP applied & saved"));
}

void cmdIpStatic(char *args) {
    IPAddress ip, gw, sn, dns(8, 8, 8, 8);
    int idx = 0;
    char *tok = strtok(args, " ");
    while (tok && idx < 4) {
        IPAddress tmp;
        if (!tmp.fromString(tok)) {
            Serial.println(F("[ERR] invalid address"));
            return;
        }
        if (idx == 0) ip  = tmp;
        if (idx == 1) gw  = tmp;
        if (idx == 2) sn  = tmp;
        if (idx == 3) dns = tmp;
        idx++;
        tok = strtok(NULL, " ");
    }
    if (idx < 3) {
        Serial.println(F("[ERR] Usage: ip static <ip> <gateway> <subnet> [dns]"));
        return;
    }

    staticIP  = ip;
    staticGW  = gw;
    staticSN  = sn;
    staticDNS = dns;
    useStaticIP = true;

    applyNetwork();
    saveSettings();
    Serial.println(F("[OK] static IP applied & saved"));
}

void cmdPortShow() {
    Serial.print(F("UDP port: "));
    Serial.println(udpPort);
}

void cmdPortSet(char *args) {
    int port = atoi(args);
    if (port < 1 || port > 65535) {
        Serial.println(F("[ERR] port must be 1..65535"));
        return;
    }
    udpPort = (uint16_t)port;
    Udp.stop();
    Udp.begin(udpPort);
    saveSettings();
    Serial.print(F("[OK] port = "));
    Serial.print(udpPort);
    Serial.println(F(" (saved)"));
}

void cmdClear() {
    Serial.print(F("\033[2J\033[H"));
}

void cliProcessLine(char *line)
{
    while (*line == ' ') line++;
    char *end = line + strlen(line) - 1;
    while (end > line && (*end == ' ' || *end == '\r' || *end == '\n')) {
        *end = 0;
        end--;
    }
    if (*line == 0) return;

    char cmd[CLI_BUF_SIZE];
    strncpy(cmd, line, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;
    strToLower(cmd);

    char *args = strchr(cmd, ' ');
    if (args) {
        *args = 0;
        args++;
        while (*args == ' ') args++;
    } else {
        args = cmd + strlen(cmd);
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmdHelp();
    }
    else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "st") == 0) {
        cmdStatus();
    }
    else if (strcmp(cmd, "stats") == 0 || strcmp(cmd, "stat") == 0) {
        cmdStats();
    }
    else if (strcmp(cmd, "ip") == 0) {
        if (*args == 0) {
            cmdIpShow();
        } else if (strncmp(args, "dhcp", 4) == 0) {
            cmdIpDhcp();
        } else if (strncmp(args, "static ", 7) == 0) {
            cmdIpStatic(args + 7);
        } else {
            Serial.println(F("[ERR] ip | ip dhcp | ip static <ip> <gw> <sn> [dns]"));
        }
    }
    else if (strcmp(cmd, "port") == 0) {
        if (*args == 0) cmdPortShow();
        else            cmdPortSet(args);
    }
    else if (strcmp(cmd, "save") == 0) {
        saveSettings();
    }
    else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        cmdClear();
    }
    else if (strcmp(cmd, "reboot") == 0) {
        Serial.println(F("Rebooting..."));
        delay(120);
        rp2040.reboot();
    }
    else {
        Serial.print(F("[ERR] unknown command: "));
        Serial.println(cmd);
        Serial.println(F("Type 'help'"));
    }
}

// ================== ЗАДАЧИ FreeRTOS ==================
void cli_task(void *param)
{
    printPrompt();
    while (true)
    {
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                Serial.println();
                if (cliIndex > 0) {
                    cliBuf[cliIndex] = 0;
                    cliProcessLine(cliBuf);
                    cliIndex = 0;
                }
                printPrompt();
            }
            else if (c == 0x08 || c == 0x7F) {
                if (cliIndex > 0) {
                    cliIndex--;
                    Serial.print(F("\b \b"));
                }
            }
            else if (cliIndex < CLI_BUF_SIZE - 1 && c >= 32 && c < 127) {
                cliBuf[cliIndex++] = c;
                Serial.print(c);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void net_task(void *param)
{
    uint8_t rxBuf[UDP_BUFFER_SIZE];
    bool wasReceiving = false;
    uint32_t lastStatusPrint = 0;

    while (true)
    {
        if (!ethReady) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        int packetSize = Udp.parsePacket();
        if (packetSize > 0)
        {
            int len = Udp.read(rxBuf, min(packetSize, (int)UDP_BUFFER_SIZE));
            if (len > 0)
            {
                if (len == BYTES_PER_FRAME) {
                    AudioPacket packet;
                    memcpy(packet.data, rxBuf, len);
                    packet.len = len;
                    // Keep latency bounded: if playback falls behind, discard
                    // the oldest queued frame and retain the newest audio.
                    if (xQueueSend(audioQueue, &packet, 0) != pdTRUE) {
                        AudioPacket stale;
                        if (xQueueReceive(audioQueue, &stale, 0) == pdTRUE) {
                            droppedAudioPackets++;
                        }
                        xQueueSend(audioQueue, &packet, 0);
                    }
                }

                lastRxTime = millis();
                rxPackets++;
                rxBytes += len;

                // Старт потока
                if (!wasReceiving) {
                    wasReceiving = true;
                    receiving = true;
                    Serial.println(F(">>> RX DATA START"));
                }
            }
        }

        // Стоп потока по таймауту
        if (wasReceiving && (millis() - lastRxTime > 400)) {
            wasReceiving = false;
            receiving = false;
            Serial.println(F("<<< RX DATA STOP"));
        }

        // Периодический статус во время приёма
        if (wasReceiving && (millis() - lastStatusPrint > 2000)) {
            lastStatusPrint = millis();
            Serial.print(F("[RX] packets="));
            Serial.print(rxPackets);
            Serial.print(F("  bytes="));
            Serial.println(rxBytes);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void audio_task(void *param)
{
    AudioPacket packet;

    while (true)
    {
        if (xQueueReceive(audioQueue, &packet, portMAX_DELAY) == pdTRUE && packet.len > 0) {
            int16_t *samples = reinterpret_cast<int16_t *>(packet.data);
            const size_t sampleCount = packet.len / sizeof(int16_t);
            for (size_t i = 0; i < sampleCount; ++i) {
                int16_t filtered = static_cast<int16_t>(highPass.process(static_cast<float>(samples[i])));
                samples[i] = compressor.process(filtered);
            }
            i2s.write(packet.data, packet.len);
        }
    }
}


// ================== SETUP & LOOP ==================
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 2500);

    Serial.println();
    Serial.println(F("════════════════════════════════════"));
    Serial.println(F("   RP2040 Audio Receiver  +  CLI"));
    Serial.println(F("════════════════════════════════════"));

    // 1. Автозагрузка настроек
    loadSettings();

    audioQueue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(AudioPacket));
    if (audioQueue == NULL) {
        Serial.println(F("[ERR] Audio queue allocation failed"));
        while (1) delay(1000);
    }

    // 2. Сброс W5500
    pinMode(W5500_RST, OUTPUT);
    digitalWrite(W5500_RST, LOW);
    delay(70);
    digitalWrite(W5500_RST, HIGH);
    delay(180);

    Ethernet.init(W5500_CS);

    // 3. Автоприменение сети
    Serial.println(F("[BOOT] Applying network settings..."));

    if (useStaticIP && staticIP != IPAddress(0, 0, 0, 0)) {
        Serial.println(F("[BOOT] Mode: STATIC"));
        Serial.print  (F("       IP : ")); Serial.println(staticIP);
        Serial.print  (F("       GW : ")); Serial.println(staticGW);
        Serial.print  (F("       SN : ")); Serial.println(staticSN);
        Serial.print  (F("       DNS: ")); Serial.println(staticDNS);
        Ethernet.begin(mac, staticIP, staticDNS, staticGW, staticSN);
    }
    else {
        Serial.println(F("[BOOT] Mode: DHCP"));
        useStaticIP = false;
        if (Ethernet.begin(mac) == 0) {
            Serial.println(F("[BOOT] DHCP failed, retry..."));
            delay(2000);
            if (Ethernet.begin(mac) == 0) {
                Serial.println(F("[BOOT] DHCP failed again"));
            } else {
                Serial.println(F("[BOOT] DHCP OK (retry)"));
            }
        } else {
            Serial.println(F("[BOOT] DHCP OK"));
        }
    }

    delay(600);

    // 4. UDP
    Udp.begin(udpPort);
    ethReady = true;

    // 5. Сводка
    Serial.println(F("───────── Boot Summary ─────────"));
    Serial.print  (F(" IP        : ")); Serial.println(Ethernet.localIP());
    Serial.print  (F(" Gateway   : ")); Serial.println(Ethernet.gatewayIP());
    Serial.print  (F(" UDP port  : ")); Serial.println(udpPort);
    Serial.print  (F(" Mode      : ")); Serial.println(useStaticIP ? F("Static") : F("DHCP"));
    Serial.println(F("────────────────────────────────"));

    // 6. I2S
    AudioLogger::instance().begin(Serial, AudioLogger::Info);

    auto config = i2s.defaultConfig(TX_MODE);
    config.copyFrom(info);
    config.buffer_count     = 4;
    config.buffer_size      = 480;
    config.bits_per_sample  = 16;
    config.channels         = 1;
    config.pin_ws           = WS_PIN;
    config.pin_bck          = BCK_PIN;
    config.pin_data         = RX_DATA_PIN;

    if (!i2s.begin(config)) {
        Serial.println(F("[ERR] AudioTools I2S begin failed"));
        while (1) delay(1000);
    }
    Serial.println(F("[I2S] AudioTools 48 kHz mono → PCM5102A"));

    xTaskCreate(net_task,   "net",   4096, NULL, 4, &netTaskHandle);
    xTaskCreate(audio_task, "audio", 4096, NULL, 5, &audioTaskHandle);
    xTaskCreate(cli_task,   "cli",   3072, NULL, 2, &cliTaskHandle);

    Serial.println(F("[SYS] tasks started"));
    Serial.println();
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
