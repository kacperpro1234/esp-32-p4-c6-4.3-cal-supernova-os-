/* =====================================================================
 * SUPERNOVA ALFA 1.0 (FORMERLY V8 PRO)
 * + Modern Android-Style Settings App (Vertical List & Submenus)
 * + Smooth Physics & Damping (App Drawer Swipe Up/Down)
 * + App Launch / Close Zoom Animations
 * + PPU 2D VRAM Support
 * + NAPRAWA: Przywrócono i zanimowano Szybki Panel (Quick Settings)!
 * ===================================================================== */

#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <time.h>
#include <Wire.h> 
#include <Preferences.h>
#include <Update.h> 
#include <math.h> // Potrzebne do fizyki animacji (Cubic Ease)
#include "esp_ota_ops.h"
#include "esp_partition.h"

// WŁĄCZAMY NASZE MODUŁY
#include "lua_main.h"
#include "recovery_module.h"

// ================== 1. KONFIGURACJA SPRZĘTU ==================
int SCR_W = 480; 
int SCR_H = 800;
bool isLandscape = false;

int NAV_BAR_H = 50; 
int STATUS_BAR_H = 30; 

#define TOUCH_SDA 7
#define TOUCH_SCL 8
#define TOUCH_INT -1
#define TOUCH_RST -1
#define BACKLIGHT_PIN 4 

#include "src/st7701_lcd.h"
#include "src/font_map.h"
#include "src/touch/gt911_touch.h"

st7701_lcd lcd(5);
gt911_touch ts(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT); 

uint16_t* frame = nullptr;          
uint16_t* display_frame = nullptr;  

Preferences preferences; 

// --- ZMIENNE DLA SZYBKIEGO WĄTKU DOTYKU ---
volatile bool global_isTouched = false;
volatile uint16_t global_raw_tx = 0;
volatile uint16_t global_raw_ty = 0;

// ================== 2. ZMIENNE SYSTEMOWE ==================
SystemState currentState = STATE_HOME;

int currentPage = 1; 
String timeString = "--:--";
int screenBrightness = 200; 
bool ntpSynced = false;

String keyboardBuffer = "";
String selectedSSID = "";
const char* keyboardLayout[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};

// System Animacji (Fizyka)
bool isQuickPanelOpen = false;
float quickPanelY = -450;       
float targetQuickPanelY = -450; 

float appDrawerY = 800; 
float targetAppDrawerY = 800;
bool isAssistiveMenuOpen = false; 

// Animacje Aplikacji i Orientacji
bool isAppAnimating = false;
bool isAppClosing = false;
float animAppScale = 0.0;
int animAppX = 240;
int animAppY = 400;
int pendingAppIndex = -1;

bool isRotAnimating = false;
float rotAnimProgress = 1.0;

unsigned long touchStartTime = 0;
int startX = 0, startY = 0;
bool wasTouching = false;
bool isLongPressTriggered = false;
int global_tx = 0, global_ty = 0; 

// ================== 3. BAZY DANYCH ==================
#define MAX_APPS 30
struct AppRecord { String name; String path; bool isValid; bool hasWidget; };
AppRecord luaApps[MAX_APPS];
int luaAppCount = 0;
int validAppCount = 0;

#define MAX_WIDGETS 4
struct WidgetRecord { bool active; String appName; int x, y, w, h; String widgetContent; };
WidgetRecord desktopWidgets[MAX_WIDGETS];

#define MAX_FILES 30

String fileNames[MAX_FILES]; bool fileIsDir[MAX_FILES]; int fileCount = 0;

enum SettingsTab { TAB_MAIN, TAB_BRIGHTNESS, TAB_WIFI, TAB_WALLPAPER, TAB_INFO, TAB_UPDATE };
SettingsTab currentTab = TAB_MAIN;
String wifiNetworks[15]; int wifiCount = 0; bool isScanningWiFi = false;
String wallpapers[10]; int wallpaperCount = 0; String currentWallpaperPath = ""; 

#define MAX_UPDATES 10
String updateFiles[MAX_UPDATES]; 
int updateFileCount = 0;
String updateStatusMsg = "Wybierz opcje ponizej";

// Funkcje wyzwalające animacje
void triggerAppLaunch(int index, int iconX, int iconY) {
    pendingAppIndex = index;
    animAppX = iconX;
    animAppY = iconY;
    isAppAnimating = true;
    isAppClosing = false;
    animAppScale = 0.0;
}

void triggerAppClose() {
    isAppAnimating = true;
    isAppClosing = true;
    animAppScale = 1.0;
    if(currentForegroundTask >= 0) { 
        luaTasks[currentForegroundTask].isForeground = false; 
        currentForegroundTask = -1; 
    }
}

// =====================================================================
// PPU 2D SPRITE ACCELERATOR (VRAM CACHE)
// =====================================================================
#define MAX_SPRITES 15
struct Sprite { bool active; int w, h; uint16_t* data; };
Sprite ppuVRAM[MAX_LUA_TASKS][MAX_SPRITES];

void loadSprite_PPU(int taskId, int id, String path) {
    if(taskId < 0 || taskId >= MAX_LUA_TASKS || id < 0 || id >= MAX_SPRITES) return;
    if(ppuVRAM[taskId][id].active) { heap_caps_free(ppuVRAM[taskId][id].data); ppuVRAM[taskId][id].active = false; }
    
    File f = SD_MMC.open(path); if(!f) return;
    uint8_t header[54]; if(f.read(header, 54) != 54) { f.close(); return; }
    int w = *(int*)&header[18]; int h = *(int*)&header[22];
    
    uint16_t* sData = (uint16_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
    if(!sData) { f.close(); return; }
    
    int rowSize = ((24 * w + 31) / 32) * 4;
    uint8_t* row = (uint8_t*)malloc(rowSize);
    for (int y = 0; y < h; y++) { 
        f.seek(54 + ((h - 1 - y) * rowSize)); f.read(row, rowSize);
        for (int x = 0; x < w; x++) {
            uint16_t color = ((row[x * 3 + 2] & 0xF8) << 8) | ((row[x * 3 + 1] & 0xFC) << 3) | (row[x * 3 + 0] >> 3);
            sData[y * w + x] = color;
        }
    }
    free(row); f.close();
    ppuVRAM[taskId][id].w = w; ppuVRAM[taskId][id].h = h; ppuVRAM[taskId][id].data = sData; ppuVRAM[taskId][id].active = true;
}

void drawSprite_PPU(int taskId, int id, int x, int y) {
    if(taskId < 0 || taskId >= MAX_LUA_TASKS || id < 0 || id >= MAX_SPRITES) return;
    if(!ppuVRAM[taskId][id].active) return;
    
    int sw = ppuVRAM[taskId][id].w; int sh = ppuVRAM[taskId][id].h;
    uint16_t* sData = ppuVRAM[taskId][id].data;

    for(int j = 0; j < sh; j++) {
        int dy = y + j;
        if(dy < STATUS_BAR_H || dy >= SCR_H - NAV_BAR_H) continue; 
        int dx = x; int copyLen = sw; int sx = 0;
        
        if (dx < 0) { sx = -dx; copyLen += dx; dx = 0; }
        if (dx + copyLen > SCR_W) { copyLen = SCR_W - dx; }
        if (copyLen <= 0) continue;

        if (!isLandscape) { memcpy(&frame[dy * 480 + dx], &sData[j * sw + sx], copyLen * 2); } 
        else { for(int p=0; p<copyLen; p++) { frame[(799 - (dx+p)) * 480 + dy] = sData[j * sw + sx + p]; } }
    }
}

void freeSprites_PPU(int taskId) {
    if(taskId < 0 || taskId >= MAX_LUA_TASKS) return;
    for(int i=0; i<MAX_SPRITES; i++) {
        if(ppuVRAM[taskId][i].active) { heap_caps_free(ppuVRAM[taskId][i].data); ppuVRAM[taskId][i].active = false; }
    }
}

void handleWidgetUpdate(int callerId, String newText) {
    for(int w=0; w<MAX_WIDGETS; w++) {
        if(desktopWidgets[w].active && desktopWidgets[w].appName == luaTasks[callerId].appName) {
            desktopWidgets[w].widgetContent = newText;
        }
    }
}

// ================== 6. SILNIK GRAFICZNY I ROTACJA ==================
void fbClear(uint16_t color) { 
    uint32_t c32 = (color << 16) | color;
    uint32_t* f32 = (uint32_t*)frame;
    for (int i = 0; i < (480 * 800)/2; i++) f32[i] = c32; 
}

void fbPush() { 
    memcpy(display_frame, frame, 480 * 800 * 2);
    lcd.draw16bitbergbbitmap(0, 0, 480, 800, display_frame); 
}

inline void fbDrawPixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    if (isLandscape) { frame[(799 - x) * 480 + y] = color; } 
    else { frame[y * 480 + x] = color; }
}

void fillRectFB(int x, int y, int w, int h, uint16_t color) {
    for (int i = x; i < x + w; i++) for (int j = y; j < y + h; j++) fbDrawPixel(i, j, color);
}

void fillRectFB_Accel(int x, int y, int w, int h, uint16_t color) {
    if (isLandscape) { fillRectFB(x,y,w,h,color); return; } 
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= SCR_H) continue;
        int startX = max(0, x); int endX = min(SCR_W, x + w);
        int len = endX - startX; if (len <= 0) continue;
        uint16_t* linePtr = &frame[j * 480 + startX];
        while(len--) *linePtr++ = color;
    }
}

void drawStringFB(int x, int y, String text, uint16_t color, int scale) {
    int charW = 6 * scale;
    for (int i = 0; i < text.length(); i++) {
        int idx = toupper(text[i]) - 32; 
        if (idx < 0 || idx > 95) idx = 0;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = font_pixels[idx][col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) { fillRectFB_Accel(x + (i * charW) + (col * scale), y + (row * scale), scale, scale, color); }
            }
        }
    }
}

void drawBMP(String path) {
    if (path == "") return; File f = SD_MMC.open(path); if (!f) return;
    uint8_t header[54]; if (f.read(header, 54) != 54) { f.close(); return; }
    int w = *(int*)&header[18]; int h = *(int*)&header[22];
    int rowSize = ((24 * w + 31) / 32) * 4;
    uint8_t* row = (uint8_t*)malloc(rowSize); if (!row) { f.close(); return; }
    for (int y = 0; y < (SCR_H - NAV_BAR_H) && y < h; y++) { 
        f.seek(54 + ((h - 1 - y) * rowSize)); f.read(row, rowSize);
        for (int x = 0; x < SCR_W && x < w; x++) {
            uint16_t color = ((row[x * 3 + 2] & 0xF8) << 8) | ((row[x * 3 + 1] & 0xFC) << 3) | (row[x * 3 + 0] >> 3);
            fbDrawPixel(x, y, color);
        }
    }
    free(row); f.close();
}

// ================== 9. FUNKCJE BOOTOWANIA I SYSTEMOWE ==================
void showBootScreen(String stepName, int progress, uint16_t textColor = 0xFFFF) {
    fbClear(0x1111); 
    drawStringFB(120, 250, "SUPERNOVA OS", 0x07E0, 4);
    drawStringFB(170, 300, "ALFA 1.0", 0xFFFF, 2);
    fillRectFB(90, 450, 300, 15, 0x4208); fillRectFB(90, 450, 3 * progress, 15, 0x07E0); 
    drawStringFB(80, 480, stepName, textColor, 2); fbPush();
}

void updateClock() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!ntpSynced) { configTime(3600, 3600, "pool.ntp.org"); ntpSynced = true; }
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10) && timeinfo.tm_year > 100) { 
            char tBuf[16]; strftime(tBuf, sizeof(tBuf), "%H:%M", &timeinfo); timeString = String(tBuf);
        }
    } else { ntpSynced = false; timeString = "--:--"; }
}

void scanLuaApps() {
    luaAppCount = 0; validAppCount = 0;
    if (!SD_MMC.exists("/app")) SD_MMC.mkdir("/app");
    File root = SD_MMC.open("/app"); if (!root) return;
    File file = root.openNextFile();
    
    while (file && luaAppCount < MAX_APPS) {
        if (file.isDirectory()) {
            String fName = String(file.name());
            int lastSlash = fName.lastIndexOf('/'); if (lastSlash >= 0) fName = fName.substring(lastSlash + 1);
            String mainPath = "/app/" + fName + "/main.lua";
            String fullVfsPath = "/sdcard" + mainPath;
            if (SD_MMC.exists(mainPath)) {
                luaApps[luaAppCount].name = fName; luaApps[luaAppCount].path = fullVfsPath;
                lua_State* L_validator = luaL_newstate(); 
                showBootScreen("Walidacja: " + fName, 60 + (luaAppCount*2));
                if (luaL_loadfile(L_validator, fullVfsPath.c_str()) == LUA_OK) { luaApps[luaAppCount].isValid = true; validAppCount++; } 
                else { luaApps[luaAppCount].isValid = false; }
                lua_close(L_validator);
                luaApps[luaAppCount].hasWidget = SD_MMC.exists("/app/" + fName + "/widget/");
                luaAppCount++;
            }
        } file = root.openNextFile();
    }
}

void loadWallpapers() {
    wallpaperCount = 0; if (!SD_MMC.exists("/wallpaper")) SD_MMC.mkdir("/wallpaper");
    File root = SD_MMC.open("/wallpaper"); if (!root) return;
    File file = root.openNextFile();
    while (file && wallpaperCount < 10) {
        if (!file.isDirectory() && String(file.name()).endsWith(".bmp")) {
            String fName = String(file.name()); int lastSlash = fName.lastIndexOf('/'); if (lastSlash >= 0) fName = fName.substring(lastSlash + 1);
            wallpapers[wallpaperCount] = fName; wallpaperCount++;
        } file = root.openNextFile();
    }
}

void scanUpdateFiles() {
    updateFileCount = 0; updateStatusMsg = "Skanowanie...";
    String scanPath = "/update/offline";
    if (!SD_MMC.exists(scanPath)) { if(SD_MMC.exists("/update/ofline")) scanPath = "/update/ofline"; else { updateStatusMsg = "Brak /update/offline"; return; } }
    File root = SD_MMC.open(scanPath); if (!root) { updateStatusMsg = "Blad odczytu folderu"; return; }
    File file = root.openNextFile();
    while (file && updateFileCount < MAX_UPDATES) {
        if (!file.isDirectory() && String(file.name()).endsWith(".bin")) {
            String fName = String(file.name()); int lastSlash = fName.lastIndexOf('/'); if (lastSlash >= 0) fName = fName.substring(lastSlash + 1);
            updateFiles[updateFileCount] = fName; updateFileCount++;
        } file = root.openNextFile();
    }
    if (updateFileCount == 0) updateStatusMsg = "Brak plikow .bin"; else updateStatusMsg = "Znaleziono: " + String(updateFileCount) + " plik(ow)";
}

void touchPollingTask(void * pvParameters) {
    while(true) {
        uint16_t tx, ty;
        if (ts.getTouch(&tx, &ty)) { global_raw_tx = tx; global_raw_ty = ty; global_isTouched = true; } else { global_isTouched = false; }
        vTaskDelay(pdMS_TO_TICKS(5)); 
    }
}

// ================== RYSOWANIE GUI OS ==================

void drawAssistiveMenu() {
    int cx = SCR_W - 35; 
    int cy = SCR_H / 2;
    
    if (!isAssistiveMenuOpen) {
        fillRectFB(cx - 20, cy - 20, 40, 40, 0x4208);
        fillRectFB(cx - 20, cy - 20, 40, 2, 0x07E0); 
        drawStringFB(cx - 10, cy - 7, "M", 0xFFFF, 2); 
    } else {
        int menuX = cx - 120;
        int menuY = cy - 100;
        fillRectFB(menuX, menuY, 150, 200, 0x1111);
        fillRectFB(menuX, menuY, 150, 2, 0x07E0);
        drawStringFB(menuX + 10, menuY + 10, "TOUCH MENU", 0x07E0, 1);
        
        fillRectFB(menuX + 10, menuY + 30, 130, 35, 0x2104); drawStringFB(menuX + 40, menuY + 40, "HOME", 0xFFFF, 2);
        fillRectFB(menuX + 10, menuY + 70, 130, 35, 0x2104); drawStringFB(menuX + 35, menuY + 80, "PLIKI", 0xFFFF, 2);
        fillRectFB(menuX + 10, menuY + 110, 130, 35, 0x2104); drawStringFB(menuX + 35, menuY + 120, "USTAW", 0xFFFF, 2);
        fillRectFB(menuX + 10, menuY + 150, 130, 35, 0xF800); drawStringFB(menuX + 20, menuY + 160, "ZAMKNIJ", 0xFFFF, 2);
    }
}

void drawStatusBar() {
    fillRectFB(0, 0, SCR_W, STATUS_BAR_H, 0x0000); 
    drawStringFB(10, 7, "Supernova V8", 0xFFFF, 2);
    if (WiFi.status() == WL_CONNECTED) {
        String netName = WiFi.SSID(); if (netName.length() == 0) netName = "WiFi"; if (netName.length() > 8) netName = netName.substring(0, 6) + "..";
        drawStringFB(SCR_W - 80 - (netName.length() * 12), 7, netName, 0x07E0, 2);
    }
    drawStringFB(SCR_W - 70, 7, timeString, 0xFFFF, 2);
}

void drawNavigationBar() {
    int navY = SCR_H - NAV_BAR_H;
    fillRectFB(0, navY, SCR_W, NAV_BAR_H, 0x0000); 
    drawStringFB(SCR_W/4 - 10, navY + 15, "<", 0xFFFF, 3); 
    drawStringFB(SCR_W/2 - 15, navY + 15, "O", 0xFFFF, 3); 
    drawStringFB(SCR_W - SCR_W/4 - 10, navY + 15, "|||", 0xFFFF, 3); 
}

// ZAAWANSOWANY, ANIMOWANY SZYBKI PANEL ALFA 1.0
void drawQuickPanel() {
    if (quickPanelY <= -450) return;
    
    // Tło główne panelu
    fillRectFB_Accel(0, quickPanelY, SCR_W, 450, 0x1111); 
    fillRectFB(0, quickPanelY + 448, SCR_W, 2, 0x07E0); // Akcentowy pasek na dole
    
    // Duży Zegar
    drawStringFB(20, quickPanelY + 25, timeString, 0xFFFF, 4);

    // Suwak Jasności
    fillRectFB(20, quickPanelY + 80, SCR_W - 40, 40, 0x2104); 
    int sliderW = (screenBrightness * (SCR_W - 40)) / 255; 
    fillRectFB(20, quickPanelY + 80, sliderW, 40, 0xCE79);    
    drawStringFB(30, quickPanelY + 90, "JASN.", 0x0000, 2);

    int tileW = (SCR_W - 60) / 2; int tileH = 70;

    // Wi-Fi Kafelek
    fillRectFB(20, quickPanelY + 140, tileW, tileH, WiFi.status() == WL_CONNECTED ? 0xCE79 : 0x2104);
    drawStringFB(30, quickPanelY + 150, "Wi-Fi", WiFi.status() == WL_CONNECTED ? 0x0000 : 0xFFFF, 2);
    String panelWifiText = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "Rozlaczone";
    if(panelWifiText.length() > 14) panelWifiText = panelWifiText.substring(0, 11) + "...";
    drawStringFB(30, quickPanelY + 180, panelWifiText, WiFi.status() == WL_CONNECTED ? 0x0000 : 0x8410, 1);

    // Ustawienia Kafelek
    fillRectFB(20 + tileW + 20, quickPanelY + 140, tileW, tileH, 0x2104); 
    drawStringFB(20 + tileW + 30, quickPanelY + 160, "Ustawienia", 0xFFFF, 2);

    // Obrot Kafelek
    fillRectFB(20, quickPanelY + 230, tileW, tileH, isLandscape ? 0xCE79 : 0x2104); 
    drawStringFB(30, quickPanelY + 240, "Auto-Obrot", isLandscape ? 0x0000 : 0xFFFF, 2); 
    drawStringFB(30, quickPanelY + 270, isLandscape ? "Tablet" : "Telefon", isLandscape ? 0x0000 : 0x8410, 1);
    
    // Zasilanie Kafelek
    fillRectFB(20 + tileW + 20, quickPanelY + 230, tileW, tileH, 0x2104); 
    drawStringFB(20 + tileW + 30, quickPanelY + 260, "Zasilanie", 0xFFFF, 2);

    // Uchwyt powrotny
    fillRectFB(SCR_W/2 - 40, quickPanelY + 430, 80, 6, 0x4208);
}

void drawAppDrawer() {
    if (appDrawerY >= SCR_H - 10) return; 
    fillRectFB_Accel(0, appDrawerY, SCR_W, SCR_H - appDrawerY, 0x1111);
    
    fillRectFB(SCR_W/2 - 30, appDrawerY + 15, 60, 6, 0x4208);
    drawStringFB(20, appDrawerY + 40, "Wszystkie Aplikacje", 0xFFFF, 2);
    fillRectFB(20, appDrawerY + 70, SCR_W-40, 2, 0x07E0);
    
    int drawIndex = 0;
    int cols = SCR_W / 110; 
    for (int i = 0; i < luaAppCount; i++) {
        if (!luaApps[i].isValid) continue; 
        int col = drawIndex % cols; int row = drawIndex / cols; 
        int ix = 30 + (col * 110); int iy = appDrawerY + 90 + (row * 130);
        
        if (iy < SCR_H) { 
            fillRectFB(ix, iy, 80, 80, 0x1DEB); 
            String dName = luaApps[i].name; if (dName.length() > 8) dName = dName.substring(0, 6) + "..";
            drawStringFB(ix + 5, iy + 90, dName, 0xFFFF, 1);
        }
        drawIndex++;
    }
}

void drawHomeSettings() {
    fillRectFB(0, SCR_H - NAV_BAR_H - 200, SCR_W, 200, 0x1111); fillRectFB(0, SCR_H - NAV_BAR_H - 200, SCR_W, 2, 0x07E0);
    drawStringFB(20, SCR_H - NAV_BAR_H - 180, "Edycja Pulpitu", 0xFFFF, 2);
    fillRectFB(40, SCR_H - NAV_BAR_H - 130, 180, 60, 0x4208); drawStringFB(75, SCR_H - NAV_BAR_H - 110, "Tapety", 0xFFFF, 2);
    fillRectFB(260, SCR_H - NAV_BAR_H - 130, 180, 60, 0x4208); drawStringFB(290, SCR_H - NAV_BAR_H - 110, "Widzety", 0xFFFF, 2);
}

void drawDesktopWidgets() {
    for(int i=0; i<MAX_WIDGETS; i++) {
        if(desktopWidgets[i].active) {
            int wx = desktopWidgets[i].x; int wy = desktopWidgets[i].y;
            fillRectFB_Accel(wx, wy, desktopWidgets[i].w, desktopWidgets[i].h, 0x2104); fillRectFB(wx, wy, desktopWidgets[i].w, 2, 0x07E0);
            drawStringFB(wx + 10, wy + 10, desktopWidgets[i].appName, 0xFFFF, 2);
            String content = desktopWidgets[i].widgetContent; if(content == "") content = "Oczekuje na dane z LUA...";
            drawStringFB(wx + 10, wy + 50, content, 0xCE79, 2);
        }
    }
}

void drawSettingsApp() {
    fbClear(0x1111); 
    
    fillRectFB(0, STATUS_BAR_H, SCR_W, 60, 0x1111);
    drawStringFB(20, STATUS_BAR_H + 20, "Ustawienia", 0xFFFF, 3);
    fillRectFB(20, STATUS_BAR_H + 55, SCR_W - 40, 1, 0x4208);

    if (currentTab == TAB_MAIN) {
        int listY = STATUS_BAR_H + 70;
        String menuNames[] = {"Ekran & Jasnosc", "Siec Wi-Fi", "Tapety Pulpitu", "Informacje o systemie", "Aktualizacje & Kopia"};
        uint16_t menuColors[] = {0xCE79, 0x07E0, 0x1DEB, 0xFD20, 0xF800}; 
        
        for(int i=0; i<5; i++) {
            fillRectFB(20, listY + i*75, SCR_W - 40, 60, 0x2104); 
            fillRectFB(20, listY + i*75, 10, 60, menuColors[i]);  
            drawStringFB(50, listY + i*75 + 22, menuNames[i], 0xFFFF, 2);
        }
    } 
    else {
        fillRectFB(20, STATUS_BAR_H + 70, 120, 40, 0x2104);
        drawStringFB(30, STATUS_BAR_H + 80, "<- Wroc", 0x07E0, 2);
        
        int contentY = STATUS_BAR_H + 130;
        
        if (currentTab == TAB_BRIGHTNESS) {
            drawStringFB(20, contentY, "Poziom Jasnosci", 0xFFFF, 2);
            fillRectFB(20, contentY + 30, 255, 30, 0x4208); fillRectFB(20, contentY + 30, screenBrightness, 30, 0xCE79);
            
            drawStringFB(20, contentY + 90, "Zarzadzanie Widgetami LUA:", 0xFFFF, 2);
            int wy = contentY + 120;
            bool hasAnyWidget = false;
            for (int i = 0; i < luaAppCount; i++) {
                if (luaApps[i].hasWidget) {
                    hasAnyWidget = true;
                    fillRectFB(20, wy, SCR_W - 40, 40, 0x18C3); drawStringFB(30, wy + 10, "+ Dodaj Widget: " + luaApps[i].name, 0xFFFF, 2);
                    wy += 50;
                }
            }
            if (!hasAnyWidget) { drawStringFB(20, wy, "Brak apek LUA z widgetami.", 0x8410, 2); wy += 40; }
            fillRectFB(20, wy, SCR_W - 40, 40, 0xF800); drawStringFB(30, wy + 10, "USUN WSZYSTKIE WIDGETY", 0xFFFF, 2);
        } 
        else if (currentTab == TAB_WIFI) {
            if (isScanningWiFi) { drawStringFB(20, contentY, "Skanowanie...", 0xFFFF, 2); } 
            else { 
                fillRectFB(20, contentY, 150, 40, 0x18C3); drawStringFB(35, contentY + 10, "SKANUJ", 0xFFFF, 2);
                fillRectFB(190, contentY, 200, 40, 0xF800); drawStringFB(200, contentY + 10, "ZAPOMNIJ SIEC", 0xFFFF, 2);
                for(int i=0; i<wifiCount; i++) {
                    drawStringFB(20, contentY + 60 + (i*40), wifiNetworks[i], 0xFFFF, 2); fillRectFB(20, contentY + 90 + (i*40), SCR_W - 40, 1, 0x4208);
                }
            }
        }
        else if (currentTab == TAB_WALLPAPER) {
            drawStringFB(20, contentY, "Pliki /wallpaper", 0xFFFF, 2);
            fillRectFB(20, contentY + 30, 200, 40, 0xF800); drawStringFB(30, contentY + 40, "USUN TAPETE", 0xFFFF, 2);
            for(int i=0; i<wallpaperCount; i++) {
                fillRectFB(20, contentY + 80 + (i*50), SCR_W - 40, 40, 0x2104); drawStringFB(30, contentY + 90 + (i*50), wallpapers[i], 0xFFFF, 2);
            }
        }
        else if (currentTab == TAB_INFO) {
            drawStringFB(20, contentY, "Supernova Alfa 1.0 (P4)", 0xFFFF, 2);
            float psramTotal = ESP.getPsramSize() / 1024.0 / 1024.0; float psramFree = ESP.getFreePsram() / 1024.0 / 1024.0; float psramUsed = psramTotal - psramFree;
            char buf[64]; sprintf(buf, "RAM (OS): %.1f / %.1f MB", psramUsed, psramTotal); drawStringFB(20, contentY + 40, String(buf), 0xFFFF, 2);
            fillRectFB(20, contentY + 65, 300, 15, 0x4208); if(psramTotal > 0) fillRectFB(20, contentY + 65, (psramUsed/psramTotal)*300, 15, 0x07E0);

            float luaRamUsed = mmu_state.totalAllocated / 1024.0 / 1024.0; float luaRamLimit = mmu_state.limit / 1024.0 / 1024.0;
            char buf2[64]; sprintf(buf2, "RAM LUA: %.2f / %.1f MB", luaRamUsed, luaRamLimit); drawStringFB(20, contentY + 90, String(buf2), 0xFFFF, 2);
            fillRectFB(20, contentY + 115, 300, 15, 0x4208); if(luaRamLimit > 0) fillRectFB(20, contentY + 115, (luaRamUsed/luaRamLimit)*300, 15, 0x1DEB);

            drawStringFB(20, contentY + 150, "Zainstalowane Apki: " + String(luaAppCount), 0xFFFF, 2);
            drawStringFB(20, contentY + 180, "Sprawne Apki: " + String(validAppCount), 0x07E0, 2);
            fillRectFB(20, contentY + 230, 250, 50, 0xF800); drawStringFB(40, contentY + 245, "RESTART SYSTEMU", 0xFFFF, 2);
        }
        else if (currentTab == TAB_UPDATE) {
            drawStringFB(20, contentY, "Narzedzia Ratunkowe", 0xFFFF, 2); drawStringFB(20, contentY + 30, updateStatusMsg, 0x07E0, 1); 
            fillRectFB(20, contentY + 50, 160, 40, 0x2104); drawStringFB(40, contentY + 60, "SKANUJ", 0xFFFF, 2);
            fillRectFB(200, contentY + 50, SCR_W - 220, 40, 0xF800); drawStringFB(215, contentY + 60, "RECOVERY", 0xFFFF, 2);
            int listY = contentY + 110;
            for(int i=0; i<updateFileCount; i++) {
                fillRectFB(20, listY + (i*50), SCR_W - 40, 40, 0x2104); drawStringFB(30, listY + (i*50) + 10, updateFiles[i], 0xFFFF, 2);
            }
        }
    }
}

void drawKeyboard() { 
    fbClear(0x2104); drawStringFB(20, 50, "Wpisz haslo:", 0x07E0, 2); drawStringFB(20, 80, selectedSSID, 0xFFFF, 2);
    fillRectFB(20, 120, SCR_W-40, 50, 0x0000); drawStringFB(30, 135, keyboardBuffer + "_", 0xFFFF, 2);
    int keyW = SCR_W>480 ? 60 : 40, keyH = 55, spacing = 5;
    for(int r=0; r<4; r++) {
        int len = strlen(keyboardLayout[r]); int startX = (SCR_W - (len*(keyW+spacing)))/2;
        for(int c=0; c<len; c++) {
            fillRectFB(startX + c*(keyW+spacing), 200 + r*(keyH+spacing), keyW, keyH, 0x4208);
            drawStringFB(startX + c*(keyW+spacing) + (keyW/2-6), 200 + r*(keyH+spacing) + 20, String(keyboardLayout[r][c]), 0xFFFF, 2);
        }
    }
    fillRectFB(20, SCR_H-NAV_BAR_H-60, 100, 50, 0xF800); drawStringFB(30, SCR_H-NAV_BAR_H-45, "COFNIJ", 0xFFFF, 2);
    fillRectFB(130, SCR_H-NAV_BAR_H-60, 200, 50, 0x4208); drawStringFB(185, SCR_H-NAV_BAR_H-45, "SPACJA", 0xFFFF, 2);
    fillRectFB(340, SCR_H-NAV_BAR_H-60, SCR_W-360, 50, 0x07E0); drawStringFB(360, SCR_H-NAV_BAR_H-45, "ENTER", 0x0000, 2);
}

void drawFilesApp() {
    fbClear(0xFFFF); fillRectFB(0, STATUS_BAR_H, SCR_W, 60, 0x1111); drawStringFB(20, STATUS_BAR_H + 20, "Pliki na SD", 0xFFFF, 3);
    int yOffset = STATUS_BAR_H + 80;
    for (int i = 0; i < fileCount; i++) {
        fillRectFB(20, yOffset, 30, 30, fileIsDir[i] ? 0xFD20 : 0x4208);
        drawStringFB(65, yOffset + 5, fileNames[i], 0x0000, 2); fillRectFB(20, yOffset + 40, SCR_W - 40, 1, 0xCE79); yOffset += 50;
    }
}

void drawRecents() {
    fbClear(0x1111); drawStringFB(20, STATUS_BAR_H + 20, "Zadania w tle", 0xFFFF, 3);
    int yOff = STATUS_BAR_H + 80;
    for(int i=0; i<MAX_LUA_TASKS; i++) {
        if(luaTasks[i].isRunning) {
            fillRectFB(20, yOff, SCR_W-40, 80, 0x2104);
            drawStringFB(40, yOff + 15, "Aplikacja: " + luaTasks[i].appName, 0xFFFF, 2);
            drawStringFB(40, yOff + 45, "STATUS: ZAWIESZONA", 0x07E0, 2);
            fillRectFB(SCR_W - 100, yOff + 20, 60, 40, 0xF800); drawStringFB(SCR_W - 85, yOff + 30, "X", 0xFFFF, 2);
            yOff += 100;
        }
    }
    if (yOff == STATUS_BAR_H + 80) drawStringFB(20, yOff + 50, "Brak aplikacji w tle.", 0x8410, 2);
}

void drawAndroidLauncher() {
    if (currentWallpaperPath != "") drawBMP(currentWallpaperPath); else fbClear(0x18C3); 
    drawDesktopWidgets(); 

    int dotY = SCR_H - NAV_BAR_H - 30; 
    for (int i = 0; i < 3; i++) { fillRectFB(SCR_W/2 - 40 + (i * 35), dotY, 15, 15, (i == currentPage) ? 0xFFFF : 0x4208); }

    if (currentPage == 1) {
        int dockY = SCR_H - NAV_BAR_H - 120;
        int space = SCR_W / 4;
        
        fillRectFB(space - 40, dockY, 80, 80, 0x1111); fillRectFB(space - 25, dockY + 30, 50, 30, 0xFD20); fillRectFB(space - 25, dockY + 25, 20, 5, 0xFD20);  
        drawStringFB(space - 25, dockY + 90, "Pliki", 0xFFFF, 2);

        int sx = space*2; int sy = dockY + 40;
        fillRectFB(sx - 40, dockY, 80, 80, 0x1111); fillRectFB(sx - 25, sy - 6, 50, 12, 0xBDD7); fillRectFB(sx - 6, sy - 25, 12, 50, 0xBDD7); 
        for(int i=-16; i<=16; i++) { for(int w=-4; w<=4; w++) { fbDrawPixel(sx + i + w, sy + i - w, 0xBDD7); fbDrawPixel(sx + i + w, sy - i + w, 0xBDD7); } }
        for (int y = -18; y <= 18; y++) { for (int x = -18; x <= 18; x++) { if (x * x + y * y <= 18 * 18) fbDrawPixel(sx + x, sy + y, 0xBDD7); } }
        for (int y = -8; y <= 8; y++) { for (int x = -8; x <= 8; x++) { if (x * x + y * y <= 8 * 8) fbDrawPixel(sx + x, sy + y, 0x1111); } }
        drawStringFB(sx - 35, dockY + 90, "Ustaw.", 0xFFFF, 2);

        fillRectFB(space*3 - 40, dockY, 80, 80, 0x1111);
        for(int x=0; x<3; x++) { for(int y=0; y<3; y++) { fillRectFB(space*3 - 25 + x*18, dockY + 22 + y*18, 8, 8, 0xFFFF); } }
        drawStringFB(space*3 - 35, dockY + 90, "Aplik.", 0xFFFF, 2);
    }
    if (currentState == STATE_HOME_SETTINGS) drawHomeSettings();
}

// ================== LOGIKA DOTYKU OS ==================
void handleAppTouch(int tx, int ty, int diffX, int diffY) {
    if (currentState == STATE_HOME_SETTINGS) {
        if (ty >= SCR_H - NAV_BAR_H - 130 && ty <= SCR_H - NAV_BAR_H - 70) {
            if (tx >= 40 && tx <= 220) { currentTab = TAB_WALLPAPER; currentState = STATE_SETTINGS; } 
            else if (tx >= 260 && tx <= 440) { currentState = STATE_SETTINGS; currentTab = TAB_INFO; } 
        } else { currentState = STATE_HOME; }
        return;
    }
    else if (currentState == STATE_APP_DRAWER) {
        if (diffY > 40 && abs(diffX) < 40) { targetAppDrawerY = SCR_H; return; }
        if (appDrawerY < STATUS_BAR_H + 10) {
            int drawIndex = 0; int cols = SCR_W / 110;
            for (int i = 0; i < luaAppCount; i++) {
                if (!luaApps[i].isValid) continue;
                int col = drawIndex % cols; int row = drawIndex / cols; 
                int ix = 30 + (col * 110); int iy = 110 + (row * 130);
                if (tx >= ix && tx <= ix + 80 && ty >= iy && ty <= iy + 80) {
                    targetAppDrawerY = SCR_H; 
                    triggerAppLaunch(i, ix + 40, iy + 40); 
                    break;
                }
                drawIndex++;
            }
        }
    }
    else if (currentState == STATE_SETTINGS) {
        if (currentTab == TAB_MAIN) {
            int listY = STATUS_BAR_H + 70;
            if (ty > listY && ty < listY + 60) currentTab = TAB_BRIGHTNESS;
            else if (ty > listY + 75 && ty < listY + 75 + 60) currentTab = TAB_WIFI;
            else if (ty > listY + 150 && ty < listY + 150 + 60) currentTab = TAB_WALLPAPER;
            else if (ty > listY + 225 && ty < listY + 225 + 60) currentTab = TAB_INFO;
            else if (ty > listY + 300 && ty < listY + 300 + 60) currentTab = TAB_UPDATE;
        } 
        else {
            if (ty > STATUS_BAR_H + 70 && ty < STATUS_BAR_H + 110 && tx < 150) { currentTab = TAB_MAIN; return; }
            int contentY = STATUS_BAR_H + 130;
            if (currentTab == TAB_BRIGHTNESS) {
                int wy = contentY + 120;
                for (int i = 0; i < luaAppCount; i++) {
                    if (luaApps[i].hasWidget) {
                        if (ty >= wy && ty <= wy + 40 && tx >= 20 && tx <= SCR_W - 20) {
                            for(int w=0; w<MAX_WIDGETS; w++) {
                                if(!desktopWidgets[w].active) {
                                    desktopWidgets[w].active = true; desktopWidgets[w].appName = luaApps[i].name; desktopWidgets[w].widgetContent = "Uruchamianie..."; 
                                    desktopWidgets[w].x = 20; desktopWidgets[w].y = 80 + (w * 130); desktopWidgets[w].w = SCR_W - 40; desktopWidgets[w].h = 100;
                                    preferences.putString(("w_app" + String(w)).c_str(), luaApps[i].name); break;
                                }
                            }
                            currentState = STATE_HOME; return;
                        }
                        wy += 50;
                    }
                }
                if (ty >= wy && ty <= wy + 40 && tx >= 20 && tx <= SCR_W - 20) {
                    for(int w=0; w<MAX_WIDGETS; w++) { desktopWidgets[w].active = false; preferences.putString(("w_app" + String(w)).c_str(), ""); }
                    currentState = STATE_HOME; return;
                }
            }
            else if (currentTab == TAB_WIFI) {
                if (ty >= contentY && ty <= contentY + 40 && tx >= 20 && tx <= 170) { if (!isScanningWiFi) { isScanningWiFi = true; WiFi.scanNetworks(true); } }
                else if (ty >= contentY && ty <= contentY + 40 && tx >= 190 && tx <= 390) { preferences.putString("ssid", ""); preferences.putString("pass", ""); WiFi.disconnect(); }
                else { for(int i=0; i<wifiCount; i++) { if (ty >= contentY + 60 + (i*40) && ty <= contentY + 60 + (i*40) + 40) { selectedSSID = wifiNetworks[i]; keyboardBuffer = ""; currentState = STATE_KEYBOARD; } } }
            }
            else if (currentTab == TAB_WALLPAPER) {
                for(int i=0; i<wallpaperCount; i++) { int btnY = contentY + 80 + (i*50); if (ty >= btnY && ty <= btnY + 40 && tx >= 20 && tx <= SCR_W-20) { currentWallpaperPath = "/wallpaper/" + wallpapers[i]; currentState = STATE_HOME; } }
            }
            else if (currentTab == TAB_INFO) { if (ty >= contentY + 230 && ty <= contentY + 280 && tx >= 20 && tx <= 270) { ESP.restart(); } }
            else if (currentTab == TAB_UPDATE) {
                if (ty >= contentY + 50 && ty <= contentY + 90 && tx >= 20 && tx <= 180) { scanUpdateFiles(); }
                else if (ty >= contentY + 50 && ty <= contentY + 90 && tx >= 200 && tx <= SCR_W - 20) { triggerRecovery(); }
                else {
                    int listY = contentY + 110;
                    for(int i=0; i<updateFileCount; i++) { int btnY = listY + (i*50); if (ty >= btnY && ty <= btnY + 40 && tx >= 20 && tx <= SCR_W - 20) {
                            String basePath = SD_MMC.exists("/update/ofline") ? "/update/ofline/" : "/update/offline/"; String fullPath = basePath + updateFiles[i]; File updateBin = SD_MMC.open(fullPath);
                            if (updateBin) { size_t updateSize = updateBin.size(); if (updateSize == 0) { updateStatusMsg = "Blad: Pusty plik .bin!"; updateBin.close(); return; }
                                if (Update.begin(updateSize, U_FLASH)) { fbClear(0x1111); drawStringFB(20, 100, "SUPERNOVA UPDATER", 0x07E0, 3); fbPush();
                                    uint8_t buff[1024]; size_t written = 0; int lastPercent = -1; bool flashError = false;
                                    while (updateBin.available()) { size_t len = updateBin.read(buff, sizeof(buff)); if (Update.write(buff, len) != len) { flashError = true; updateStatusMsg = "Blad Flash"; break; } written += len;
                                        int percent = (written * 100) / updateSize; if (percent != lastPercent) { fillRectFB(20, 300, SCR_W - 40, 30, 0x4208); fillRectFB(20, 300, (percent * (SCR_W - 40)) / 100, 30, 0x07E0); drawStringFB(20, 350, "Zapisano: " + String(percent) + "%", 0xFFFF, 2); fbPush(); lastPercent = percent; } yield(); vTaskDelay(pdMS_TO_TICKS(1)); 
                                    }
                                    if (!flashError && written == updateSize) { if (Update.end()) { ESP.restart(); } else { updateStatusMsg = "Weryfikacja: " + String(Update.errorString()); } } else if (!flashError) { updateStatusMsg = "Blad: Przerwano odczyt SD!"; }
                                } else { updateStatusMsg = "Brak partycji OTA!"; } updateBin.close();
                            } else { updateStatusMsg = "Blad odczytu z karty"; }
                        }
                    }
                }
            }
        }
    }
    else if (currentState == STATE_RECENTS) {
        int yOff = STATUS_BAR_H + 80;
        for(int i=0; i<MAX_LUA_TASKS; i++) {
            if(luaTasks[i].isRunning) {
                if (tx > SCR_W - 100 && ty > yOff + 20 && ty < yOff + 60) luaTasks[i].isRunning = false; 
                else if (ty > yOff && ty < yOff + 80) { triggerAppLaunch(i, SCR_W/2, yOff + 40); return; }
                yOff += 100;
            }
        }
    }
    else if (currentState == STATE_HOME) {
        if (diffY < -50 && abs(diffX) < 50) { 
            currentState = STATE_APP_DRAWER;
            targetAppDrawerY = STATUS_BAR_H;
            return;
        }

        if (diffX < -50) currentPage = min(2, currentPage + 1);
        else if (diffX > 50) currentPage = max(0, currentPage - 1);
        else if (abs(diffX) < 15 && abs(diffY) < 15 && currentPage == 1) {
            int dockY = SCR_H - NAV_BAR_H - 120; int space = SCR_W / 4;
            if (ty >= dockY && ty <= dockY + 80) {
                if (tx >= space - 40 && tx <= space + 40) { 
                    fileCount = 0; File root = SD_MMC.open("/"); File file = root.openNextFile();
                    while(file && fileCount < MAX_FILES) { fileNames[fileCount] = file.name(); fileIsDir[fileCount] = file.isDirectory(); fileCount++; file = root.openNextFile(); }
                    currentState = STATE_FILES;
                }
                else if (tx >= space*2 - 40 && tx <= space*2 + 40) { loadWallpapers(); currentState = STATE_SETTINGS; currentTab = TAB_MAIN; }
                else if (tx >= space*3 - 40 && tx <= space*3 + 40) { currentState = STATE_APP_DRAWER; targetAppDrawerY = STATUS_BAR_H; } 
            }
        }
    }
}

// ================== 12. SETUP BOOTLOADERA ==================
void setup() {
    Serial.begin(115200);
    preferences.begin("supernova", false);
    screenBrightness = preferences.getInt("bright", 200); 
    pinMode(BACKLIGHT_PIN, OUTPUT); analogWrite(BACKLIGHT_PIN, screenBrightness); 
    lcd.begin();
    
    frame = (uint16_t*)heap_caps_malloc(480 * 800 * 2, MALLOC_CAP_SPIRAM);
    display_frame = (uint16_t*)heap_caps_malloc(480 * 800 * 2, MALLOC_CAP_SPIRAM);
    
    Wire1.begin(TOUCH_SDA, TOUCH_SCL); ts.begin(); SD_MMC.begin("/sdcard"); 
    if (preferences.getBool("recovery", false)) { runGraphicalRecovery(); }

    showBootScreen("Inicjalizacja MMU", 10); delay(200); init_MMU(); 
    showBootScreen("Modul dotyku OK", 25); delay(200);
    xTaskCreatePinnedToCore(touchPollingTask, "TouchTask", 2048, NULL, 3, NULL, 1);

    showBootScreen("Ladowanie konfiguracji...", 30);
    String savedSSID = preferences.getString("ssid", ""); String savedPass = preferences.getString("pass", "");
    if (savedSSID.length() > 0) { WiFi.begin(savedSSID.c_str(), savedPass.c_str()); }

    if (!SD_MMC.exists("/")) { showBootScreen("BLAD: Brak SD!", 100, 0xF800); while(true) delay(100); }
    showBootScreen("Wczytywanie Systemu Plikow", 40); delay(200); scanLuaApps(); loadWallpapers();
    
    for(int i=0; i<MAX_LUA_TASKS; i++) {
        luaTasks[i].isRunning = false;
        xTaskCreatePinnedToCore(luaBackgroundCoreTask, "LuaVM", 32768, (void*)i, 1, &luaTasks[i].taskHandle, 0);
    }
    
    showBootScreen("Witaj w Alfa 1.0!", 100); delay(400);
    
    for(int w=0; w<MAX_WIDGETS; w++) {
        String savedAppWidget = preferences.getString(("w_app" + String(w)).c_str(), "");
        if (savedAppWidget.length() > 0) {
            desktopWidgets[w].active = true; desktopWidgets[w].appName = savedAppWidget; desktopWidgets[w].widgetContent = "Oczekuje na dane...";
            desktopWidgets[w].x = 20; desktopWidgets[w].y = 80 + (w * 130); desktopWidgets[w].w = SCR_W - 40; desktopWidgets[w].h = 100;
        }
    }
}

// ================== 13. GŁÓWNA PĘTLA WYKONAWCZA ==================
void loop() {
    static unsigned long lastRefresh = 0;
    updateClock(); 

    if (isScanningWiFi) {
        int n = WiFi.scanComplete();
        if (n >= 0) { wifiCount = n; for(int i=0; i<n && i<15; i++) wifiNetworks[i] = WiFi.SSID(i); WiFi.scanDelete(); isScanningWiFi = false; }
    }

    // Ustawienie Celu dla Szybkiego Panelu
    targetQuickPanelY = isQuickPanelOpen ? 0 : -450; 

    // --- FIZYKA PŁYNNOŚCI (Damping) ---
    if (quickPanelY != targetQuickPanelY) {
        quickPanelY += (targetQuickPanelY - quickPanelY) * 0.25; 
        if (abs(quickPanelY - targetQuickPanelY) < 2) quickPanelY = targetQuickPanelY;
    }
    if (appDrawerY != targetAppDrawerY) {
        appDrawerY += (targetAppDrawerY - appDrawerY) * 0.2; 
        if (abs(appDrawerY - targetAppDrawerY) < 2) {
            appDrawerY = targetAppDrawerY;
            if (appDrawerY >= SCR_H && currentState == STATE_APP_DRAWER) currentState = STATE_HOME;
        }
    }

    bool currentTouched = global_isTouched;
    uint16_t raw_tx = global_raw_tx;
    uint16_t raw_ty = global_raw_ty;

    if (currentTouched) {
        if (!wasTouching) { touchStartTime = millis(); startX = raw_tx; startY = raw_ty; isLongPressTriggered = false; }
        
        uint16_t tx = raw_tx; uint16_t ty = raw_ty;
        if (isLandscape) { tx = 799 - raw_ty; ty = raw_tx; }
        global_tx = tx; global_ty = ty - STATUS_BAR_H; 
        
        if (isQuickPanelOpen && ty >= quickPanelY + 60 && ty <= quickPanelY + 100 && tx >= 20 && tx <= SCR_W - 20) {
            int newB = ((tx - 20) * 255) / (SCR_W - 40); if(newB < 10) newB = 10; if(newB > 255) newB = 255;
            screenBrightness = newB; analogWrite(BACKLIGHT_PIN, screenBrightness);
        }
        else if (isQuickPanelOpen && !wasTouching) {
            int tileW = (SCR_W - 60) / 2;
            if (ty >= quickPanelY + 200 && ty <= quickPanelY + 270 && tx >= 20 && tx <= 20 + tileW) {
                isRotAnimating = true; rotAnimProgress = 0.0;
                isLandscape = !isLandscape;
                if (isLandscape) { SCR_W = 800; SCR_H = 480; } else { SCR_W = 480; SCR_H = 800; }
                isQuickPanelOpen = false; targetQuickPanelY = -450;
            }
            else if (ty >= quickPanelY + 320 && ty <= quickPanelY + 360 && tx >= SCR_W/2 - 60 && tx <= SCR_W/2 + 60) {
                isQuickPanelOpen = false; targetQuickPanelY = -450; preferences.putInt("bright", screenBrightness);
            }
        }
        wasTouching = true;
    } else {
        if (wasTouching && !isLongPressTriggered && !isAppAnimating && !isRotAnimating) { 
            uint16_t tx = startX; uint16_t ty = startY;
            if (isLandscape) { tx = 799 - startY; ty = startX; }
            int diffX = global_tx - tx; int diffY = (global_ty + STATUS_BAR_H) - ty;
            
            if (!isAssistiveMenuOpen && abs(diffX) < 15 && abs(diffY) < 15 && ty >= (SCR_H/2) - 20 && ty <= (SCR_H/2) + 20 && tx >= SCR_W - 55 && tx <= SCR_W - 15) {
                isAssistiveMenuOpen = true;
            }
            else if (isAssistiveMenuOpen) {
                int menuX = SCR_W - 155; int menuY = (SCR_H/2) - 100;
                if (tx >= menuX && tx <= menuX + 150 && ty >= menuY && ty <= menuY + 200) {
                    if (ty >= menuY + 30 && ty <= menuY + 65) { currentState = STATE_HOME; isAssistiveMenuOpen = false; }
                    else if (ty >= menuY + 70 && ty <= menuY + 105) { currentState = STATE_FILES; isAssistiveMenuOpen = false; }
                    else if (ty >= menuY + 110 && ty <= menuY + 145) { currentState = STATE_SETTINGS; currentTab = TAB_MAIN; isAssistiveMenuOpen = false; }
                    else if (ty >= menuY + 150 && ty <= menuY + 185) { isAssistiveMenuOpen = false; }
                } else { isAssistiveMenuOpen = false; }
            }
            // GEST DO OTWIERANIA/ZAMYKANIA SZYBKIEGO PANELU
            else if (ty < STATUS_BAR_H + 20 && diffY > 40) { 
                isQuickPanelOpen = true; 
            }
            else if (isQuickPanelOpen && diffY < -40) { 
                isQuickPanelOpen = false; preferences.putInt("bright", screenBrightness); 
            }
            else if (abs(diffX) < 15 && abs(diffY) < 15 && ty <= STATUS_BAR_H && tx <= 180) {
                isQuickPanelOpen = !isQuickPanelOpen; 
                if(!isQuickPanelOpen) preferences.putInt("bright", screenBrightness);
            }
            else if (global_ty + STATUS_BAR_H > SCR_H - NAV_BAR_H) {
                if (global_tx < SCR_W/3) { 
                    if(currentState == STATE_APP_DRAWER) { targetAppDrawerY = SCR_H; }
                    else if(currentForegroundTask >= 0) { triggerAppClose(); }
                    else { currentState = STATE_HOME; }
                }
                else if (global_tx > SCR_W/3 && global_tx < (SCR_W/3)*2) { 
                    targetAppDrawerY = SCR_H; 
                    if(currentForegroundTask >= 0) { triggerAppClose(); }
                    else { currentState = STATE_HOME; }
                }
                else if (global_tx > (SCR_W/3)*2) { 
                    if(currentForegroundTask >= 0) { triggerAppClose(); }
                    currentState = STATE_RECENTS; 
                }
            }
            else { handleAppTouch(global_tx, global_ty + STATUS_BAR_H, diffX, diffY); }
        }
        wasTouching = false; isLongPressTriggered = false; 
    }

    if (millis() - lastRefresh > 30) {
        
        // Render Lua jeśli jest aktywna i NIE JEST W TRAKCIE ANIMACJI
        if (currentState == STATE_LUA_APP && !isAppAnimating && !isRotAnimating) {
            if(currentForegroundTask >= 0 && luaTasks[currentForegroundTask].errorMsg[0] != '\0') {
                fbClear(0xF800); drawStringFB(20, 100, "BLAD APLIKACJI LUA:", 0xFFFF, 2);
                String err = String((char*)luaTasks[currentForegroundTask].errorMsg); int eY = 140;
                while(err.length() > 0) { String chunk = err.substring(0, 30); err = err.substring(chunk.length()); drawStringFB(20, eY, chunk, 0xFFFF, 2); eY += 30; }
                fbPush(); 
            }
        } 
        else {
            // Render Tła
            if (currentState == STATE_FILES) drawFilesApp();
            else if (currentState == STATE_SETTINGS) drawSettingsApp();
            else if (currentState == STATE_RECENTS) drawRecents();
            else if (currentState == STATE_KEYBOARD) drawKeyboard();
            else { drawAndroidLauncher(); drawAppDrawer(); }
            
            // RENDER ANIMACJI APLIKACJI
            if (isAppAnimating) {
                if (!isAppClosing) animAppScale += 0.15; else animAppScale -= 0.15;
                if (animAppScale > 1.0) {
                    animAppScale = 1.0; isAppAnimating = false;
                    if (!isAppClosing) launchLuaApp(luaApps[pendingAppIndex].path, luaApps[pendingAppIndex].name);
                } else if (animAppScale < 0.0) {
                    animAppScale = 0.0; isAppAnimating = false; currentState = STATE_HOME;
                }
                if (isAppAnimating) {
                    float ease = 1.0 - pow(1.0 - animAppScale, 3);
                    int curW = SCR_W * ease; int curH = SCR_H * ease;
                    int curX = animAppX - (curW / 2); int curY = animAppY - (curH / 2);
                    if(curX < 0) curX = 0; if(curX + curW > SCR_W) curW = SCR_W - curX;
                    if(curY < 0) curY = 0; if(curY + curH > SCR_H) curH = SCR_H - curY;
                    fillRectFB_Accel(curX, curY, curW, curH, 0x18C3); 
                }
            }

            // RENDER ANIMACJI ROTACJI (Wipe)
            if (isRotAnimating) {
                rotAnimProgress += 0.1;
                if (rotAnimProgress >= 1.0) { rotAnimProgress = 1.0; isRotAnimating = false; }
                int wipeW = SCR_W * (1.0 - rotAnimProgress);
                fillRectFB_Accel(0, 0, wipeW, SCR_H, 0x0000); 
            }

            drawStatusBar(); 
            drawNavigationBar(); 
            drawAssistiveMenu(); 
            drawQuickPanel(); 
            fbPush();
        }
        lastRefresh = millis();
    }
}