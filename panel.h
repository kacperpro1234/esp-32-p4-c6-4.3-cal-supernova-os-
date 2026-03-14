#pragma once
#include <Arduino.h>
#include <WiFi.h>

// Deklaracje funkcji i zmiennych z głównego pliku, żeby panel wiedział jak rysować
extern void fillRectFB(int x, int y, int w, int h, uint16_t color);
extern void drawStringFB(int x, int y, String text, uint16_t color, int scale);
extern int screenBrightness;

// Zmienne Szybkiego Panelu (widoczne w całym systemie)
bool isQuickPanelOpen = false;
int quickPanelY = -400;       
int targetQuickPanelY = -400; 

// Funkcja rysująca pasek
void drawQuickPanel() {
    if (quickPanelY <= -400) return; // Nie rysuj, gdy panel jest całkowicie schowany
    
    // Tło panelu (ciemnoszare, lekko półprzezroczyste w wyobraźni)
    fillRectFB(0, quickPanelY, 480, 400, 0x2104); 
    fillRectFB(0, quickPanelY + 398, 480, 2, 0x07E0); // Turkusowa ramka końcowa
    
    drawStringFB(20, quickPanelY + 20, "Szybkie Ustawienia", 0xFFFF, 2);
    
    // Pasek Jasności
    drawStringFB(20, quickPanelY + 70, "Jasnosc:", 0xFFFF, 2);
    fillRectFB(130, quickPanelY + 70, 255, 20, 0x4208); // Tło paska
    fillRectFB(130, quickPanelY + 70, screenBrightness, 20, 0x07E0); // Wypełnienie jasności

    // Przyciski "WiFi" i "Ustawienia"
    fillRectFB(20, quickPanelY + 130, 130, 60, 0x4208);
    drawStringFB(45, quickPanelY + 150, "WiFi", WiFi.status() == WL_CONNECTED ? 0x07E0 : 0xFFFF, 2);
    
    fillRectFB(170, quickPanelY + 130, 130, 60, 0x4208);
    drawStringFB(185, quickPanelY + 150, "Ustaw.", 0xFFFF, 2);
    
    // Uchwyt zamykania na dole panelu
    fillRectFB(480/2 - 40, quickPanelY + 360, 80, 10, 0xFFFF); 
}