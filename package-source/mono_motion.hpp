#pragma once
#include <unordered_map>
#include <cmath>
struct MonoMotionState {float amount=0;int seen=0;};
static void MonoGlow(bool selected=false){
 if(!ImGui::IsItemVisible())return;
 static std::unordered_map<ImGuiID,MonoMotionState> motion;
 auto& state=motion[ImGui::GetItemID()];state.seen=ImGui::GetFrameCount();
 float target=ImGui::IsItemActive()?1.f:ImGui::IsItemHovered()? .85f:selected?.35f:0.f;
 state.amount+=(target-state.amount)*(1.f-std::exp(-12.f*ImGui::GetIO().DeltaTime));
 auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();auto draw=ImGui::GetWindowDrawList();
 float pulse=.92f+.08f*std::sin(float(ImGui::GetTime())*2.f);
 for(int layer=3;layer>=0;--layer){float spread=float(layer)*1.3f;int alpha=int((layer?10.f:95.f)*state.amount*pulse*ImGui::GetStyle().Alpha);
  draw->AddRect(ImVec2(a.x-spread,a.y-spread),ImVec2(b.x+spread,b.y+spread),IM_COL32(255,255,255,alpha),4.f+spread,0,layer?2.f:1.f);
 }
 if(ImGui::GetFrameCount()%240==0 && motion.size()>256)for(auto i=motion.begin();i!=motion.end();)if(ImGui::GetFrameCount()-i->second.seen>240)i=motion.erase(i);else ++i;
}
static bool MonoButton(const char* label,ImVec2 size=ImVec2(0,0)){
 bool light=ImGui::GetStyleColorVec4(ImGuiCol_Button).x>.7f;
 if(light){ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(1,1,1,1));ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.78f,.78f,.78f,1));}
 bool result=ImGui::Button(label,size);MonoGlow();if(light)ImGui::PopStyleColor(2);return result;
}
static bool MonoSelectable(const char* label,bool selected=false,ImGuiSelectableFlags flags=0,ImVec2 size=ImVec2(0,0)){bool result=ImGui::Selectable(label,selected,flags,size);MonoGlow(selected);return result;}
static bool MonoCheckbox(const char* label,bool* value){bool result=ImGui::Checkbox(label,value);MonoGlow(*value);return result;}
static bool MonoSliderFloat(const char* label,float* value,float min,float max,const char* format="%.3f"){bool result=ImGui::SliderFloat(label,value,min,max,format);MonoGlow();return result;}
static bool MonoSliderInt(const char* label,int* value,int min,int max){bool result=ImGui::SliderInt(label,value,min,max);MonoGlow();return result;}
static bool MonoInputTextWithHint(const char* label,const char* hint,char* buffer,size_t size){bool result=ImGui::InputTextWithHint(label,hint,buffer,size);MonoGlow();return result;}
