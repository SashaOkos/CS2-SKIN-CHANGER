#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include "pattern.hpp"
#include "build_identity.hpp"
#include "menu_shared.hpp"
#pragma comment(lib,"bcrypt.lib")
static bool HasButterflyModel(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary); if(!file) return false;
    uint32_t header[3]{};file.read(reinterpret_cast<char*>(header),sizeof(header));
    if(!file || header[0]!=0x55AA1234 || (header[1]!=1 && header[1]!=2) || header[2]>64*1024*1024) return false;
    file.seekg(header[1]==2 ? 28 : 12);
    std::vector<char> tree(header[2]);file.read(tree.data(),tree.size());if(!file) return false;
    size_t pos=0;bool good=true;
    auto text=[&]() {
        size_t begin=pos;while(pos<tree.size() && tree[pos]) ++pos;
        if(pos>=tree.size()) {good=false;return std::string{};}
        std::string result(tree.data()+begin,pos-begin);++pos;return result;
    };
    for(auto ext=text();good && !ext.empty();ext=text()) {
        for(auto folder=text();good && !folder.empty();folder=text()) {
            for(auto name=text();good && !name.empty();name=text()) {
                if(pos+18>tree.size()) return false;
                uint16_t preload=0,terminator=0;memcpy(&preload,tree.data()+pos+4,2);memcpy(&terminator,tree.data()+pos+16,2);
                if(terminator!=0xffff || pos+18+preload>tree.size()) return false;
                pos+=18+preload;
                if(ext=="vmdl_c" && folder+"/"+name+".vmdl"==kButterflyModel) return true;
            }
        }
    }
    return false;
}
static std::wstring HashFile(const wchar_t* path) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) {CloseHandle(file);return {};}
    DWORD size=0,returned=0; BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&returned,0);
    std::vector<unsigned char> object(size); std::wstring result;
    if(BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0) {
        unsigned char buffer[65536]; DWORD read=0; bool good=true;
        while(true) { if(!ReadFile(file,buffer,sizeof(buffer),&read,nullptr)) {good=false;break;} if(!read) break;
            if(BCryptHashData(hash,buffer,read,0)<0) {good=false;break;} }
        unsigned char digest[32];
        if(good && BCryptFinishHash(hash,digest,sizeof(digest),0)>=0) {
            wchar_t hex[3];for(auto byte:digest) {swprintf_s(hex,L"%02X",byte);result+=hex;}
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg,0);CloseHandle(file);return result;
}
static std::wstring HashBytes(const void* data,DWORD size){
 BCRYPT_ALG_HANDLE alg=nullptr;if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
 unsigned char digest[32];auto result=BCryptHash(alg,nullptr,0,(PUCHAR)data,size,digest,sizeof(digest));BCryptCloseAlgorithmProvider(alg,0);
 if(result<0)return {};std::wstring text;wchar_t hex[3];for(auto byte:digest){swprintf_s(hex,L"%02X",byte);text+=hex;}return text;
}
static bool PrepareRuntime(const void* data,DWORD size,DWORD pid,std::wstring&dll,std::wstring&status){
 auto hash=HashBytes(data,size);if(hash.empty()){wprintf(L"Cannot hash embedded DLL.\n");return false;}
 wchar_t base[MAX_PATH];DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
 if(!length || length>=MAX_PATH){length=GetTempPathW(MAX_PATH,base);if(!length || length>=MAX_PATH)return false;}
 auto directory=std::filesystem::path(base)/L"CS2Collection"/L"Runtime"/hash/std::to_wstring(pid);
 std::error_code error;std::filesystem::create_directories(directory,error);
 if(error){wprintf(L"Cannot create runtime directory: %d\n",error.value());return false;}
 dll=(directory/L"butterfly-runtime.dll").wstring();status=(directory/L"butterfly-status.txt").wstring();
 if(HashFile(dll.c_str())==hash){wprintf(L"Verified cached DLL; no overwrite required.\n");return true;}
 HANDLE output=CreateFileW(dll.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(output==INVALID_HANDLE_VALUE){wprintf(L"Cannot create runtime DLL: %lu\n",GetLastError());return false;}
 DWORD written=0;bool ok=WriteFile(output,data,size,&written,nullptr) && written==size;if(ok)ok=FlushFileBuffers(output)!=FALSE;CloseHandle(output);
 if(!ok || HashFile(dll.c_str())!=hash){wprintf(L"Runtime DLL verification failed.\n");return false;}
 wprintf(L"Prepared verified DLL for CS2 PID %lu.\n",pid);return true;
}
static DWORD FindProcess() {
    DWORD result=0; HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(snapshot,&entry)) do {if(!_wcsicmp(entry.szExeFile,L"cs2.exe")) {result=entry.th32ProcessID;break;}}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}
struct Module {uintptr_t base;std::wstring path;};
static Module FindModule(DWORD pid,const wchar_t* name) {
    Module result{};HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snapshot==INVALID_HANDLE_VALUE) return result;
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Module32FirstW(snapshot,&entry)) do {if(!_wcsicmp(entry.szModule,name)) {result={reinterpret_cast<uintptr_t>(entry.modBaseAddr),entry.szExePath};break;}}while(Module32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}
static uintptr_t RemoteExport(DWORD pid, const char* name) {
    auto local=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),name);
    HMODULE owner=nullptr;
    if(!local || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(local),&owner))return 0;
    wchar_t path[MAX_PATH]{};
    if(!GetModuleFileNameW(owner,path,MAX_PATH))return 0;
    auto slash=wcsrchr(path,L'\\');auto remote=FindModule(pid,slash?slash+1:path);
    return remote.base?remote.base+reinterpret_cast<uintptr_t>(local)-reinterpret_cast<uintptr_t>(owner):0;
}
static void WindowsError(const wchar_t* operation,DWORD error) {
    wchar_t* message=nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,reinterpret_cast<wchar_t*>(&message),0,nullptr);
    wprintf(L"%ls failed: %lu (0x%08lX): %ls\n",operation,error,error,message?message:L"No system error description");
    if(message)LocalFree(message);
}
static bool RuntimeReady(DWORD pid) {
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,MenuMap);
    if(!mapping)return false;
    auto shared=static_cast<const SharedMenu*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(SharedMenu)));
    bool ready=shared && shared->magic==MenuMagic && shared->runtimePid==pid && shared->runtimeConnected && GetTickCount64()-shared->runtimeHeartbeat<2500;
    if(shared)UnmapViewOfFile(shared);CloseHandle(mapping);return ready;
}
// Execute LoadLibraryW and capture GetLastError in the same target thread.
// GetExitCodeThread is only 32 bits and cannot transport an x64 HMODULE.
struct RemoteLoadResult {uintptr_t path,load,lastError,module;DWORD error;DWORD reserved;};
static_assert(sizeof(RemoteLoadResult)==40,"x64 loader context required");
static int LoadModule(DWORD pid,HANDLE process,const std::wstring& dll) {
    const unsigned char code[]={
        0x53,                         // push rbx
        0x48,0x83,0xEC,0x20,         // sub rsp,32 (shadow space, aligned)
        0x48,0x89,0xCB,              // mov rbx,rcx
        0x48,0x8B,0x0B,              // mov rcx,[rbx] (DLL path)
        0xFF,0x53,0x08,              // call [rbx+8] (LoadLibraryW)
        0x48,0x89,0x43,0x18,         // mov [rbx+24],rax (full HMODULE)
        0xFF,0x53,0x10,              // call [rbx+16] (GetLastError)
        0x89,0x43,0x20,              // mov [rbx+32],eax
        0x31,0xC0,                   // xor eax,eax
        0x48,0x83,0xC4,0x20,         // add rsp,32
        0x5B,0xC3                    // pop rbx; ret
    };
    auto remoteLoad=RemoteExport(pid,"LoadLibraryW"),remoteError=RemoteExport(pid,"GetLastError");
    if(!remoteLoad || !remoteError){wprintf(L"Could not resolve target loader functions.\n");return 4;}
    SIZE_T bytes=sizeof(RemoteLoadResult)+(dll.size()+1)*sizeof(wchar_t);
    auto data=VirtualAllocEx(process,nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    auto routine=VirtualAllocEx(process,nullptr,sizeof(code),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    auto cleanup=[&](){if(data)VirtualFreeEx(process,data,0,MEM_RELEASE);if(routine)VirtualFreeEx(process,routine,0,MEM_RELEASE);};
    if(!data || !routine){auto error=GetLastError();cleanup();WindowsError(L"VirtualAllocEx",error);return 4;}
    RemoteLoadResult context{reinterpret_cast<uintptr_t>(data)+sizeof(RemoteLoadResult),remoteLoad,remoteError,0,0,0};
    SIZE_T written=0;
    if(!WriteProcessMemory(process,data,&context,sizeof(context),&written) || written!=sizeof(context) ||
       !WriteProcessMemory(process,reinterpret_cast<void*>(context.path),dll.c_str(),bytes-sizeof(context),&written) || written!=bytes-sizeof(context) ||
       !WriteProcessMemory(process,routine,code,sizeof(code),&written) || written!=sizeof(code)){
        auto error=GetLastError();cleanup();WindowsError(L"WriteProcessMemory",error);return 4;
    }
    DWORD protection=0;
    if(!VirtualProtectEx(process,routine,sizeof(code),PAGE_EXECUTE_READ,&protection) || !FlushInstructionCache(process,routine,sizeof(code))){
        auto error=GetLastError();cleanup();WindowsError(L"Prepare loader routine",error);return 4;
    }
    HANDLE thread=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(routine),data,0,nullptr);
    if(!thread){auto error=GetLastError();cleanup();WindowsError(L"CreateRemoteThread",error);return 4;}
    auto wait=WaitForSingleObject(thread,10000);
    DWORD exitCode=0;bool exitRead=GetExitCodeThread(thread,&exitCode)!=FALSE;CloseHandle(thread);
    if(wait!=WAIT_OBJECT_0){wprintf(L"Loader did not complete; memory retained until CS2 exits. Restart CS2 before retrying.\n");return 7;}
    SIZE_T read=0;bool resultRead=ReadProcessMemory(process,data,&context,sizeof(context),&read)!=FALSE && read==sizeof(context);
    auto error=GetLastError();cleanup();
    if(!resultRead){WindowsError(L"Read loader result",error);return 7;}
    if(!exitRead || exitCode){wprintf(L"Loader thread failed: 0x%08lX. Restart CS2 before retrying.\n",exitCode);return 7;}
    if(!context.module){
        WindowsError(L"Target LoadLibraryW",context.error);
        if(context.error==ERROR_MOD_NOT_FOUND)wprintf(L"Verify DLL dependencies and CS2 launch option -allow_third_party_software. Trusted Mode can reject third-party DLLs.\n");
        return 5;
    }
    wprintf(L"Target LoadLibraryW succeeded: module=0x%llX.\n",static_cast<unsigned long long>(context.module));
    return 0;
}
static bool CheckClient(const wchar_t* path) {
    if(HashFile(path)!=expectedHash) {wprintf(L"Unsupported client.dll build. No injection performed.\n");return false;}
    auto module=LoadLibraryExW(path,nullptr,DONT_RESOLVE_DLL_REFERENCES);
    if(!module) {wprintf(L"Could not map client.dll: %lu\n",GetLastError());return false;}
    bool valid=CompatibleClientProfile(module) && UniquePattern(module,kSetModelPattern) && UniquePattern(module,kEntityPattern) && UniquePattern(module,kControllerPattern);
    FreeLibrary(module);
    wprintf(valid ? L"PASS: SHA-256, native entry points and all three unique signatures match.\n" : L"FAIL: missing or ambiguous signature.\n");
    if(!valid) return false;
    auto vpk=std::filesystem::path(path).parent_path().parent_path().parent_path()/L"pak01_dir.vpk";
    if(!HasButterflyModel(vpk)) {wprintf(L"FAIL: butterfly model is absent from the installed VPK. No injection performed.\n");return false;}
    wprintf(L"PASS: actual butterfly model exists in the installed VPK.\n");return true;
}
DWORD ReadyGameProcess(){auto pid=FindProcess();return pid && FindModule(pid,L"client.dll").base?pid:0;}
static volatile LONG* loaderProgress=nullptr;
static void LoadStage(LONG value){if(loaderProgress)InterlockedExchange(loaderProgress,value);}
int wmain(int argc,wchar_t** argv) {
    _setmode(_fileno(stdout),_O_U8TEXT);
    for(int i=1;i+1<argc;i++)if(!wcscmp(argv[i],L"--progress")){auto mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,argv[i+1]);if(mapping){loaderProgress=(volatile LONG*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(LONG));CloseHandle(mapping);}}
    if(argc==1 || (argc>1 && !wcscmp(argv[1],L"--launcher-preview"))){FreeConsole();return RunStyleMenu();}
    if(argc>1 && !wcscmp(argv[1],L"--connection-test"))return RunStyleMenu();
    if(argc>1 && (!wcscmp(argv[1],L"--menu") || !wcscmp(argv[1],L"--preview"))) return RunStyleMenu();
    if(argc==3 && !wcscmp(argv[1],L"--check")) return CheckClient(argv[2]) ? 0 : 2;
    if(argc==3 && !wcscmp(argv[1],L"--check-model")) return HasButterflyModel(argv[2]) ? 0 : 2;
    bool connectOnly=argc>1 && (!wcscmp(argv[1],L"--connect") || !wcscmp(argv[1],L"--status"));
    struct MenuOnExit {bool show;~MenuOnExit(){if(show)RunStyleMenu();}} menuOnExit{!connectOnly};
    wprintf(L"CS2 Butterfly — experimental client model replacement\nF8: toggle. Restart CS2 to fully unload.\n");
    LoadStage(10);
    DWORD pid=FindProcess();for(int i=1;i+1<argc;i++)if(!wcscmp(argv[i],L"--pid"))pid=wcstoul(argv[i+1],nullptr,10);
    if(pid){HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W e{};e.dwSize=sizeof(e);bool found=false;if(snap!=INVALID_HANDLE_VALUE){if(Process32FirstW(snap,&e))do{if(e.th32ProcessID==pid && !_wcsicmp(e.szExeFile,L"cs2.exe"))found=true;}while(Process32NextW(snap,&e));CloseHandle(snap);}if(!found)pid=0;}
    if(!pid) {wprintf(L"CS2 is not running. Start CS2 and run this program again.\n");return 1;}
    if(argc>1 && !wcscmp(argv[1],L"--status")){
        bool module=FindModule(pid,L"butterfly-runtime.dll").base!=0;
        bool ready=module && RuntimeReady(pid);
        wprintf(L"PID=%lu module=%ls runtime=%ls\n",pid,module?L"loaded":L"absent",ready?L"ready":L"not-ready");
        return ready?0:1;
    }

    auto client=FindModule(pid,L"client.dll"); if(!client.base) {wprintf(L"Wait for the CS2 main menu and retry.\n");return 1;}
    LoadStage(25);
    if(!CheckClient(client.path.c_str())) return 2;
    if(FindModule(pid,L"butterfly-runtime.dll").base) {
        if(!RuntimeReady(pid)){wprintf(L"Module exists but is not ready. Restart CS2 before retrying.\n");return 7;}
        wprintf(L"Already loaded and ready.\n");LoadStage(100);return 0;
    }
    LoadStage(45);
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
    std::wstring folder=exe;folder.resize(folder.find_last_of(L"\\")+1);
    std::wstring dll,status;
    auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(101),MAKEINTRESOURCEW(10));
    if(!resource) {wprintf(L"Embedded module is missing.\n");return 3;}
    auto data=LockResource(LoadResource(nullptr,resource));auto size=SizeofResource(nullptr,resource);
    if(!data || size<sizeof(IMAGE_DOS_HEADER)){wprintf(L"Embedded DLL is empty or truncated.\n");return 3;}
    auto dos=static_cast<const IMAGE_DOS_HEADER*>(data);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || static_cast<size_t>(dos->e_lfanew)>size || size-static_cast<size_t>(dos->e_lfanew)<sizeof(IMAGE_NT_HEADERS64)){wprintf(L"Invalid DLL PE header.\n");return 3;}
    auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(static_cast<const unsigned char*>(data)+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !(nt->FileHeader.Characteristics&IMAGE_FILE_DLL)){wprintf(L"An x64 PE DLL is required.\n");return 3;}
    if(!PrepareRuntime(data,size,pid,dll,status))return 3;
    auto bridgeResource=FindResourceW(nullptr,MAKEINTRESOURCEW(102),MAKEINTRESOURCEW(10));
    if(!bridgeResource)return 3;
    auto bridgeData=LockResource(LoadResource(nullptr,bridgeResource));auto bridgeSize=SizeofResource(nullptr,bridgeResource);
    auto bridgePath=(std::filesystem::path(dll).parent_path()/L"collection-lifecycle.dll").wstring();
    if(!bridgeData || !bridgeSize)return 3;
    auto bridgeHash=HashBytes(bridgeData,bridgeSize);
    if(HashFile(bridgePath.c_str())!=bridgeHash){
        HANDLE out=CreateFileW(bridgePath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(out==INVALID_HANDLE_VALUE)return 3;DWORD written=0;bool ok=WriteFile(out,bridgeData,bridgeSize,&written,nullptr)&&written==bridgeSize;CloseHandle(out);
        if(!ok || HashFile(bridgePath.c_str())!=bridgeHash)return 3;
    }
    DeleteFileW(status.c_str());
    LoadStage(60);
    HANDLE process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!process) {wprintf(L"OpenProcess failed: %lu\n",GetLastError());return 4;}
    LoadStage(75);
    int loaded=LoadModule(pid,process,dll);
    CloseHandle(process);
    if(loaded)return loaded;
    if(!FindModule(pid,L"butterfly-runtime.dll").base) {wprintf(L"Loader returned a module but it is absent from the module list. Restart CS2 before retrying.\n");return 7;}
    LoadStage(90);
    std::wstring previousStatus;
    for(int i=0;i<100;++i) {
        if(RuntimeReady(pid)){wprintf(L"READY: module and runtime heartbeat confirmed for PID %lu.\n",pid);LoadStage(100);return 0;}
        FILE* f=nullptr;
        if(_wfopen_s(&f,status.c_str(),L"r, ccs=UTF-8")==0 && f) {
            wchar_t line[512]{};auto read=fgetws(line,512,f);fclose(f);
            if(read && line[0]) {if(previousStatus!=line){wprintf(L"%ls",line);previousStatus=line;}if(!wcsncmp(line,L"ERROR",5))return 7;}
        }
        Sleep(100);
    }
    wprintf(L"Module loaded but readiness was not confirmed. Restart CS2; see butterfly-status.txt.\n");return 7;
}
