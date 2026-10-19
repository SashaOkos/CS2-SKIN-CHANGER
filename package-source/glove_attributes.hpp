#pragma once
#include <windows.h>
#include <atomic>
#include <cstring>
#include "menu_shared.hpp"
using GlovePaintFn=int(__fastcall*)(void*);
using GloveAttributeFn=uintptr_t(__fastcall*)(void*,void*);
using FinishWearFn=float(__fastcall*)(void*);
using FinishDefinitionFn=void*(__fastcall*)(void*);
struct FinishTable {void** table;GlovePaintFn paint;GloveAttributeFn attributes;FinishDefinitionFn definition=nullptr;GlovePaintFn seed=nullptr;FinishWearFn wear=nullptr;};
static SRWLOCK finishTablesLock=SRWLOCK_INIT;
static FinishTable finishTables[16]{};static size_t finishTableCount=0;
static FinishTable OriginalFinishTable(void* item){
 auto table=*reinterpret_cast<void***>(item);
 thread_local FinishTable cached{};if(cached.table==table)return cached;
 FinishTable result{};
 AcquireSRWLockShared(&finishTablesLock);
 for(size_t i=0;i<finishTableCount;i++)if(finishTables[i].table==table){result=finishTables[i];break;}
 ReleaseSRWLockShared(&finishTablesLock);if(result.table)cached=result;return result;
}
static std::atomic<uintptr_t> gloveTarget{0},gloveDefinitionSource{0};
static SRWLOCK finishTargetsLock=SRWLOCK_INIT;
static uintptr_t weaponFinishTargets[64]{};
static size_t weaponFinishCount=0;
static void SetWeaponFinishTargets(const uintptr_t* items,size_t count){
 count=count>64?64:count;
 AcquireSRWLockShared(&finishTargetsLock);
 bool same=weaponFinishCount==count && (!count || !memcmp(weaponFinishTargets,items,count*sizeof(uintptr_t)));
 ReleaseSRWLockShared(&finishTargetsLock);if(same)return;
 AcquireSRWLockExclusive(&finishTargetsLock);
 if(weaponFinishCount==count && (!count || !memcmp(weaponFinishTargets,items,count*sizeof(uintptr_t)))){ReleaseSRWLockExclusive(&finishTargetsLock);return;}
 weaponFinishCount=count;
 for(size_t i=0;i<weaponFinishCount;i++)weaponFinishTargets[i]=items[i];
 ReleaseSRWLockExclusive(&finishTargetsLock);
}
static std::atomic<unsigned long long> glovePaintCalls{0},gloveAttributeCalls{0};
static SharedMenu* gloveMenuState;
static std::atomic<bool>* gloveActive=nullptr;
static bool GloveChoice(void* item,SkinChoice& choice){
 if(!gloveMenuState || (gloveActive && !gloveActive->load()))return false;
 auto address=reinterpret_cast<uintptr_t>(item);
 bool weapon=false;
 AcquireSRWLockShared(&finishTargetsLock);
 for(size_t i=0;i<weaponFinishCount;i++)if(address==weaponFinishTargets[i]){weapon=true;break;}
 ReleaseSRWLockShared(&finishTargetsLock);
 if(!weapon && address!=gloveTarget.load(std::memory_order_relaxed))return false;
 LONG revision=gloveMenuState->equipmentRevision;if(revision&1)return false;
 int id=weapon?*reinterpret_cast<const uint16_t*>(address+0x1ba):gloveMenuState->glove;
 if(id<=0 || id>=8192)return false;
 choice=gloveMenuState->equippedChoices[id];return revision==gloveMenuState->equipmentRevision && choice.active;
}
static int __fastcall GetGlovePaint(void* item){SkinChoice choice;if(GloveChoice(item,choice)){glovePaintCalls.fetch_add(1,std::memory_order_relaxed);return choice.paint;}auto original=OriginalFinishTable(item).paint;return original?original(item):0;}
static int __fastcall GetFinishSeed(void* item){SkinChoice choice;if(GloveChoice(item,choice))return choice.seed;auto original=OriginalFinishTable(item).seed;return original?original(item):0;}
static float __fastcall GetFinishWear(void* item){SkinChoice choice;if(GloveChoice(item,choice))return choice.wear;auto original=OriginalFinishTable(item).wear;return original?original(item):0.0f;}
struct GloveAttribute{uintptr_t table,owner;unsigned char pad[32];uint16_t def,reserved;float value,initial;int currency;bool bonus;unsigned char tail[7];};
struct GloveVector{uint64_t count;GloveAttribute* data;};
static_assert(sizeof(GloveAttribute)==0x48);
static uintptr_t CallGloveAttributes(void* item,void* visitor,const SkinChoice& choice){
 auto original=OriginalFinishTable(item).attributes;if(!original)return 0;
 auto p=reinterpret_cast<uintptr_t>(item);auto vector=reinterpret_cast<GloveVector*>(p+0x210);auto old=*vector;if(old.count>64 || (old.count&&!old.data))return original(item,visitor);
 auto network=*reinterpret_cast<GloveVector*>(p+0x288);
 if(network.count>64 || (network.count&&!network.data))return original(item,visitor);
 thread_local GloveAttribute attrs[131];
 thread_local alignas(8) unsigned char shadow[0x5B0];
 thread_local bool visiting=false;if(visiting)return original(item,visitor);
 memcpy(shadow,item,sizeof(shadow));size_t count=0;
 for(size_t n=0;n<old.count;n++){
  auto def=old.data[n].def;
  // kill eater / kill eater score type; sticker slots (kit, wear, scale, rotation).
  if((def>=80 && def<=89) || (def>=113 && def<=136) || (def>=278 && def<=295))continue;
  attrs[count++]=old.data[n];
 }
 // The native iterator visits NetworkedDynamicAttributes after local attributes
 // when SOC lookup is disabled. Replaying that list would overwrite our finish
 // with the server-provided paint/seed/wear (and reintroduce stickers/StatTrak).
 // Preserve unrelated attributes in the shadow only; never mutate either real list.
 for(size_t n=0;n<network.count;n++){
  auto def=network.data[n].def;
  if((def>=6 && def<=8) || (def>=80 && def<=89) || (def>=113 && def<=136) || (def>=278 && def<=295))continue;
  attrs[count++]=network.data[n];
 }
 const float values[]={float(choice.paint),float(choice.seed),choice.wear};
 for(int n=0;n<3;n++){size_t i=0;for(;i<count;i++)if(attrs[i].def==6+n)break;if(i==count){attrs[i]={};count++;}attrs[i].def=uint16_t(6+n);attrs[i].value=attrs[i].initial=values[n];}
 auto localVector=reinterpret_cast<GloveVector*>(shadow+0x210);
 *localVector={count,attrs};
 *reinterpret_cast<GloveVector*>(shadow+0x288)={};
 *reinterpret_cast<bool*>(shadow+0x1e9)=true;uintptr_t result=0;
 visiting=true;
 __try{gloveAttributeCalls.fetch_add(1,std::memory_order_relaxed);result=original(shadow,visitor);}__finally{visiting=false;}
 return result;
}
static uintptr_t __fastcall IterateGloveAttributes(void* item,void* visitor){
 SkinChoice choice;
 if(GloveChoice(item,choice))return CallGloveAttributes(item,visitor,choice);
 auto original=OriginalFinishTable(item).attributes;return original?original(item,visitor):0;
}
static void* __fastcall GetFinishDefinition(void* item){
 auto own=OriginalFinishTable(item).definition;if(!own)return nullptr;
 SkinChoice choice;
 if(reinterpret_cast<uintptr_t>(item)==gloveTarget.load(std::memory_order_relaxed) && GloveChoice(item,choice)){
  auto source=gloveDefinitionSource.load(std::memory_order_relaxed);
  if(source && source!=reinterpret_cast<uintptr_t>(item)){
   auto original=OriginalFinishTable(reinterpret_cast<void*>(source)).definition;
   if(!original){auto table=*reinterpret_cast<void***>(source);original=reinterpret_cast<FinishDefinitionFn>(table[16]);}
   if(original && original!=GetFinishDefinition)return original(reinterpret_cast<void*>(source));
  }
 }
 return own(item);
}
static bool BindGloveAttributes(uintptr_t item,SharedMenu* state,std::atomic<bool>* on=nullptr,bool weapon=false){
 gloveMenuState=state;gloveActive=on;if(!item)return false;
 auto table=*reinterpret_cast<void***>(item);
 if(OriginalFinishTable(reinterpret_cast<void*>(item)).table){if(!weapon)gloveTarget=item;return true;}
 AcquireSRWLockExclusive(&finishTablesLock);
 bool known=false;for(size_t i=0;i<finishTableCount;i++)if(finishTables[i].table==table){known=true;break;}
 if(!known){
  auto client=GetModuleHandleW(L"client.dll");MEMORY_BASIC_INFORMATION info{};
  const unsigned char paintPrefix[]={0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x40};
  const unsigned char iterPrefix[]={0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
  bool valid=finishTableCount<16 && VirtualQuery(table[3],&info,sizeof(info)) && info.AllocationBase==client && !memcmp(table[3],paintPrefix,sizeof(paintPrefix)) &&
   VirtualQuery(table[29],&info,sizeof(info)) && info.AllocationBase==client && !memcmp(table[29],iterPrefix,sizeof(iterPrefix));
  valid=valid && VirtualQuery(table[4],&info,sizeof(info)) && info.AllocationBase==client && VirtualQuery(table[5],&info,sizeof(info)) && info.AllocationBase==client;
   if(!valid){ReleaseSRWLockExclusive(&finishTablesLock);return false;}
  DWORD old;if(!VirtualProtect(&table[3],27*8,PAGE_READWRITE,&old)){ReleaseSRWLockExclusive(&finishTablesLock);return false;}
  finishTables[finishTableCount++]={table,reinterpret_cast<GlovePaintFn>(table[3]),reinterpret_cast<GloveAttributeFn>(table[29]),reinterpret_cast<FinishDefinitionFn>(table[16]),reinterpret_cast<GlovePaintFn>(table[4]),reinterpret_cast<FinishWearFn>(table[5])};
  bool installed=InstallLifecycleSlot(&table[4],reinterpret_cast<void*>(&GetFinishSeed),10) && InstallLifecycleSlot(&table[5],reinterpret_cast<void*>(&GetFinishWear),11) && InstallLifecycleSlot(&table[3],reinterpret_cast<void*>(&GetGlovePaint),10) && InstallLifecycleSlot(&table[29],reinterpret_cast<void*>(&IterateGloveAttributes),4);
  if(!installed){DWORD ignored;VirtualProtect(&table[3],27*8,old,&ignored);ReleaseSRWLockExclusive(&finishTablesLock);return false;}
  DWORD ignored;VirtualProtect(&table[3],27*8,old,&ignored);
 }
 ReleaseSRWLockExclusive(&finishTablesLock);
 if(!weapon)gloveTarget=item;return true;
}
