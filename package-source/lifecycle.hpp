#pragma once
#include <windows.h>
#include "minhook/minhook.h"
static HMODULE runtimeModule=nullptr,lifecycleModule=nullptr;
using RegisterGateFn=void*(*)(int,void*,void*);
using SetFallbackFn=void(*)(void*,void*);
using RetireOwnerFn=BOOL(*)(void*,DWORD);
static RegisterGateFn registerGate=nullptr;
static SetFallbackFn setGateFallback=nullptr;
static RetireOwnerFn retireOwner=nullptr;
struct LifecycleSlot {void** slot;void* original;void* gate;};
static LifecycleSlot lifecycleSlots[80]{};static unsigned lifecycleSlotCount=0;
static bool InitializeLifecycle(){
 wchar_t path[MAX_PATH];if(!GetModuleFileNameW(runtimeModule,path,MAX_PATH))return false;
 auto slash=wcsrchr(path,L'\\');if(!slash)return false;wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"collection-lifecycle.dll");
 lifecycleModule=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!lifecycleModule)return false;
 registerGate=reinterpret_cast<RegisterGateFn>(GetProcAddress(lifecycleModule,"RegisterGate"));setGateFallback=reinterpret_cast<SetFallbackFn>(GetProcAddress(lifecycleModule,"SetFallback"));retireOwner=reinterpret_cast<RetireOwnerFn>(GetProcAddress(lifecycleModule,"RetireOwner"));
 auto begin=reinterpret_cast<BOOL(*)(void*)>(GetProcAddress(lifecycleModule,"BeginOwner"));
 return registerGate&&setGateFallback&&retireOwner&&begin&&begin(runtimeModule);
}
static bool InstallLifecycleSlot(void** slot,void* target,int kind){
 if(lifecycleSlotCount>=80)return false;
 auto original=*slot;auto gate=registerGate(kind,target,original);if(!gate)return false;
 DWORD old;if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
 lifecycleSlots[lifecycleSlotCount++]={slot,original,gate};InterlockedExchangePointer(slot,gate);
 DWORD ignored;VirtualProtect(slot,sizeof(void*),old,&ignored);return true;
}
static bool RestoreLifecycleSlots(){
 bool ok=true;for(unsigned i=lifecycleSlotCount;i>0;--i){auto& entry=lifecycleSlots[i-1];DWORD old;
 if(!VirtualProtect(entry.slot,sizeof(void*),PAGE_READWRITE,&old)){ok=false;continue;}
 InterlockedCompareExchangePointer(entry.slot,entry.original,entry.gate);DWORD ignored;VirtualProtect(entry.slot,sizeof(void*),old,&ignored);
 }return ok;
}
static MH_STATUS CreateLifecycleHook(void* target,void* callback,void** original,int kind){
 auto gate=registerGate(kind,callback,target);if(!gate)return MH_ERROR_MEMORY_ALLOC;
 auto status=MH_CreateHook(target,gate,original);if(status==MH_OK)setGateFallback(callback,*original);return status;
}
// Retired gate calls may still be on native trampolines. Keep their small executable
// allocations valid until process exit, even after the core module is released.
static MH_STATUS KeepLifecycleHook(void* target){return MH_DisableHook(target);}
