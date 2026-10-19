#pragma once
#include <cstdint>
#include <cstddef>

// Compare stable resource handles, not the asynchronously populated resources
// behind them. Streaming completion must not trigger another material rebuild.
struct MaterialBinding {
    int counts[2]{};
    uintptr_t handles[2][16]{};
    bool valid=false;
    bool operator==(const MaterialBinding& rhs) const {
        if(!valid || !rhs.valid)return false;
        for(int group=0;group<2;++group){
            if(counts[group]!=rhs.counts[group])return false;
            for(int i=0;i<counts[group];++i)
                if(handles[group][i]!=rhs.handles[group][i])return false;
        }
        return true;
    }
};
template<class ReadValue,class ReadRange>
MaterialBinding CaptureMaterialBinding(uintptr_t entity,ReadValue read,ReadRange readable){
    MaterialBinding result;
    if(!entity){result.valid=true;return result;}
    for(int group=0;group<2;++group){
        auto offset=group?0xAC8:0xAB0;
        if(!readable(entity+offset,16))return result;
        auto count=static_cast<int>(read(entity+offset,4));
        if(count<0 || count>16)return result;
        result.counts[group]=count;
        auto data=read(entity+offset+8,sizeof(uintptr_t));
        if(count && (!data || !readable(data,count*sizeof(uintptr_t))))return result;
        for(int i=0;i<count;++i)result.handles[group][i]=read(data+i*sizeof(uintptr_t),sizeof(uintptr_t));
    }
    result.valid=true;return result;
}
