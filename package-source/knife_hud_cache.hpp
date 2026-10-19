#pragma once
#include <cstdint>
struct KnifeHudCacheState {
 uintptr_t panel=0;uint32_t handle=0;uint64_t item=0;int definition=0;
 bool Matches(uintptr_t p,uint32_t h,uint64_t i,int d)const{return panel==p && handle==h && item==i && definition==d;}
};
