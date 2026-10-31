// ================== NVS ==================
void loadSettings()
{
    prefs.begin("ptt-cfg", true);

    String tip = prefs.getString("target_ip", DEFAULT_TARGET_IP);
    strncpy(targetIP, tip.c_str(), sizeof(targetIP) - 1);
    targetIP[sizeof(targetIP) - 1] = '\0';

    targetPort   = prefs.getUShort("target_port", DEFAULT_TARGET_PORT);
    useStaticIP  = prefs.getBool("use_static", false);

    if (useStaticIP) {
        staticIP.fromString(prefs.getString("static_ip", "0.0.0.0"));
        staticGateway.fromString(prefs.getString("static_gw", "0.0.0.0"));
        staticSubnet.fromString(prefs.getString("static_sn", "255.255.255.0"));
        staticDNS.fromString(prefs.getString("static_dns", "8.8.8.8"));
    }
    prefs.end();

    Serial.println("Settings loaded from NVS");
    Serial.printf("  Target : %s:%u\n", targetIP, targetPort);
    Serial.printf("  Mode   : %s\n", useStaticIP ? "Static" : "DHCP");
}

void saveTargetSettings()
{
    prefs.begin("ptt-cfg", false);
    prefs.putString("target_ip", targetIP);
    prefs.putUShort("target_port", targetPort);
    prefs.end();
}

void saveIpSettings()
{
    prefs.begin("ptt-cfg", false);
    prefs.putBool("use_static", useStaticIP);
    if (useStaticIP) {
        prefs.putString("static_ip",  staticIP.toString());
        prefs.putString("static_gw",  staticGateway.toString());
        prefs.putString("static_sn",  staticSubnet.toString());
        prefs.putString("static_dns", staticDNS.toString());
    }
    prefs.end();
}
