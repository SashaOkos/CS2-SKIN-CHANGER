@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /LD lifecycle_bridge.cpp /Fe:collection-lifecycle.dll
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /LD butterfly.cpp minhook\buffer.c minhook\hook.c minhook\trampoline.c minhook\hde\hde64.c /Fe:butterfly-runtime.dll /link user32.lib
if errorlevel 1 exit /b 1
rem Resource script contains the runtime and actual skin images.
rc /nologo butterfly.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /Iimgui loader.cpp menu.cpp imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_widgets.cpp imgui\imgui_tables.cpp imgui\imgui_impl_win32.cpp imgui\imgui_impl_dx11.cpp butterfly.res /Fe:CS2-Collection.exe /link user32.lib bcrypt.lib d3d11.lib dxgi.lib windowscodecs.lib ole32.lib gdi32.lib dwmapi.lib
