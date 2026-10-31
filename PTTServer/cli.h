// ================== ОБРАБОТЧИКИ КОМАНД ==================
int handle_status(const char **argv, size_t argc, void *ctx)
{
    auto ctl = reinterpret_cast<esp32serialctl::ESP32SerialCtl<> *>(ctx);

    ctl->printBody("ETH connected : %s", eth_connected ? "yes" : "no");
    if (eth_connected) {
        ctl->printBody("ETH IP        : %s", ETH.localIP().toString().c_str());
        ctl->printBody("Mode          : %s", useStaticIP ? "Static" : "DHCP");
    }
    ctl->printBody("Client        : %s", clientOnline ? "ONLINE" : "OFFLINE");
    ctl->printBody("PTT           : %s", pttPressed ? "PRESSED" : "released");
    ctl->printBody("Transmitting  : %s", isTransmitting ? "YES" : "no");
    ctl->printBody("Target        : %s:%u", targetIP, targetPort);
    ctl->printBody("Audio         : 48000 Hz mono 16-bit");
    return 0;
}

int handle_target(const char **argv, size_t argc, void *ctx)
{
    auto ctl = reinterpret_cast<esp32serialctl::ESP32SerialCtl<> *>(ctx);

    if (argc == 0) {
        ctl->printBody("Current target: %s:%u", targetIP, targetPort);
        return 0;
    }

    IPAddress testIP;
    if (!testIP.fromString(argv[0])) {
        ctl->printError(400, "Invalid IP address");
        return -1;
    }

    strncpy(targetIP, argv[0], sizeof(targetIP) - 1);
    targetIP[sizeof(targetIP) - 1] = '\0';

    if (argc >= 2) {
        int port = atoi(argv[1]);
        if (port < 1 || port > 65535) {
            ctl->printError(400, "Port must be 1..65535");
            return -1;
        }
        targetPort = (uint16_t)port;
    }

    saveTargetSettings();
    ctl->printBody("Target set to %s:%u (saved)", targetIP, targetPort);
    return 0;
}

int handle_port(const char **argv, size_t argc, void *ctx)
{
    auto ctl = reinterpret_cast<esp32serialctl::ESP32SerialCtl<> *>(ctx);

    if (argc != 1) {
        ctl->printError(400, "Usage: port <port>");
        return -1;
    }

    int port = atoi(argv[0]);
    if (port < 1 || port > 65535) {
        ctl->printError(400, "Port must be 1..65535");
        return -1;
    }

    targetPort = (uint16_t)port;
    saveTargetSettings();
    ctl->printBody("Port set to %u (saved)", targetPort);
    return 0;
}

int handle_ip(const char **argv, size_t argc, void *ctx)
{
    auto ctl = reinterpret_cast<esp32serialctl::ESP32SerialCtl<> *>(ctx);

    if (argc == 0) {
        ctl->printBody("Mode    : %s", useStaticIP ? "Static" : "DHCP");
        if (eth_connected) {
            ctl->printBody("IP      : %s", ETH.localIP().toString().c_str());
            ctl->printBody("Gateway : %s", ETH.gatewayIP().toString().c_str());
            ctl->printBody("Subnet  : %s", ETH.subnetMask().toString().c_str());
            ctl->printBody("DNS     : %s", ETH.dnsIP().toString().c_str());
        }
        return 0;
    }

    if (strcmp(argv[0], "dhcp") == 0) {
        useStaticIP = false;
        saveIpSettings();
        ctl->printBody("Switched to DHCP (saved). Reboot: sys reset");
        return 0;
    }

    if (strcmp(argv[0], "static") == 0) {
        if (argc < 4) {
            ctl->printError(400, "Usage: ip static <ip> <gateway> <subnet> [dns]");
            return -1;
        }

        IPAddress ip, gw, sn, dns;
        if (!ip.fromString(argv[1]) || !gw.fromString(argv[2]) || !sn.fromString(argv[3])) {
            ctl->printError(400, "Invalid IP/Gateway/Subnet");
            return -1;
        }

        if (argc >= 5) {
            if (!dns.fromString(argv[4])) {
                ctl->printError(400, "Invalid DNS");
                return -1;
            }
        } else {
            dns = IPAddress(8, 8, 8, 8);
        }

        staticIP      = ip;
        staticGateway = gw;
        staticSubnet  = sn;
        staticDNS     = dns;
        useStaticIP   = true;

        saveIpSettings();
        ctl->printBody("Static IP saved: %s", staticIP.toString().c_str());
        ctl->printBody("Reboot recommended: sys reset");
        return 0;
    }

    ctl->printError(400, "Usage: ip | ip dhcp | ip static <ip> <gw> <subnet> [dns]");
    return -1;
}

static const esp32serialctl::CommandEntry kAppCommands[] = {
    {
        "status",
        { {"en", "Show status"}, {"ru", "Показать статус"} },
        {},
        handle_status
    },
    {
        "target",
        { {"en", "Get/set target IP [port]"}, {"ru", "Целевой IP [порт]"} },
        {
            {"ip",   "string", false, "Target IP"},
            {"port", "int",    false, "1-65535"}
        },
        handle_target
    },
    {
        "port",
        { {"en", "Set target port"}, {"ru", "Установить порт"} },
        {
            {"port", "int", true, "1-65535"}
        },
        handle_port
    },
    {
        "ip",
        { {"en", "Show/set local IP"}, {"ru", "Свой IP"} },
        {
            {"mode", "string", false, "dhcp|static"},
            {"ip",   "string", false, ""},
            {"gw",   "string", false, ""},
            {"sn",   "string", false, ""},
            {"dns",  "string", false, ""}
        },
        handle_ip
    },
};


static constexpr esp32serialctl::ConfigEntry kAppConfigEntries[] = {
    {
        "pong",
        "Pong!",
        {
            {"en", "Response string for ping command"},
        },
    },
};

// ----- ESP32SerialCtl -----
static esp32serialctl::ESP32SerialCtl<> esp32SerialCtl(kAppConfigEntries, kAppCommands);


