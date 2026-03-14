#include "lua_main.h"
#include <WiFi.h>

extern int SCR_W;
extern int SCR_H;
extern int STATUS_BAR_H;
extern int NAV_BAR_H;
extern SystemState currentState; 
extern volatile bool wasTouching;
extern int global_tx;
extern int global_ty;
extern int screenBrightness;
extern String timeString;

extern void fillRectFB(int x, int y, int w, int h, uint16_t color);
extern void fillRectFB_Accel(int x, int y, int w, int h, uint16_t color);
extern void drawStringFB(int x, int y, String text, uint16_t color, int scale);
extern void fbDrawPixel(int x, int y, uint16_t color);
extern void drawStatusBar();
extern void drawNavigationBar();
extern void drawQuickPanel();
extern void fbPush();
extern void triggerRecovery(); 
extern void handleWidgetUpdate(int callerId, String newText); 

LuaProcess luaTasks[MAX_LUA_TASKS];
int currentForegroundTask = -1;

MMU_Block mmu_state;

void* mmu_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; 
    if (nsize == 0) { 
        if (ptr) { 
            heap_caps_free(ptr); 
            mmu_state.totalAllocated -= osize; 
        } 
        return NULL; 
    }
    
    if (ptr == NULL) {
        if (mmu_state.totalAllocated + nsize > mmu_state.limit) return NULL;
    } else if (nsize > osize) {
        if (mmu_state.totalAllocated + (nsize - osize) > mmu_state.limit) return NULL;
    }

    void* new_ptr = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
    if (new_ptr) {
        if (ptr == NULL) mmu_state.totalAllocated += nsize;
        else {
            if (nsize > osize) mmu_state.totalAllocated += (nsize - osize);
            else mmu_state.totalAllocated -= (osize - nsize);
        }
    }
    return new_ptr;
}

void init_MMU() { 
    mmu_state.totalAllocated = 0; 
    mmu_state.limit = 20 * 1024 * 1024; // 20 MB Limit
}

static bool isCallerForeground(lua_State* L) {
    if (currentForegroundTask < 0) return false;
    return (luaTasks[currentForegroundTask].L == L && luaTasks[currentForegroundTask].isForeground);
}

// =======================================================
// NOWE FUNKCJE LUA: OBSŁUGA SPRZĘTOWA GPIO
// =======================================================
static int lua_pin_mode(lua_State* L) {
    int pin = luaL_checkinteger(L, 1);
    int mode = luaL_checkinteger(L, 2);
    // UWAGA: Nie blokujemy wywołań w tle! Aplikacja może sterować pinami będąc ukryta.
    pinMode(pin, mode);
    return 0;
}

static int lua_digital_write(lua_State* L) {
    int pin = luaL_checkinteger(L, 1);
    int val = luaL_checkinteger(L, 2);
    digitalWrite(pin, val);
    return 0;
}

static int lua_digital_read(lua_State* L) {
    int pin = luaL_checkinteger(L, 1);
    int val = digitalRead(pin);
    lua_pushinteger(L, val);
    return 1;
}

static int lua_analog_write(lua_State* L) {
    int pin = luaL_checkinteger(L, 1);
    int val = luaL_checkinteger(L, 2);
    analogWrite(pin, val);
    return 0;
}

static int lua_analog_read(lua_State* L) {
    int pin = luaL_checkinteger(L, 1);
    int val = analogRead(pin);
    lua_pushinteger(L, val);
    return 1;
}

// =======================================================
// API AKCELERATORA SPRITE 2D PPU
// =======================================================
static int lua_load_sprite(lua_State* L) {
    int callerId = -1;
    for(int i=0; i<MAX_LUA_TASKS; i++) { if(luaTasks[i].L == L) { callerId = i; break; } }
    if (callerId < 0) return 0;
    
    int id = luaL_checkinteger(L, 1);
    String path = luaL_checkstring(L, 2);
    loadSprite_PPU(callerId, id, path);
    return 0;
}

static int lua_draw_sprite(lua_State* L) {
    if (!isCallerForeground(L)) return 0; 
    int callerId = -1;
    for(int i=0; i<MAX_LUA_TASKS; i++) { if(luaTasks[i].L == L) { callerId = i; break; } }
    if (callerId < 0) return 0;

    int id = luaL_checkinteger(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3) + STATUS_BAR_H;
    
    drawSprite_PPU(callerId, id, x, y);
    return 0;
}

// =======================================================
// API GRAFICZNE I SYSTEMOWE
// =======================================================
static int l_lcd_fill_batch(lua_State *L) {
    if (!isCallerForeground(L)) return 0;
    if (!lua_istable(L, 1)) return 0;
    
    int len = lua_objlen(L, 1);
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1); int x = lua_tointeger(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 2); int y = lua_tointeger(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 3); int w = lua_tointeger(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 4); int h = lua_tointeger(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, -1, 5); int col = lua_tointeger(L, -1); lua_pop(L, 1);
            
            y += STATUS_BAR_H;
            if (y < STATUS_BAR_H) { h -= (STATUS_BAR_H - y); y = STATUS_BAR_H; }
            if (y + h > SCR_H - NAV_BAR_H) h = (SCR_H - NAV_BAR_H) - y;
            if (h > 0) fillRectFB_Accel(x, y, w, h, (uint16_t)col);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int lua_color_rgb(lua_State* L) {
    int r = luaL_checkinteger(L, 1); int g = luaL_checkinteger(L, 2); int b = luaL_checkinteger(L, 3);
    lua_pushinteger(L, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); return 1;
}

static int lua_lcd_fill(lua_State* L) {
    if (!isCallerForeground(L)) return 0; 
    int x = luaL_checkinteger(L, 1); int y = luaL_checkinteger(L, 2) + STATUS_BAR_H; 
    int w = luaL_checkinteger(L, 3); int h = luaL_checkinteger(L, 4); int c = luaL_checkinteger(L, 5);
    if (y < STATUS_BAR_H) { h -= (STATUS_BAR_H - y); y = STATUS_BAR_H; }
    if (y + h > SCR_H - NAV_BAR_H) h = (SCR_H - NAV_BAR_H) - y;
    if (h > 0) fillRectFB(x, y, w, h, c); return 0;
}

static int lua_lcd_fill_accel(lua_State* L) {
    if (!isCallerForeground(L)) return 0; 
    int x = luaL_checkinteger(L, 1); int y = luaL_checkinteger(L, 2) + STATUS_BAR_H; 
    int w = luaL_checkinteger(L, 3); int h = luaL_checkinteger(L, 4); int c = luaL_checkinteger(L, 5);
    if (y < STATUS_BAR_H) { h -= (STATUS_BAR_H - y); y = STATUS_BAR_H; }
    if (y + h > SCR_H - NAV_BAR_H) h = (SCR_H - NAV_BAR_H) - y;
    if (h > 0) fillRectFB_Accel(x, y, w, h, c); return 0;
}

static int lua_lcd_text(lua_State* L) {
    if (!isCallerForeground(L)) return 0; 
    int x = luaL_checkinteger(L, 1); int y = luaL_checkinteger(L, 2) + STATUS_BAR_H; 
    if (y >= STATUS_BAR_H && y <= SCR_H - NAV_BAR_H - 16) {
        drawStringFB(x, y, luaL_checkstring(L, 3), luaL_checkinteger(L, 4), luaL_checkinteger(L, 5));
    } return 0;
}

static int lua_lcd_push(lua_State* L) {
    if (!isCallerForeground(L)) {
        vTaskDelay(pdMS_TO_TICKS(50)); 
        return 0; 
    }
    
    static unsigned long lastLuaPush = 0;
    unsigned long now = millis();
    int frameTime = 30; 
    if (now - lastLuaPush < frameTime) { 
        vTaskDelay(pdMS_TO_TICKS(frameTime - (now - lastLuaPush))); 
    }
    lastLuaPush = millis();

    drawStatusBar(); 
    drawNavigationBar(); 
    drawQuickPanel(); 
    fbPush(); 
    return 0;
}

static int lua_touch_get(lua_State* L) {
    if (!isCallerForeground(L)) { lua_pushboolean(L, false); lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 3; }
    lua_pushboolean(L, wasTouching); lua_pushinteger(L, global_tx); lua_pushinteger(L, global_ty); return 3;
}

static int lua_millis(lua_State* L) { lua_pushinteger(L, millis()); return 1; }

static int lua_delay(lua_State* L) { 
    int callerId = -1;
    for(int i=0; i<MAX_LUA_TASKS; i++) { if(luaTasks[i].L == L) { callerId = i; break; } }
    
    if (callerId >= 0 && !luaTasks[callerId].isRunning) {
        return luaL_error(L, "PROCESS_KILLED"); 
    }
    
    vTaskDelay(pdMS_TO_TICKS(luaL_checkinteger(L, 1))); 
    return 0; 
}

static int lua_wifi_status(lua_State* L) { lua_pushboolean(L, WiFi.status() == WL_CONNECTED); return 1; }
static int lua_wifi_ip(lua_State* L) {
    if (WiFi.status() == WL_CONNECTED) { lua_pushstring(L, WiFi.localIP().toString().c_str()); } 
    else { lua_pushstring(L, "Brak IP"); } return 1;
}

static int lua_get_brightness(lua_State* L) { lua_pushinteger(L, screenBrightness); return 1; }

static int lua_lcd_circle(lua_State* L) {
    if (!isCallerForeground(L)) return 0;
    int x0 = luaL_checkinteger(L, 1); int y0 = luaL_checkinteger(L, 2) + STATUS_BAR_H;
    int r = luaL_checkinteger(L, 3); int color = luaL_checkinteger(L, 4);
    for (int y = -r; y <= r; y++) { for (int x = -r; x <= r; x++) { if (x * x + y * y <= r * r) { fbDrawPixel(x0 + x, y0 + y, color); } } } return 0;
}

static int lua_update_widget(lua_State* L) {
    int callerId = -1;
    for(int i=0; i<MAX_LUA_TASKS; i++) { if(luaTasks[i].L == L) { callerId = i; break; } }
    if (callerId >= 0) {
        String newText = luaL_checkstring(L, 1);
        handleWidgetUpdate(callerId, newText); 
    }
    return 0;
}

static int lua_get_time(lua_State* L) { lua_pushstring(L, timeString.c_str()); return 1; }

// --- RDZEŃ MULTITASKINGU ---
void luaBackgroundCoreTask(void * pvParameters) {
    int taskId = (int)pvParameters;
    while(true) {
        if (luaTasks[taskId].isRunning && luaTasks[taskId].L == nullptr) {
            memset((void*)luaTasks[taskId].errorMsg, 0, 256); 
            luaTasks[taskId].L = lua_newstate(mmu_lua_alloc, NULL);
            luaL_openlibs(luaTasks[taskId].L);
            
            // Rejestracja klasycznego API
            lua_register(luaTasks[taskId].L, "color_rgb", lua_color_rgb);
            lua_register(luaTasks[taskId].L, "rgb_color", lua_color_rgb); 
            lua_register(luaTasks[taskId].L, "lcd_fill", lua_lcd_fill);
            lua_register(luaTasks[taskId].L, "lcd_fill_accel", lua_lcd_fill_accel);
            lua_register(luaTasks[taskId].L, "lcd_fill_batch", l_lcd_fill_batch); 
            lua_register(luaTasks[taskId].L, "lcd_text", lua_lcd_text);
            lua_register(luaTasks[taskId].L, "lcd_push", lua_lcd_push);
            lua_register(luaTasks[taskId].L, "touch_get", lua_touch_get);
            lua_register(luaTasks[taskId].L, "millis", lua_millis);
            lua_register(luaTasks[taskId].L, "delay", lua_delay);
            lua_register(luaTasks[taskId].L, "wifi_status", lua_wifi_status);
            lua_register(luaTasks[taskId].L, "wifi_ip", lua_wifi_ip);
            lua_register(luaTasks[taskId].L, "get_brightness", lua_get_brightness);
            lua_register(luaTasks[taskId].L, "lcd_circle", lua_lcd_circle);
            lua_register(luaTasks[taskId].L, "update_widget", lua_update_widget);
            lua_register(luaTasks[taskId].L, "get_time", lua_get_time);
            
            // Rejestracja PPU
            lua_register(luaTasks[taskId].L, "lcd_load_sprite", lua_load_sprite);
            lua_register(luaTasks[taskId].L, "lcd_draw_sprite", lua_draw_sprite);

            // REJESTRACJA GPIO
            lua_register(luaTasks[taskId].L, "pin_mode", lua_pin_mode);
            lua_register(luaTasks[taskId].L, "digital_write", lua_digital_write);
            lua_register(luaTasks[taskId].L, "digital_read", lua_digital_read);
            lua_register(luaTasks[taskId].L, "analog_write", lua_analog_write);
            lua_register(luaTasks[taskId].L, "analog_read", lua_analog_read);

            // Wstrzyknięcie stałych Arduino do LUA
            lua_pushinteger(luaTasks[taskId].L, INPUT);        lua_setglobal(luaTasks[taskId].L, "INPUT");
            lua_pushinteger(luaTasks[taskId].L, OUTPUT);       lua_setglobal(luaTasks[taskId].L, "OUTPUT");
            lua_pushinteger(luaTasks[taskId].L, INPUT_PULLUP); lua_setglobal(luaTasks[taskId].L, "INPUT_PULLUP");
            lua_pushinteger(luaTasks[taskId].L, HIGH);         lua_setglobal(luaTasks[taskId].L, "HIGH");
            lua_pushinteger(luaTasks[taskId].L, LOW);          lua_setglobal(luaTasks[taskId].L, "LOW");

            lua_register(luaTasks[taskId].L, "sys_recovery", [](lua_State* L) -> int {
                triggerRecovery(); return 0;
            });

            if (luaL_dofile(luaTasks[taskId].L, luaTasks[taskId].appPath.c_str()) == LUA_OK) {
                lua_getglobal(luaTasks[taskId].L, "main");
                if (lua_isfunction(luaTasks[taskId].L, -1)) {
                    if (lua_pcall(luaTasks[taskId].L, 0, 0, 0) != LUA_OK) {
                        String err = lua_tostring(luaTasks[taskId].L, -1);
                        if(err.indexOf("PROCESS_KILLED") == -1) {
                            strncpy((char*)luaTasks[taskId].errorMsg, err.c_str(), 255);
                        }
                    }
                } else { strcpy((char*)luaTasks[taskId].errorMsg, "Blad: Brak funkcji main()!"); }
            } else { strncpy((char*)luaTasks[taskId].errorMsg, lua_tostring(luaTasks[taskId].L, -1), 255); }
            
            if (luaTasks[taskId].L != nullptr) {
                lua_close(luaTasks[taskId].L); 
                luaTasks[taskId].L = nullptr; 
            }
            
            freeSprites_PPU(taskId);

            luaTasks[taskId].isRunning = false;
            if(currentForegroundTask == taskId) { currentForegroundTask = -1; currentState = STATE_HOME; }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); 
    } 
}

void launchLuaApp(String path, String name) {
    for(int i=0; i<MAX_LUA_TASKS; i++) {
        if(luaTasks[i].isRunning && luaTasks[i].appPath == path) {
            luaTasks[i].isForeground = true; currentForegroundTask = i; currentState = STATE_LUA_APP; return;
        }
    }
    for(int i=0; i<MAX_LUA_TASKS; i++) {
        if(!luaTasks[i].isRunning) {
            luaTasks[i].appPath = path; luaTasks[i].appName = name; luaTasks[i].isRunning = true;
            luaTasks[i].isForeground = true; currentForegroundTask = i; currentState = STATE_LUA_APP; return;
        }
    }
}