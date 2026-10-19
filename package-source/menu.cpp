#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <wincodec.h>
#include <string>
#include <algorithm>
#include <cstdio>
#include <new>
#include <cstring>
#include <cctype>
#include <cmath>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include "catalog.hpp"
#include "menu_shared.hpp"
#include "menu_keys.hpp"
static MenuKeys menuKeys;
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
static ID3D11Device* dev;static ID3D11DeviceContext* ctx;static IDXGISwapChain* swap;static ID3D11RenderTargetView* rtv;
static bool launcherMode=false,launcherStarted=false;
static HWND window;static SharedMenu* state;static wchar_t iniPath[MAX_PATH];
static HWND gameMenuParent=nullptr;
static int menuWidth=1600,menuHeight=1000;
static bool ResizeMenuSurface(){
 int width=std::min(1600,GetSystemMetrics(SM_CXSCREEN)-40),height=std::min(1000,GetSystemMetrics(SM_CYSCREEN)-60);
 ctx->OMSetRenderTargets(0,nullptr,nullptr);rtv->Release();rtv=nullptr;
 HRESULT hr=swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0);
 ID3D11Texture2D* back=nullptr;
 if(SUCCEEDED(swap->GetBuffer(0,IID_PPV_ARGS(&back)))){dev->CreateRenderTargetView(back,nullptr,&rtv);back->Release();}
 if(FAILED(hr) || !rtv){PostQuitMessage(3);return false;}
 menuWidth=width;menuHeight=height;
 SetWindowPos(window,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
 return true;
}
static bool AttachGameMenu(){
 if(launcherMode || !state || !state->runtimePid)return false;
 if(gameMenuParent && IsWindow(gameMenuParent) && GetParent(window)==gameMenuParent)return true;
 struct Search {DWORD pid;HWND found;LONG area;} search{state->runtimePid,nullptr,0};
 EnumWindows([](HWND h,LPARAM value)->BOOL{
  auto&s=*reinterpret_cast<Search*>(value);DWORD pid=0;GetWindowThreadProcessId(h,&pid);RECT r{};
  if(pid==s.pid && IsWindowVisible(h) && !GetWindow(h,GW_OWNER) && GetClientRect(h,&r)){
   LONG area=(r.right-r.left)*(r.bottom-r.top);if(area>s.area){s.found=h;s.area=area;}
  }return TRUE;
 },reinterpret_cast<LPARAM>(&search));
 if(!search.found)return false;
 auto oldStyle=GetWindowLongPtrW(window,GWL_STYLE);auto oldEx=GetWindowLongPtrW(window,GWL_EXSTYLE);
 SetWindowLongPtrW(window,GWL_STYLE,(oldStyle&~WS_POPUP)|WS_CHILD);
 SetWindowLongPtrW(window,GWL_EXSTYLE,(oldEx&~(WS_EX_TOPMOST|WS_EX_APPWINDOW))|WS_EX_TOOLWINDOW);
 SetLastError(0);auto previous=SetParent(window,search.found);
 if(!previous && GetLastError()!=0){SetWindowLongPtrW(window,GWL_STYLE,oldStyle);SetWindowLongPtrW(window,GWL_EXSTYLE,oldEx);return false;}
 gameMenuParent=search.found;RECT r{};GetClientRect(gameMenuParent,&r);
 SetWindowPos(window,HWND_TOP,std::max(0L,(r.right-menuWidth)/2),std::max(0L,(r.bottom-menuHeight)/2),menuWidth,menuHeight,SWP_FRAMECHANGED|SWP_NOACTIVATE);
 return true;
}
static int category=4,itemIndex=0,skinIndex=-1,theme=0;static char filter[128]{};static bool visible=true;
static ImVec4 accent(0.55f,0.43f,1.f,1.f);static ImFont* normal;static ImFont* title;
static void MenuVisibility(bool open);
static constexpr UINT ShowExistingMenu=WM_APP+42;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
static LRESULT WINAPI Procedure(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_NCHITTEST){
  RECT rect{};GetWindowRect(h,&rect);int x=int(short(LOWORD(l)))-rect.left,y=int(short(HIWORD(l)))-rect.top;
  if(x>=0 && x<(launcherMode?390:800) && y>=0 && y<64)return HTCAPTION;
 }
 if(m==ShowExistingMenu){MenuVisibility(true);return 0;}
 if(ImGui_ImplWin32_WndProcHandler(h,m,w,l))return true;
 if(m==WM_CLOSE){if(launcherMode){PostQuitMessage(0);return 0;}visible=false;if(state)InterlockedExchange(&state->menuOpen,0);ShowWindow(h,SW_HIDE);return 0;}
 if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
 return DefWindowProcW(h,m,w,l);
}
extern DWORD ReadyGameProcess();

static HANDLE connectionProcess=nullptr;
static DWORD connectionExit=0;
static DWORD selectedProcess=0;
static DWORD connectionTarget=0;
static DWORD restartRequiredPid=0;
static DWORD launchError=0;
static bool manualDisconnect=false;
static bool CoreModulePresent(DWORD pid){
 HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
 if(snap==INVALID_HANDLE_VALUE)return true;
 MODULEENTRY32W e{};e.dwSize=sizeof(e);bool found=false;
 if(Module32FirstW(snap,&e))do{if(!_wcsicmp(e.szModule,L"butterfly-runtime.dll")){found=true;break;}}while(Module32NextW(snap,&e));
 CloseHandle(snap);return found;
}

static HANDLE progressMapping=nullptr;
static volatile LONG* progressValue=nullptr;
static std::wstring progressName;
static void RequestGameConnection(){
 if(connectionProcess)return;
 manualDisconnect=false;
 connectionTarget=selectedProcess?selectedProcess:ReadyGameProcess();
 if(connectionTarget && connectionTarget==restartRequiredPid){connectionExit=7;return;}
 launchError=0;
 wchar_t executable[MAX_PATH];GetModuleFileNameW(nullptr,executable,MAX_PATH);
 std::wstring command=L"\"";command+=executable;command+=L"\" --connect --pid ";command+=std::to_wstring(connectionTarget);command+=L" --progress ";command+=progressName;if(progressValue)InterlockedExchange(progressValue,0);
 STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
 std::wstring logPath=executable;logPath.resize(logPath.find_last_of(L"\\")+1);logPath+=L"connection.log";
 HANDLE log=CreateFileW(logPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
 bool redirect=log!=INVALID_HANDLE_VALUE && input!=INVALID_HANDLE_VALUE;
 if(redirect){startup.dwFlags|=STARTF_USESTDHANDLES;startup.hStdOutput=log;startup.hStdError=log;startup.hStdInput=input;}
 PROCESS_INFORMATION child{};
 if(CreateProcessW(executable,command.data(),nullptr,nullptr,redirect,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)){connectionProcess=child.hProcess;CloseHandle(child.hThread);connectionExit=0;}else {launchError=GetLastError();connectionExit=4;}
 if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
}
static void PollGameConnection(bool preview){
 if(preview)return;
 if(manualDisconnect)return;
 if(connectionProcess){DWORD result=0;if(GetExitCodeProcess(connectionProcess,&result) && result!=STILL_ACTIVE){connectionExit=result;if(result==7)restartRequiredPid=connectionTarget;CloseHandle(connectionProcess);connectionProcess=nullptr;}}
 if(launcherMode && launcherStarted && !connectionProcess && !connectionExit && progressValue && *progressValue==100 && state->runtimePid==connectionTarget && state->runtimeConnected && GetTickCount64()-state->runtimeHeartbeat<2500){
  launcherMode=false;MenuVisibility(false);ResizeMenuSurface();return;
 }
 static ULONGLONG nextPoll=0,nextRetry=0;static DWORD attemptedPid=0;static unsigned attempts=0;
 auto now=GetTickCount64();if(now<nextPoll)return;nextPoll=now+2000;
 auto pid=ReadyGameProcess();if(!pid){attemptedPid=0;attempts=0;nextRetry=0;return;} if(pid!=attemptedPid){attemptedPid=pid;attempts=0;nextRetry=0;connectionExit=0;}
 bool connected=state->runtimePid==pid && state->runtimeConnected && now-state->runtimeHeartbeat<2500;
 if(!launcherMode && !connected && !connectionProcess && attempts<3 && now>=nextRetry && (connectionExit==0 || connectionExit==1)){selectedProcess=pid;++attempts;nextRetry=now+5000;RequestGameConnection();}
}
static ULONGLONG saveDue=0;
static bool saveFailed=false;
static void Save(){saveDue=GetTickCount64()+400;saveFailed=false;}
static void FlushSave(bool force=false){
 if(!saveDue || (!force && GetTickCount64()<saveDue))return;
 std::wstring text=L"[Menu]\r\n";wchar_t value[256];
 swprintf_s(value,L"Knife=%d\r\nGloves=%d\r\nTheme=%d\r\n",state->knife,state->glove,theme);text+=value;
 auto glow=NormalizeGlow(state->glow);
 swprintf_s(value,L"[Glow]\r\nEnabled=%d\r\nTarget=%d\r\nRed=%.6f\r\nGreen=%.6f\r\nBlue=%.6f\r\nIntensity=%.6f\r\n",glow.enabled,glow.target,double(glow.color[0]),double(glow.color[1]),double(glow.color[2]),double(glow.intensity));text+=value;
 for(auto&w:catalog){auto&s=state->choices[w.id];if(!s.active && !s.paint && !s.seed && s.wear==.01f)continue;swprintf_s(value,L"[Item%d]\r\nPaint=%d\r\nSeed=%d\r\nWear=%.8f\r\nActive=%d\r\n",w.id,s.paint,s.seed,double(s.wear),s.active);text+=value;}
 LONG equipmentRevision=state->equipmentRevision;if(equipmentRevision&1){saveDue=GetTickCount64()+100;return;}
 for(int team=2;team<=3;team++)for(int slot=0;slot<LoadoutSlots;slot++){auto entry=state->loadout[team][slot];if(!entry.definition)continue;
  swprintf_s(value,L"[Equipment%d_%d]\r\nItemID=%llu\r\nDefinition=%d\r\nCustom=%d\r\nPaint=%d\r\nSeed=%d\r\nWear=%.8f\r\n",team,slot,(unsigned long long)entry.itemId,entry.definition,entry.custom,entry.choice.paint,entry.choice.seed,double(entry.choice.wear));text+=value;
 }
 if(equipmentRevision!=state->equipmentRevision){saveDue=GetTickCount64()+100;return;}
 std::wstring temporary=iniPath;temporary+=L".tmp";
 HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 bool ok=false;
 if(file!=INVALID_HANDLE_VALUE){DWORD written=0;WORD bom=0xfeff;ok=WriteFile(file,&bom,sizeof(bom),&written,nullptr) && written==sizeof(bom);
  DWORD bytes=DWORD(text.size()*sizeof(wchar_t));if(ok)ok=WriteFile(file,text.data(),bytes,&written,nullptr) && written==bytes;
  if(ok)ok=FlushFileBuffers(file)!=FALSE;CloseHandle(file);
 }
 if(ok && GetFileAttributesW(iniPath)!=INVALID_FILE_ATTRIBUTES){std::wstring backup=iniPath;backup+=L".bak";ok=CopyFileW(iniPath,backup.c_str(),FALSE)!=FALSE;}
 if(ok)ok=MoveFileExW(temporary.c_str(),iniPath,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
 saveFailed=!ok;saveDue=ok?0:GetTickCount64()+2000;
}
static void ResetSelections(){
 state->resetAll=1;memset(state->resetDefinitions,0,sizeof(state->resetDefinitions));
 InterlockedIncrement(&state->revision);
 InterlockedIncrement(&state->equipmentRevision);
 for(auto& choice:state->choices)choice=SkinChoice{};
 for(auto& team:state->loadout)for(auto& entry:team)entry=EquippedItem{};
 for(auto& choice:state->equippedChoices)choice=SkinChoice{};
 state->resetResult=0;state->resetRemoved=0;InterlockedIncrement(&state->resetSerial);InterlockedIncrement(&state->equipmentRevision);
 state->knife=515;state->glove=0;state->inventoryDefinition=0;state->inventoryResult=0;state->inventoryAddedId=0;
 InterlockedIncrement(&state->revision);Save();FlushSave(true);
}
static void ResetSelectedDefinitions(const bool* selected){
 InterlockedIncrement(&state->revision);InterlockedIncrement(&state->equipmentRevision);
 state->resetAll=0;
 for(int id=0;id<8192;id++){state->resetDefinitions[id]=selected[id]?1:0;if(selected[id]){state->choices[id]=SkinChoice{};state->equippedChoices[id]=SkinChoice{};}}
 for(auto& team:state->loadout)for(auto& entry:team)if(entry.definition>0 && entry.definition<8192 && selected[entry.definition])entry=EquippedItem{};
 if(state->knife>0 && state->knife<8192 && selected[state->knife])state->knife=0;
 if(state->glove>0 && state->glove<8192 && selected[state->glove])state->glove=0;
 if(state->inventoryDefinition>0 && selected[state->inventoryDefinition]){state->inventoryDefinition=0;state->inventoryResult=0;state->inventoryAddedId=0;}
 state->resetResult=0;state->resetRemoved=0;InterlockedIncrement(&state->resetSerial);
 InterlockedIncrement(&state->equipmentRevision);InterlockedIncrement(&state->revision);Save();FlushSave(true);
}
static void Load(){
 InterlockedIncrement(&state->revision);
 GlowSettings glow;glow.enabled=GetPrivateProfileIntW(L"Glow",L"Enabled",0,iniPath);glow.target=GetPrivateProfileIntW(L"Glow",L"Target",2,iniPath);
 const wchar_t* glowKeys[]={L"Red",L"Green",L"Blue",L"Intensity"};
 for(int i=0;i<4;i++){wchar_t v[64];GetPrivateProfileStringW(L"Glow",glowKeys[i],i==3?L"0.7":L"1",v,64,iniPath);if(i<3)glow.color[i]=float(_wtof(v));else glow.intensity=float(_wtof(v));}
 InterlockedIncrement(&state->glowRevision);state->glow=NormalizeGlow(glow);InterlockedIncrement(&state->glowRevision);
 state->knife=GetPrivateProfileIntW(L"Menu",L"Knife",515,iniPath);state->glove=GetPrivateProfileIntW(L"Menu",L"Gloves",0,iniPath);theme=GetPrivateProfileIntW(L"Menu",L"Theme",0,iniPath);
 for(auto&w:catalog){wchar_t section[32],value[64];swprintf_s(section,L"Item%d",w.id);auto&s=state->choices[w.id];s.paint=GetPrivateProfileIntW(section,L"Paint",0,iniPath);s.seed=GetPrivateProfileIntW(section,L"Seed",0,iniPath);s.active=GetPrivateProfileIntW(section,L"Active",0,iniPath);GetPrivateProfileStringW(section,L"Wear",L"0.01",value,64,iniPath);s.wear=(float)_wtof(value);if(!std::isfinite(s.wear) || s.wear<0 || s.wear>1)s.wear=.01f;s.seed=std::clamp(s.seed,0,1000);s.active=s.active?1:0;bool validPaint=s.paint==0;for(auto&skin:w.skins)if(skin.id==s.paint)validPaint=true;if(!validPaint){s.paint=0;s.active=0;}}
 for(int team=2;team<=3;team++)for(int slot=0;slot<LoadoutSlots;slot++){
  wchar_t section[64],value[128];swprintf_s(section,L"Equipment%d_%d",team,slot);auto& entry=state->loadout[team][slot];entry=EquippedItem{};
  entry.definition=GetPrivateProfileIntW(section,L"Definition",0,iniPath);if(entry.definition<=0 || entry.definition>=8192){entry=EquippedItem{};continue;}
  bool known=false;for(auto&w:catalog)if(w.id==entry.definition){known=true;break;}if(!known){entry=EquippedItem{};continue;}
  GetPrivateProfileStringW(section,L"ItemID",L"0",value,128,iniPath);entry.itemId=_wcstoui64(value,nullptr,10);entry.custom=GetPrivateProfileIntW(section,L"Custom",0,iniPath)?1:0;
  entry.choice.paint=GetPrivateProfileIntW(section,L"Paint",0,iniPath);entry.choice.seed=std::clamp(int(GetPrivateProfileIntW(section,L"Seed",0,iniPath)),0,1000);
  GetPrivateProfileStringW(section,L"Wear",L"0.01",value,128,iniPath);entry.choice.wear=float(_wtof(value));if(!std::isfinite(entry.choice.wear) || entry.choice.wear<0 || entry.choice.wear>1)entry.choice.wear=.01f;
  bool valid=entry.choice.paint==0;for(auto&w:catalog)if(w.id==entry.definition)for(auto&skin:w.skins)if(skin.id==entry.choice.paint)valid=true;
  if(entry.custom&&!valid){entry=EquippedItem{};continue;}entry.choice.active=entry.custom;
 }
 theme=std::clamp(theme,0,2);bool knifeValid=false,gloveValid=state->glove==0;
 for(auto&w:catalog){if(w.category==4 && w.id==state->knife)knifeValid=true;if(w.category==5 && w.id==state->glove)gloveValid=true;}
 if(!knifeValid)state->knife=515;if(!gloveValid)state->glove=0;
 InterlockedIncrement(&state->revision);
}
static ImU32 Color(ImVec4 v){return ImGui::ColorConvertFloat4ToU32(v);}
static bool Match(const char* s,const char* q){
 auto lower=[](const char* text){int length=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);std::wstring result(length?length:1,L'\0');if(length)MultiByteToWideChar(CP_UTF8,0,text,-1,result.data(),length);if(!result.empty())result.pop_back();if(!result.empty())CharLowerBuffW(result.data(),DWORD(result.size()));return result;};
 return lower(s).find(lower(q))!=std::wstring::npos;
}

static void Style(){
 accent=ImVec4(.94f,.94f,.94f,1);
 ImGui::StyleColorsDark();auto&s=ImGui::GetStyle();
 s.WindowRounding=0;s.ChildRounding=6;s.FrameRounding=4;s.PopupRounding=6;s.ScrollbarRounding=4;s.ScrollbarSize=7;s.WindowPadding=ImVec2(20,18);s.FramePadding=ImVec2(12,8);s.ItemSpacing=ImVec2(10,8);s.WindowBorderSize=0;s.ChildBorderSize=1;
 auto*c=s.Colors;for(int i=0;i<ImGuiCol_COUNT;++i){float gray=(c[i].x+c[i].y+c[i].z)/3;c[i]=ImVec4(gray,gray,gray,c[i].w);}
 c[ImGuiCol_WindowBg]=ImVec4(.035f,.035f,.035f,1);c[ImGuiCol_ChildBg]=ImVec4(.065f,.065f,.065f,1);c[ImGuiCol_PopupBg]=ImVec4(.09f,.09f,.09f,1);
 c[ImGuiCol_Border]=ImVec4(.20f,.20f,.20f,1);c[ImGuiCol_Separator]=c[ImGuiCol_Border];c[ImGuiCol_Text]=accent;c[ImGuiCol_TextDisabled]=ImVec4(.58f,.58f,.58f,1);
 c[ImGuiCol_FrameBg]=ImVec4(.12f,.12f,.12f,1);c[ImGuiCol_FrameBgHovered]=ImVec4(.19f,.19f,.19f,1);c[ImGuiCol_FrameBgActive]=ImVec4(.24f,.24f,.24f,1);
 c[ImGuiCol_Button]=ImVec4(.15f,.15f,.15f,1);c[ImGuiCol_ButtonHovered]=ImVec4(.25f,.25f,.25f,1);c[ImGuiCol_ButtonActive]=ImVec4(.32f,.32f,.32f,1);
 c[ImGuiCol_Header]=ImVec4(.24f,.24f,.24f,1);c[ImGuiCol_HeaderHovered]=ImVec4(.29f,.29f,.29f,1);c[ImGuiCol_HeaderActive]=ImVec4(.34f,.34f,.34f,1);
 c[ImGuiCol_SliderGrab]=ImVec4(.75f,.75f,.75f,1);c[ImGuiCol_SliderGrabActive]=accent;c[ImGuiCol_CheckMark]=accent;c[ImGuiCol_PlotHistogram]=accent;
}
#include "mono_motion.hpp"
#include "modern_menu.inc"
#include "launcher_menu.inc"
static bool Capture(const wchar_t* path){
 ID3D11Texture2D* back=nullptr;swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back);if(!back)return false;
 D3D11_TEXTURE2D_DESC d{};back->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;ID3D11Texture2D* read=nullptr;dev->CreateTexture2D(&d,nullptr,&read);ctx->CopyResource(read,back);back->Release();D3D11_MAPPED_SUBRESOURCE m{};
 if(FAILED(ctx->Map(read,0,D3D11_MAP_READ,0,&m))){read->Release();return false;}
 IWICImagingFactory*f=nullptr;IWICStream*stream=nullptr;IWICBitmapEncoder*encoder=nullptr;IWICBitmapFrameEncode*frame=nullptr;
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f));
 if(SUCCEEDED(hr))hr=f->CreateStream(&stream);if(SUCCEEDED(hr))hr=stream->InitializeFromFilename(path,GENERIC_WRITE);if(SUCCEEDED(hr))hr=f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);if(SUCCEEDED(hr))hr=encoder->Initialize(stream,WICBitmapEncoderNoCache);if(SUCCEEDED(hr))hr=encoder->CreateNewFrame(&frame,nullptr);if(SUCCEEDED(hr))hr=frame->Initialize(nullptr);if(SUCCEEDED(hr))hr=frame->SetSize(d.Width,d.Height);WICPixelFormatGUID fmt=GUID_WICPixelFormat32bppBGRA;if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&fmt);if(SUCCEEDED(hr))hr=frame->WritePixels(d.Height,m.RowPitch,m.RowPitch*d.Height,(BYTE*)m.pData);if(SUCCEEDED(hr))hr=frame->Commit();if(SUCCEEDED(hr))hr=encoder->Commit();if(frame)frame->Release();if(encoder)encoder->Release();if(stream)stream->Release();if(f)f->Release();ctx->Unmap(read,0);read->Release();CoUninitialize();return SUCCEEDED(hr);
}
int RunStyleMenu(){
 bool connectionTest=wcsstr(GetCommandLineW(),L"--connection-test")!=nullptr;
 int testExit=0;ULONGLONG testDeadline=GetTickCount64()+30000;
 bool launcherPreview=wcsstr(GetCommandLineW(),L"--launcher-preview")!=nullptr;
 bool preview=wcsstr(GetCommandLineW(),L"--preview")!=nullptr || launcherPreview;
 launcherMode=launcherPreview || (wcsstr(GetCommandLineW(),L"--menu")==nullptr && !preview);
 progressName=L"Local\\CS2.Collection.Progress."+std::to_wstring(GetCurrentProcessId());
 progressMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(LONG),progressName.c_str());
 if(progressMapping)progressValue=(volatile LONG*)MapViewOfFile(progressMapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(LONG));
 HANDLE mutex=CreateMutexW(nullptr,FALSE,preview?L"Local\\CS2Collection.Preview.v12":L"Local\\CS2Collection.Final.v12");if(GetLastError()==ERROR_ALREADY_EXISTS){
  HWND existing=nullptr;for(int attempt=0;attempt<20 && !existing;attempt++){existing=FindWindowW(L"CS2StyleStudio",nullptr);if(!existing)Sleep(50);}
  if(existing){DWORD owner=0;GetWindowThreadProcessId(existing,&owner);AllowSetForegroundWindow(owner);PostMessageW(existing,ShowExistingMenu,0,0);}
  else MessageBoxW(nullptr,L"Программа уже запущена, но её окно недоступно. Закройте старую копию и откройте эту снова.",L"Коллекция CS2",MB_OK|MB_ICONINFORMATION);
  CloseHandle(mutex);return 0;
 }
 std::wstring mapName=MenuMap;if(preview)mapName+=L".Preview";
 HANDLE map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedMenu),mapName.c_str());bool fresh=GetLastError()!=ERROR_ALREADY_EXISTS;state=(SharedMenu*)MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedMenu));if(!state)return 2;if(fresh || state->magic!=MenuMagic){new(state)SharedMenu;}
 GetModuleFileNameW(nullptr,iniPath,MAX_PATH);auto sep=wcsrchr(iniPath,L'\\');if(sep)wcscpy_s(sep+1,MAX_PATH-(sep+1-iniPath),L"loadout.ini");Load();
 WNDCLASSEXW cls={sizeof(cls),CS_CLASSDC,Procedure,0,0,GetModuleHandleW(nullptr),nullptr,LoadCursor(nullptr,IDC_ARROW),nullptr,nullptr,L"CS2StyleStudio",nullptr};RegisterClassExW(&cls);
 menuWidth=launcherMode?520:std::min(1600,GetSystemMetrics(SM_CXSCREEN)-40);menuHeight=launcherMode?420:std::min(1000,GetSystemMetrics(SM_CYSCREEN)-60);
 int x=(GetSystemMetrics(SM_CXSCREEN)-menuWidth)/2,y=(GetSystemMetrics(SM_CYSCREEN)-menuHeight)/2;
 window=CreateWindowExW(WS_EX_TOPMOST,cls.lpszClassName,L"Коллекция CS2 v12",WS_POPUP,x,y,menuWidth,menuHeight,nullptr,nullptr,cls.hInstance,nullptr);
 DXGI_SWAP_CHAIN_DESC d{};d.BufferCount=2;d.BufferDesc.Width=menuWidth;d.BufferDesc.Height=menuHeight;d.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.OutputWindow=window;d.SampleDesc.Count=1;d.Windowed=TRUE;d.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
 if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&dev,nullptr,&ctx)))return 3;
 ID3D11Texture2D* back=nullptr;swap->GetBuffer(0,IID_PPV_ARGS(&back));dev->CreateRenderTargetView(back,nullptr,&rtv);back->Release();
 ImGui::CreateContext();auto&io=ImGui::GetIO();io.IniFilename=nullptr;io.MouseDrawCursor=true;normal=io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf",17,nullptr,io.Fonts->GetGlyphRangesCyrillic());title=io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf",28,nullptr,io.Fonts->GetGlyphRangesCyrillic());if(!normal)normal=io.Fonts->AddFontDefault();if(!title)title=normal;
 ImGui_ImplWin32_Init(window);ImGui_ImplDX11_Init(dev,ctx);
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imageFactory));for(size_t i=0;i<catalog.size();i++)if(catalog[i].id==state->knife){itemIndex=(int)i;break;}if(preview && wcsstr(GetCommandLineW(),L"--wear-demo"))for(size_t i=0;i<catalog.size();i++)if(catalog[i].id==7) {
 itemIndex=int(i);category=1;for(auto& skin:catalog[i].skins)if(skin.max>=.7f){state->choices[7].paint=skin.id;state->choices[7].wear=wcsstr(GetCommandLineW(),L"--high")?.7f:skin.min;break;}break;
 }
 if(preview && wcsstr(GetCommandLineW(),L"--reset-demo")){category=6;state->choices[508]={577,0,.01f,1};state->choices[9]={344,0,.01f,1};selectedForReset[508]=true;}
 if(preview && wcsstr(GetCommandLineW(),L"--settings-demo"))category=7;
 if(!preview)MenuVisibility(true);else ShowWindow(window,SW_SHOW);MSG message{};bool running=true;int frames=0;bool wasInsert=(GetAsyncKeyState(VK_INSERT)&0x8000)!=0;bool wasEscape=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0;
 if(connectionTest){selectedProcess=ReadyGameProcess();launcherStarted=true;RequestGameConnection();}
 LONG lastEquipmentRevision=state->equipmentRevision;
 while(running){if(!preview){LONG currentEquipmentRevision=state->equipmentRevision;if(!(currentEquipmentRevision&1) && currentEquipmentRevision!=lastEquipmentRevision){lastEquipmentRevision=currentEquipmentRevision;Save();}FlushSave();}PollGameConnection(preview);state->menuHeartbeat=GetTickCount64();while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);if(message.message==WM_QUIT)running=false;}
 if(connectionTest){
  if(!connectionProcess && connectionExit){testExit=int(connectionExit);running=false;}
  else if(!launcherMode && progressValue && *progressValue==100 && state->runtimePid==connectionTarget && state->runtimeConnected){running=false;}
  else if(GetTickCount64()>=testDeadline){testExit=7;running=false;}
  if(!running){wprintf(L"Connection test: PID=%lu stage=%ld result=%d\n",connectionTarget,progressValue?*progressValue:0,testExit);break;}
 }
 if(!preview && !launcherMode)AttachGameMenu();
 bool gameFocused=launcherMode || GetForegroundWindow()==window || (gameMenuParent && GetAncestor(GetForegroundWindow(),GA_ROOT)==gameMenuParent);
 int keyAction=menuKeys.Poll((GetAsyncKeyState(VK_INSERT)&0x8000)!=0,(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0,gameFocused,visible);
 if(!preview && keyAction==1)MenuVisibility(!visible);
 else if(!preview && keyAction==2)MenuVisibility(false);
 if(!visible){Sleep(25);continue;}
 ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();if(launcherMode)DrawLauncher(preview);else Draw();ImGui::Render();const float clear[4]={.035f,.035f,.035f,1};ctx->OMSetRenderTargets(1,&rtv,nullptr);ctx->ClearRenderTargetView(rtv,clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());TrimTextureCache();
 if(connectionTest && progressValue && *progressValue==100 && launcherMode)Capture(L"connection-test.png");
 if(preview&&++frames==45){Capture(launcherPreview?L"launcher-preview.png":wcsstr(GetCommandLineW(),L"--high")?L"menu-preview-high.png":L"menu-preview.png");running=false;}else swap->Present(1,0);
 }
 InterlockedExchange(&state->menuOpen,0);if(connectionProcess)CloseHandle(connectionProcess);if(!preview){Save();FlushSave(true);}for(auto&entry:textureCache)entry.second.view->Release();textureCache.clear();if(wearPreview)wearPreview->Release();if(imageFactory)imageFactory->Release();CoUninitialize();ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();rtv->Release();swap->Release();ctx->Release();dev->Release();DestroyWindow(window);UnmapViewOfFile(state);CloseHandle(map);CloseHandle(mutex);if(progressValue)UnmapViewOfFile((void*)progressValue);if(progressMapping)CloseHandle(progressMapping);return testExit;
}


