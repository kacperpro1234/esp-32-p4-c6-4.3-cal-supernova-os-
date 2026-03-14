#pragma once
#include <Arduino.h>
#include <SD_MMC.h>

#define MAX_APPS 20

// Struktura przechowująca informacje o znalezionych apkach
struct AppRecord {
    String name;
    String path;
};

AppRecord apps[MAX_APPS];
int appCount = 0;

// Funkcja skanująca kartę SD
void scanLuaApps() {
    appCount = 0;
    
    // Upewniamy się, że folder istnieje
    if (!SD_MMC.exists("/app")) {
        SD_MMC.mkdir("/app");
        Serial.println("Utworzono folder /app na karcie SD.");
        return;
    }

    File root = SD_MMC.open("/app");
    if (!root || !root.isDirectory()) {
        Serial.println("Blad odczytu folderu /app!");
        return;
    }

    File file = root.openNextFile();
    while (file && appCount < MAX_APPS) {
        if (file.isDirectory()) {
            String folderName = String(file.name());
            
            // Czyszczenie nazwy (w zależności od wersji biblioteki SD)
            int lastSlash = folderName.lastIndexOf('/');
            if (lastSlash >= 0) folderName = folderName.substring(lastSlash + 1);

            // Sprawdzamy, czy w folderze jest main.lua
            String luaPath = "/app/" + folderName + "/main.lua";
            if (SD_MMC.exists(luaPath)) {
                apps[appCount].name = folderName;
                apps[appCount].path = luaPath;
                appCount++;
                Serial.println("Znalazlem aplikacje Lua: " + folderName);
            }
        }
        file = root.openNextFile();
    }
    
    Serial.println("Znaleziono lacznie: " + String(appCount) + " aplikacji.");
}