#pragma once
// Included after entity and schema helpers. Only first-person attachments are selected.
struct ViewGlowRecord {
    uintptr_t entity=0,identity=0;
    uint32_t handle=0;
    unsigned char original[9][12]{};
};
static ViewGlowRecord viewGlow[2]{};
struct ViewGlowFields {
    int component=-1;
    int fields[9]{};
    int sizes[9]={12,4,4,4,4,4,1,1,1};
    bool valid=false;
    ViewGlowFields(){
        component=Field("C_BaseModelEntity","m_Glow");
        const char* names[]={"m_fGlowColor","m_iGlowType","m_iGlowTeam","m_nGlowRange","m_nGlowRangeMin","m_glowColorOverride","m_bFlashing","m_bEligibleForScreenHighlight","m_bGlowing"};
        valid=component>=0;
        for(int i=0;i<9;i++){fields[i]=Field("CGlowProperty",names[i]);if(fields[i]<0 || fields[i]+sizes[i]>0x58)valid=false;}
    }
};
static ViewGlowFields& GlowFields(){static ViewGlowFields fields;return fields;}
static bool SameGlowEntity(const ViewGlowRecord& record){
    return record.entity && Read<uintptr_t>(record.entity+0x10)==record.identity &&
        Read<uint32_t>(record.identity+0x10)==record.handle && Entity(record.handle)==record.entity;
}
static void RestoreViewGlow(ViewGlowRecord& record){
    auto& f=GlowFields();
    if(f.valid && SameGlowEntity(record) && Readable(record.entity+f.component,0x58))
        for(int i=0;i<9;i++)memcpy(reinterpret_cast<void*>(record.entity+f.component+f.fields[i]),record.original[i],f.sizes[i]);
    record={};
}
static void RestoreViewGlow(){for(auto& record:viewGlow)RestoreViewGlow(record);if(menuState)menuState->glowStatus=0;}
static bool SetViewGlow(ViewGlowRecord& record,uintptr_t entity,const GlowSettings& config){
    auto& f=GlowFields();
    if(record.entity!=entity || !SameGlowEntity(record))RestoreViewGlow(record);
    if(!entity || !f.valid || !Readable(entity+f.component,0x58))return false;
    if(!record.entity){
        auto identity=Read<uintptr_t>(entity+0x10);auto handle=Read<uint32_t>(identity+0x10);
        if(!identity || Entity(handle)!=entity)return false;
        record.entity=entity;record.identity=identity;record.handle=handle;
        for(int i=0;i<9;i++)memcpy(record.original[i],reinterpret_cast<const void*>(entity+f.component+f.fields[i]),f.sizes[i]);
    }
    float color[3];uint32_t packed=0xff000000;
    for(int i=0;i<3;i++){color[i]=config.color[i]*config.intensity;packed|=uint32_t(color[i]*255.f+0.5f)<<(8*i);}
    int type=3,team=0,range=0;bool no=false,yes=true;
    const void* values[]={color,&type,&team,&range,&range,&packed,&no,&yes,&yes};
    for(int i=0;i<9;i++)memcpy(reinterpret_cast<void*>(entity+f.component+f.fields[i]),values[i],f.sizes[i]);
    return true;
}
static void UpdateViewGlow(){
    if(!menuState)return;
    LONG revision=menuState->glowRevision;if(revision&1)return;
    auto config=NormalizeGlow(menuState->glow);MemoryBarrier();if(revision!=menuState->glowRevision)return;
    if(!config.enabled || config.intensity<=0 || !enabled.load() || failed.load()){RestoreViewGlow();return;}
    if(!GlowFields().valid){menuState->glowStatus=-1;return;}
    auto controller=Read<uintptr_t>(controllerGlobal);
    auto pawn=Entity(Read<uint32_t>(controller+offsets.pawn));
    if(!pawn || Read<int>(pawn+offsets.health)<=0){RestoreViewGlow();return;}
    auto services=Read<uintptr_t>(pawn+offsets.services);
    auto weapon=Entity(Read<uint32_t>(services+offsets.active));
    auto arms=Entity(Read<uint32_t>(pawn+offsets.arms));
    auto hud=weapon?HudWeapon(pawn,weapon):0;
    bool hands=SetViewGlow(viewGlow[0],config.target!=1?arms:0,config);
    bool gun=SetViewGlow(viewGlow[1],config.target!=0 && hud!=arms?hud:0,config);
    menuState->glowStatus=hands||gun?1:0;
}
