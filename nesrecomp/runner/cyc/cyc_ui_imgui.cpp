// cyc_ui_imgui.cpp - Dear ImGui for recomp-ui's runtime menu and toast over
// the cycle host's SDL_Renderer (cyc_ui.h). The host keeps the ImGui context;
// recomp-ui only draws into it. Everything here is drawn after the picture's
// texture is copied to the renderer, at the window's full resolution: the
// picture itself (cyc_frame_argb, screenshots, hashes) never contains it.
#include "cyc_ui.h"

#include "recomp_runtime_ui.h"

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
struct AchievementCard { std::string title, body; int points; };
std::deque<AchievementCard> s_achievements;
Uint64 s_achievement_start;
SDL_AudioDeviceID s_unlock_device;
SDL_AudioSpec s_unlock_spec;
Uint8 *s_unlock_wav;
Uint32 s_unlock_length;
CycSettings *s_settings;
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
    SDL_Surface *surface=SDL_LoadBMP(path.c_str());
    if(!surface) return nullptr;
    SDL_Texture *texture=SDL_CreateTextureFromSurface(ren,surface);
    SDL_FreeSurface(surface); return texture;
}
}

extern "C" void cyc_ui_achievement(const char *title,const char *body,int points)
{
    if(title) s_achievements.push_back({title,body ? body : "",points});
    else { s_achievements.clear(); s_achievement_start=0; if(s_unlock_device) SDL_ClearQueuedAudio(s_unlock_device); }
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
    if(host->extras && host->extras->menu_item_count && !strncmp(host->extras->menu_items[0].key,"kickle.",7)) {
        s_background=load_art(ren,"background.bmp"); s_logo=load_art(ren,"logo.bmp");
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
        recomp_runtime_ui_set_artwork(nullptr,nullptr);
        SDL_DestroyTexture(s_background); SDL_DestroyTexture(s_logo);
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
}

extern "C" void cyc_ui_render(void)
{
    RecompRuntimeUi *ui = cyc_ui_runtime();
    if (!s_ready || !ui) return;
    if (!recomp_runtime_ui_is_open(ui) && !recomp_runtime_ui_toast_visible(ui) && s_achievements.empty()) return;
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
    recomp_runtime_ui_render_imgui(ui);
    draw_achievement();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_ren);
    SDL_RenderSetLogicalSize(s_ren, lw, lh);
    SDL_RenderSetViewport(s_ren, &viewport);
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
}
