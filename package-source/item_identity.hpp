#pragma once
#include <cstdint>
#include <cstring>
// Supported CEconItemView layout. The client profile validates the native SOC
// resolver, which checks account at 0x1D8 and first uses its borrowed SOC at 0x1E0.
struct ItemIdentity { uint64_t id=0; uint32_t account=0; };
inline bool ItemIdentityNeedsUpdate(const void* object,ItemIdentity selected){
 auto item=static_cast<const unsigned char*>(object);
 uint64_t full=0;uint32_t high=0,low=0,account=0;
 memcpy(&full,item+0x1C8,8);memcpy(&high,item+0x1D0,4);memcpy(&low,item+0x1D4,4);memcpy(&account,item+0x1D8,4);
 return full!=selected.id || high!=uint32_t(selected.id>>32) || low!=uint32_t(selected.id) || account!=selected.account;
}
inline void WriteItemIdentity(void* object,ItemIdentity selected,bool staleBorrow){
 auto item=static_cast<unsigned char*>(object);
 bool changed=ItemIdentityNeedsUpdate(object,selected);
 uint32_t high=uint32_t(selected.id>>32),low=uint32_t(selected.id);
 memcpy(item+0x1C8,&selected.id,8);memcpy(item+0x1D0,&high,4);memcpy(item+0x1D4,&low,4);memcpy(item+0x1D8,&selected.account,4);
 // Invalidate only the borrowed cache. The native resolver owns lookup and
 // lifetime; never attach a saved pointer from another inventory view.
 if(changed || staleBorrow){uintptr_t empty=0;memcpy(item+0x1E0,&empty,sizeof(empty));}
}
