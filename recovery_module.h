#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "src/touch/gt911_touch.h"

// Deklaracje z espp.ino
extern int SCR_W;
extern int SCR_H;
extern void fbClear(uint16_t color);
extern void fillRectFB(int x, int y, int w, int h, uint16_t color);
extern void drawStringFB(int x, int y, String text, uint16_t color, int scale);
extern void fbPush();
extern gt911_touch ts;

// ======================================================
//  STRUKTURA LISTY PLIKÓW .BIN DLA RECOVERY
// ======================================================

struct RecBinFileEntry {
    String name;
    int y;
};

static RecBinFileEntry recFileList[64];
static int recFileCount = 0;
static int recScrollOffset = 0;
static bool recIsScrolling = false;
static int recLastTouchY = 0;

// ======================================================
//  UI LISTY PLIKÓW
// ======================================================

void drawFileListUI(String msg) {
    fbClear(0x1111);

    // Nagłówek
    fillRectFB(0, 0, SCR_W, 70, 0x07E0);
    drawStringFB(20, 25, "WYBIERZ PLIK .BIN", 0x0000, 3);

    // Pasek dolny / komunikaty
    fillRectFB(0, SCR_H - 120, SCR_W, 120, 0x0000);
    fillRectFB(0, SCR_H - 120, SCR_W, 3, 0xFFFF);
    drawStringFB(10, SCR_H - 100, msg, 0x07E0, 2);

    // Lista plików
    for (int i = 0; i < recFileCount; i++) {
        int y = recFileList[i].y + recScrollOffset;

        if (y < 80 || y > SCR_H - 150) continue;

        fillRectFB(40, y, SCR_W - 80, 50, 0x2222);
        drawStringFB(60, y + 15, recFileList[i].name, 0xFFFF, 2);
    }

    fbPush();
}

// ======================================================
//  MENU WYBORU PLIKU Z /update/offline/
//  ZWRACA PEŁNĄ ŚCIEŻKĘ LUB "" JEŚLI BRAK
// ======================================================

String openFileSelectionMenu() {
    recFileCount = 0;
    recScrollOffset = 0;
    recIsScrolling = false;
    recLastTouchY = 0;

    File dir = SD_MMC.open("/update/offline/");
    if (!dir || !dir.isDirectory()) {
        drawFileListUI("Brak folderu /update/offline/");
        delay(2000);
        return "";
    }

    File f;
    while ((f = dir.openNextFile())) {
        String name = String(f.name());
        if (name.endsWith(".bin")) {
            if (recFileCount < 64) {
                recFileList[recFileCount].name = name;
                recFileList[recFileCount].y = 100 + recFileCount * 60;
                recFileCount++;
            }
        }
        f.close();
    }

    if (recFileCount == 0) {
        drawFileListUI("Brak plikow .bin");
        delay(2000);
        return "";
    }

    drawFileListUI("Przewin i wybierz plik");

    bool wasTouch = false;
    uint16_t lastTapY = 0;

    while (true) {
        uint16_t tx, ty;
        bool t = ts.getTouch(&tx, &ty);

        if (t && !wasTouch) {
            recLastTouchY = ty;
            lastTapY = ty;
            recIsScrolling = false;
        }

        if (t && wasTouch) {
            int dy = (int)ty - (int)recLastTouchY;

            if (abs(dy) > 10) {
                recIsScrolling = true;
                recScrollOffset += dy;
                recLastTouchY = ty;

                if (recScrollOffset > 0) recScrollOffset = 0;

                int minOffset = -((recFileCount * 60) - 300);
                if (minOffset > 0) minOffset = 0;
                if (recScrollOffset < minOffset) recScrollOffset = minOffset;

                drawFileListUI("Przewijanie...");
            }
        }

        if (!t && wasTouch) {
            if (!recIsScrolling) {
                for (int i = 0; i < recFileCount; i++) {
                    int y = recFileList[i].y + recScrollOffset;
                    if (lastTapY > y && lastTapY < y + 50) {
                        return String("/update/offline/") + recFileList[i].name;
                    }
                }
            }
        }

        wasTouch = t;
        delay(20);
    }
}

// ======================================================
//  GŁÓWNY UI RECOVERY
// ======================================================

inline void drawRecoveryUI(String statusMsg, int progress = -1) {
    fbClear(0x1111);

    fillRectFB(0, 0, SCR_W, 70, 0xF800);
    drawStringFB(20, 25, "SUPERNOVA RECOVERY", 0xFFFF, 3);

    fillRectFB(40, 100, SCR_W - 80, 60, 0x4208);
    drawStringFB(110, 120, "1. REBOOT SYSTEM", 0xFFFF, 2);

    fillRectFB(40, 180, SCR_W - 80, 60, 0x18C3);
    drawStringFB(125, 200, "2. BACKUP TO SD", 0xFFFF, 2);

    fillRectFB(40, 260, SCR_W - 80, 60, 0x07E0);
    drawStringFB(100, 280, "3. RESTORE FROM SD", 0x0000, 2);

    fillRectFB(40, 340, SCR_W - 80, 60, 0xF800);
    drawStringFB(120, 360, "4. FACTORY RESET", 0xFFFF, 2);

    fillRectFB(0, SCR_H - 120, SCR_W, 120, 0x0000);
    fillRectFB(0, SCR_H - 120, SCR_W, 3, 0xFFFF);
    drawStringFB(10, SCR_H - 100, "Konsola:", 0x8410, 2);
    drawStringFB(10, SCR_H - 70, statusMsg, 0x07E0, 2);

    if (progress >= 0) {
        fillRectFB(10, SCR_H - 30, SCR_W - 20, 15, 0x4208);
        fillRectFB(10, SCR_H - 30, ((SCR_W - 20) * progress) / 100, 15, 0x07E0);
    }

    fbPush();
}

// ======================================================
//  GŁÓWNA PĘTLA RECOVERY
// ======================================================

inline void runGraphicalRecovery() {
    String statusMsg = "Oczekiwanie na polecenie...";
    bool wasTouching = false;
    drawRecoveryUI(statusMsg);

    while (true) {
        uint16_t tx = 0, ty = 0;
        bool isTouched = ts.getTouch(&tx, &ty);

        if (isTouched && !wasTouching) {

            // 1. REBOOT
            if (ty > 100 && ty < 160 && tx > 40 && tx < SCR_W - 80) {
                drawRecoveryUI("Restartowanie...");
                Preferences prefs;
                prefs.begin("supernova", false);
                prefs.putBool("recovery", false);
                prefs.end();
                delay(1000);
                ESP.restart();
            }

            // 2. BACKUP
            else if (ty > 180 && ty < 240 && tx > 40 && tx < SCR_W - 80) {
                statusMsg = "Tworzenie kopii...";
                drawRecoveryUI(statusMsg, 0);

                File backupBin = SD_MMC.open("/recovery_backup.bin", FILE_WRITE);
                if (backupBin) {
                    const esp_partition_t* running = esp_ota_get_running_partition();
                    uint32_t fwSize = ESP.getSketchSize();

                    if (running && fwSize > 0) {
                        uint8_t buff[4096];
                        int lastPct = -1;

                        for (uint32_t offset = 0; offset < fwSize; offset += 4096) {
                            uint32_t remaining = fwSize - offset;
                            uint32_t readLen = (remaining < 4096) ? remaining : 4096;

                            esp_partition_read(running, offset, buff, readLen);
                            backupBin.write(buff, readLen);

                            int pct = (offset * 100) / fwSize;
                            if (pct != lastPct) {
                                drawRecoveryUI("Zapis: " + String(offset / 1024) + " KB", pct);
                                lastPct = pct;
                            }
                            yield();
                        }
                        drawRecoveryUI("Sukces: recovery_backup.bin", 100);
                    } else {
                        drawRecoveryUI("BŁĄD: Brak partycji OTA!");
                    }
                    backupBin.close();
                } else {
                    drawRecoveryUI("BŁĄD: Brak karty SD!");
                }
            }

            // 3. RESTORE – MENU WYBORU PLIKU Z /update/offline/
            else if (ty > 260 && ty < 320 && tx > 40 && tx < SCR_W - 80) {
                String selectedPath = openFileSelectionMenu();
                if (selectedPath == "") {
                    drawRecoveryUI("Brak pliku do flashowania");
                    continue;
                }

                drawRecoveryUI("Wgrywanie: " + selectedPath, 0);

                File updateBin = SD_MMC.open(selectedPath, FILE_READ);
                if (updateBin) {
                    size_t updateSize = updateBin.size();

                    if (Update.begin(updateSize, U_FLASH)) {
                        uint8_t buff[2048];
                        size_t written = 0;
                        int lastPct = -1;

                        while (updateBin.available()) {
                            size_t len = updateBin.read(buff, sizeof(buff));
                            Update.write(buff, len);
                            written += len;

                            int pct = (written * 100) / updateSize;
                            if (pct != lastPct) {
                                drawRecoveryUI("Wgrywanie: " + String(pct) + "%", pct);
                                lastPct = pct;
                            }
                            yield();
                        }

                        if (Update.end()) {
                            drawRecoveryUI("Zakonczono! Kliknij REBOOT.", 100);
                        } else {
                            drawRecoveryUI("BŁĄD: " + String(Update.errorString()));
                        }
                    } else {
                        drawRecoveryUI("BŁĄD: Brak miejsca!");
                    }
                    updateBin.close();
                } else {
                    drawRecoveryUI("BŁĄD: Nie mozna otworzyc pliku!");
                }
            }

            // 4. FACTORY RESET – FULL FLASH ERASE
            else if (ty > 340 && ty < 400 && tx > 40 && tx < SCR_W - 80) {
                drawRecoveryUI("Kasowanie calej pamieci FLASH...");

                esp_err_t res = esp_flash_erase_chip(NULL);

                if (res == ESP_OK) {
                    drawRecoveryUI("FLASH wyczyszczony! Wgraj firmware przez USB.", 100);
                } else {
                    drawRecoveryUI("BŁĄD ERASE: " + String(res));
                }

                while (1) delay(100);
            }
        }

        wasTouching = isTouched;
        delay(30);
    }
}

// ======================================================
//  TRIGGER Z SYSTEMU
// ======================================================

inline void triggerRecovery() {
    Preferences prefs;
    prefs.begin("supernova", false);
    prefs.putBool("recovery", true);
    prefs.end();

    fbClear(0x0000);
    drawStringFB(100, 300, "RESTARTING TO RECOVERY...", 0xF800, 2);
    fbPush();
    delay(1500);
    ESP.restart();
}
