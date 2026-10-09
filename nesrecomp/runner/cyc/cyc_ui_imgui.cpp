// cyc_ui_imgui.cpp - Dear ImGui for recomp-ui's runtime menu and toast over
// the cycle host's SDL_Renderer (cyc_ui.h). The host keeps the ImGui context;
// recomp-ui only draws into it. Everything here is drawn after the picture's
// texture is copied to the renderer, at the window's full resolution: the
// picture itself (cyc_frame_argb, screenshots, hashes) never contains it.
#include "cyc_ui.h"

#include "recomp_runtime_ui.h"
#include "kickle_badges.h"

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <vector>
#include <algorithm>
#include "cyc_settings.h"
extern "C" void recomp_runtime_ui_set_artwork(void *, void *);
extern "C" void recomp_runtime_ui_set_achievement_badge(int, int, void *);
extern "C" unsigned char *launcher_image_load_rgba(const char *, int *, int *);
extern "C" void launcher_image_free(unsigned char *);

extern "C" {
RecompRuntimeUi *cyc_ui_menu_create(const CycUiHost *host);
void cyc_ui_menu_destroy(void);
void cyc_ui_menu_refresh(void);
RecompRuntimeUi *cyc_ui_runtime(void);
}

namespace {
ImGuiContext *s_ctx;
SDL_Renderer *s_ren;
bool s_ready;
SDL_Texture *s_background, *s_logo;
SDL_Texture *s_achievement_art;
SDL_Texture *s_badges[23][2]{};
SDL_Texture *badge_for(const char *key,bool unlocked) { int id=kickle_badge_index(key); return id<0 ? nullptr : s_badges[id][unlocked ? 1 : 0]; }
struct AchievementCard { std::string title, body; int points; };
std::deque<AchievementCard> s_achievements;
Uint64 s_achievement_start;
SDL_AudioDeviceID s_unlock_device;
SDL_AudioSpec s_unlock_spec;
Uint8 *s_unlock_wav;
Uint32 s_unlock_length;
CycSettings *s_settings;
ImFont *s_browser_font;
bool s_controls_open, s_controls_return, s_controls_consumed;
int s_controls_row, s_controls_column, s_capture=-1;
const char *control_label(int row) {
    static const char *labels[]={"A - Ice pillars","B - Freeze/Push","Select","Start / Pause","Up","Down","Left","Right"};
    return labels[row];
}
void controls_close() {
    s_controls_open=false; s_capture=-1;
    if(s_controls_return) recomp_runtime_ui_open(cyc_ui_runtime());
}
void controls_activate() {
    if(s_controls_row==8) {
        s_settings->bind.source[0]=(s_settings->bind.source[0]+1)%3;
        cyc_ui_save_settings();
    } else if(s_controls_row==9) {
        CycBindings defaults; cyc_bindings_default(&defaults);
        for(int i=0;i<8;++i) s_settings->bind.button[0][i]=defaults.button[0][i];
        s_settings->bind.source[0]=defaults.source[0];
        s_settings->bind.deadzone[0]=defaults.deadzone[0];
        cyc_ui_save_settings();
    } else if(s_controls_row==10) controls_close();
    else s_capture=s_controls_row;
}
void draw_controls() {
    ImVec2 screen=ImGui::GetIO().DisplaySize;
    if(s_background) ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)s_background,ImVec2(0,0),screen);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0),screen,IM_COL32(0,0,0,102));
    ImGui::SetNextWindowPos(ImVec2(screen.x*.5f,screen.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(screen.x*(screen.x>=1100 ? .70f : .90f),screen.y*.92f),ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(22,18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,18);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.045f,.075f,.12f,1));
    if(s_browser_font) ImGui::PushFont(s_browser_font);
    ImGui::Begin("##control-setup",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(std::clamp(screen.y/900.f,.65f,1.25f));
    ImGui::TextColored(ImVec4(.3f,.75f,.95f,1),"Control Setup");
    ImGui::TextWrapped("Choose a binding, then press a key or controller button. Changes save automatically.");
    ImGui::Separator();
    if(ImGui::BeginTable("##bindings",3,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action",ImGuiTableColumnFlags_WidthStretch,1.4f);
        ImGui::TableSetupColumn("Keyboard"); ImGui::TableSetupColumn("Controller"); ImGui::TableHeadersRow();
        for(int i=0;i<8;++i) {
            ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(control_label(i));
            for(int column=0;column<2;++column) {
                ImGui::TableNextColumn(); ImGui::PushID(column);
                char label[80]; auto &binding=s_settings->bind.button[0][i];
                if(s_capture==i && s_controls_column==column) snprintf(label,sizeof(label),"Press input... (Esc cancels)");
                else if(column==0) cyc_key_text(binding.key,label,sizeof(label));
                else cyc_pad_text(binding.pad,label,sizeof(label));
                if(ImGui::Selectable(label,s_controls_row==i && s_controls_column==column)) {
                    s_controls_row=i; s_controls_column=column; controls_activate();
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    const char *sources[]={"Disabled","Keyboard","Controller"};
    char source[80]; snprintf(source,sizeof(source),"Input source: %s",sources[std::clamp(s_settings->bind.source[0],0,2)]);
    const char *footer[]={source,"Restore gameplay defaults","Back"};
    for(int i=0;i<3;++i) if(ImGui::Selectable(footer[i],s_controls_row==8+i)) { s_controls_row=8+i; controls_activate(); }
    ImGui::TextWrapped("D-pad / Arrows: select   Left / Right: keyboard or controller   A / Enter: change   B / Esc: back");
    ImGui::End();
    if(s_browser_font) ImGui::PopFont();
    ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}
bool s_browser_open;
int s_browser_selected;
std::vector<const RecompRuntimeUiItem *> s_browser_items;
const RecompRuntimeUiCallbacks *s_browser_callbacks;
bool browser_unlocked(const RecompRuntimeUiItem *item) {
    int value=0;
    if(s_browser_callbacks && s_browser_callbacks->get_value)
        s_browser_callbacks->get_value(s_browser_callbacks->context,item,&value);
    return value!=0;
}
void draw_achievement_browser() {
    if(!s_browser_open || s_browser_items.empty()) return;
    ImVec2 screen=ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0),screen,IM_COL32(10,19,35,190));
    ImGui::SetNextWindowPos(ImVec2(screen.x*.06f,screen.y*.07f),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(screen.x*.88f,screen.y*.86f),ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(22,18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,14);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(12,10));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.08f,.13f,.22f,1));
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.22f,.34f,.5f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.27f,.42f,.6f,1));
    if(s_browser_font) ImGui::PushFont(s_browser_font);
    ImGui::Begin("##achievement-browser",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav);
    float scale=std::clamp(screen.y/720.f,.8f,1.6f);
    ImGui::SetWindowFontScale(scale);
    int earned=0;
    for(auto *item:s_browser_items) earned+=browser_unlocked(item);
    ImGui::TextColored(ImVec4(1,.83f,.5f,1),"Achievements");
    ImGui::SameLine(); ImGui::Text("  %d / %d unlocked",earned,(int)s_browser_items.size());
    ImGui::TextDisabled("Local challenges saved on this PC");
    ImGui::Separator();
    float width=ImGui::GetContentRegionAvail().x;
    float height=ImGui::GetContentRegionAvail().y-90;
    int page=s_browser_selected/6;
    ImGui::BeginChild("##achievement-list",ImVec2(width*.48f,height),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(scale);
    for(int i=page*6;i<std::min((page+1)*6,(int)s_browser_items.size());++i) {
        const auto *item=s_browser_items[i];
        ImGui::PushID(i);
        float row=std::max(26.f,(height-28)/6-10);
                ImVec2 at=ImGui::GetCursorScreenPos();
        if(ImGui::Selectable("##achievement",i==s_browser_selected,0,ImVec2(0,row))) s_browser_selected=i;
        auto *draw=ImGui::GetWindowDrawList();
        float icon=std::min(48.f*scale,row);
        if(auto *badge=badge_for(item->key,browser_unlocked(item)))
            draw->AddImage((ImTextureID)(intptr_t)badge,at,ImVec2(at.x+icon,at.y+icon));
        draw->AddText(ImVec2(at.x+icon+12,at.y+(row-ImGui::GetTextLineHeight())*.5f),IM_COL32(235,242,255,255),item->label);
        ImGui::PopID();
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##achievement-detail",ImVec2(0,height),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(scale);
    const auto *item=s_browser_items[s_browser_selected];
    bool unlocked=browser_unlocked(item);
        if(auto *badge=badge_for(item->key,unlocked))
        ImGui::Image((ImTextureID)(intptr_t)badge,ImVec2(96*scale,96*scale));
    ImGui::TextWrapped("%s",item->label);
    ImGui::Spacing();
    ImGui::TextColored(unlocked ? ImVec4(.55f,.95f,.65f,1) : ImVec4(.75f,.8f,.9f,1),"%s",unlocked ? "UNLOCKED" : "LOCKED");
    ImGui::Separator(); ImGui::Spacing();
    ImGui::TextWrapped("%s",item->description ? item->description : "");
    ImGui::EndChild();
    if(ImGui::Button("Prev")) s_browser_selected=(s_browser_selected+ (int)s_browser_items.size()-6)%(int)s_browser_items.size();
    ImGui::SameLine(); ImGui::Text("Page %d / %d",s_browser_selected/6+1,((int)s_browser_items.size()+5)/6);
    ImGui::SameLine(); if(ImGui::Button("Next")) s_browser_selected=(s_browser_selected+6)%(int)s_browser_items.size();
    ImGui::SameLine(); if(ImGui::Button("Back")) s_browser_open=false;
    ImGui::SetWindowFontScale(scale*.8f);
    ImGui::TextWrapped("Up/Down: select   Left/Right: page   B/Esc: back");
    ImGui::End();
    if(s_browser_font) ImGui::PopFont();
    ImGui::PopStyleColor(3); ImGui::PopStyleVar(3);
}
void play_unlock() {
    if(!s_unlock_wav || !s_unlock_device) return;
    int volume=s_settings ? std::clamp(s_settings->volume,0,100) : 100;
    if(s_settings && !s_settings->audio_enabled) volume=0;
    if(!volume) return;
    std::vector<Uint8> mixed(s_unlock_length,s_unlock_spec.silence);
    SDL_MixAudioFormat(mixed.data(),s_unlock_wav,s_unlock_spec.format,s_unlock_length,SDL_MIX_MAXVOLUME*volume/100);
    SDL_ClearQueuedAudio(s_unlock_device);
    SDL_QueueAudio(s_unlock_device,mixed.data(),s_unlock_length);
    SDL_PauseAudioDevice(s_unlock_device,0);
}
void draw_achievement() {
    if(s_achievements.empty()) return;
    Uint64 now=SDL_GetTicks64();
    if(!s_achievement_start) { s_achievement_start=now; play_unlock(); }
    float age=float(now-s_achievement_start)/1000.f;
    if(age>=5.5f) { s_achievements.pop_front(); s_achievement_start=0; return; }
    float alpha=std::min(1.f,std::min(age/.2f,(5.5f-age)/.4f));
    auto &card=s_achievements.front();
    ImVec2 screen=ImGui::GetIO().DisplaySize;
    if(s_achievement_art) {
        // Half the supplied 2171 x 724 card, constrained to the drawable.
        float width=std::min(1085.5f,std::max(1.f,screen.x-36.f));
        width=std::min(width,std::max(1.f,screen.y-36.f)*2171.f/724.f);
        float height=width*724.f/2171.f;
        ImVec2 origin(screen.x-18.f-width+(1-alpha)*40.f,screen.y-18.f-height);
        auto *draw=ImGui::GetForegroundDrawList();
                auto tint=IM_COL32(255,255,255,int(alpha*255));
        draw->AddImage((ImTextureID)(intptr_t)s_achievement_art,origin,
            ImVec2(origin.x+width,origin.y+height),ImVec2(0,0),ImVec2(1,1),tint);
                SDL_Texture *badge=nullptr;
        for(auto *item:s_browser_items) if(card.title==item->label) { badge=badge_for(item->key,true); break; }
        if(badge) {
            float size=width*.14f;
            ImVec2 at(origin.x+width*.055f,origin.y+height*.45f);
            draw->AddImage((ImTextureID)(intptr_t)badge,at,ImVec2(at.x+size,at.y+size),ImVec2(0,0),ImVec2(1,1),tint);
        }
        auto text=[&](const std::string &value,float x,float y,float w,float h,float size,ImU32 color,bool outline) {
            ImFont *font=ImGui::GetFont();
            float px=size*width;
            ImVec2 extent=font->CalcTextSizeA(px,FLT_MAX,0,value.c_str());
            if(extent.x>w*width) px*=w*width/extent.x;
            extent=font->CalcTextSizeA(px,FLT_MAX,0,value.c_str());
            ImVec2 at(origin.x+(x+w*.5f)*width-extent.x*.5f,
                      origin.y+(y+h*.5f)*height-extent.y*.5f);
            if(outline) {
                float stroke=std::max(1.f,width*.002f);
                for(int dx=-1;dx<=1;++dx) for(int dy=-1;dy<=1;++dy) if(dx || dy)
                    draw->AddText(font,px,ImVec2(at.x+dx*stroke,at.y+dy*stroke),IM_COL32(0,0,0,int(alpha*255)),value.c_str());
            }
            draw->AddText(font,px,at,color,value.c_str());
        };
        text(card.title,.225f,.49f,.56f,.22f,.065f,IM_COL32(255,218,20,int(alpha*255)),true);
        text(card.body,.225f,.735f,.575f,.115f,.020f,IM_COL32(15,15,15,int(alpha*255)),false);
        text(std::to_string(card.points),.892f,.685f,.06f,.15f,.045f,IM_COL32(255,224,25,int(alpha*255)),true);
        return;
    }
    float width=std::min(440.f,screen.x-32.f);
    ImGui::SetNextWindowPos(ImVec2(screen.x-18.f+(1-alpha)*40.f,screen.y-18.f),ImGuiCond_Always,ImVec2(1,1));
    ImGui::SetNextWindowSize(ImVec2(width,0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(18,16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,10.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.035f,.07f,.045f,.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(.15f,.65f,.25f,1));
    ImGui::Begin("##offline-achievement",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(.4f,1.f,.45f,1),"ACHIEVEMENT UNLOCKED   %d G",card.points);
    ImGui::Separator(); ImGui::Spacing();
    ImGui::TextWrapped("%s",card.title.c_str());
    ImGui::TextWrapped("%s",card.body.c_str());
    ImGui::End(); ImGui::PopStyleColor(2); ImGui::PopStyleVar(3);
}
SDL_Texture *load_art(SDL_Renderer *ren,const char *name) {
    char *base=SDL_GetBasePath();
    std::string path=(base ? base : ""); SDL_free(base);
    path += "assets/kickle/"; path += name;
    int w=0,h=0;
    unsigned char *pixels=launcher_image_load_rgba(path.c_str(),&w,&h);
    SDL_Surface *surface=pixels ? SDL_CreateRGBSurfaceWithFormatFrom(pixels,w,h,32,w*4,SDL_PIXELFORMAT_RGBA32) : nullptr;
    if(!surface) { launcher_image_free(pixels); return nullptr; }
    SDL_Texture *texture=SDL_CreateTextureFromSurface(ren,surface);
    SDL_FreeSurface(surface); launcher_image_free(pixels);
    if(texture) { SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear); }
    return texture;
}
}

extern "C" void cyc_ui_achievement(const char *title,const char *body,int points)
{
    if(title) s_achievements.push_back({title,body ? body : "",points});
    else { s_achievements.clear(); s_achievement_start=0; if(s_unlock_device) SDL_ClearQueuedAudio(s_unlock_device); }
}
extern "C" bool cyc_ui_achievements_is_open(void) { return s_browser_open; }
extern "C" void cyc_ui_open_achievements(void) {
    if(!s_ready || s_browser_items.empty()) return;
    recomp_runtime_ui_close(cyc_ui_runtime());
    s_browser_selected=0;
    s_browser_open=true;
}
extern "C" void cyc_ui_achievements_nav(int input) {
    if(!s_browser_open || s_browser_items.empty()) return;
    int n=(int)s_browser_items.size(), delta=0;
    if(input==RECOMP_RUNTIME_UI_INPUT_BACK || input==RECOMP_RUNTIME_UI_INPUT_TOGGLE) { s_browser_open=false; return; }
    if(input==RECOMP_RUNTIME_UI_INPUT_UP) delta=-1;
    if(input==RECOMP_RUNTIME_UI_INPUT_DOWN) delta=1;
    if(input==RECOMP_RUNTIME_UI_INPUT_LEFT) delta=-6;
    if(input==RECOMP_RUNTIME_UI_INPUT_RIGHT) delta=6;
    s_browser_selected=(s_browser_selected+n+delta)%n;
}

extern "C" bool cyc_ui_init(SDL_Window *win, SDL_Renderer *ren, const CycUiHost *host)
{
    if (!cyc_ui_menu_create(host)) {
        std::fprintf(stderr, "cyc ui: the runtime menu could not be created\n");
        return false;
    }
    IMGUI_CHECKVERSION();
    s_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    char *font_base=SDL_GetBasePath();
    std::string font_path=(font_base ? font_base : ""); SDL_free(font_base);
    font_path += "assets/fonts/LatoLatin-Regular.ttf";
    if(FILE *file=std::fopen(font_path.c_str(),"rb")) {
        std::fclose(file);
        s_browser_font=io.Fonts->AddFontFromFileTTF(font_path.c_str(),24.f);
    }
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL2_InitForSDLRenderer(win, ren)) {
        std::fprintf(stderr, "cyc ui: ImGui SDL2 platform initialization failed\n");
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        return false;
    }
    if (!ImGui_ImplSDLRenderer2_Init(ren)) {
        std::fprintf(stderr, "cyc ui: ImGui SDL_Renderer2 initialization failed\n");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        return false;
    }
    s_ren = ren;
    s_settings=host->settings;
    s_browser_open=false; s_browser_selected=0; s_browser_items.clear();
    s_browser_callbacks=host->extras ? host->extras->menu_callbacks : nullptr;
    if(host->extras) for(size_t i=0;i<host->extras->menu_item_count;++i) {
        const auto *item=&host->extras->menu_items[i];
        if(item->section && !strcmp(item->section,"Achievements")) s_browser_items.push_back(item);
    }
    if(host->extras && host->extras->menu_item_count && !strncmp(host->extras->menu_items[0].key,"kickle.",7)) {
        s_background=load_art(ren,"background.bmp"); s_logo=load_art(ren,"logo.bmp");
        s_achievement_art=load_art(ren,"achievement-card.png");
        for(int i=0;i<23;++i) for(int state=0;state<2;++state) {
            char path[80]; kickle_badge_path(path,sizeof(path),i,state!=0);
            s_badges[i][state]=load_art(ren,path);
            recomp_runtime_ui_set_achievement_badge(i,state,s_badges[i][state]);
            if(s_badges[i][state]) SDL_SetTextureScaleMode(s_badges[i][state],SDL_ScaleModeNearest);
        }
        recomp_runtime_ui_set_artwork(s_background,s_logo);
        char *base=SDL_GetBasePath();
        std::string sound=(base ? base : ""); SDL_free(base);
        sound += "assets/kickle/unlock.wav";
        if(SDL_LoadWAV(sound.c_str(),&s_unlock_spec,&s_unlock_wav,&s_unlock_length)) {
            s_unlock_spec.callback=nullptr;
            s_unlock_device=SDL_OpenAudioDevice(nullptr,0,&s_unlock_spec,nullptr,0);
        }
    }
    s_ready = true;
    return true;
}

extern "C" void cyc_ui_shutdown(void)
{
    if (s_ready) {
        if(s_unlock_device) SDL_CloseAudioDevice(s_unlock_device);
        SDL_FreeWAV(s_unlock_wav); s_unlock_wav=nullptr; s_unlock_device=0;
        s_achievements.clear(); s_achievement_start=0; s_settings=nullptr;
        s_browser_open=false; s_browser_items.clear(); s_browser_callbacks=nullptr;
        s_controls_open=false; s_capture=-1;
        s_browser_font=nullptr;
        recomp_runtime_ui_set_artwork(nullptr,nullptr);
        SDL_DestroyTexture(s_background); SDL_DestroyTexture(s_logo);
        SDL_DestroyTexture(s_achievement_art); s_achievement_art=nullptr;
        for(int i=0;i<23;++i) for(int state=0;state<2;++state) {
            recomp_runtime_ui_set_achievement_badge(i,state,nullptr);
            SDL_DestroyTexture(s_badges[i][state]); s_badges[i][state]=nullptr;
        }
        s_background=s_logo=nullptr;
        ImGui::SetCurrentContext(s_ctx);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        s_ready = false;
    }
    cyc_ui_menu_destroy();
}

extern "C" void cyc_ui_process_event(const SDL_Event *ev)
{
    if (!s_ready) return;
    ImGui::SetCurrentContext(s_ctx);
    ImGui_ImplSDL2_ProcessEvent(ev);
    s_controls_consumed=false;
    if(s_controls_open && s_capture>=0) {
        s_controls_consumed=true;
        if(ev->type==SDL_KEYDOWN && ev->key.keysym.scancode==SDL_SCANCODE_ESCAPE) { s_capture=-1; return; }
        int value=-1;
        if(s_controls_column==0 && ev->type==SDL_KEYDOWN && !ev->key.repeat) value=ev->key.keysym.scancode;
        if(s_controls_column==1 && ev->type==SDL_CONTROLLERBUTTONDOWN) value=CYC_PAD_BUTTON(ev->cbutton.button);
        if(s_controls_column==1 && ev->type==SDL_CONTROLLERAXISMOTION && abs(ev->caxis.value)>20000)
            value=CYC_PAD_AXIS(ev->caxis.axis,ev->caxis.value>0);
        if(value>=0) {
            auto &binding=s_settings->bind.button[0][s_capture];
            if(s_controls_column==0) binding.key=value; else binding.pad=value;
            s_capture=-1; cyc_ui_save_settings();
        }
    }
}

extern "C" void cyc_ui_open_controls(void) {
    if(!s_settings) return;
    s_controls_return=recomp_runtime_ui_is_open(cyc_ui_runtime());
    recomp_runtime_ui_close(cyc_ui_runtime());
    s_controls_open=true; s_controls_row=s_controls_column=0; s_capture=-1;
}
extern "C" bool cyc_ui_controls_is_open(void) { return s_controls_open; }
extern "C" void cyc_ui_controls_nav(int input) {
    if(s_controls_consumed || s_capture>=0) return;
    if(input==RECOMP_RUNTIME_UI_INPUT_BACK || input==RECOMP_RUNTIME_UI_INPUT_TOGGLE) { controls_close(); return; }
    if(input==RECOMP_RUNTIME_UI_INPUT_UP) s_controls_row=(s_controls_row+10)%11;
    if(input==RECOMP_RUNTIME_UI_INPUT_DOWN) s_controls_row=(s_controls_row+1)%11;
    if(input==RECOMP_RUNTIME_UI_INPUT_LEFT || input==RECOMP_RUNTIME_UI_INPUT_RIGHT) s_controls_column^=1;
    if(input==RECOMP_RUNTIME_UI_INPUT_ACCEPT) controls_activate();
}

extern "C" void cyc_ui_render(void)
{
    RecompRuntimeUi *ui = cyc_ui_runtime();
    if (!s_ready || !ui) return;
    if (!s_controls_open && !s_browser_open && !recomp_runtime_ui_is_open(ui) && !recomp_runtime_ui_toast_visible(ui) && s_achievements.empty()) return;
    cyc_ui_menu_refresh();
    // The picture uses a logical size; the menu uses the whole drawable, then
    // the picture's logical size and viewport come back for the next frame.
    int lw = 0, lh = 0;
    SDL_Rect viewport{};
    SDL_RenderGetLogicalSize(s_ren, &lw, &lh);
    SDL_RenderGetViewport(s_ren, &viewport);
    SDL_RenderSetLogicalSize(s_ren, 0, 0);
    SDL_RenderSetViewport(s_ren, nullptr);
    ImGui::SetCurrentContext(s_ctx);
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if(s_controls_open) draw_controls();
    else if(s_browser_open) draw_achievement_browser();
    else { if(s_browser_font) ImGui::PushFont(s_browser_font); recomp_runtime_ui_render_imgui(ui); if(s_browser_font) ImGui::PopFont(); }
    draw_achievement();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_ren);
    SDL_RenderSetLogicalSize(s_ren, lw, lh);
    SDL_RenderSetViewport(s_ren, &viewport);
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
}
