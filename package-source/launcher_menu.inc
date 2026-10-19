// Launcher uses the same renderer and the existing loader.
#include <tlhelp32.h>
static std::vector<DWORD> LauncherProcesses(){
 std::vector<DWORD> list;HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W e{};e.dwSize=sizeof(e);
 if(snap!=INVALID_HANDLE_VALUE){if(Process32FirstW(snap,&e))do{if(!_wcsicmp(e.szExeFile,L"cs2.exe"))list.push_back(e.th32ProcessID);}while(Process32NextW(snap,&e));CloseHandle(snap);}return list;
}
static void DrawLauncher(bool preview){
 Style();openProgress=std::min(1.f,openProgress+ImGui::GetIO().DeltaTime*5.f);ImGui::GetStyle().Alpha=.45f+.55f*openProgress;ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(float(menuWidth),float(menuHeight)));
 ImGui::Begin("##launcher",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
 ImGui::SetCursorPos(ImVec2(24,22));ImGui::PushFont(title);ImGui::TextUnformatted("COLLECTION");ImGui::PopFont();
 ImGui::SetCursorPos(ImVec2(404,22));if(MonoButton("_",ImVec2(42,32)))MenuVisibility(false);
 if(ImGui::IsItemHovered())ImGui::SetTooltip(u8"INSERT: показать окно");
 ImGui::SetCursorPos(ImVec2(454,22));if(MonoButton("X",ImVec2(42,32)))PostMessageW(window,WM_CLOSE,0,0);
 ImGui::SetCursorPos(ImVec2(24,78));ImGui::BeginChild("##connect-card",ImVec2(472,282),false);
 static std::vector<DWORD> processes;static ULONGLONG nextScan=0;
 if(GetTickCount64()>nextScan && !connectionProcess){processes=LauncherProcesses();nextScan=GetTickCount64()+2000;if(std::find(processes.begin(),processes.end(),selectedProcess)==processes.end())selectedProcess=processes.empty()?0:processes.front();}
 ImGui::TextUnformatted(u8"Процесс игры");std::string label=selectedProcess?"cs2.exe   /   PID "+std::to_string(selectedProcess):u8"CS2 не запущена";
 ImGui::BeginDisabled(connectionProcess!=nullptr);ImGui::SetNextItemWidth(-1);
 if(ImGui::BeginCombo("##process",label.c_str())){for(auto pid:processes){auto name="cs2.exe   /   PID "+std::to_string(pid);if(MonoSelectable(name.c_str(),pid==selectedProcess))selectedProcess=pid;}ImGui::EndCombo();}ImGui::EndDisabled();
 bool available=selectedProcess!=0;
 bool restart=selectedProcess && selectedProcess==restartRequiredPid;
 ImGui::Dummy(ImVec2(0,12));ImGui::TextColored(available&&!restart?ImVec4(.88f,.88f,.88f,1):ImVec4(.62f,.62f,.62f,1),restart?u8"Перезапустите CS2":available?u8"Процесс выбран":u8"Запустите игру для продолжения");
 ImGui::Dummy(ImVec2(0,18));ImGui::BeginDisabled(!available || connectionProcess || preview || restart);
 ImGui::PushStyleColor(ImGuiCol_Button,accent);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.04f,.04f,.04f,1));
 if(MonoButton(connectionProcess?"Injecting...":"Inject",ImVec2(-1,52))){launcherStarted=true;RequestGameConnection();}
 ImGui::PopStyleColor(2);ImGui::EndDisabled();ImGui::Dummy(ImVec2(0,18));
 LONG stage=progressValue?*progressValue:0;stage=std::clamp<LONG>(stage,0,100);char percent[16];sprintf_s(percent,"%ld%%",stage);ImGui::ProgressBar(stage/100.f,ImVec2(-1,9),"");
 ImGui::TextColored(accent,"%s",percent);ImGui::SameLine();
 const char* status=stage>=100?u8"Подключено":stage>=90?u8"Ожидание готовности модуля":stage>=75?u8"Загрузка модуля":stage>=60?u8"Подготовка подключения":stage>=45?u8"Подготовка файлов":stage>=25?u8"Проверка совместимости":stage>=10?u8"Проверка процесса":u8"Ожидание";
 if(launcherStarted && !connectionProcess && connectionExit){status=connectionExit==7?u8"Перезапустите CS2 перед повторной попыткой":connectionExit==2?u8"Версия игры не поддерживается":connectionExit==3?u8"Не удалось подготовить DLL":connectionExit==4?u8"Ошибка доступа или загрузки DLL":connectionExit==5?u8"DLL не загрузилась; проверьте зависимости":u8"Игра ещё не готова или процесс завершён";}
 ImGui::TextWrapped("%s",status);
 if(launchError)ImGui::TextWrapped(u8"Не удалось запустить загрузчик: ошибка Windows %lu",launchError);
 if(launcherStarted && !connectionProcess && connectionExit)ImGui::TextDisabled(u8"Подробности: connection.log рядом с программой");
 ImGui::EndChild();
 ImGui::SetCursorPos(ImVec2(24,375));ImGui::TextDisabled(u8"INSERT: скрыть / показать");
 ImGui::End();
}
