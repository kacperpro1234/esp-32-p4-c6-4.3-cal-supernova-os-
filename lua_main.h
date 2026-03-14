#pragma once
#include <Arduino.h>

extern "C" {
  #include <lua.h>
  #include <lualib.h>
  #include <lauxlib.h>
}

enum SystemState { 
    STATE_HOME, STATE_FILES, STATE_SETTINGS, STATE_LUA_APP, 
    STATE_RECENTS, STATE_APP_DRAWER, STATE_KEYBOARD, STATE_HOME_SETTINGS 
};

#define MAX_LUA_TASKS 3

struct LuaProcess {
    lua_State* L; 
    String appName; 
    String appPath;
    volatile bool isRunning; 
    bool isForeground; 
    TaskHandle_t taskHandle; 
    char errorMsg[256];
};

struct MMU_Block { 
    size_t totalAllocated; 
    size_t limit; 
};

extern MMU_Block mmu_state;
extern LuaProcess luaTasks[MAX_LUA_TASKS];
extern int currentForegroundTask;

void init_MMU();
void luaBackgroundCoreTask(void * pvParameters);
void launchLuaApp(String path, String name);
void loadSprite_PPU(int taskId, int id, String path);
void drawSprite_PPU(int taskId, int id, int x, int y);
void freeSprites_PPU(int taskId);