#pragma once
#include <Arduino.h>
#include <SD_MMC.h>

// Zmienne z głównego pliku
extern uint16_t* frame;
extern void drawStringFB(int x, int y, String text, uint16_t color, int scale);
extern void fillRectFB(int x, int y, int w, int h, uint16_t color);
extern void fbClear(uint16_t color);
extern String CITY;
extern String tempText;

#define SCR_W 480
#define SCR_H 800
#define STATUS_BAR_H 30

// Stany UI
enum SystemState { HOME, WALLPAPER_MENU, FILE_MANAGER };
SystemState currentState = HOME;

int currentPage = 1; // 0 = Lewa, 1 = Środek, 2 = Prawa
uint16_t bgColor = 0x18C3; // Domyślna tapeta (Ciemny niebieski)

// ================== MENEDŻER PLIKÓW ==================
void drawFileManager() {
    fillRectFB(0, 0, SCR_W, SCR_H, 0xFFFF); // Białe tło apki plików
    fillRectFB(0, 0, SCR_W, 60, 0x4208); // Pasek tytułowy (Szary Android)
    
    drawStringFB(15, 20, "Menedzer Plikow (SD)", 0xFFFF, 2);
    
    // Przycisk ZAMKNIJ (X)
    fillRectFB(SCR_W - 60, 10, 40, 40, 0xF800);
    drawStringFB(SCR_W - 45, 20, "X", 0xFFFF, 2);

    int yOffset = 80;
    File root = SD_MMC.open("/");
    if (!root) {
        drawStringFB(20, yOffset, "Brak karty SD!", 0xF800, 2);
        return;
    }

    File file = root.openNextFile();
    int count = 0;
    while (file && count < 15) { // Pokazujemy do 15 plików
        String name = String(file.name());
        bool isDir = file.isDirectory();
        
        // Ikonka pliku/folderu
        fillRectFB(20, yOffset, 30, 30, isDir ? 0xFD20 : 0x7BEF); // Żółty dla folderu, szary dla pliku
        
        // Nazwa
        drawStringFB(60, yOffset + 8, name, 0x0000, 2);
        
        // Linia oddzielająca
        fillRectFB(20, yOffset + 38, SCR_W - 40, 1, 0xC618);
        
        yOffset += 45;
        count++;
        file = root.openNextFile();
    }
}

// ================== MENU TAPETY (Long Press) ==================
void drawWallpaperMenu() {
    // Przyciemnienie tła
    for (int i = 0; i < SCR_W * SCR_H; i+=2) frame[i] = 0x0000; 

    // Panel z dołu
    int menuH = 200;
    int menuY = SCR_H - menuH;
    fillRectFB(0, menuY, SCR_W, menuH, 0x0000);
    fillRectFB(0, menuY, SCR_W, 2, 0x07E0); // Cyjanowy akcent

    drawStringFB(20, menuY + 20, "Opcje ekranu glownego", 0xFFFF, 2);

    // Przyciski opcji
    fillRectFB(20, menuY + 70, 130, 100, 0x4208);
    drawStringFB(40, menuY + 110, "Tapety", 0xFFFF, 2);

    fillRectFB(170, menuY + 70, 130, 100, 0x4208);
    drawStringFB(190, menuY + 110, "Widzety", 0xFFFF, 2);

    fillRectFB(320, menuY + 70, 130, 100, 0x4208);
    drawStringFB(330, menuY + 110, "Ustawienia", 0xFFFF, 1);
}

// ================== GŁÓWNY PULPIT ==================
void drawAndroidLauncher() {
    if (currentState == FILE_MANAGER) {
        drawFileManager();
        return;
    }

    // 1. Pusty pulpit (Tapeta)
    fbClear(bgColor); 

    // 2. Oryginalny górny pasek
    fillRectFB(0, 0, SCR_W, STATUS_BAR_H, 0x0000);
    drawStringFB(10, 8, tempText + " | " + CITY, 0xFFFF, 2);
    
    time_t now = time(nullptr);
    if (now > 100000) {
        struct tm* t = localtime(&now);
        char tBuf[16]; strftime(tBuf, sizeof(tBuf), "%H:%M", t);
        drawStringFB(SCR_W - 80, 8, String(tBuf), 0x07E0, 2); // Zegar na zielono/turkusowo
    } else {
        drawStringFB(SCR_W - 80, 8, "12:00", 0x07E0, 2);
    }

    // 3. Wskaźnik stron (Kropki)
    int dotY = SCR_H - 100;
    for (int i = 0; i < 3; i++) {
        uint16_t color = (i == currentPage) ? 0xFFFF : 0x4208;
        fillRectFB(SCR_W/2 - 40 + (i * 30), dotY, 12, 12, color);
    }

    // 4. Przycisk FILES (Aplikacja na pulpicie)
    // Rysujemy go tylko na środkowej stronie (currentPage == 1)
    if (currentPage == 1) {
        int fx = SCR_W / 2 - 40;
        int fy = SCR_H - 220;
        fillRectFB(fx, fy, 80, 80, 0xFD20); // Żółta ikona folderu
        fillRectFB(fx + 10, fy + 10, 30, 20, 0xFFFF); // Ozdoba ikony
        drawStringFB(fx + 15, fy + 90, "Files", 0xFFFF, 2);
    }

    // 5. Menu przytrzymania palca
    if (currentState == WALLPAPER_MENU) {
        drawWallpaperMenu();
    }
}