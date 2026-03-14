#pragma once
#include <WiFi.h>

const char* ssid     = "NETIASPOT-7Xxj";
const char* password = "duuCXJ8kcCVh5";

void setupWiFi() {
    Serial.print("Laczenie z WiFi...");
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] Polaczono! IP: " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[WIFI] Blad polaczenia.");
    }
}