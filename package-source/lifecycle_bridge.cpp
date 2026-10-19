#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <utility>
#include <type_traits>
#include <cstring>
#include <string>
#include <set>
#include <mutex>
// Remains loaded: an already-fetched virtual function pointer must stay valid.
struct Record { void* target=nullptr; void* fallback=nullptr; void* owner=nullptr; LONG active=0; };
static SRWLOCK lock=SRWLOCK_INIT;
static Record records[12][64];
static unsigned used[12]{};
struct OwnerState {void* owner;bool retired;};
static OwnerState owners[64]{};static unsigned ownerCount=0;
extern "C" __declspec(dllexport) BOOL BeginOwner(void* owner){
 AcquireSRWLockExclusive(&lock);
 for(unsigned i=0;i<ownerCount;i++)if(owners[i].owner==owner){owners[i].retired=false;ReleaseSRWLockExclusive(&lock);return TRUE;}
 if(ownerCount==64){ReleaseSRWLockExclusive(&lock);return FALSE;}
 owners[ownerCount++]={owner,false};ReleaseSRWLockExclusive(&lock);return TRUE;
}
static const char* KeepName(const char* text){
 if(!text)return nullptr;
 static std::mutex mutex;static std::set<std::string> names;
 std::lock_guard<std::mutex> guard(mutex);return names.emplace(text).first->c_str();
}
template<int Kind,class R,class... A> struct Gates {
 template<size_t N> static R Entry(A... args){
  auto& r=records[Kind][N];void* fn;bool tracked;
  AcquireSRWLockExclusive(&lock);tracked=r.target!=nullptr;fn=tracked?r.target:r.fallback;
  if(tracked)++r.active;ReleaseSRWLockExclusive(&lock);
  __try {
   if constexpr(std::is_void_v<R>){if(fn)reinterpret_cast<R(*)(A...)>(fn)(args...);return;}
   else if constexpr(std::is_same_v<R,const char*>){return fn?KeepName(reinterpret_cast<R(*)(A...)>(fn)(args...)):nullptr;}
   else {return fn?reinterpret_cast<R(*)(A...)>(fn)(args...):R{};}
  } __finally {if(tracked){AcquireSRWLockExclusive(&lock);--r.active;ReleaseSRWLockExclusive(&lock);}}
 }
 template<size_t... N> static void* At(size_t index,std::index_sequence<N...>){static void* entries[]={reinterpret_cast<void*>(&Entry<N>)...};return entries[index];}
 static void* At(size_t n){return At(n,std::make_index_sequence<64>{});}
};
extern "C" __declspec(dllexport) void* RegisterGate(int kind,void* target,void* fallback){
 if(kind<0||kind>=12||!target)return nullptr;
 MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(target,&info,sizeof(info)))return nullptr;
 AcquireSRWLockExclusive(&lock);
 bool permitted=false;for(unsigned i=0;i<ownerCount;i++)if(owners[i].owner==info.AllocationBase && !owners[i].retired)permitted=true;
 if(!permitted){ReleaseSRWLockExclusive(&lock);return nullptr;}
 unsigned n=used[kind];
 if(n==64){ReleaseSRWLockExclusive(&lock);return nullptr;}
 ++used[kind];records[kind][n]={target,fallback,info.AllocationBase,0};ReleaseSRWLockExclusive(&lock);
 switch(kind){
 case 0:return Gates<0,void,void*,const char*>::At(n);
 case 1:return Gates<1,void,void*,int>::At(n);
 case 2:return Gates<2,void,void*,void*,bool,bool,void*>::At(n);
 case 3:return Gates<3,const char*,void*>::At(n);
 case 4:return Gates<4,uintptr_t,void*,void*>::At(n);
 case 5:return Gates<5,void,void*,void*,bool>::At(n);
 case 6:return Gates<6,int,void*,uint32_t,void*,void*>::At(n);
 case 7:return Gates<7,bool,void*>::At(n);
 case 8:return Gates<8,void,void*,const uintptr_t*>::At(n);
 case 9:return Gates<9,void,void*,void*,uintptr_t*>::At(n);
 case 10:return Gates<10,int,void*>::At(n);
 case 11:return Gates<11,float,void*>::At(n);
 }return nullptr;
}
extern "C" __declspec(dllexport) void SetFallback(void* target,void* fallback){
 AcquireSRWLockExclusive(&lock);for(int k=0;k<12;k++)for(unsigned i=0;i<used[k];i++)if(records[k][i].target==target)records[k][i].fallback=fallback;ReleaseSRWLockExclusive(&lock);
}
extern "C" __declspec(dllexport) BOOL RetireOwner(void* owner,DWORD timeout){
 AcquireSRWLockExclusive(&lock);for(unsigned i=0;i<ownerCount;i++)if(owners[i].owner==owner)owners[i].retired=true;for(int k=0;k<12;k++)for(unsigned i=0;i<used[k];i++)if(records[k][i].owner==owner)records[k][i].target=nullptr;ReleaseSRWLockExclusive(&lock);
 auto deadline=GetTickCount64()+timeout;
 do {LONG active=0;AcquireSRWLockShared(&lock);for(int k=0;k<12;k++)for(unsigned i=0;i<used[k];i++)if(records[k][i].owner==owner)active+=records[k][i].active;ReleaseSRWLockShared(&lock);if(!active)return TRUE;Sleep(1);}while(GetTickCount64()<deadline);
 return FALSE;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(h);return TRUE;}
