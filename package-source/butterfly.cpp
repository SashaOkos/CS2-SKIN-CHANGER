#include "item_identity.hpp"
#include <windows.h>
#include "composite_recipe.hpp"
#include "knife_subclass.hpp"
#include "knife_hud_cache.hpp"
#include <intrin.h>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <new>
#include "pattern.hpp"
#include "build_identity.hpp"
#include "catalog.hpp"
#include "menu_shared.hpp"
#include "lifecycle.hpp"
#include "glove_attributes.hpp"
#include "minhook/minhook.h"

static constexpr const char* butterfly = kButterflyModel;
using SetModelFn = void(__fastcall*)(void*, const char*);
using StageFn = void(__fastcall*)(void*, int);
using FinishRefreshFn = void(__fastcall*)(void*,void*,bool,bool,void*);
static FinishRefreshFn originalFinishRefresh=nullptr;
using WeaponNameFn=const char*(__fastcall*)(void*);
static WeaponNameFn originalWeaponName=nullptr;
using CompositeSubmitFn=uintptr_t(__fastcall*)(void*,void*);
static CompositeSubmitFn originalCompositeSubmit=nullptr;
using HudUpdateFn=void(__fastcall*)(void*,void*,bool);
using HudAddFn=int(__fastcall*)(void*,uint32_t,void*,void*);
static HudUpdateFn originalHudUpdate=nullptr;
static HudAddFn originalHudAdd=nullptr;
static std::atomic<unsigned long long> knifeHudRebuilds{0},knifeHudViews{0};
static std::atomic<unsigned long long> compositeRecipeRedirects{0};
static std::atomic<unsigned long long> selectedNameQueries{0},selectedMaterialQueries{0};
static SetModelFn setModel;
static StageFn originalStage;
static std::atomic<bool> shutdownPrepared{false};
static uintptr_t entityGlobal, controllerGlobal;
static void* scope;
static std::atomic<bool> enabled{true}, failed{false};
static wchar_t statusPath[MAX_PATH];
static bool loggedApplied;
static std::atomic<unsigned long long> gloveRenderUpdates{0},gloveKeptObjects{0},gloveHudUpdates{0};
using GloveHiddenFn=bool(__fastcall*)(void*);
static GloveHiddenFn originalGloveHidden;
static std::atomic<const wchar_t*> pendingStatus{nullptr};
using SymbolSetter = void(__fastcall*)(void*,const uintptr_t*);
static SymbolSetter originalSymbolSetter;
using SymbolGetter=void(__fastcall*)(void*,void*,uintptr_t*);
static SymbolGetter originalSymbolGetter;
static std::atomic<uintptr_t> animationNode{0};
static std::atomic<uintptr_t> butterflySymbol{0};
static std::atomic<int> knifeDefinition{515};
static std::atomic<const char*> knifeModel{kButterflyModel};
static SharedMenu* menuState;
static std::atomic<unsigned long long> modelCalls{0}, animationWrites{0};
static std::atomic<unsigned long long> modelOverrides{0},finishRefreshes{0},finishFailures{0};
static bool IsKnife(uint16_t d) { return d==42 || d==59 || (d>=500 && d<=526); }
static uintptr_t LocalKnife();
static void __fastcall SetSymbol(void* node,const uintptr_t* value) {
    __try {
        if(reinterpret_cast<uintptr_t>(node)==animationNode.load(std::memory_order_relaxed) && enabled.load() && !failed.load() && LocalKnife()) {
            uintptr_t symbol=butterflySymbol.load(); originalSymbolSetter(node,&symbol); ++animationWrites; return;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { animationNode=0; }
    originalSymbolSetter(node,value);
}
static void __fastcall GetSymbol(void* node,void* context,uintptr_t* out) {
    originalSymbolGetter(node,context,out);
    if(reinterpret_cast<uintptr_t>(node)!=animationNode.load(std::memory_order_relaxed)) return;
    __try {
        if(enabled.load() && !failed.load() && LocalKnife()) {*out=butterflySymbol.load();++animationWrites;}
    } __except(EXCEPTION_EXECUTE_HANDLER) {animationNode=0;}
}
struct Offsets {
    int pawn, services, active, arms, node, owner, child, sibling, nodeOwner;
    int attributes, item, definition, modelState, modelName, health, graph, weaponType;
} offsets;

static void Status(const wchar_t* message) {
    FILE* f=nullptr; if (_wfopen_s(&f, statusPath, L"w, ccs=UTF-8") == 0 && f) { fputws(message,f); fclose(f); }
}
static bool Readable(uintptr_t ptr, size_t bytes=8) {
    if (ptr < 0x10000 || ptr > 0x00007FFFFFFFFFFF) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(ptr), &info, sizeof(info))) return false;
    return info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS|PAGE_GUARD)) &&
        ptr+bytes <= reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
}
template<class T> static T Read(uintptr_t ptr) {
    return ptr>=0x10000 && ptr<=0x00007FFFFFFFFFFF-sizeof(T) ? *reinterpret_cast<T*>(ptr) : T{};
}
template<class F> static F Virtual(void* obj, int slot) {
    auto table = Read<uintptr_t>(reinterpret_cast<uintptr_t>(obj));
    return reinterpret_cast<F>(Read<uintptr_t>(table+8*slot));
}
static int Field(const char* cls, const char* name) {
    void* info=nullptr;
    auto find=Virtual<void(__fastcall*)(void*,void**,const char*)>(scope,2);
    if (!find) return -1;
    find(scope,&info,cls);
    auto base=reinterpret_cast<uintptr_t>(info);
    if (!Readable(base,0x48)) return -1;
    // Layout of SchemaClassInfoData in this CS2 generation (2026-10-06).
    auto count=Read<int16_t>(base+0x24);
    auto fields=Read<uintptr_t>(base+0x30);
    if (count<0 || count>4096 || !Readable(fields,static_cast<size_t>(count)*0x20)) return -1;
    for (int i=0;i<count;++i) {
        auto field=fields+i*0x20;
        auto fieldName=Read<const char*>(field);
        if (fieldName && Readable(reinterpret_cast<uintptr_t>(fieldName),strlen(name)+1) && !strcmp(fieldName,name)) {
            int result=Read<int>(field+0x10);
            return result>=0 && result<0x10000 ? result : -1;
        }
    }
    return -1;
}
static uintptr_t Entity(uint32_t handle) {
    if (handle==0xFFFFFFFF) return 0;
    auto list=Read<uintptr_t>(entityGlobal);
    auto chunk=Read<uintptr_t>(list+0x10+8*((handle&0x7FFF)>>9));
    auto identity=chunk+0x70*(handle&0x1FF);
    if (!chunk) return 0;
    // Resolve full handles with their serial number; index-only lookup is not accepted.
    if (Read<uint32_t>(identity+0x10)!=handle) return 0;
    return Read<uintptr_t>(identity);
}
// Keep only selected local cosmetic gloves in the glove component's visibility query.
// Other callers of this generic engine query retain their original result.
static __declspec(noinline) bool __fastcall SelectedGloveHidden(void* owner) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    bool hidden=originalGloveHidden(owner);
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
    if(!hidden || caller!=base+0xC65413 || !menuState || !enabled.load() || failed.load())return hidden;
    auto controller=Read<uintptr_t>(controllerGlobal);
    auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
    if(reinterpret_cast<uintptr_t>(owner)!=pawn || !pawn || Read<int>(pawn+offsets.health)<=0)return hidden;
    SkinChoice choice;
    static const int gloveOffset=Field("C_CSPlayerPawn","m_EconGloves");
    if(gloveOffset<0 || !GloveChoice(reinterpret_cast<void*>(pawn+gloveOffset),choice))return hidden;
    gloveKeptObjects.fetch_add(1,std::memory_order_relaxed);return false;
}
static uintptr_t LocalKnife() {
    auto controller=Read<uintptr_t>(controllerGlobal);
    auto pawn=controller ? Entity(Read<uint32_t>(controller+offsets.pawn)) : 0;
    if(!pawn || Read<int>(pawn+offsets.health)<=0) return 0;
    auto services=Read<uintptr_t>(pawn+offsets.services);
    auto weapon=services ? Entity(Read<uint32_t>(services+offsets.active)) : 0;
    return weapon && IsKnife(Read<uint16_t>(weapon+offsets.attributes+offsets.item+offsets.definition)) ? weapon : 0;
}
static const char* __fastcall SelectedWeaponName(void* entity){
    __try {
        if(menuState && enabled.load() && !failed.load()){
            LONG revision=menuState->equipmentRevision;
            auto controller=Read<uintptr_t>(controllerGlobal);
            auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
            auto weapon=reinterpret_cast<uintptr_t>(entity);
            int selected=menuState->knife;
            if(!(revision&1) && pawn && weapon && selected>0 && selected<8192 &&
               Entity(Read<uint32_t>(weapon+offsets.owner))==pawn &&
               IsKnife(Read<uint16_t>(weapon+offsets.attributes+offsets.item+offsets.definition)) &&
               menuState->equippedChoices[selected].active && revision==menuState->equipmentRevision){
                for(auto& entry:catalog)if(entry.id==selected && IsKnife(static_cast<uint16_t>(entry.id))){
                    selectedNameQueries.fetch_add(1,std::memory_order_relaxed);return entry.key;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return originalWeaponName(entity);
}
static void BindAnimation(uintptr_t pawn) {
    auto arms=Entity(Read<uint32_t>(pawn+offsets.arms));
    auto graph=arms ? Read<uintptr_t>(arms+offsets.graph) : 0;
    if(!graph) { animationNode=0; return; }
    auto param=graph+offsets.weaponType;
    auto name=Read<const char*>(param);
    if(!name || strcmp(name,"weapon_type")) {animationNode=0;return;}
    auto instance=Read<uintptr_t>(param+8);
    auto index=Read<int16_t>(param+16);
    if(!instance || index<0 || index>=Read<int>(instance+8)) {animationNode=0;return;}
    auto nodes=Read<uintptr_t>(instance+16);
    auto node=Read<uintptr_t>(nodes+8*index);
    if(!node) {animationNode=0;return;}
    auto table=Read<void**>(node);
    if(!originalSymbolSetter) {
        auto setter=table[10];
        const unsigned char expected[]={0x48,0x8b,0x02,0x48,0x89,0x41,0x18,0xc3};
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(setter,&info,sizeof(info)) || info.AllocationBase!=GetModuleHandleW(L"animationsystem.dll") || memcmp(setter,expected,sizeof(expected))) return;
        auto getter=table[9];
        const unsigned char getterPrefix[]={0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0x42,0x18,0x49,0x8b,0xf8,0x48,0x8b,0xd9};
        if(!VirtualQuery(getter,&info,sizeof(info)) || info.AllocationBase!=GetModuleHandleW(L"animationsystem.dll") || memcmp(getter,getterPrefix,sizeof(getterPrefix))) return;
        DWORD protect;
        if(!VirtualProtect(&table[9],16,PAGE_READWRITE,&protect)) return;
        originalSymbolGetter=reinterpret_cast<SymbolGetter>(getter);
        if(!InstallLifecycleSlot(&table[9],reinterpret_cast<void*>(&GetSymbol),9)){DWORD ignored;VirtualProtect(&table[9],16,protect,&ignored);return;}
        originalSymbolSetter=reinterpret_cast<SymbolSetter>(setter);
        if(!InstallLifecycleSlot(&table[10],reinterpret_cast<void*>(&SetSymbol),8)){DWORD ignored;VirtualProtect(&table[9],16,protect,&ignored);return;}
        DWORD ignored;VirtualProtect(&table[9],16,protect,&ignored);
    }
    if(!originalSymbolSetter) {animationNode=0;return;}
    animationNode=node;

}
static void ChangeModel(uintptr_t entity,const char* name) { ++modelCalls; setModel(reinterpret_cast<void*>(entity),name); }
static std::string Model(uintptr_t entity) {
    auto node=Read<uintptr_t>(entity+offsets.node);
    auto name=Read<uintptr_t>(node+offsets.modelState+offsets.modelName);
    if (!name) return {};
    char buffer[256]{};
    for (size_t i=0;i<sizeof(buffer)-1;++i) {
        buffer[i]=Read<char>(name+i); if (!buffer[i]) return buffer;
    }
    return {};
}
static uintptr_t HudWeapon(uintptr_t pawn, uintptr_t weapon) {
    // Prefer the explicit first-person attachment; the pawn's arms tree also
    // contains presentation models that are not the active view attachment.
    static const int attachmentOff=Field("C_EconEntity","m_hViewmodelAttachment");
    if(attachmentOff>=0){auto attachment=Entity(Read<uint32_t>(weapon+attachmentOff));if(attachment)return attachment;}

    auto arms=Entity(Read<uint32_t>(pawn+offsets.arms));
    if (!arms) return 0;
    auto node=Read<uintptr_t>(arms+offsets.node);
    auto child=Read<uintptr_t>(node+offsets.child);
    for (int i=0;child && i<64;++i) {
        auto owner=Read<uintptr_t>(child+offsets.nodeOwner);
        if (owner && Entity(Read<uint32_t>(owner+offsets.owner))==weapon) return owner;
        auto next=Read<uintptr_t>(child+offsets.sibling); if(next==child) break; child=next;
    }
    return 0;
}
static void __fastcall SelectedKnifeModel(void* entity,const char* requested) {
    const char* selected=requested;
    __try {
        if(enabled.load() && !failed.load() && menuState && menuState->equippedChoices[knifeDefinition.load()].active){
            auto weapon=LocalKnife();auto address=reinterpret_cast<uintptr_t>(entity);
            if(weapon && (address==weapon || Entity(Read<uint32_t>(address+offsets.owner))==weapon)){
                selected=knifeModel.load();++modelOverrides;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    setModel(entity,selected);
}
static uint32_t SelectedKnifeSubclass(uintptr_t weapon,const char* wanted){
    auto client=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
    auto system=Read<uintptr_t>(client+0x23BB7C0);
    if(!system || !Readable(system+0x68,0x18))return 0;
    auto nodes=Read<uintptr_t>(system+0x70);int capacity=Read<int>(system+0x7C);
    if(capacity<=0 || capacity>4096 || !Readable(nodes,static_cast<size_t>(capacity)*24))return 0;
    auto table=Read<uintptr_t>(weapon);
    auto scopeFn=reinterpret_cast<int(__fastcall*)(void*)>(Read<uintptr_t>(table+0x638));
    int scopeIndex=scopeFn(reinterpret_cast<void*>(weapon));if(scopeIndex<0)return 0;
    auto lookup=reinterpret_cast<uintptr_t(__fastcall*)(void*,int,uint32_t)>(client+0x3783E0);
    return ResolveKnifeSubclass(capacity,wanted,[&](int index){
        KnifeSubclassEntry result{};auto node=nodes+index*24;auto data=Read<uintptr_t>(node+16);
        if(!data || !Readable(data,0x728))return result;
        auto name=Read<const char*>(data+0x720);
        if(!name || !Readable(reinterpret_cast<uintptr_t>(name),strlen(wanted)+1))return result;
        result.key=Read<uint32_t>(node+8);result.name=name;result.melee=Read<bool>(data+0x71C);return result;
    },[&](uint32_t key,int index){return lookup(reinterpret_cast<void*>(system),scopeIndex,key)==Read<uintptr_t>(nodes+index*24+16);});
}
static void SetKnifeSubclass(uintptr_t weapon,uint32_t value){
    if(Read<uint32_t>(weapon+0x380)==value)return;
    *reinterpret_cast<uint32_t*>(weapon+0x380)=value;
    auto client=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
    reinterpret_cast<void(__fastcall*)(void*)>(client+0xB064D0)(reinterpret_cast<void*>(weapon));
}
struct Saved { uint32_t handle; uint16_t definition; std::string model; std::string hudModel; uint32_t subclass=0; };
static std::vector<Saved> saved;
static bool ResetDefinition(int definition);
static void Restore(bool selectedOnly=false) {
    auto controller=Read<uintptr_t>(controllerGlobal);
    auto pawn=controller ? Entity(Read<uint32_t>(controller+offsets.pawn)) : 0;
    for (auto& entry:saved) {
        auto weapon=Entity(entry.handle); if (!weapon) continue;
        auto def=weapon+offsets.attributes+offsets.item+offsets.definition;
        if (!IsKnife(Read<uint16_t>(def))) continue;
        if(selectedOnly && !ResetDefinition(Read<uint16_t>(def)))continue;
        *reinterpret_cast<uint16_t*>(def)=entry.definition;
        SetKnifeSubclass(weapon,entry.subclass);
        if (!entry.model.empty()) ChangeModel(weapon,entry.model.c_str());
        auto hud=pawn ? HudWeapon(pawn,weapon) : 0;
        if(hud && !entry.hudModel.empty()) ChangeModel(hud,entry.hudModel.c_str());
    }
    animationNode=0;
    if(selectedOnly)saved.erase(std::remove_if(saved.begin(),saved.end(),[](const Saved& entry){auto entity=Entity(entry.handle);return !entity || Model(entity)==entry.model;}),saved.end());else saved.clear();
    loggedApplied=false;
}
static void RememberItem(uintptr_t owner,uint32_t handle,uintptr_t item,bool glove);
static void Apply() {
    if(!menuState || menuState->revision&1)return;
    int selectedKnife=menuState->knife;
    if(selectedKnife<=0 || selectedKnife>=8192 || !menuState->equippedChoices[selectedKnife].active){if(!saved.empty())Restore();return;}
    static int lastState=-1;
    auto waiting=[](int state,const wchar_t* message) { if(lastState!=state) {lastState=state;pendingStatus=message;} };
    auto controller=Read<uintptr_t>(controllerGlobal); if(!controller) { waiting(0,L"WAITING: local player controller.\n");return; }
    auto pawn=Entity(Read<uint32_t>(controller+offsets.pawn)); if (!pawn) { waiting(1,L"WAITING: local player pawn. Join a match.\n");return; }
    int health=Read<int>(pawn+offsets.health); if (health<=0 || health>500) return;
    auto services=Read<uintptr_t>(pawn+offsets.services); if(!services) return;
    auto handle=Read<uint32_t>(services+offsets.active);
    auto weapon=Entity(handle); if(!weapon) return;
    auto definition=weapon+offsets.attributes+offsets.item+offsets.definition;
    auto def=Read<uint16_t>(definition);
    if (!IsKnife(def)) {animationNode=0;waiting(2,L"WAITING: select your knife (key 3).\n");return;}
    BindAnimation(pawn);
    auto hud=HudWeapon(pawn,weapon); if(!hud) {waiting(3,L"WAITING: first-person knife entity.\n");return;}
    std::string weaponModel=Model(weapon), hudModel=Model(hud);
    if (weaponModel.empty() || hudModel.empty()) {waiting(4,L"WAITING: model names were not available.\n");return;}
    bool known=false;
    for(auto& entry:saved) if(entry.handle==handle) {known=true;break;}
    if(!known) { if(saved.size()>64) saved.erase(saved.begin()); saved.push_back({handle,def,weaponModel,hudModel,Read<uint32_t>(weapon+0x380)}); }
    RememberItem(weapon,handle,weapon+offsets.attributes+offsets.item,false);
    *reinterpret_cast<uint16_t*>(definition)=static_cast<uint16_t>(knifeDefinition.load());
    for(auto& entry:catalog)if(entry.id==selectedKnife){
        auto data=Read<uintptr_t>(weapon+0x388);
        auto name=data && Readable(data+0x720,8)?Read<const char*>(data+0x720):nullptr;
        if(!name || !Readable(reinterpret_cast<uintptr_t>(name),strlen(entry.key)+1) || strcmp(name,entry.key)){
            auto subclass=SelectedKnifeSubclass(weapon,entry.key);if(subclass)SetKnifeSubclass(weapon,subclass);
        }
        break;
    }
    if(weaponModel!=knifeModel.load()) ChangeModel(weapon,knifeModel.load());
    if(hudModel!=knifeModel.load()) ChangeModel(hud,knifeModel.load());
    if(!loggedApplied && Model(hud)==knifeModel.load()) {
        pendingStatus=L"APPLIED: butterfly models; local animation parameter read override installed when available. F8 toggles changes.\n"; loggedApplied=true;lastState=5;
    }
}

struct OriginalItemState {uintptr_t owner=0,item=0;uint32_t handle=0;bool glove=false,restored=false,resetMatched=false;uint64_t fields[9]{};uint32_t fallback[4]{};uint64_t mask=0;};
static std::vector<OriginalItemState> originalItems;
static int* ItemSnapshotOffsets(){static int fields[]={Field("C_EconItemView","m_iItemDefinitionIndex"),Field("C_EconItemView","m_iEntityQuality"),Field("C_EconItemView","m_iItemID"),Field("C_EconItemView","m_iItemIDHigh"),Field("C_EconItemView","m_iItemIDLow"),Field("C_EconItemView","m_iAccountID"),Field("C_EconItemView","m_bInitialized"),Field("C_EconItemView","m_bDisallowSOC"),Field("C_EconItemView","m_bRestoreCustomMaterialAfterPrecache")};return fields;}
static int* FallbackSnapshotOffsets(){static int fields[]={Field("C_EconEntity","m_nFallbackPaintKit"),Field("C_EconEntity","m_nFallbackSeed"),Field("C_EconEntity","m_flFallbackWear"),Field("C_EconEntity","m_nFallbackStatTrak")};return fields;}
static void RememberItem(uintptr_t owner,uint32_t handle,uintptr_t item,bool glove){
 for(auto&record:originalItems)if(record.owner==owner&&record.handle==handle&&record.glove==glove)return;
 if(originalItems.size()>=128)return;OriginalItemState record{};record.owner=owner;record.handle=handle;record.item=item;record.glove=glove;
 auto fields=ItemSnapshotOffsets();const size_t sizes[]={2,4,8,4,4,4,1,1,1};for(int i=0;i<9;i++)if(fields[i]>=0&&Readable(item+fields[i],sizes[i]))memcpy(&record.fields[i],reinterpret_cast<void*>(item+fields[i]),sizes[i]);
 if(!glove){auto fallback=FallbackSnapshotOffsets();for(int i=0;i<4;i++)if(fallback[i]>=0)record.fallback[i]=Read<uint32_t>(owner+fallback[i]);static const int mesh=Field("CModelState","m_MeshGroupMask");auto node=Read<uintptr_t>(owner+offsets.node);if(node&&mesh>=0)record.mask=Read<uint64_t>(node+offsets.modelState+mesh);}
 originalItems.push_back(record);
}
static bool ResetDefinition(int definition){return menuState && (menuState->resetAll || (definition>0 && definition<8192 && menuState->resetDefinitions[definition]));}
static void RestoreItemStates(){
 SetWeaponFinishTargets(nullptr,0);gloveTarget=0;gloveDefinitionSource=0;
 auto controller=Read<uintptr_t>(controllerGlobal);auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
 bool resetKnife=menuState->resetAll;for(auto& entry:saved){auto entity=Entity(entry.handle);if(entity && ResetDefinition(Read<uint16_t>(entity+offsets.attributes+offsets.item+offsets.definition)))resetKnife=true;}
 for(auto& record:originalItems)record.resetMatched=Readable(record.item,0x5B0) && ResetDefinition(Read<uint16_t>(record.item+offsets.definition));
 if(resetKnife && !saved.empty())Restore(!menuState->resetAll);auto fields=ItemSnapshotOffsets();const size_t sizes[]={2,4,8,4,4,4,1,1,1};
 auto client=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));auto clear=reinterpret_cast<void(__fastcall*)(void*,bool)>(client+0x14E7AA0);auto refresh=reinterpret_cast<void(__fastcall*)(void*,void*,bool,bool,void*)>(client+0x7FF680);
 for(auto&record:originalItems){if(record.glove?record.owner!=pawn:Entity(record.handle)!=record.owner)continue;if(!Readable(record.item,0x5B0))continue;
  if(!record.resetMatched)continue;
  *reinterpret_cast<uintptr_t*>(record.item+0x1E0)=0;record.restored=true;for(int i=0;i<9;i++)if(fields[i]>=0)memcpy(reinterpret_cast<void*>(record.item+fields[i]),&record.fields[i],sizes[i]);
  if(record.glove){static const int flag=Field("C_CSPlayerPawn","m_bNeedToReApplyGloves");auto component=record.owner+0x15F8;if(flag>=0&&Read<uintptr_t>(component+0xB0)==record.owner){*reinterpret_cast<bool*>(record.owner+flag)=true;reinterpret_cast<void(__fastcall*)(void*)>(client+0xC65330)(reinterpret_cast<void*>(component));}continue;}
  auto fallback=FallbackSnapshotOffsets();for(int i=0;i<4;i++)if(fallback[i]>=0)*reinterpret_cast<uint32_t*>(record.owner+fallback[i])=record.fallback[i];
  static const int mesh=Field("CModelState","m_MeshGroupMask");auto setMask=reinterpret_cast<void(__fastcall*)(void*,const uint64_t*)>(client+0xACC551);auto hud=pawn?HudWeapon(pawn,record.owner):0;
  for(auto entity:{record.owner,hud})if(entity){auto node=Read<uintptr_t>(entity+offsets.node);if(node&&mesh>=0)setMask(reinterpret_cast<void*>(node+offsets.modelState),&record.mask);clear(reinterpret_cast<void*>(entity+0x610),true);refresh(reinterpret_cast<void*>(entity),reinterpret_cast<void*>(record.item),entity==hud,true,nullptr);}
 }
 originalItems.erase(std::remove_if(originalItems.begin(),originalItems.end(),[](const OriginalItemState& record){return menuState->resetAll || record.restored;}),originalItems.end());
}
struct SOIDLocal { uint64_t id; uint32_t type; uint32_t pad; };
using CreateEconItemFn=void*(__fastcall*)();
using GetLocalInventoryFn=void*(__fastcall*)(void*);
using SetDynamicAttributeFn=void*(__fastcall*)(void*,void*,void*);
static uintptr_t inventoryClientBase=0;
static void* source2ClientForInventory=nullptr;
static CreateEconItemFn createEconItem=nullptr;
static GetLocalInventoryFn getLocalInventory=nullptr;
static GetLocalInventoryFn getLocalInventoryAlt=nullptr;
static SetDynamicAttributeFn setDynamicAttribute=nullptr;

static bool BytesAt(uintptr_t address,const unsigned char* bytes,size_t count) {
    return Readable(address,count) && !memcmp(reinterpret_cast<const void*>(address),bytes,count);
}
static bool InitInventoryBridge(void* clientInterface) {
    auto client=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
    if(!client || !clientInterface)return false;
    // Build-specific RVAs for the client.dll already hash-gated by the loader.
    const unsigned char createPrefix[]={0x48,0x83,0xEC,0x28,0xB9,0x48,0x00,0x00,0x00};
    const unsigned char localPrefix[]={0x48,0x8B,0x81,0xD0,0x28,0x05,0x00};
    const unsigned char setAttrPrefix[]={0xE9,0x9F,0xB0,0xF8,0xFF};
    if(!BytesAt(client+0x10DC3D0,createPrefix,sizeof(createPrefix)) ||
       !BytesAt(client+0x83CA80,localPrefix,sizeof(localPrefix)) ||
       !BytesAt(client+0x11594BC,setAttrPrefix,sizeof(setAttrPrefix))) return false;
    inventoryClientBase=client;
    source2ClientForInventory=clientInterface;
    createEconItem=reinterpret_cast<CreateEconItemFn>(client+0x10DC3D0);
    getLocalInventory=reinterpret_cast<GetLocalInventoryFn>(client+0x83CA80);
    getLocalInventoryAlt=nullptr;
    setDynamicAttribute=reinterpret_cast<SetDynamicAttributeFn>(client+0x11594BC);
    return true;
}
static bool EconAttribute(void* econItem,int index,float value) {
    if(!econItem || !source2ClientForInventory || !setDynamicAttribute)return false;
    __try {
        auto schema=reinterpret_cast<uintptr_t(__fastcall*)()>(inventoryClientBase+0x118E640)();
        if(!schema || index<0 || index>=Read<int>(schema+0x150))return false;
        auto definitions=Read<uintptr_t>(schema+0x158);
        auto definition=reinterpret_cast<void*>(Read<uintptr_t>(definitions+index*sizeof(uintptr_t)));
        if(!definition)return false;
        setDynamicAttribute(econItem,definition,&value);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static uintptr_t FindInventoryView(uintptr_t inventory,uint64_t itemId) {
    if(!inventory)return 0;
    int itemIdOff=Field("C_EconItemView","m_iItemID");
    if(itemIdOff<0)return 0;
    int count=Read<int>(inventory+0x20);
    auto data=Read<uintptr_t>(inventory+0x28);
    if(count<0 || count>16384 || !data || !Readable(data,static_cast<size_t>(count)*sizeof(uintptr_t)))return 0;
    for(int i=0;i<count;i++) {
        auto view=Read<uintptr_t>(data+i*sizeof(uintptr_t));
        if(view && Read<uint64_t>(view+itemIdOff)==itemId)return view;
    }
    return 0;
}

static bool ExecutableAddress(uintptr_t address) {
    if(!address)return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&mbi,sizeof(mbi)))return false;
    DWORD p=mbi.Protect&0xff;
    return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}
static bool PlausiblePlayerInventory(uintptr_t inventory) {
    if(!inventory || !Readable(inventory,0x40))return false;
    auto table=Read<uintptr_t>(inventory);
    if(!table || !Readable(table,9*sizeof(uintptr_t)))return false;
    for(int i=0;i<3;i++)if(!ExecutableAddress(Read<uintptr_t>(table+i*sizeof(uintptr_t))))return false;
    auto owner=Read<SOIDLocal>(inventory+0x10);
    if(!owner.id)return false;
    int count=Read<int>(inventory+0x20);
    auto data=Read<uintptr_t>(inventory+0x28);
    if(count<0 || count>16384)return false;
    if(count && (!data || !Readable(data,static_cast<size_t>(count)*sizeof(uintptr_t))))return false;
    return true;
}

struct LocalInventoryChoice { uint64_t id; int definition; SkinChoice choice; };
static std::vector<LocalInventoryChoice> addedChoices;
static uintptr_t lastInventoryInterface=0;
static LONG AddSelectedToLocalInventory(int def,const SkinChoice& choice,uint64_t& addedId) {
    lastInventoryInterface=0;
    addedId=0;
    if(!createEconItem || !getLocalInventory || !setDynamicAttribute)return -2;
    if(def<=0 || def>=8192)return -3;
    bool catalogValid=false;
    for(auto&w:catalog)if(w.id==def){catalogValid=true;break;}
    if(!catalogValid)return -3;
    __try {
        auto first=reinterpret_cast<uintptr_t>(getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)));
        auto second=getLocalInventoryAlt?reinterpret_cast<uintptr_t>(getLocalInventoryAlt(nullptr)):0;
        auto inventory=PlausiblePlayerInventory(first)?first:(PlausiblePlayerInventory(second)?second:0);
        if(!inventory)return -43;
        auto owner=Read<SOIDLocal>(inventory+0x10);
        auto econ=reinterpret_cast<uintptr_t>(createEconItem());
        if(!econ || !Readable(econ,0x48))return -5;

        static std::atomic<uint32_t> sequence{1};
        uint64_t id=0x0000000200000000ull |
            ((GetTickCount64()&0x00ffffffull)<<8) |
            (sequence.fetch_add(1)&0xffu);
        *reinterpret_cast<uint64_t*>(econ+0x10)=id;
        *reinterpret_cast<uint64_t*>(econ+0x18)=0;
        *reinterpret_cast<uint32_t*>(econ+0x28)=static_cast<uint32_t>(owner.id);
        uint32_t position=1;
        int viewCount=Read<int>(inventory+0x20);
        auto views=Read<uintptr_t>(inventory+0x28);
        for(int i=0;i<viewCount && i<16384;i++) {
            auto current=Read<uint32_t>(Read<uintptr_t>(views+i*8)+0x1DC);
            if(current<0x40000000u && current>=position)position=current+1;
        }
        *reinterpret_cast<uint32_t*>(econ+0x2C)=position;
        *reinterpret_cast<uint16_t*>(econ+0x30)=static_cast<uint16_t>(def);
        auto flags=Read<uint16_t>(econ+0x32);
        flags=static_cast<uint16_t>((flags&~(0xFu<<5))|(4u<<5));
        *reinterpret_cast<uint16_t*>(econ+0x32)=flags;

        // Persist the finish on the shared object before notifying the inventory.
        if(choice.paint)EconAttribute(reinterpret_cast<void*>(econ),6,static_cast<float>(choice.paint));
        EconAttribute(reinterpret_cast<void*>(econ),7,static_cast<float>(choice.seed));
        EconAttribute(reinterpret_cast<void*>(econ),8,choice.wear);

        auto cache=Read<uintptr_t>(inventory+0x68);
        int typeCount=Read<int>(cache+0x10);
        auto types=Read<uintptr_t>(cache+0x18);
        if(typeCount<1 || typeCount>64 || !types)return -44;
        uintptr_t itemCache=0;
        for(int i=0;i<typeCount;i++) {
            auto candidate=Read<uintptr_t>(types+i*8);
            int count=Read<int>(candidate+8);
            auto objects=Read<uintptr_t>(candidate+0x10);
            if(count<1 || count>32768 || !objects)continue;
            auto firstObject=Read<uintptr_t>(objects);
            auto getType=reinterpret_cast<int(__fastcall*)(void*)>(Read<uintptr_t>(Read<uintptr_t>(firstObject)+8));
            if(getType && getType(reinterpret_cast<void*>(firstObject))==1){itemCache=candidate;break;}
        }
        if(!itemCache)return -45;
        auto addObject=reinterpret_cast<bool(__fastcall*)(void*,void*)>(Read<uintptr_t>(Read<uintptr_t>(itemCache)+8));
        if(!addObject || !addObject(reinterpret_cast<void*>(itemCache),reinterpret_cast<void*>(econ)))return -46;
        auto table=Read<uintptr_t>(inventory);
        if(!Readable(table,sizeof(uintptr_t)))return -7;
        auto soCreated=reinterpret_cast<void(__fastcall*)(void*,SOIDLocal,void*,int)>(Read<uintptr_t>(table));
        if(!soCreated)return -7;
        soCreated(reinterpret_cast<void*>(inventory),owner,reinterpret_cast<void*>(econ),4);

        auto view=FindInventoryView(inventory,id);
        if(!view)return -8;
        // The shared object already owns its persisted attributes; do not
        // temporarily replace this inventory view's attribute vector.
        addedId=id;lastInventoryInterface=view;
        if(addedChoices.size()<4096)addedChoices.push_back({id,def,choice});
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return -9; }
}
struct RestoredInventoryItem {SkinChoice choice{};uint64_t id=0;uintptr_t materialInterface=0;LONG revision=0;unsigned failures=0;ULONGLONG retryAt=0;};
static RestoredInventoryItem restoredInventory[8192]{};
static bool SameInventoryChoice(const SkinChoice&a,const SkinChoice&b){return a.paint==b.paint && a.seed==b.seed && a.wear==b.wear;}
static LONG RestoreInventoryItem(int def,const SkinChoice&choice,LONG revision,uint64_t&id){
 auto&entry=restoredInventory[def];
 if(entry.id && SameInventoryChoice(entry.choice,choice)){id=entry.id;return 1;}
 LONG result=AddSelectedToLocalInventory(def,choice,id);
 if(result==1){entry.choice=choice;entry.id=id;entry.materialInterface=lastInventoryInterface;entry.failures=0;}
 else{entry.revision=revision;++entry.failures;entry.retryAt=GetTickCount64()+2000;}
 return result;
}
static void RestoreSavedInventory(){
 if(!menuState || !enabled.load())return;
 static ULONGLONG next=0;auto now=GetTickCount64();if(now<next)return;next=now+250;
 LONG revision=menuState->revision;if(revision&1)return;
 for(auto&w:catalog){auto choice=menuState->choices[w.id];if(menuState->revision!=revision)return;if(!choice.active)continue;
  auto&entry=restoredInventory[w.id];if(entry.id && SameInventoryChoice(entry.choice,choice))continue;
  if(entry.revision!=revision){entry.revision=revision;entry.failures=0;entry.retryAt=0;}
  if(now<entry.retryAt || entry.failures>=3)continue;
  uint64_t id=0;LONG result=RestoreInventoryItem(w.id,choice,revision,id);
  // Inventory readiness is transient; keep waiting without allocating an item.
  if(result==-43)entry.failures=0;
  menuState->inventoryAddedId=id;InterlockedExchange(&menuState->inventoryResult,result);
  if(result==1)pendingStatus=L"APPLIED: saved selection restored to the local inventory.\n";
  return; // At most one insertion per tick; no per-frame bulk rebuild.
 }
}

// These local loadout records are consumed by CCSPlayerInventory::GetItemInLoadout
// (vtable slot 8). No Steam inventory or backend ownership is changed.
struct LoadoutRuntime { uint64_t observed=0,restoredId=0,baselineId=0; uint16_t baselineDefinition=0; bool seen=false,restored=false,overridden=false; unsigned failures=0; ULONGLONG retryAt=0; };
static LoadoutRuntime liveLoadout[LoadoutTeams][LoadoutSlots]{};
static bool SameEquippedItem(const EquippedItem&a,const EquippedItem&b){return a.itemId==b.itemId && a.definition==b.definition && a.custom==b.custom && SameInventoryChoice(a.choice,b.choice);}

static bool AddedHere(uint64_t id){for(auto&entry:addedChoices)if(entry.id==id)return true;return false;}
static LONG RemoveAddedInventory(uintptr_t inventory){
 auto table=Read<uintptr_t>(inventory);auto destroyedAddress=Read<uintptr_t>(table+2*8);
 if(destroyedAddress!=inventoryClientBase+0x842A00)return -61;
 auto cache=Read<uintptr_t>(inventory+0x68);int typesCount=Read<int>(cache+0x10);auto types=Read<uintptr_t>(cache+0x18);
 if(typesCount<1 || typesCount>64 || !types)return -62;
 auto owner=Read<SOIDLocal>(inventory+0x10);auto destroyed=reinterpret_cast<void(__fastcall*)(void*,SOIDLocal,void*,int)>(destroyedAddress);
 LONG removed=0;
 for(int type=0;type<typesCount;type++){
  auto itemCache=Read<uintptr_t>(types+type*8);auto removeAddress=Read<uintptr_t>(Read<uintptr_t>(itemCache)+3*8);
  if(removeAddress!=inventoryClientBase+0x1628960)continue;
  auto remove=reinterpret_cast<uintptr_t(__fastcall*)(void*,void*)>(removeAddress);
  // Restart the search after each native removal; the cache compacts its object array.
  for(int attempts=0;attempts<4096;attempts++){
   int count=Read<int>(itemCache+8);auto objects=Read<uintptr_t>(itemCache+0x10);uintptr_t target=0;
   if(count<0 || count>32768 || (count&&!Readable(objects,count*8)))return -63;
   for(int i=0;i<count;i++){auto object=Read<uintptr_t>(objects+i*8);if(object && AddedHere(Read<uint64_t>(object+0x10)) && ResetDefinition(Read<uint16_t>(object+0x30))){target=object;break;}}
   if(!target)break;
   if(remove(reinterpret_cast<void*>(itemCache),reinterpret_cast<void*>(target))!=target)return -64;
   destroyed(reinterpret_cast<void*>(inventory),owner,reinterpret_cast<void*>(target),4);++removed;
   // Do not free the detached SOC while render-side consumers may still borrow it.
  }
 }
 for(int id=0;id<8192;id++)if(ResetDefinition(id))restoredInventory[id]=RestoredInventoryItem{};
 addedChoices.erase(std::remove_if(addedChoices.begin(),addedChoices.end(),[](const auto& entry){return ResetDefinition(entry.definition);}),addedChoices.end());lastInventoryInterface=0;gloveDefinitionSource=0;gloveTarget=0;SetWeaponFinishTargets(nullptr,0);
 menuState->resetRemoved=removed;return 1;
}
static int LastEquipmentTeam(){static int last=0;
 auto controller=Read<uintptr_t>(controllerGlobal);auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
 static const int teamOff=Field("C_BaseEntity","m_iTeamNum");int team=teamOff>=0&&pawn?Read<uint8_t>(pawn+teamOff):0;
 if(team!=2&&team!=3 && teamOff>=0&&controller)team=Read<uint8_t>(controller+teamOff);
 if(team==2 || team==3)last=team;return last;
}
static uint64_t EquippedId(int definition){if(!menuState)return 0;int team=LastEquipmentTeam();if(team!=2&&team!=3)return 0;
 for(auto&item:menuState->loadout[team])if(item.custom && item.definition==definition)return item.itemId;return 0;
}
// Native client-only mutation also updates the UI caches and emits its loadout event.
// This is the helper used by inventory equip notifications, not a GC request.
static bool SetLocalLoadout(uintptr_t inventory,int team,int slot,uint64_t id,uint16_t definition){
 if(team<2 || team>3 || slot<0 || slot>=LoadoutSlots)return false;
 auto update=reinterpret_cast<void(__fastcall*)(void*,uint16_t,uint16_t,uint64_t,uint16_t,bool)>(inventoryClientBase+0x842EA0);
 update(reinterpret_cast<void*>(inventory),uint16_t(team),uint16_t(slot),id,definition,false);
 return Read<uint64_t>(inventory+0x90+16*(team*LoadoutSlots+slot))==id;
}
static void SyncEquipment(){
 if(!menuState || menuState->revision&1)return;
 auto inventory=reinterpret_cast<uintptr_t>(getLocalInventory?getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)):nullptr);
 if(!PlausiblePlayerInventory(inventory) || !Readable(inventory+0x90,LoadoutTeams*LoadoutSlots*16))return;
 static const int itemIdOff=Field("C_EconItemView","m_iItemID");if(itemIdOff<0)return;
 static LONG previousReset=0;static ULONGLONG next=0;auto now=GetTickCount64();bool reset=previousReset!=menuState->resetSerial;
 static int lastSyncedTeam=0;int currentTeam=LastEquipmentTeam();bool teamChanged=currentTeam!=lastSyncedTeam;
 if(!reset && !teamChanged && now<next)return;next=now+250;lastSyncedTeam=currentTeam;
 LONG revision=menuState->revision;auto getLoadout=reinterpret_cast<uintptr_t(__fastcall*)(void*,int,int)>(inventoryClientBase+0x83C540);
 bool changed=false,allocated=false;
 if(reset){
  RestoreItemStates();
  for(int team=2;team<=3;team++)for(int slot=0;slot<LoadoutSlots;slot++){
   auto& live=liveLoadout[team][slot];auto address=inventory+0x90+16*(team*LoadoutSlots+slot);
   if(!menuState->resetAll && !ResetDefinition(Read<uint16_t>(address+8)) && !ResetDefinition(menuState->loadout[team][slot].definition))continue;
   if((live.overridden && Read<uint64_t>(address)==live.restoredId) || AddedHere(Read<uint64_t>(address))){uint64_t baseline=AddedHere(live.baselineId) && ResetDefinition(live.baselineDefinition)?0:live.baselineId;if(!SetLocalLoadout(inventory,team,slot,baseline,baseline?live.baselineDefinition:0))return;}
   live.observed=Read<uint64_t>(address);live.baselineId=live.observed;live.baselineDefinition=Read<uint16_t>(address+8);live.seen=true;live.restored=true;live.overridden=false;live.restoredId=0;
  }
  LONG result=RemoveAddedInventory(inventory);menuState->resetResult=result;
  if(result!=1)return;
  previousReset=menuState->resetSerial;
 }
 EquippedItem snapshot[LoadoutTeams][LoadoutSlots]{};memcpy(snapshot,menuState->loadout,sizeof(snapshot));
 for(int team=2;team<=3;team++)for(int slot=0;slot<LoadoutSlots;slot++){
  auto& live=liveLoadout[team][slot];auto& entry=snapshot[team][slot];auto address=inventory+0x90+16*(team*LoadoutSlots+slot);
  if(!live.seen){live.baselineId=Read<uint64_t>(address);live.baselineDefinition=Read<uint16_t>(address+8);}
  if(!live.restored && entry.definition){
   uint64_t desired=entry.itemId;
   if(entry.custom){
    if(!live.restoredId){auto& collection=restoredInventory[entry.definition];
     if(collection.id && SameInventoryChoice(collection.choice,entry.choice))live.restoredId=collection.id;
     else if(!allocated && live.failures<3 && now>=live.retryAt){uint64_t id=0;LONG result=AddSelectedToLocalInventory(entry.definition,entry.choice,id);allocated=true;
      if(result==1)live.restoredId=id;else{if(result!=-43)++live.failures;live.retryAt=now+2000;}
     }
    }
    desired=live.restoredId;if(!desired)continue;
   }
   if(!entry.custom && desired && desired<0xF000000000000000ull && !FindInventoryView(inventory,desired)){live.restored=true;}
   else{
    if(!SetLocalLoadout(inventory,team,slot,desired,uint16_t(entry.definition)))continue;
    live.restoredId=desired;live.overridden=true;live.observed=desired;live.seen=true;live.restored=true;entry.itemId=desired;changed=true;
   }
  }else if(!live.restored){live.restored=true;}
  auto current=Read<uint64_t>(address);
  if(live.seen && live.observed==current)continue;
  live.observed=current;live.seen=true;live.overridden=false;
  EquippedItem observed{};auto view=getLoadout(reinterpret_cast<void*>(inventory),team,slot);
  if(view && Readable(view,0x5B0)){
   int definition=Read<uint16_t>(view+offsets.definition);bool known=false;for(auto&w:catalog)if(w.id==definition){known=true;break;}
   if(known){observed.itemId=current;observed.definition=definition;
    auto& collection=restoredInventory[definition];
    if(current && current==collection.id){observed.custom=1;observed.choice=collection.choice;observed.choice.active=1;}
    else if(current){for(auto& known:addedChoices)if(known.id==current && known.definition==definition){observed.custom=1;observed.choice=known.choice;observed.choice.active=1;break;}}
    if(!observed.custom){for(int otherTeam=2;otherTeam<=3;otherTeam++)for(int otherSlot=0;otherSlot<LoadoutSlots;otherSlot++){
      auto& candidate=snapshot[otherTeam][otherSlot];auto& runtime=liveLoadout[otherTeam][otherSlot];
      if(current && (current==runtime.restoredId || current==candidate.itemId) && candidate.custom && candidate.definition==definition){observed.custom=1;observed.choice=candidate.choice;observed.choice.active=1;}
     }
     if(!observed.custom){auto table=Read<uintptr_t>(view);
      auto paint=reinterpret_cast<int(__fastcall*)(void*)>(Read<uintptr_t>(table+3*8));auto seed=reinterpret_cast<int(__fastcall*)(void*)>(Read<uintptr_t>(table+4*8));auto wear=reinterpret_cast<float(__fastcall*)(void*)>(Read<uintptr_t>(table+5*8));
      if(ExecutableAddress(reinterpret_cast<uintptr_t>(paint)) && ExecutableAddress(reinterpret_cast<uintptr_t>(seed)) && ExecutableAddress(reinterpret_cast<uintptr_t>(wear))){observed.choice.paint=paint(reinterpret_cast<void*>(view));observed.choice.seed=seed(reinterpret_cast<void*>(view));observed.choice.wear=wear(reinterpret_cast<void*>(view));}
     }
    }
   }
  }
  if(!SameEquippedItem(entry,observed)){entry=observed;changed=true;}
 }
 if(revision!=menuState->revision)return;
 static SkinChoice effective[8192];for(auto&choice:effective)choice=SkinChoice{};
 auto controller=Read<uintptr_t>(controllerGlobal);auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
 int team=LastEquipmentTeam();
 int knife=0,glove=0;
 if(team==2 || team==3)for(auto&entry:snapshot[team])if(entry.custom && entry.definition>0 && entry.definition<8192){effective[entry.definition]=entry.choice;effective[entry.definition].active=1;
  for(auto&w:catalog)if(w.id==entry.definition){if(w.category==4)knife=w.id;if(w.category==5)glove=w.id;break;}
 }
 bool selectionChanged=memcmp(effective,menuState->equippedChoices,sizeof(effective)) || menuState->knife!=knife || menuState->glove!=glove;
 if(changed || selectionChanged){InterlockedIncrement(&menuState->equipmentRevision);memcpy(menuState->loadout,snapshot,sizeof(snapshot));memcpy(menuState->equippedChoices,effective,sizeof(effective));menuState->knife=knife;menuState->glove=glove;InterlockedIncrement(&menuState->equipmentRevision);}
 if(knife){for(auto&w:catalog)if(w.id==knife && w.category==4){auto factory=reinterpret_cast<uintptr_t(__cdecl*)(const char*)>(GetProcAddress(GetModuleHandleW(L"tier0.dll"),"_MakeGlobalSymbol"));auto symbol=factory?factory(w.key):0;
  if(symbol){butterflySymbol=symbol;knifeModel=w.model;knifeDefinition=knife;}break;}}
}
static void ProcessInventoryRequest() {
    if(!menuState)return;
    static LONG handled=0;
    LONG request=menuState->inventoryRequest;
    if(request==handled)return;
    if(menuState->revision&1)return;
    handled=request;
    int def=menuState->inventoryDefinition;
    if(def<=0 || def>=8192){menuState->inventoryResult=-3;return;}
    auto choice=menuState->choices[def];
    uint64_t id=0;
    LONG result=RestoreInventoryItem(def,choice,menuState->revision,id);
    menuState->inventoryAddedId=id;
    InterlockedExchange(&menuState->inventoryResult,result);
    if(result==1)pendingStatus=L"APPLIED: selected skin was inserted into the local CS2 inventory view.\n";
    else pendingStatus=L"ERROR: local inventory insertion failed; cosmetic runtime remains active.\n";
}

static uint32_t LocalCosmeticAccount(){
 auto inventory=reinterpret_cast<uintptr_t>(getLocalInventory?getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)):nullptr);
 if(!PlausiblePlayerInventory(inventory))return 0;
 auto owner=Read<SOIDLocal>(inventory+0x10);return owner.type==1?uint32_t(owner.id):0;
}
static bool StaleItemBorrow(uintptr_t item,uint64_t selectedId){
 auto soc=Read<uintptr_t>(item+0x1E0);return soc && (!Readable(soc,0x18) || Read<uint64_t>(soc+0x10)!=selectedId);
}
static void ApplyGloves(uintptr_t pawn,LONG revision) {
    int id=menuState->glove;if(id<=0 || id>=8192)return;
    bool valid=false;for(auto&w:catalog)if(w.id==id && w.category==5){valid=true;break;}if(!valid)return;
    auto choice=menuState->equippedChoices[id];if(!choice.active || menuState->revision!=revision)return;

    static const int gloveOff=Field("C_CSPlayerPawn","m_EconGloves");
    static const int flagOff=Field("C_CSPlayerPawn","m_bNeedToReApplyGloves");
    static const int changedOff=Field("C_CSPlayerPawn","m_nEconGlovesChanged");
    static const int initOff=Field("C_EconItemView","m_bInitialized");
    static const int itemIdOff=Field("C_EconItemView","m_iItemID");
    static const int itemHighOff=Field("C_EconItemView","m_iItemIDHigh");
    static const int itemLowOff=Field("C_EconItemView","m_iItemIDLow");
    static const int accountOff=Field("C_EconItemView","m_iAccountID");
    static const int qualityOff=Field("C_EconItemView","m_iEntityQuality");
    static const int restoreMatOff=Field("C_EconItemView","m_bRestoreCustomMaterialAfterPrecache");
    if(gloveOff<0||flagOff<0||changedOff<0||initOff<0||itemIdOff<0||itemHighOff<0||itemLowOff<0||accountOff<0||qualityOff<0||restoreMatOff<0)return;

    auto item=pawn+gloveOff;
    if(!BindGloveAttributes(item,menuState,&enabled)){++finishFailures;return;}
    auto currentDef=Read<uint16_t>(item+offsets.definition);
    auto&inventoryItem=restoredInventory[id];
    uint64_t selectedId=EquippedId(id);if(!selectedId)return;
    gloveDefinitionSource.store(inventoryItem.materialInterface,std::memory_order_relaxed);
    static uintptr_t previousPawn=0;static int previousId=0;static SkinChoice previousChoice{};
    static int refreshFrames=0;static uint32_t previousArms=0xffffffff;
    auto armsHandle=Read<uint32_t>(pawn+offsets.arms);
    bool sameRequest=previousArms==armsHandle && previousPawn==pawn && previousId==id && !memcmp(&choice,&previousChoice,sizeof(choice));
    if(!sameRequest || currentDef!=id || Read<uint32_t>(item+itemHighOff)!=uint32_t(selectedId>>32))refreshFrames=3;
    auto clientBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
 auto component=pawn+0x15F8;
 auto resolveGlove=reinterpret_cast<uintptr_t(__fastcall*)(void*)>(clientBase+0xC79280);
 bool componentValid=Read<uintptr_t>(component+0xB0)==pawn;
 auto gloveEntity=componentValid?resolveGlove(reinterpret_cast<void*>(component)):0;
 static ULONGLONG nextMissingRetry=0;
 auto now=GetTickCount64();
 if(sameRequest && currentDef==id && !Read<bool>(pawn+flagOff) && refreshFrames<=0){
     if(gloveEntity || now<nextMissingRetry)return;
     // Entity creation can finish after the three initial frames, or the game can destroy it.
     // Retry at a bounded rate while it is missing rather than treating flags as success.
     nextMissingRetry=now+500;refreshFrames=1;
 }

    RememberItem(pawn,0,item,true);
    // The game can restore the embedded glove item after a frame. Reapply when that happens.
    *reinterpret_cast<bool*>(item+initOff)=false;
    *reinterpret_cast<bool*>(item+restoreMatOff)=true;
    *reinterpret_cast<uint16_t*>(item+offsets.definition)=static_cast<uint16_t>(id);
    *reinterpret_cast<int*>(item+qualityOff)=3;
    *reinterpret_cast<uint64_t*>(item+itemIdOff)=selectedId;
    *reinterpret_cast<uint32_t*>(item+itemHighOff)=uint32_t(selectedId>>32);
    *reinterpret_cast<uint32_t*>(item+itemLowOff)=uint32_t(selectedId);

    auto account=LocalCosmeticAccount();if(!account)return;
    WriteItemIdentity(reinterpret_cast<void*>(item),{selectedId,account},StaleItemBorrow(item,selectedId));

    *reinterpret_cast<bool*>(item+initOff)=true;
    static const int noSOCOff=Field("C_EconItemView","m_bDisallowSOC");
    if(noSOCOff>=0)*reinterpret_cast<bool*>(item+noSOCOff)=selectedId==~uint64_t{0};
    *reinterpret_cast<bool*>(pawn+flagOff)=true;
    auto changed=reinterpret_cast<uint8_t*>(pawn+changedOff);
    *changed=static_cast<uint8_t>(*changed+1);

    // The native glove component is embedded in CCSPlayerPawn on the supported build.
    // Refresh it immediately while the selected item and its attribute providers are valid.
    if(componentValid) {
        auto nativeUpdate=reinterpret_cast<void(__fastcall*)(void*)>(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"))+0xC65330);
        nativeUpdate(reinterpret_cast<void*>(component));++gloveRenderUpdates;
        // The native HUD updater copies the world glove's materials and converts
        // the low-resolution glove template to the high-resolution first-person template.
        auto arms=Entity(armsHandle);
        if(arms && resolveGlove(reinterpret_cast<void*>(component)) && Entity(Read<uint32_t>(arms+offsets.owner))==pawn){
            auto updateHud=reinterpret_cast<void(__fastcall*)(void*)>(clientBase+0x10D38D0);
            updateHud(reinterpret_cast<void*>(arms));++gloveHudUpdates;
        }
    }
    if(refreshFrames>0)--refreshFrames;
    previousArms=armsHandle;previousPawn=pawn;previousId=id;previousChoice=choice;
    pendingStatus=componentValid && resolveGlove(reinterpret_cast<void*>(component))?L"APPLIED: glove render entity created.\n":L"WAITING: selected glove render entity is not ready; retrying.\n";
}
// Current client hash is validated by the loader before this module is loaded.
// Clear through the engine so existing material references are released correctly.
struct FinishRecord {
    uint32_t handle=0xffffffff;uintptr_t hud=0;
    int definition=0;SkinChoice choice{};unsigned long long models=0;
    uint64_t itemId=0;uintptr_t worldModel=0,hudModel=0;uint64_t worldMask=0,hudMask=0;ULONGLONG nextRetry=0,nextMaterialCheck=0,settleAt=0;unsigned settlePasses=0;bool valid=false;
};
static uint64_t FinishMeshMask(int id,int paint){
 int legacy=0;for(auto&w:catalog)if(w.id==id){for(auto&s:w.skins)if(s.id==paint){legacy=s.legacy;break;}break;}
 return IsKnife(static_cast<uint16_t>(id))?2:1+legacy;
}
static void KeepFinishMesh(uintptr_t weapon,uintptr_t hud,int id,int paint){
 static const int maskOff=Field("CModelState","m_MeshGroupMask");if(maskOff<0)return;
 auto mask=FinishMeshMask(id,paint);
 // Supported client: CModelState setter stores the mask and invalidates its render resources.
 // A raw store leaves the renderer using the previously selected meshes.
 auto setMask=reinterpret_cast<void(__fastcall*)(void*,const uint64_t*)>(reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"))+0xACC551);
 for(auto entity:{weapon,hud})if(entity){auto node=Read<uintptr_t>(entity+offsets.node);if(node && Read<uint64_t>(node+offsets.modelState+maskOff)!=mask)setMask(reinterpret_cast<void*>(node+offsets.modelState),&mask);}
}
static void MaterialDiagnostic(int id,uint64_t equippedId,int expectedPaint,int actualPaint,const char* result){
    static ULONGLONG next[8192]{};static unsigned lines=0;
    if(id<=0 || id>=8192 || lines>=128)return;
    auto now=GetTickCount64();if(now<next[id])return;next[id]=now+10000;++lines;
    wchar_t path[MAX_PATH];wcscpy_s(path,statusPath);auto slash=wcsrchr(path,L'\\');if(!slash)return;
    wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"cosmetic-materials.log");
    FILE* log=nullptr;if(!_wfopen_s(&log,path,L"a") && log){fprintf(log,"tick=%llu definition=%d item=%llu expected_paint=%d actual_paint=%d result=%s\n",now,id,equippedId,expectedPaint,actualPaint,result);fclose(log);}
}
// Feed the selected item into the engine's own rebuild as well as our initial
// application. Polling and replacing the resulting materials fights streaming.
static void __fastcall SelectedFinishRefresh(void* entity,void* item,bool highResolution,bool mode,void* output){
    void* source=item;bool originalMode=mode;
    __try {
        if(menuState && enabled.load() && !failed.load()){
            auto controller=Read<uintptr_t>(controllerGlobal);
            auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
            auto address=reinterpret_cast<uintptr_t>(entity);
            auto owner=address?Entity(Read<uint32_t>(address+offsets.owner)):0;
            auto weapon=owner==pawn?address:owner;
            if(pawn && weapon && Entity(Read<uint32_t>(weapon+offsets.owner))==pawn){
                auto localItem=weapon+offsets.attributes+offsets.item;
                SkinChoice choice;
                if(GloveChoice(reinterpret_cast<void*>(localItem),choice)){
                    int id=Read<uint16_t>(localItem+offsets.definition);
                    auto equipped=EquippedId(id);
                    auto inventory=reinterpret_cast<uintptr_t>(getLocalInventory?getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)):nullptr);
                    auto view=equipped && PlausiblePlayerInventory(inventory)?FindInventoryView(inventory,equipped):0;
                    auto paintGetter=view && Readable(view,0x5B0)?Read<uintptr_t>(Read<uintptr_t>(view)+3*8):0;
                    if(view && ExecutableAddress(paintGetter) && Read<uint16_t>(view+offsets.definition)==id && reinterpret_cast<int(__fastcall*)(void*)>(paintGetter)(reinterpret_cast<void*>(view))==choice.paint){
                        source=reinterpret_cast<void*>(view);
                        selectedMaterialQueries.fetch_add(1,std::memory_order_relaxed);
                        // Preserve the working knife builder's mode.
                        if(IsKnife(static_cast<uint16_t>(id))){source=reinterpret_cast<void*>(localItem);mode=true;}
                    }
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {source=item;mode=originalMode;}
    originalFinishRefresh(entity,source,highResolution,mode,output);
}
static uintptr_t __fastcall SelectedCompositeSubmit(void* component,void* descriptor){
    const char* selected=nullptr;
    __try {
        if(menuState && enabled.load() && !failed.load() && descriptor && Readable(reinterpret_cast<uintptr_t>(descriptor),0xA0)){
            auto entity=reinterpret_cast<uintptr_t>(component)-0x610;
            auto controller=Read<uintptr_t>(controllerGlobal);
            auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
            auto owner=Entity(Read<uint32_t>(entity+offsets.owner));
            auto weapon=owner==pawn?entity:owner;
            if(pawn && weapon && Entity(Read<uint32_t>(weapon+offsets.owner))==pawn){
                auto item=weapon+offsets.attributes+offsets.item;SkinChoice choice;
                if(GloveChoice(reinterpret_cast<void*>(item),choice)){
                    auto id=Read<uint16_t>(item+offsets.definition);
                    if(!IsKnife(id)){
                        auto nativePaintDefinition=reinterpret_cast<uintptr_t(__fastcall*)(void*)>(inventoryClientBase+0x118CA00);
                        auto paint=nativePaintDefinition(reinterpret_cast<void*>(item));
                        if(paint && Readable(paint,0xD0) && Read<int>(paint)==choice.paint){
                            auto path=Read<const char*>(paint+0xC8);
                            if(path && Readable(reinterpret_cast<uintptr_t>(path),256) && !strncmp(path,"weapons/paints/",14)){
                                auto old=Read<const char*>(reinterpret_cast<uintptr_t>(descriptor)+0x18);
                                if(old && Readable(reinterpret_cast<uintptr_t>(old),256) && strncmp(old,path,255))selected=path;
                            }
                        }
                    }
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {selected=nullptr;}
    if(selected)compositeRecipeRedirects.fetch_add(1,std::memory_order_relaxed);
    return SubmitSelectedRecipe(component,descriptor,selected,originalCompositeSubmit);
}
static uintptr_t KnifeHudView(uintptr_t weapon){
    if(!menuState || !enabled.load() || failed.load() || !weapon)return 0;
    LONG revision=menuState->equipmentRevision;if(revision&1)return 0;
    auto controller=Read<uintptr_t>(controllerGlobal);
    auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
    if(!pawn || Entity(Read<uint32_t>(weapon+offsets.owner))!=pawn)return 0;
    if(!IsKnife(Read<uint16_t>(weapon+offsets.attributes+offsets.item+offsets.definition)))return 0;
    int selected=menuState->knife;if(selected<=0 || selected>=8192 || !menuState->equippedChoices[selected].active)return 0;
    auto id=EquippedId(selected);if(!id || !getLocalInventory)return 0;
    auto inventory=reinterpret_cast<uintptr_t>(getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)));
    if(!PlausiblePlayerInventory(inventory))return 0;
    auto view=FindInventoryView(inventory,id);
    if(!view || !Readable(view,0x5B0) || Read<uint16_t>(view+offsets.definition)!=selected || !Read<bool>(view+0x1E8))return 0;
    return revision==menuState->equipmentRevision?view:0;
}
static int __fastcall SelectedHudAdd(void* panel,uint32_t handle,void* item,void* context){
    void* selected=item;
    __try{auto view=KnifeHudView(Entity(handle));if(view){selected=reinterpret_cast<void*>(view);++knifeHudViews;}}
    __except(EXCEPTION_EXECUTE_HANDLER){}
    return originalHudAdd(panel,handle,selected,context);
}
static void __fastcall SelectedHudUpdate(void* panel,void* observedPawn,bool changed){
    // HUD rows are cached by entity handle, so changing the item definition
    // alone does not rebuild their icon. Remove only a stale selected knife row;
    // the native update recreates it through SelectedHudAdd immediately.
    static KnifeHudCacheState states[32]{};static unsigned cursor=0;
    __try{
        auto address=reinterpret_cast<uintptr_t>(panel);
        if(menuState && !failed.load() && Read<uintptr_t>(address)==inventoryClientBase+0x1CE15D0){
            auto controller=Read<uintptr_t>(controllerGlobal);
            auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
            if(pawn && pawn==reinterpret_cast<uintptr_t>(observedPawn)){
                int count=Read<int>(address+0x50);auto rows=Read<uintptr_t>(address+0x58);
                if(count>0 && count<=64 && Readable(rows,static_cast<size_t>(count)*0x48)){
                    for(int i=count-1;i>=0;--i){
                        auto handle=Read<uint32_t>(rows+i*0x48+0x38);auto weapon=Entity(handle);
                        auto view=KnifeHudView(weapon);if(!view)continue;
                        uint64_t id=Read<uint64_t>(view+0x1C8);int def=Read<uint16_t>(view+offsets.definition);
                        KnifeHudCacheState* remembered=nullptr;for(auto& state:states)if(state.panel==address && state.handle==handle){remembered=&state;break;}
                        if(remembered && remembered->Matches(address,handle,id,def))continue;
                        auto remove=reinterpret_cast<int(__fastcall*)(void*,int)>(inventoryClientBase+0xEAC3D0);
                        remove(panel,i);++knifeHudRebuilds;
                        if(!remembered)remembered=&states[cursor++%32];
                        *remembered={address,handle,id,def};
                        // Native removal can move the vector; inspect it again next update.
                        break;
                    }
                }
            }
        }
    }__except(EXCEPTION_EXECUTE_HANDLER){}
    originalHudUpdate(panel,observedPawn,changed);
}
static bool RefreshFinish(uintptr_t weapon,uintptr_t hud,uintptr_t item,int id,const SkinChoice& choice) {
    static const int maskOff=Field("CModelState","m_MeshGroupMask");
    int legacy=0;for(auto&w:catalog)if(w.id==id){for(auto&s:w.skins)if(s.id==choice.paint){legacy=s.legacy;break;}break;}
    uint64_t mask=IsKnife(static_cast<uint16_t>(id))?2:1+legacy;
    auto client=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"client.dll"));
    auto clear=reinterpret_cast<void(__fastcall*)(void*,bool)>(client+0x14E7AA0);
    auto refresh=reinterpret_cast<void(__fastcall*)(void*,void*,bool,bool,void*)>(client+0x7FF680);
    // Render from the actual equipped inventory view. A weapon's transient
    // fallback attributes do not constitute its persistent inventory material.
    // Use the equipped ID, not the last added finish for this definition: T/CT
    // may equip different finishes of the same AWP or knife.
    auto inventory=reinterpret_cast<uintptr_t>(getLocalInventory?getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)):nullptr);
    uint64_t equippedId=EquippedId(id);
    bool knife=IsKnife(static_cast<uint16_t>(id));
    // Keep the previously working knife path while the inventory-source path is
    // diagnosed. Knives must not become dependent on inventory-view readiness.
    auto materialItem=knife?item:equippedId && PlausiblePlayerInventory(inventory)?FindInventoryView(inventory,equippedId):0;
    if(!materialItem || !Readable(materialItem,0x5B0) || Read<uint16_t>(materialItem+offsets.definition)!=id){MaterialDiagnostic(id,equippedId,choice.paint,-1,"missing-or-wrong-material-view");return false;}
    auto nativePaint=reinterpret_cast<int(__fastcall*)(void*)>(Read<uintptr_t>(Read<uintptr_t>(materialItem)+3*8));
    int actualPaint=ExecutableAddress(reinterpret_cast<uintptr_t>(nativePaint))?nativePaint(reinterpret_cast<void*>(materialItem)):-1;
    if(actualPaint!=choice.paint){MaterialDiagnostic(id,equippedId,choice.paint,actualPaint,"paint-mismatch");return false;}
    auto nativeMaterialMode=reinterpret_cast<bool(__fastcall*)(void*)>(client+0xDAEDF0);
    bool sourceMode=knife?true:nativeMaterialMode(reinterpret_cast<void*>(materialItem));
    bool produced=true;
    for(auto entity:{weapon,hud})if(entity) {
        auto scene=Read<uintptr_t>(entity+offsets.node);
        if(!scene){produced=false;continue;}
        if(maskOff>=0){auto setMask=reinterpret_cast<void(__fastcall*)(void*,const uint64_t*)>(client+0xACC551);if(Read<uint64_t>(scene+offsets.modelState+maskOff)!=mask)setMask(reinterpret_cast<void*>(scene+offsets.modelState),&mask);}
        clear(reinterpret_cast<void*>(entity+0x610),true);
        refresh(reinterpret_cast<void*>(entity),reinterpret_cast<void*>(materialItem),entity==hud,sourceMode,nullptr);
        if(choice.paint!=0 && Read<int>(entity+0xAB0)<=0 && Read<int>(entity+0xAC8)<=0)produced=false;
        ++finishRefreshes;
    }
    if(!produced)++finishFailures;
    MaterialDiagnostic(id,equippedId,choice.paint,actualPaint,produced?(knife?"knife-previous-source-submitted":"inventory-source-submitted"):"material-not-produced");
    return produced;
}
static void ApplySkins(bool identityOnly=false) {
    if(!menuState || !enabled.load())return;
    LONG revision=menuState->revision;if(revision&1)return;
    auto controller=Read<uintptr_t>(controllerGlobal);auto pawn=controller?Entity(Read<uint32_t>(controller+offsets.pawn)):0;
    if(!pawn || Read<int>(pawn+offsets.health)<=0){SetWeaponFinishTargets(nullptr,0);return;}
    if(!identityOnly)ApplyGloves(pawn,revision);
    auto services=Read<uintptr_t>(pawn+offsets.services);if(!services)return;
    uint32_t handles[64]{};size_t count=0;
    static const int ownedOff=Field("CPlayer_WeaponServices","m_hMyWeapons");
    if(ownedOff>=0){
        int ownedCount=Read<int>(services+ownedOff);auto data=Read<uintptr_t>(services+ownedOff+8);
        if(ownedCount>0 && ownedCount<=63 && Readable(data,size_t(ownedCount)*sizeof(uint32_t)))
            for(int i=0;i<ownedCount;i++)handles[count++]=Read<uint32_t>(data+i*sizeof(uint32_t));
    }
    auto active=Read<uint32_t>(services+offsets.active);bool included=false;
    for(size_t i=0;i<count;i++)if(handles[i]==active)included=true;
    if(!included && active!=0xffffffff)handles[count++]=active;
    uintptr_t items[64]{};size_t itemCount=0;
    for(size_t i=0;i<count;i++){auto weapon=Entity(handles[i]);if(weapon)items[itemCount++]=weapon+offsets.attributes+offsets.item;}
    SetWeaponFinishTargets(items,itemCount);
    static uintptr_t previousPawn=0;static std::vector<FinishRecord> records;
    if(previousPawn!=pawn){records.clear();previousPawn=pawn;}
    static const int fullId=Field("C_EconItemView","m_iItemID");
    static const int high=Field("C_EconItemView","m_iItemIDHigh"),low=Field("C_EconItemView","m_iItemIDLow");
    static const int initialized=Field("C_EconItemView","m_bInitialized"),noSOC=Field("C_EconItemView","m_bDisallowSOC");
    static const int restore=Field("C_EconItemView","m_bRestoreCustomMaterialAfterPrecache");
    static const int statTrak=Field("C_EconEntity","m_nFallbackStatTrak"),quality=Field("C_EconItemView","m_iEntityQuality");
    static const int paint=Field("C_EconEntity","m_nFallbackPaintKit"),seed=Field("C_EconEntity","m_nFallbackSeed"),wear=Field("C_EconEntity","m_flFallbackWear");
    if(high<0 || low<0 || initialized<0 || noSOC<0 || restore<0 || paint<0 || seed<0 || wear<0)return;
    auto now=GetTickCount64();auto localAccount=LocalCosmeticAccount();if(!localAccount)return;
    for(size_t i=0;i<count;i++){
        auto h=handles[i];auto weapon=Entity(h);if(!weapon)continue;
        auto item=weapon+offsets.attributes+offsets.item;
        int id=Read<uint16_t>(item+offsets.definition);
        if(IsKnife(static_cast<uint16_t>(id)))id=knifeDefinition.load();
        if(id<=0 || id>=8192)continue;
        auto choice=menuState->equippedChoices[id];if(menuState->revision!=revision)return;if(!choice.active)continue;
        if(!identityOnly && !BindGloveAttributes(item,menuState,&enabled,true)){++finishFailures;continue;}
        auto hud=HudWeapon(pawn,weapon);
        static const int meshOff=Field("CModelState","m_MeshGroupMask");
        auto worldNode=Read<uintptr_t>(weapon+offsets.node);auto hudNode=hud?Read<uintptr_t>(hud+offsets.node):0;
        auto worldModel=worldNode?Read<uintptr_t>(worldNode+offsets.modelState+0xA8):0;auto hudModel=hudNode?Read<uintptr_t>(hudNode+offsets.modelState+0xA8):0;
        auto worldMask=meshOff>=0&&worldNode?Read<uint64_t>(worldNode+offsets.modelState+meshOff):0;auto hudMask=meshOff>=0&&hudNode?Read<uint64_t>(hudNode+offsets.modelState+meshOff):0;
        if(!identityOnly)KeepFinishMesh(weapon,hud,id,choice.paint);
        RememberItem(weapon,h,item,false);
        if(statTrak>=0)*reinterpret_cast<int*>(weapon+statTrak)=-1;
        RememberItem(weapon,h,item,false);
        if(quality>=0)*reinterpret_cast<int*>(item+quality)=IsKnife(static_cast<uint16_t>(id))?3:4;
        auto&inventoryItem=restoredInventory[id];
        uint64_t selectedId=EquippedId(id);if(!selectedId)continue;
        bool staleBorrow=StaleItemBorrow(item,selectedId);
        bool identityChanged=ItemIdentityNeedsUpdate(reinterpret_cast<void*>(item),{selectedId,localAccount});
        bool replaced=identityChanged || staleBorrow || Read<uint16_t>(item+offsets.definition)!=id || Read<uint32_t>(item+high)!=uint32_t(selectedId>>32) || Read<uint32_t>(item+low)!=uint32_t(selectedId) ||
            Read<int>(weapon+paint)!=choice.paint || Read<int>(weapon+seed)!=choice.seed || Read<float>(weapon+wear)!=choice.wear;
        FinishRecord* record=nullptr;for(auto&r:records)if(r.handle==h){record=&r;break;}
        if(!record){if(records.size()>=64)records.erase(records.begin());records.push_back({});record=&records.back();record->handle=h;}
        bool newSelection=record->definition!=id || record->hud!=hud || record->itemId!=selectedId ||
            record->choice.paint!=choice.paint || record->choice.seed!=choice.seed || record->choice.wear!=choice.wear;
        if(!identityOnly && newSelection){record->settlePasses=2;record->settleAt=now+250;record->nextRetry=0;}
        // The first material may be a temporary template while resources stream.
        // Two delayed passes cover that transition without a permanent rebuild loop.
        if(!identityOnly && record->valid && record->settlePasses && now>=record->settleAt){--record->settlePasses;record->settleAt=now+500;record->valid=false;}
        if(staleBorrow)record->valid=false;
        // A successful world material does not prove the view model has a finish.
        // Detect material loss at a bounded rate instead of rebuilding every frame.
        if(record->valid && choice.paint!=0 && now>=record->nextMaterialCheck){
            record->nextMaterialCheck=now+500;
            for(auto entity:{weapon,hud})if(entity && Read<int>(entity+0xAB0)<=0 && Read<int>(entity+0xAC8)<=0)record->valid=false;
        }
        auto models=modelCalls.load();
        bool changed=!record->valid || record->worldModel!=worldModel || record->hudModel!=hudModel || record->worldMask!=worldMask || record->hudMask!=hudMask || record->definition!=id || record->hud!=hud || record->choice.paint!=choice.paint ||
            record->choice.seed!=choice.seed || record->choice.wear!=choice.wear || record->models!=models || record->itemId!=selectedId;
        // Network snapshots may reset fallback scalars without invalidating materials.
        // Reassert those scalars, but rebuild only for a changed cosmetic or entity.
        if(!changed && !replaced && (fullId<0 || Read<uint64_t>(item+fullId)==selectedId))continue;
        *reinterpret_cast<uint16_t*>(item+offsets.definition)=static_cast<uint16_t>(id);
        RememberItem(weapon,h,item,false);
        WriteItemIdentity(reinterpret_cast<void*>(item),{selectedId,localAccount},staleBorrow);
        *reinterpret_cast<uint32_t*>(item+high)=uint32_t(selectedId>>32);
        *reinterpret_cast<uint32_t*>(item+low)=uint32_t(selectedId);
        *reinterpret_cast<bool*>(item+initialized)=true;
        *reinterpret_cast<bool*>(item+noSOC)=selectedId==~uint64_t{0};
        *reinterpret_cast<bool*>(item+restore)=true;
        *reinterpret_cast<int*>(weapon+paint)=choice.paint;
        *reinterpret_cast<int*>(weapon+seed)=choice.seed;
        *reinterpret_cast<float*>(weapon+wear)=choice.wear;
        if(identityOnly || !changed || now<record->nextRetry)continue;
        bool ready=RefreshFinish(weapon,hud,item,id,choice);
        record->worldModel=worldModel;record->hudModel=hudModel;record->worldMask=FinishMeshMask(id,choice.paint);record->hudMask=hud?record->worldMask:0;record->itemId=selectedId;record->hud=hud;record->definition=id;record->choice=choice;record->models=modelCalls.load();
        record->valid=ready;record->nextRetry=now+250;
        if(ready && menuState->revision==revision){
            menuState->appliedDefinition=id;menuState->appliedPaint=choice.paint;InterlockedIncrement(&menuState->applySerial);
            pendingStatus=L"APPLIED: engine cosmetic materials rebuilt for the selected finish.\n";
        }
    }
}
static void ControlMenuMouse() {
    if(!menuState)return;
    static auto sdl=GetModuleHandleW(L"SDL3.dll");
    static auto windows=reinterpret_cast<void**(__cdecl*)(int*)>(GetProcAddress(sdl,"SDL_GetWindows"));
    static auto release=reinterpret_cast<void(__cdecl*)(void*)>(GetProcAddress(sdl,"SDL_free"));
    static auto relative=reinterpret_cast<bool(__cdecl*)(void*,bool)>(GetProcAddress(sdl,"SDL_SetWindowRelativeMouseMode"));
    static auto getRelative=reinterpret_cast<bool(__cdecl*)(void*)>(GetProcAddress(sdl,"SDL_GetWindowRelativeMouseMode"));
    static auto show=reinterpret_cast<bool(__cdecl*)()>(GetProcAddress(sdl,"SDL_ShowCursor"));
    static void* gameWindow=nullptr;static bool previousOpen=false,wasRelative=false;
    if(!gameWindow && windows && release){int count=0;auto list=windows(&count);if(list){if(count>0)gameWindow=list[0];release(list);}}
    if(!gameWindow || !relative || !getRelative)return;
    bool open=menuState->menuOpen && GetTickCount64()-menuState->menuHeartbeat<2000;
    if(open){if(!previousOpen)wasRelative=getRelative(gameWindow);if(getRelative(gameWindow))relative(gameWindow,false);if(show)show();ClipCursor(nullptr);}
    else if(previousOpen){relative(gameWindow,wasRelative);}
    previousOpen=open;
}
#include "viewmodel_glow.hpp"
static bool PrepareCoreUnload(){
    enabled=false;menuState->menuOpen=0;ControlMenuMouse();
    LONG previous=menuState->resetAll;bool ready=false;
    __try {
        RestoreViewGlow();menuState->resetAll=1;RestoreItemStates();
        if(addedChoices.empty()){ready=true;}
        else {
            auto inventory=reinterpret_cast<uintptr_t>(getLocalInventory?getLocalInventory(reinterpret_cast<void*>(inventoryClientBase+0x24D8AA0)):nullptr);
            if(PlausiblePlayerInventory(inventory) && Readable(inventory+0x90,LoadoutTeams*LoadoutSlots*16)){
                ready=true;
                for(int team=2;team<=3;team++)for(int slot=0;slot<LoadoutSlots;slot++){
                    auto& live=liveLoadout[team][slot];auto address=inventory+0x90+16*(team*LoadoutSlots+slot);
                    if((live.overridden && Read<uint64_t>(address)==live.restoredId) || AddedHere(Read<uint64_t>(address))){
                        uint64_t baseline=AddedHere(live.baselineId)?0:live.baselineId;
                        if(!SetLocalLoadout(inventory,team,slot,baseline,baseline?live.baselineDefinition:0))ready=false;
                    }
                }
                if(ready)ready=RemoveAddedInventory(inventory)==1;
            }
        }
    } __finally {menuState->resetAll=previous;}
    animationNode=0;return ready;
}
static void __fastcall Stage(void* self,int stage) {
    if(menuState && menuState->unloadRequest){
        if(stage==5 && !shutdownPrepared.load() && menuState->unloadResult>=0){
            __try {
                if(PrepareCoreUnload())shutdownPrepared=true;
                else InterlockedExchange(&menuState->unloadResult,-4);
            } __except(EXCEPTION_EXECUTE_HANDLER){InterlockedExchange(&menuState->unloadResult,-1);}
        }
        originalStage(self,stage);return;
    }
    if(stage==5)__try{ControlMenuMouse();UpdateViewGlow();}__except(EXCEPTION_EXECUTE_HANDLER){if(menuState)menuState->glowStatus=-1;}
    if(stage==5 && !failed.load()){
        __try{SyncEquipment();if(enabled.load()){Apply();ApplySkins(true);}else if(!saved.empty())Restore();}__except(EXCEPTION_EXECUTE_HANDLER){failed=true;pendingStatus=L"ERROR: pre-render model update fault. Restart CS2.\n";}
    }
    originalStage(self,stage);
    if(stage!=5 || failed.load()) return;
    __try { ProcessInventoryRequest(); if(enabled.load()) {RestoreSavedInventory();SyncEquipment();ApplySkins();} else if(!saved.empty()) Restore(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        failed=true; animationNode=0; pendingStatus=L"ERROR: client memory or model update fault; changes stopped. Restart CS2.\n";
    }
}
static void* Interface(const wchar_t* moduleName,const char* name) {
    auto module=GetModuleHandleW(moduleName); if(!module) return nullptr;
    auto create=reinterpret_cast<void*(__cdecl*)(const char*,int*)>(GetProcAddress(module,"CreateInterface"));
    return create ? create(name,nullptr) : nullptr;
}
static bool Initialize() {
    auto client=GetModuleHandleW(L"client.dll"); if(!client || !CompatibleClientProfile(client)) return false;
    auto model=UniquePattern(client,kSetModelPattern);
    auto entities=UniquePattern(client,kEntityPattern);
    auto controller=UniquePattern(client,kControllerPattern);
    if(!model || !entities || !controller) { Status(L"ERROR: client signatures are missing or ambiguous.\n"); return false; }
    setModel=reinterpret_cast<SetModelFn>(model); entityGlobal=RipTarget(entities); controllerGlobal=RipTarget(controller);
    auto schema=Interface(L"schemasystem.dll","SchemaSystem_001"); if(!schema) return false;
    auto getScope=Virtual<void*(__fastcall*)(void*,const char*,void*)>(schema,13);
    if(!getScope || !(scope=getScope(schema,"client.dll",nullptr))) return false;
    offsets.pawn=Field("CCSPlayerController","m_hPlayerPawn");
    offsets.services=Field("C_BasePlayerPawn","m_pWeaponServices");
    offsets.active=Field("CPlayer_WeaponServices","m_hActiveWeapon");
    offsets.arms=Field("C_CSPlayerPawn","m_hHudModelArms");
    offsets.node=Field("C_BaseEntity","m_pGameSceneNode");
    offsets.owner=Field("C_BaseEntity","m_hOwnerEntity");
    offsets.health=Field("C_BaseEntity","m_iHealth");
    offsets.child=Field("CGameSceneNode","m_pChild");
    offsets.sibling=Field("CGameSceneNode","m_pNextSibling");
    offsets.nodeOwner=Field("CGameSceneNode","m_pOwner");
    offsets.attributes=Field("C_EconEntity","m_AttributeManager");
    offsets.item=Field("C_AttributeContainer","m_Item");
    offsets.definition=Field("C_EconItemView","m_iItemDefinitionIndex");
    offsets.modelState=Field("CSkeletonInstance","m_modelState");
    offsets.modelName=Field("CModelState","m_ModelName");
    offsets.graph=Field("CBaseAnimGraph","m_pMainGraphController");
    offsets.weaponType=Field("CCS2WeaponGraphController","m_weaponType");
    auto makeSymbol=reinterpret_cast<uintptr_t(__cdecl*)(const char*)>(GetProcAddress(GetModuleHandleW(L"tier0.dll"),"_MakeGlobalSymbol"));
    if(!makeSymbol || !(butterflySymbol=makeSymbol("weapon_knife_butterfly"))) return false;
    for(size_t i=0;i<sizeof(offsets)/sizeof(int);++i) if(reinterpret_cast<int*>(&offsets)[i]<0) {
        Status(L"ERROR: required schema fields were not resolved.\n"); return false;
    }
    const char* cosmeticFields[]={"m_iItemIDHigh","m_AttributeList","m_bInitialized"};
    for(auto field:cosmeticFields)if(Field("C_EconItemView",field)<0)return false;
    if(Field("CAttributeList","m_Attributes")<0 || Field("C_EconEntity","m_nFallbackPaintKit")<0 || Field("C_EconEntity","m_nFallbackSeed")<0 || Field("C_EconEntity","m_flFallbackWear")<0 || Field("C_CSPlayerPawn","m_EconGloves")<0 || Field("C_CSPlayerPawn","m_bNeedToReApplyGloves")<0)return false;
    auto clientInterface=Interface(L"client.dll","Source2Client002"); if(!clientInterface) return false;
    if(!InitInventoryBridge(clientInterface)) {
        Status(L"ERROR: inventory entry points do not match this client build.\n");
        return false;
    }
    auto table=*reinterpret_cast<void***>(clientInterface); if(!Readable(reinterpret_cast<uintptr_t>(table),37*8)) return false;
    originalStage=reinterpret_cast<StageFn>(table[36]);
    MEMORY_BASIC_INFORMATION memory{}; VirtualQuery(reinterpret_cast<void*>(originalStage),&memory,sizeof(memory));
    if(memory.AllocationBase!=client || !(memory.Protect & (PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE))) return false;
    DWORD oldProtection=0; if(!VirtualProtect(&table[36],sizeof(void*),PAGE_READWRITE,&oldProtection)) return false;
    auto initialized=MH_Initialize();
    if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED){DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;}
    if(CreateLifecycleHook(reinterpret_cast<void*>(model),reinterpret_cast<void*>(&SelectedKnifeModel),reinterpret_cast<void**>(&setModel),0)!=MH_OK ||
       MH_EnableHook(reinterpret_cast<void*>(model))!=MH_OK){DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;}
    auto gloveHidden=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)+0x789F50);
    if(CreateLifecycleHook(gloveHidden,reinterpret_cast<void*>(&SelectedGloveHidden),reinterpret_cast<void**>(&originalGloveHidden),7)!=MH_OK || MH_EnableHook(gloveHidden)!=MH_OK){
        MH_DisableHook(reinterpret_cast<void*>(model));KeepLifecycleHook(reinterpret_cast<void*>(model));
        KeepLifecycleHook(gloveHidden);DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;
    }
    auto finishRefresh=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)+0x7FF680);
    if(CreateLifecycleHook(finishRefresh,reinterpret_cast<void*>(&SelectedFinishRefresh),reinterpret_cast<void**>(&originalFinishRefresh),2)!=MH_OK || MH_EnableHook(finishRefresh)!=MH_OK){
        MH_DisableHook(gloveHidden);KeepLifecycleHook(gloveHidden);
        MH_DisableHook(reinterpret_cast<void*>(model));KeepLifecycleHook(reinterpret_cast<void*>(model));
        KeepLifecycleHook(finishRefresh);DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;
    }
    auto weaponName=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)+0x7CCDB0);
    if(CreateLifecycleHook(weaponName,reinterpret_cast<void*>(&SelectedWeaponName),reinterpret_cast<void**>(&originalWeaponName),3)!=MH_OK || MH_EnableHook(weaponName)!=MH_OK){
        MH_DisableHook(finishRefresh);KeepLifecycleHook(finishRefresh);
        MH_DisableHook(gloveHidden);KeepLifecycleHook(gloveHidden);
        MH_DisableHook(reinterpret_cast<void*>(model));KeepLifecycleHook(reinterpret_cast<void*>(model));
        KeepLifecycleHook(weaponName);DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;
    }
    auto compositeSubmit=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)+0x95A410);
    if(CreateLifecycleHook(compositeSubmit,reinterpret_cast<void*>(&SelectedCompositeSubmit),reinterpret_cast<void**>(&originalCompositeSubmit),4)!=MH_OK || MH_EnableHook(compositeSubmit)!=MH_OK){
        MH_DisableHook(weaponName);KeepLifecycleHook(weaponName);
        MH_DisableHook(finishRefresh);KeepLifecycleHook(finishRefresh);
        MH_DisableHook(gloveHidden);KeepLifecycleHook(gloveHidden);
        MH_DisableHook(reinterpret_cast<void*>(model));KeepLifecycleHook(reinterpret_cast<void*>(model));
        KeepLifecycleHook(compositeSubmit);DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;
    }
    auto hudUpdate=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)+0xED6BF0);
    auto hudAdd=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)+0xEA7660);
    if(CreateLifecycleHook(hudAdd,reinterpret_cast<void*>(&SelectedHudAdd),reinterpret_cast<void**>(&originalHudAdd),6)!=MH_OK ||
       CreateLifecycleHook(hudUpdate,reinterpret_cast<void*>(&SelectedHudUpdate),reinterpret_cast<void**>(&originalHudUpdate),5)!=MH_OK ||
       MH_EnableHook(hudAdd)!=MH_OK || MH_EnableHook(hudUpdate)!=MH_OK){
        for(auto hook:{hudAdd,hudUpdate,compositeSubmit,weaponName,finishRefresh,gloveHidden,reinterpret_cast<void*>(model)}){MH_DisableHook(hook);KeepLifecycleHook(hook);}
        DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;
    }
    if(!InstallLifecycleSlot(&table[36],reinterpret_cast<void*>(&Stage),1)){DWORD ignored;VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);return false;}
    DWORD ignored; VirtualProtect(&table[36],sizeof(void*),oldProtection,&ignored);
    Status(L"READY: hook and schema initialized. Hold your knife in a match. F8 toggles changes; Uninject is available in Settings.\n");
    return true;
}
static DWORD WINAPI Worker(void*) {
    bool success=false;
    __try {success=InitializeLifecycle() && Initialize();} __except(EXCEPTION_EXECUTE_HANDLER) { Status(L"ERROR: client initialization fault. No frame hook installed.\n"); }
    auto mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedMenu),MenuMap);
    bool fresh=GetLastError()!=ERROR_ALREADY_EXISTS;
    menuState=static_cast<SharedMenu*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedMenu)));
    if(menuState && (fresh || menuState->magic!=MenuMagic)) { new(menuState)SharedMenu; }
    if(!success) {
        if(menuState) InterlockedExchange(&menuState->runtimeConnected,0);
        Status(L"ERROR: initialization did not complete; no frame hook installed.\n");
        return 1;
    }
    if(menuState) {
        menuState->unloadRequest=0;menuState->unloadResult=0;menuState->runtimePid=GetCurrentProcessId();
        InterlockedExchange(&menuState->runtimeConnected,1);
        menuState->runtimeHeartbeat=GetTickCount64();
    }
    auto symbolFactory=reinterpret_cast<uintptr_t(__cdecl*)(const char*)>(GetProcAddress(GetModuleHandleW(L"tier0.dll"),"_MakeGlobalSymbol"));
    while(true) {
        if(menuState && menuState->unloadRequest && shutdownPrepared.load()){
            InterlockedExchange(&menuState->unloadResult,1);
            if(!retireOwner(runtimeModule,5000)){
                InterlockedExchange(&menuState->unloadResult,-2);Sleep(100);continue;
            }
            auto disabled=MH_DisableHook(MH_ALL_HOOKS);
            if((disabled!=MH_OK && disabled!=MH_ERROR_DISABLED) || !RestoreLifecycleSlots()){
                InterlockedExchange(&menuState->unloadResult,-3);Sleep(100);continue;
            }
            InterlockedExchange(&menuState->runtimeConnected,0);
            InterlockedExchange(&menuState->unloadResult,2);
            Status(L"UNLOADED: core hooks removed; CS2 remains running. Lifetime bridge remains idle until process exit.\n");
            UnmapViewOfFile(menuState);menuState=nullptr;CloseHandle(mapping);
            FreeLibraryAndExitThread(runtimeModule,0);
        }
        if(menuState) {
            InterlockedExchange(&menuState->runtimeConnected,failed.load()?0:1);
            menuState->runtimeHeartbeat=GetTickCount64();
        }
        if(auto message=pendingStatus.exchange(nullptr)) Status(message);
        static ULONGLONG nextStats=0;
        if(GetTickCount64()>nextStats) {
            nextStats=GetTickCount64()+1000;
            wchar_t path[MAX_PATH];wcscpy_s(path,statusPath);
            auto slash=wcsrchr(path,L'\\');if(slash)wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"butterfly-performance.txt");
            FILE* f=nullptr;
            if(!_wfopen_s(&f,path,L"w") && f) {fwprintf(f,L"model_calls_total=%llu\nmodel_intercepts=%llu\nanimation_overrides_total=%llu\nanimation_node_bound=%d\nglove_paint_queries=%llu\nglove_attribute_queries=%llu\nglove_render_updates=%llu\nglove_objects_kept=%llu\nglove_hud_updates=%llu\nfinish_refreshes=%llu\nfinish_pending=%llu\nselected_knife=%d\nselected_gloves=%d\n",modelCalls.load(),modelOverrides.load(),animationWrites.load(),animationNode.load()!=0,glovePaintCalls.load(),gloveAttributeCalls.load(),gloveRenderUpdates.load(),gloveKeptObjects.load(),gloveHudUpdates.load(),finishRefreshes.load(),finishFailures.load(),menuState?menuState->knife:0,menuState?menuState->glove:0);fwprintf(f,L"selected_material_queries=%llu\nselected_weapon_name_queries=%llu\ncomposite_recipe_redirects=%llu\nknife_hud_rebuilds=%llu\nknife_hud_views=%llu\n",selectedMaterialQueries.load(),selectedNameQueries.load(),compositeRecipeRedirects.load(),knifeHudRebuilds.load(),knifeHudViews.load());fclose(f);}
        }
        if((!menuState || !menuState->unloadRequest) && (GetAsyncKeyState(VK_F8)&1)) {enabled=!enabled.load(); Status(enabled ? L"ENABLED: hold your knife.\n" : L"DISABLED: weapon definition and world model restoration requested. Restart CS2 for full reset.\n");} Sleep(100); }
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        runtimeModule=instance;DisableThreadLibraryCalls(instance);
        GetModuleFileNameW(instance,statusPath,MAX_PATH);
        wchar_t* slash=wcsrchr(statusPath,L'\\'); if(slash) wcscpy_s(slash+1,MAX_PATH-(slash+1-statusPath),L"butterfly-status.txt");
        auto thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr); if(thread) CloseHandle(thread);
    }
    return TRUE;
}


