#pragma once
#include <cstdint>
#include <cstring>
struct KnifeSubclassEntry {uint32_t key=0;const char* name=nullptr;bool melee=false;};
// Read candidates from the installed game's registry, then validate through its
// own lookup. No assumed hash algorithm or borrowed entry from another scope.
template<class ReadEntry,class Validate>
uint32_t ResolveKnifeSubclass(int capacity,const char* wanted,ReadEntry readEntry,Validate validate){
 if(capacity<=0 || capacity>4096 || !wanted || !*wanted)return 0;
 for(int i=0;i<capacity;++i){auto entry=readEntry(i);
  if(entry.key && entry.melee && entry.name && !strcmp(entry.name,wanted) && validate(entry.key,i))return entry.key;
 }
 return 0;
}
