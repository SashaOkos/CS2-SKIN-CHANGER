#pragma once
#include <cstdint>
#include <cstring>
// Validated descriptor layout for the supported client. The engine consumes
// stack descriptors synchronously and retains its own resource description.
template<class Submit>
uintptr_t SubmitSelectedRecipe(void* component,void* descriptor,const char* selected,Submit submit){
    if(!selected || !*selected)return submit(component,descriptor);
    auto bytes=static_cast<unsigned char*>(descriptor);
    const char* previous=nullptr;memcpy(&previous,bytes+0x18,sizeof(previous));
    if(previous && !strcmp(previous,selected))return submit(component,descriptor);
    alignas(16) unsigned char copy[0xA0];memcpy(copy,descriptor,sizeof(copy));
    memcpy(copy+0x18,&selected,sizeof(selected));
    auto result=submit(component,copy);
    // Preserve native output/context fields without transferring ownership of
    // the schema's persistent path to the caller's temporary CUtlString.
    memcpy(copy+0x18,&previous,sizeof(previous));memcpy(descriptor,copy,sizeof(copy));
    return result;
}
