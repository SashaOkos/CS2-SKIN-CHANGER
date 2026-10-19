#pragma once
#include <windows.h>
#include <cstdint>
#include "glow_settings.hpp"
constexpr uint32_t MenuMagic=0x424D5343;
struct SkinChoice { int paint=0,seed=0; float wear=0.01f; int active=0; };
struct EquippedItem { uint64_t itemId=0; int definition=0,custom=0; SkinChoice choice{}; };
constexpr int LoadoutTeams=4,LoadoutSlots=58;
struct SharedMenu {
    uint32_t magic=MenuMagic;
    volatile LONG revision=0;
    LONG enabled=1;
    int knife=515,glove=0;
    SkinChoice choices[8192]{};
    volatile LONG menuOpen=0;
    ULONGLONG menuHeartbeat=0;
    volatile LONG runtimeConnected=0;
    ULONGLONG runtimeHeartbeat=0;
    volatile LONG applySerial=0;
    int appliedDefinition=0;
    int appliedPaint=0;
    volatile LONG inventoryRequest=0;
    int inventoryDefinition=0;
    volatile LONG inventoryResult=0; // 0 idle, 1 added, negative = runtime error
    uint64_t inventoryAddedId=0;
    DWORD runtimePid=0;
    EquippedItem loadout[LoadoutTeams][LoadoutSlots]{};
    SkinChoice equippedChoices[8192]{};
    volatile LONG equipmentRevision=0;
    volatile LONG resetSerial=0;
    volatile LONG resetResult=0,resetRemoved=0;
    LONG resetAll=1;
    unsigned char resetDefinitions[8192]{};
    volatile LONG unloadRequest=0,unloadResult=0;
    volatile LONG glowRevision=0;
    GlowSettings glow{};
    volatile LONG glowStatus=0; // -1 unavailable schema, 0 idle, 1 targets updated
};
inline const wchar_t* MenuMap=L"Local\\CS2.StyleMenu.v13";
int RunStyleMenu();
