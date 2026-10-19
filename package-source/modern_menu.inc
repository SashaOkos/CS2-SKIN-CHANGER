#include "image_resources.hpp"
#include <unordered_map>
static IWICImagingFactory* imageFactory=nullptr;
struct WearPixel{unsigned char r,g,b,a;};
static ID3D11ShaderResourceView* wearPreview=nullptr;
static float openProgress=1.f;
struct MenuTexture{ID3D11ShaderResourceView* view;uint64_t used;};
static std::unordered_map<int,MenuTexture> textureCache;
static uint64_t textureTick=0;
static ID3D11ShaderResourceView* Texture(int item,int paint){
 int id=ImageResource(item,paint);if(!id)return nullptr;
 auto it=textureCache.find(id);if(it!=textureCache.end()){it->second.used=++textureTick;return it->second.view;}
 auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));if(!resource)return nullptr;
 auto data=static_cast<BYTE*>(LockResource(LoadResource(nullptr,resource)));DWORD size=SizeofResource(nullptr,resource);
 IWICStream* stream=nullptr;IWICBitmapDecoder* decoder=nullptr;IWICBitmapFrameDecode* frame=nullptr;IWICFormatConverter* converter=nullptr;ID3D11Texture2D* texture=nullptr;ID3D11ShaderResourceView* view=nullptr;UINT width=0,height=0;
 HRESULT hr=imageFactory->CreateStream(&stream);if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(data,size);if(SUCCEEDED(hr))hr=imageFactory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder);if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);if(SUCCEEDED(hr))hr=imageFactory->CreateFormatConverter(&converter);if(SUCCEEDED(hr))hr=converter->Initialize(frame,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
 std::vector<unsigned char> pixels(width*height*4);if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,width*4,(UINT)pixels.size(),pixels.data());
 D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA source{pixels.data(),width*4,0};if(SUCCEEDED(hr))hr=dev->CreateTexture2D(&desc,&source,&texture);if(SUCCEEDED(hr))hr=dev->CreateShaderResourceView(texture,nullptr,&view);
 if(texture)texture->Release();if(converter)converter->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(stream)stream->Release();
 if(FAILED(hr))return nullptr;

 textureCache[id]={view,++textureTick};return view;
}
static void TrimTextureCache(){
 // ImGui stores SRV pointers until its draw commands have been submitted.
 while(textureCache.size()>96){auto oldest=textureCache.begin();for(auto i=textureCache.begin();i!=textureCache.end();++i)if(i->second.used<oldest->second.used)oldest=i;oldest->second.view->Release();textureCache.erase(oldest);}
}
static HWND lastGameWindow=nullptr;
static void MenuVisibility(bool open){
 if(open){openProgress=0.f;menuKeys.Reset();}
 visible=open;InterlockedExchange(&state->menuOpen,open?1:0);
 if(open){
  if(!launcherMode)AttachGameMenu();
  auto foreground=GetForegroundWindow();if(foreground && foreground!=window)lastGameWindow=foreground;
  ShowWindow(window,SW_SHOW);DWORD thread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr),own=GetCurrentThreadId();bool attached=thread && thread!=own && AttachThreadInput(own,thread,TRUE);
  if(gameMenuParent)SetForegroundWindow(gameMenuParent);BringWindowToTop(window);if(!gameMenuParent)SetForegroundWindow(window);SetFocus(window);if(attached)AttachThreadInput(own,thread,FALSE);
  ReleaseCapture();ClipCursor(nullptr);SetCursor(LoadCursor(nullptr,IDC_ARROW));
 }else{ShowWindow(window,SW_HIDE);if(lastGameWindow && IsWindow(lastGameWindow))SetForegroundWindow(lastGameWindow);}
}
static ID3D11ShaderResourceView* WearTexture(int item,int paint,float wear,int seed) {
 static int lastResource=0,lastStep=-1,lastSeed=-1;
 int resourceId=ImageResource(item,paint),step=int(std::clamp(wear,0.f,1.f)*100+.5f);
 if(!paint || !resourceId)return Texture(item,paint);
 if(resourceId==lastResource && step==lastStep && seed==lastSeed)return wearPreview;
 auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(resourceId),MAKEINTRESOURCEW(10));
 if(!resource || !imageFactory)return Texture(item,paint);
 auto data=static_cast<BYTE*>(LockResource(LoadResource(nullptr,resource)));DWORD length=SizeofResource(nullptr,resource);
 IWICStream* stream=nullptr;IWICBitmapDecoder* decoder=nullptr;IWICBitmapFrameDecode* frame=nullptr;IWICFormatConverter* converter=nullptr;UINT width=0,height=0;
 HRESULT hr=imageFactory->CreateStream(&stream);
 if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(data,length);
 if(SUCCEEDED(hr))hr=imageFactory->CreateDecoderFromStream(stream,nullptr,WICDecodeMetadataCacheOnLoad,&decoder);
 if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
 if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
 if(SUCCEEDED(hr))hr=imageFactory->CreateFormatConverter(&converter);
 if(SUCCEEDED(hr))hr=converter->Initialize(frame,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
 std::vector<WearPixel> pixels(width*height);
 if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,width*4,UINT(pixels.size()*4),reinterpret_cast<BYTE*>(pixels.data()));
 // Illustrative wear: deterministic chipped coating and fading, preserving the icon alpha.
 float amount=step/100.f;
 for(UINT y=0;SUCCEEDED(hr) && y<height;y++)for(UINT x=0;x<width;x++) {
  auto& pixel=pixels[y*width+x];if(!pixel.a)continue;
  uint32_t hash=(x/3)*0x9e3779b9u ^ (y/2)*0x85ebca6bu ^ uint32_t(seed+17)*0xc2b2ae35u;
  hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
  float noise=float(hash&65535)/65535.f;
  float scratch=(noise<amount*.5f)?(.25f+amount*.6f):amount*.10f;
  float gray=.25f*pixel.r+.55f*pixel.g+.2f*pixel.b;
  auto channel=[&](unsigned char value){return static_cast<unsigned char>(std::clamp(value*(1-scratch)+gray*scratch*(1-amount*.13f),0.f,255.f));};
  pixel.r=channel(pixel.r);pixel.g=channel(pixel.g);pixel.b=channel(pixel.b);
 }
 ID3D11Texture2D* texture=nullptr;ID3D11ShaderResourceView* view=nullptr;
 D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA source{pixels.data(),width*4,0};
 if(SUCCEEDED(hr))hr=dev->CreateTexture2D(&desc,&source,&texture);
 if(SUCCEEDED(hr))hr=dev->CreateShaderResourceView(texture,nullptr,&view);
 if(texture)texture->Release();if(converter)converter->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(stream)stream->Release();
 if(FAILED(hr))return Texture(item,paint);
 if(wearPreview)wearPreview->Release();wearPreview=view;lastResource=resourceId;lastStep=step;lastSeed=seed;return view;
}
static void ImageContained(ID3D11ShaderResourceView* view,ImVec2 pos,ImVec2 size){
 if(!view)return;ID3D11Resource* resource=nullptr;view->GetResource(&resource);if(!resource)return;
 ID3D11Texture2D* image=nullptr;HRESULT result=resource->QueryInterface(IID_PPV_ARGS(&image));resource->Release();if(FAILED(result))return;
 D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);image->Release();if(!desc.Width || !desc.Height)return;
 float scale=std::min(size.x/desc.Width,size.y/desc.Height);ImVec2 fitted(desc.Width*scale,desc.Height*scale);
 ImVec2 a(pos.x+(size.x-fitted.x)*.5f,pos.y+(size.y-fitted.y)*.5f);
 ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)view,a,ImVec2(a.x+fitted.x,a.y+fitted.y));
}
static void Picture(int item,int paint,ImVec2 size,float wear=0.f,int seed=0){
 auto pos=ImGui::GetCursorScreenPos();auto texture=WearTexture(item,paint,wear,seed);
 ImGui::GetWindowDrawList()->AddRectFilled(pos,ImVec2(pos.x+size.x,pos.y+size.y),IM_COL32(24,24,24,255),4);
 if(texture)ImageContained(texture,ImVec2(pos.x+8,pos.y+8),ImVec2(size.x-16,size.y-16));
 else ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x+16,pos.y+size.y/2),IM_COL32(145,145,145,255),u8"Стандартный вид");
 ImGui::Dummy(size);
}
static std::unordered_map<int,SkinChoice> drafts;
static bool resetRequested=false,selectiveResetRequested=false;
static bool selectedForReset[8192]{};
static void Draw(){
 Style();openProgress=std::min(1.f,openProgress+ImGui::GetIO().DeltaTime*5.f);ImGui::GetStyle().Alpha=.45f+.55f*openProgress;ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImVec2(float(menuWidth),float(menuHeight)));
 ImGui::Begin(u8"Коллекция",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar);
 ImGui::PushFont(title);ImGui::TextColored(accent,u8"КОЛЛЕКЦИЯ");ImGui::PopFont();
 ImGui::SameLine(float(menuWidth-420));if(MonoButton(u8"Сбросить конфиг",ImVec2(210,34)))resetRequested=true;
 ImGui::SameLine();if(MonoButton(u8"Скрыть",ImVec2(150,34)))MenuVisibility(false);
 ImGui::TextDisabled(u8"Выберите отделку и добавьте предмет в свою коллекцию");
 bool runtime=state->runtimeConnected && GetTickCount64()-state->runtimeHeartbeat<2500;
 ImGui::SameLine(800);ImGui::TextColored(runtime?ImVec4(.88f,.88f,.88f,1):ImVec4(.64f,.64f,.64f,1),runtime?u8"CS2 ПОДКЛЮЧЕНА":u8"CS2 НЕ ПОДКЛЮЧЕНА");
 if(!runtime){ImGui::SameLine();if(MonoButton(connectionProcess?u8"Подключаюсь...":u8"Подключить"))RequestGameConnection();}
 ImGui::Dummy(ImVec2(0,5));ImGui::Separator();ImGui::Dummy(ImVec2(0,5));
 ImGui::BeginChild("categories",ImVec2(menuWidth*.13f,float(menuHeight-210)),true);ImGui::TextDisabled(u8"КАТЕГОРИИ");ImGui::Dummy(ImVec2(0,10));
 const char* cats[]={u8"Пистолеты",u8"Винтовки",u8"ПП",u8"Тяжёлое",u8"Ножи",u8"Перчатки"};
 for(int i=0;i<6;i++)if(MonoSelectable(cats[i],category==i,0,ImVec2(0,32))){category=i;if(i<6){for(size_t j=0;j<catalog.size();j++)if(catalog[j].category==i){itemIndex=(int)j;break;}}else{for(size_t j=0;j<catalog.size();j++)if(state->choices[catalog[j].id].active){itemIndex=(int)j;break;}}filter[0]=0;}
 ImGui::Separator();if(MonoSelectable(u8"Сброс предметов",category==6,0,ImVec2(0,38)))category=6;
 if(MonoSelectable(u8"Настройки",category==7,0,ImVec2(0,38)))category=7;
 ImGui::EndChild();ImGui::SameLine();
 if(category<6){
 ImGui::BeginChild("items",ImVec2(menuWidth*.145f,float(menuHeight-210)),true);ImGui::TextDisabled(u8"МОДЕЛЬ");ImGui::Dummy(ImVec2(0,10));
 int inventoryCount=0;
 for(size_t i=0;i<catalog.size();i++){bool show=catalog[i].category==category;if(show){inventoryCount++;ImGui::PushID((int)i);if(MonoSelectable(catalog[i].name,itemIndex==(int)i,0,ImVec2(0,34)))itemIndex=(int)i;ImGui::PopID();}}
 ImGui::EndChild();ImGui::SameLine();
 auto &w=catalog[itemIndex];
 auto draft=drafts.find(w.id);if(draft==drafts.end())draft=drafts.emplace(w.id,state->choices[w.id]).first;
 auto choice=draft->second;
 ImGui::BeginChild("gallery",ImVec2(menuWidth*.37f,float(menuHeight-210)),true);ImGui::TextDisabled(u8"ОТДЕЛКИ");ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-43);
 if(ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F))ImGui::SetKeyboardFocusHere();
 MonoInputTextWithHint("##search",u8"Найти скин...",filter,sizeof(filter));ImGui::SameLine(0,5);if(MonoButton("X",ImVec2(38,0)))filter[0]=0;ImGui::BeginChild("cards",ImVec2(0,0));
 const float cardWidth=std::max(80.f,(ImGui::GetContentRegionAvail().x-10.f)*.5f);int column=0;
 auto card=[&](int paint,const char* name){
  ImGui::BeginGroup();ImGui::PushID(paint);auto pos=ImGui::GetCursorScreenPos();ImVec2 size(cardWidth,196);
  if(ImGui::InvisibleButton("card",size)){choice.paint=paint;for(auto&s:w.skins)if(s.id==paint)choice.wear=std::clamp(choice.wear,s.min,s.max);}
  bool cardVisible=ImGui::IsItemVisible();
  bool selected=choice.paint==paint,hover=ImGui::IsItemHovered();
  static std::unordered_map<ImGuiID,float> hoverValues;auto &hoverAmount=hoverValues[ImGui::GetID("card")];hoverAmount+=(float(hover)-hoverAmount)*std::min(1.f,ImGui::GetIO().DeltaTime*12.f);
  auto dl=ImGui::GetWindowDrawList();dl->AddRectFilled(pos,ImVec2(pos.x+size.x,pos.y+size.y),Color(ImVec4((25+14*hoverAmount)/255.f,(25+14*hoverAmount)/255.f,(25+14*hoverAmount)/255.f,1)),4);
  if(cardVisible)if(auto texture=Texture(w.id,paint))ImageContained(texture,ImVec2(pos.x+5,pos.y+3),ImVec2(size.x-10,152));
  if(selected)dl->AddRect(pos,ImVec2(pos.x+size.x,pos.y+size.y),Color(accent),4,0,1.5f);
  MonoGlow(selected);
  ImGui::SetCursorScreenPos(ImVec2(pos.x+12,pos.y+151));ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+size.x-24);ImGui::TextUnformatted(name);ImGui::PopTextWrapPos();ImGui::SetCursorScreenPos(ImVec2(pos.x,pos.y+196));ImGui::PopID();ImGui::EndGroup();
  if(++column%2)ImGui::SameLine(0,10);
 };
 if(Match(u8"Без окраски",filter) && w.category!=5)card(0,u8"Без окраски");
 for(auto&s:w.skins)if(Match(s.name,filter))card(s.id,s.name);
 ImGui::EndChild();ImGui::EndChild();ImGui::SameLine();
 ImGui::BeginChild("selection",ImVec2(0,float(menuHeight-210)),true);ImGui::BeginChild("selectionDetails",ImVec2(0,ImGui::GetContentRegionAvail().y-135),false);ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10,5));ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(8,6));ImGui::TextDisabled(u8"ВАШ ВЫБОР");ImGui::Dummy(ImVec2(0,8));ImGui::PushTextWrapPos(0);ImGui::PushFont(title);ImGui::TextUnformatted(w.name);ImGui::PopFont();ImGui::PopTextWrapPos();
 const char* skinName=u8"Без окраски";float min=0,max=1;for(auto&s:w.skins)if(s.id==choice.paint){skinName=s.name;min=s.min;max=s.max;}
 Picture(w.id,choice.paint,ImVec2(ImGui::GetContentRegionAvail().x,220),choice.wear,choice.seed);ImGui::TextDisabled(u8"Изображение каталога / примерный износ");ImGui::PushTextWrapPos(0);ImGui::TextColored(accent,"%s",skinName);ImGui::PopTextWrapPos();ImGui::Dummy(ImVec2(0,6));ImGui::TextDisabled(u8"ДЕТАЛИ");
 ImGui::TextUnformatted(u8"Износ");ImGui::SetNextItemWidth(-1);MonoSliderFloat("##wear",&choice.wear,min,max,"%.4f");const char* wearNames[]={u8"Прямо с завода",u8"Немного поношенное",u8"После полевых испытаний",u8"Поношенное",u8"Закалённое в боях"};
 int wearLevel=choice.wear<.07f?0:choice.wear<.15f?1:choice.wear<.38f?2:choice.wear<.45f?3:4;
 ImGui::TextColored(accent,"%s",wearNames[wearLevel]);
 const float presets[]={.02f,.10f,.25f,.42f,.70f};const char* presetLabels[]={"FN","MW","FT","WW","BS"};
 for(int i=0;i<5;i++){if(i)ImGui::SameLine(0,3);if(MonoButton(presetLabels[i],ImVec2(39,25)))choice.wear=std::clamp(presets[i],min,max);if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",wearNames[i]);}

 draft->second=choice;ImGui::PopStyleVar(2);ImGui::EndChild();
 auto commit=[&](){InterlockedIncrement(&state->revision);state->choices[w.id]=choice;state->choices[w.id].active=1;InterlockedIncrement(&state->revision);draft->second=state->choices[w.id];Save();};
 ImGui::TextDisabled(saveFailed?u8"Не удалось сохранить конфиг":saveDue?u8"Сохранение...":u8"Предметы и экипировка сохраняются");
 ImGui::PushStyleColor(ImGuiCol_Button,accent);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.04f,.04f,.04f,1));
 if(MonoButton(u8"Добавить в инвентарь",ImVec2(-1,44))){commit();InterlockedIncrement(&state->revision);state->inventoryDefinition=w.id;state->inventoryResult=0;InterlockedIncrement(&state->inventoryRequest);InterlockedIncrement(&state->revision);Save();}
 ImGui::PopStyleColor(2);ImGui::PushTextWrapPos(0);
 if(state->inventoryResult==1)ImGui::TextColored(ImVec4(.88f,.88f,.88f,1),u8"Предмет добавлен в локальный инвентарь");
 else if(state->inventoryResult<0)ImGui::TextColored(ImVec4(.78f,.78f,.78f,1),u8"Не удалось добавить предмет (%ld)",state->inventoryResult);
 else ImGui::TextDisabled(u8"Выбор отделки - только предпросмотр");
 if(state->resetResult==1)ImGui::TextDisabled(u8"Сброс выполнен. Удалено предметов: %ld",state->resetRemoved);
 else if(state->resetResult<0)ImGui::TextColored(ImVec4(.78f,.78f,.78f,1),u8"Сброс инвентаря не завершён (%ld)",state->resetResult);
 if(!runtime)ImGui::TextDisabled(connectionProcess?u8"Подключение к игре...":u8"Для добавления подключите CS2");
 ImGui::PopTextWrapPos();ImGui::EndChild();
 }else if(category==6){
 ImGui::BeginChild("selectiveReset",ImVec2(0,float(menuHeight-210)),true);
 ImGui::PushFont(title);ImGui::TextUnformatted(u8"Выборочный сброс");ImGui::PopFont();
 ImGui::TextWrapped(u8"Выберите модели. Сброс удалит их сохранённые отделки, экипировку T / CT и все копии этих моделей, добавленные программой. Остальные предметы сохранятся.");
 ImGui::TextDisabled(u8"Настоящие предметы Steam остаются в инвентаре.");
 ImGui::Dummy(ImVec2(0,10));int count=0,checked=0;
 ImGui::BeginChild("resetItems",ImVec2(0,370),true);
 for(auto& entry:catalog){bool present=state->choices[entry.id].active;
  for(int team=2;team<=3;team++)for(auto& equipment:state->loadout[team])if(equipment.custom && equipment.definition==entry.id)present=true;
  if(!present){selectedForReset[entry.id]=false;continue;}++count;
  ImGui::PushID(entry.id);MonoCheckbox(entry.name,&selectedForReset[entry.id]);
  if(selectedForReset[entry.id])++checked;
  const char* finish=u8"Без окраски";for(auto& skin:entry.skins)if(skin.id==state->choices[entry.id].paint)finish=skin.name;
  ImGui::SameLine(240);ImGui::TextDisabled("%s",finish);ImGui::Separator();ImGui::PopID();
 }
 if(!count)ImGui::TextDisabled(u8"Нет сохранённых предметов для сброса.");
 ImGui::EndChild();ImGui::TextDisabled(u8"Выбрано моделей: %d",checked);
 ImGui::BeginDisabled(!checked || (runtime && state->resetSerial && !state->resetResult));
 if(MonoButton(u8"Сбросить выбранные",ImVec2(240,38)))selectiveResetRequested=true;
 ImGui::EndDisabled();ImGui::SameLine();if(MonoButton(u8"Снять выделение",ImVec2(180,38)))memset(selectedForReset,0,sizeof(selectedForReset));
 if(state->resetResult==1)ImGui::TextColored(ImVec4(.88f,.88f,.88f,1),u8"Готово. Удалено локальных предметов: %ld",state->resetRemoved);
 else if(state->resetResult<0)ImGui::TextColored(ImVec4(.78f,.78f,.78f,1),u8"Сброс не завершён (%ld)",state->resetResult);
 else if(state->resetSerial)ImGui::TextDisabled(runtime?u8"Обновление локального инвентаря...":u8"Конфиг сохранён. Подключите CS2 для обновления инвентаря.");
 ImGui::EndChild();
 }
 if(category==7){
 ImGui::BeginChild("settings",ImVec2(0,float(menuHeight-210)),true);
 ImGui::PushFont(title);ImGui::TextUnformatted(u8"Настройки");ImGui::PopFont();
 ImGui::TextDisabled(u8"Коллекция и управление меню");ImGui::Dummy(ImVec2(0,20));
 ImGui::BeginChild("settings_glow",ImVec2(0,280),true);
 ImGui::TextUnformatted("Glow");ImGui::Separator();
 auto glow=NormalizeGlow(state->glow);bool glowOn=glow.enabled!=0;bool glowChanged=ImGui::Checkbox(u8"Свечение от первого лица",&glowOn);glow.enabled=glowOn;
 const char* glowTargets[]={u8"Руки",u8"Оружие",u8"Руки и оружие"};
 ImGui::SetNextItemWidth(280);glowChanged=ImGui::Combo(u8"Объекты",&glow.target,glowTargets,3)||glowChanged;
 ImGui::SetNextItemWidth(280);glowChanged=ImGui::ColorEdit3(u8"Цвет свечения",glow.color,ImGuiColorEditFlags_NoInputs)||glowChanged;
 float glowPercent=glow.intensity*100.f;ImGui::SetNextItemWidth(280);
 if(ImGui::SliderFloat(u8"Интенсивность",&glowPercent,0.f,100.f,"%.0f%%")){glow.intensity=glowPercent/100.f;glowChanged=true;}
 if(MonoButton(u8"Сбросить Glow",ImVec2(180,32))){glow=GlowSettings{};glowChanged=true;}
 if(glowChanged){InterlockedIncrement(&state->glowRevision);state->glow=NormalizeGlow(glow);InterlockedIncrement(&state->glowRevision);Save();}
 if(state->glowStatus<0)ImGui::TextWrapped(u8"Свечение недоступно для этой версии модели.");
 ImGui::EndChild();ImGui::Dummy(ImVec2(0,14));
 ImGui::BeginChild("settings_collection",ImVec2(0,155),true);
 ImGui::TextUnformatted(u8"Сохранённые предметы");ImGui::Separator();ImGui::Spacing();
 ImGui::TextWrapped(u8"Предметы и выбранная экипировка сохраняются автоматически.");
 ImGui::Spacing();if(MonoButton(u8"Выбрать предметы для сброса",ImVec2(300,38)))category=6;
 ImGui::SameLine();if(MonoButton(u8"Сбросить всё",ImVec2(180,38)))resetRequested=true;
 ImGui::EndChild();ImGui::Dummy(ImVec2(0,14));
 ImGui::TextUnformatted(u8"Подключение к игре");ImGui::Separator();ImGui::Spacing();
 ImGui::TextWrapped(u8"Uninject отключает функции и выгружает основную DLL. CS2 продолжит работать, конфиг сохранится.");
 ImGui::TextDisabled(u8"Служебный модуль управления остаётся до выхода из игры.");
 bool unloading=state->unloadRequest && state->unloadResult>=0 && state->unloadResult<2;
 ImGui::Dummy(ImVec2(0,14));ImGui::BeginDisabled(!runtime || unloading);
 if(MonoButton("Uninject",ImVec2(220,44)))ImGui::OpenPopup(u8"Выгрузить модуль?");ImGui::EndDisabled();
 if(unloading)ImGui::TextWrapped(u8"Ожидание завершения игровых вызовов...");
 else if(state->unloadResult==2)ImGui::TextWrapped(CoreModulePresent(state->runtimePid)?u8"Завершение выгрузки...":u8"Основная DLL выгружена. Игра продолжает работать.");
 else if(state->unloadResult<0)ImGui::TextWrapped(u8"Выгрузка не завершена (%ld). Основная DLL сохранена в памяти.",state->unloadResult);
 else if(!runtime)ImGui::TextDisabled(u8"Доступно после подключения к CS2.");
 ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(.5f,.5f));
 if(ImGui::BeginPopupModal(u8"Выгрузить модуль?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
 ImGui::TextUnformatted(u8"Отключить модификации и выгрузить основную DLL?");
 ImGui::TextUnformatted(u8"CS2 останется открытой. Конфиг будет сохранён.");
 if(MonoButton("Uninject",ImVec2(200,40))){
  Save();FlushSave(true);manualDisconnect=true;state->unloadResult=0;InterlockedExchange(&state->unloadRequest,1);
  ImGui::CloseCurrentPopup();MenuVisibility(false);
 }
 ImGui::SameLine();if(MonoButton(u8"Отмена",ImVec2(130,40)))ImGui::CloseCurrentPopup();ImGui::EndPopup();
 }

 ImGui::EndChild();
 }
 ImGui::Dummy(ImVec2(0,3));ImGui::Separator();ImGui::TextDisabled(u8"INSERT / ESC - скрыть меню       CTRL+F - поиск       Добавление не экипирует предмет. Выберите экипировку в инвентаре CS2.");
 if(resetRequested){ImGui::OpenPopup(u8"Сбросить конфиг?");resetRequested=false;}
 ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(.5f,.5f));
 if(ImGui::BeginPopupModal(u8"Сбросить конфиг?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
  ImGui::TextUnformatted(u8"Очистить сохранённые предметы и экипировку T / CT?");
  ImGui::TextDisabled(u8"Добавленные программой вещи будут удалены из локального инвентаря.");
  if(MonoButton(u8"Сбросить",ImVec2(140,32))){ResetSelections();drafts.clear();memset(selectedForReset,0,sizeof(selectedForReset));filter[0]=0;skinIndex=-1;ImGui::CloseCurrentPopup();}
  ImGui::SameLine();if(MonoButton(u8"Отмена",ImVec2(140,32)))ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
 }
 if(selectiveResetRequested){ImGui::OpenPopup(u8"Сбросить выбранные предметы?");selectiveResetRequested=false;}
 ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(.5f,.5f));
 if(ImGui::BeginPopupModal(u8"Сбросить выбранные предметы?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
  ImGui::TextUnformatted(u8"Удалить настройки и локальные копии выбранных моделей?");
  if(MonoButton(u8"Сбросить",ImVec2(140,32))){ResetSelectedDefinitions(selectedForReset);for(int id=0;id<8192;id++)if(selectedForReset[id])drafts.erase(id);memset(selectedForReset,0,sizeof(selectedForReset));ImGui::CloseCurrentPopup();}
  ImGui::SameLine();if(MonoButton(u8"Отмена",ImVec2(140,32)))ImGui::CloseCurrentPopup();ImGui::EndPopup();
 }
 ImGui::End();
}



