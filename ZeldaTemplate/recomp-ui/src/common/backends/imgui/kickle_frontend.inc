// Kickle's launch screen uses the existing verified ROM picker and pad navigation.
static const RecompRuntimeUiItem *kickle_item(const char *key) {
    for(size_t i=0;i<g_game_item_count;++i)
        if(!strcmp(g_game_items[i].key,key)) return &g_game_items[i];
    return nullptr;
}
static int kickle_value(const char *key) {
    int value=0; const auto *item=kickle_item(key);
    if(item && g_game_callbacks && g_game_callbacks->get_value)
        g_game_callbacks->get_value(g_game_callbacks->context,item,&value);
    return value;
}
static void kickle_toggle(const char *label,const char *key) {
    bool value=kickle_value(key)!=0;
    if(ImGui::Checkbox(label,&value)) {
        const auto *item=kickle_item(key);
        if(item && g_game_callbacks->set_value)
            g_game_callbacks->set_value(g_game_callbacks->context,item,value);
    }
}
static int kickle_action(const char *key) {
    const auto *item=kickle_item(key);
    if(item && g_game_callbacks && g_game_callbacks->run_action)
        return g_game_callbacks->run_action(g_game_callbacks->context,item);
    return 0;
}
static void draw_kickle_frontend(LauncherModel *m,const LauncherTheme &th) {
    static int page=0;
    bool focus_options=false;
    ImVec2 size=ImGui::GetIO().DisplaySize;
    if(ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape)) page=0;
    auto *bg=ImGui::GetBackgroundDrawList();
    if(g_kickle_background.id) bg->AddImage(tid(g_kickle_background),ImVec2(0,0),size);
    bg->AddRectFilled(ImVec2(size.x*.44f,0),size,IM_COL32(4,24,53,100));
    float left=size.x>=1000 ? size.x*.46f : size.x*.08f;
    float width=size.x-left-size.x*.04f;
    ImGui::SetNextWindowPos(ImVec2(left,size.y*.045f),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width,size.y*.91f),ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.025f,.09f,.18f,.91f));
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.08f,.25f,.43f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.15f,.43f,.62f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.1f,.55f,.7f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(.55f,.94f,1,1));
    ImGui::PushStyleColor(ImGuiCol_NavCursor,ImVec4(1,.82f,.2f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,18);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,8);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(14,14));
    ImGui::Begin("##kickle-home",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(std::clamp(size.y/720.f,1.f,1.6f));
    float logo_w=std::min(width-40.f,220.f), logo_h=logo_w*9.f/16.f;
    if(g_kickle_logo.id) {
        ImGui::SetCursorPosX((width-logo_w)*.5f);
        ImGui::Image(tid(g_kickle_logo),ImVec2(logo_w,logo_h));
    } else ImGui::TextUnformatted("KICKLE CUBICLE NES RECOMP");
    const char *pages[]={"Start Game","Display Options","Audio Options","Controls","Cheats","Level Select","Additional Modes","Achievements"};
    float nav_w=std::min(190.f,width*.33f);
    ImGui::BeginChild("##kickle-nav",ImVec2(nav_w,-42),ImGuiChildFlags_Borders|ImGuiChildFlags_NavFlattened);
    for(int i=0;i<8;++i) {
        if(ImGui::Selectable(pages[i],page==i,0,ImVec2(0,30))) {
            page=i;
            focus_options=true;
        }
    }
    ImGui::Spacing();

    if(ImGui::Button("Quit",ImVec2(-1,32))) m->action=LNG_ACTION_QUIT;
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##kickle-options",ImVec2(0,-42),ImGuiChildFlags_Borders|ImGuiChildFlags_NavFlattened);
    ImGui::TextUnformatted(pages[page]); ImGui::Separator(); ImGui::Spacing();
    // A/Enter or a mouse click on a section enters its first option.
    // This also works when selecting the already-active section.
    if(focus_options) {
        ImGui::SetScrollY(0);
        ImGui::SetKeyboardFocusHere();
    }
    if(page==0) {
        ImGui::TextWrapped("%s",m->rom_present ? m->rom_file : "Select your Kickle Cubicle (USA) ROM.");
        if(ImGui::Button("Load ROM",ImVec2(-1,48))) {
            const char *patterns[]={"*.nes"};
            request_rom_picker(m,"Load Kickle Cubicle ROM",patterns,1,"NES ROM",true);
        }
        bool ready=launcher_model_can_launch(m);
        if(!ready) ImGui::BeginDisabled();
        if(ImGui::Button("Start Game",ImVec2(-1,56))) {
            // A normal launch opens the cartridge title. Explicit challenge
            // selections and Level Select still start their requested runs.
            kickle_action(kickle_value("kickle.hardcore") ? "kickle.start_mode" :
                kickle_value("kickle.boss_rush") ? "kickle.start_boss_rush" : "kickle.reset_title");
            m->action=LNG_ACTION_LAUNCH;
        }
        if(!ready) { ImGui::EndDisabled(); ImGui::TextWrapped("A matching original ROM is required to start."); }
        ImGui::Spacing();
        ImGui::TextWrapped("Choose display settings, cheats and additional modes before starting.");
    } else if(page==1) {
        draw_display_controls(m,th);
        const auto *filter=kickle_item("kickle.filter");
        int preset=kickle_value("kickle.filter");
        if(filter && ImGui::Combo("Display Filter",&preset,filter->choices,(int)filter->choice_count))
            g_game_callbacks->set_value(g_game_callbacks->context,filter,preset);
        kickle_toggle("Widescreen Mode","kickle.widescreen");
        // Isometric mode is retained in the renderer, pending further work.
    } else if(page==2) {
        draw_audio_controls(m,th);
    } else if(page==3) {
        ImGui::TextWrapped("Choose keyboard or controller, then Configure to remap buttons and shortcuts.");
        draw_player_panel(m,th,0,ImGui::GetContentRegionAvail().x);
    } else if(page==4) {
        kickle_toggle("Infinite Lives","kickle.infinite_lives");
        ImGui::TextWrapped("Keep nine spare lives. Death and respawning still work normally.");
        kickle_toggle("Invincibility","kickle.invincibility");
        ImGui::TextWrapped("Prevent lethal enemy and hazard collisions.");
        kickle_toggle("Freeze Timer","kickle.freeze_timer");
        ImGui::TextWrapped("Stop the level countdown at its current value. Completion time bonuses still count down normally.");
        ImGui::TextWrapped("Cheats are unavailable during a hardcore run.");
    } else if(page==5) {
        bool locked=kickle_value("kickle.hardcore")!=0;
        if(locked) ImGui::BeginDisabled();
        const auto *world_item=kickle_item("kickle.world");
        const auto *level_item=kickle_item("kickle.level");
        int world=kickle_value("kickle.world");
        if(world_item && ImGui::Combo("World",&world,world_item->choices,(int)world_item->choice_count))
            g_game_callbacks->set_value(g_game_callbacks->context,world_item,world);
        int level=kickle_value("kickle.level");
        // Special Zones have thirty rounds; Cake has sixteen, other lands seventeen.
        if(level_item && ImGui::SliderInt("Level",&level,1,world==4 ? 30 : (world==2 ? 16 : 17)))
            g_game_callbacks->set_value(g_game_callbacks->context,level_item,level);
        bool ready=launcher_model_can_launch(m);
        if(!ready) ImGui::BeginDisabled();
        if(ImGui::Button("Start Selected Level",ImVec2(-1,48)) && kickle_action("kickle.start_level"))
            m->action=LNG_ACTION_LAUNCH;
        if(!ready) ImGui::EndDisabled();
        if(locked) ImGui::EndDisabled();
        ImGui::TextWrapped(locked ? "Level Select is unavailable in 1 Life, 1 Credit Clear mode." : "Starts a fresh run at this puzzle. Special Zones selects the 30 post-game rounds. Boss Rush is turned off.");
        if(!ready) ImGui::TextWrapped("Load your matching ROM on the Start Game page first.");
    } else if(page==6) {
        kickle_toggle("1 Life, 1 Credit Clear (Experimental)","kickle.hardcore");
        ImGui::TextWrapped("Hardcore: one life, no cheats, rewind, fast-forward or save/load states.");
        ImGui::TextWrapped("Caution: a complete hardcore run has not been verified. Bugs may interrupt or end your attempt.");
        kickle_toggle("Boss Rush","kickle.boss_rush");
        ImGui::TextWrapped("Boss Rush visits all four bosses. Start Game begins the selected mode.");
    } else {
        static bool confirm_reset=false;
        kickle_toggle("Enable Achievements","kickle.achievements_enabled");
        ImGui::TextWrapped("Disabling pauses new unlocks and notifications. Earned achievements are kept.");
        if(!confirm_reset) {
            if(ImGui::Button("Reset Achievements")) {
                kickle_action("kickle.cancel_achievement_reset");
                kickle_action("kickle.reset_achievements");
                confirm_reset=true;
            }
        } else {
            ImGui::TextWrapped("Erase every local unlock and start earning them again?");
            if(ImGui::Button("Confirm Reset")) { kickle_action("kickle.reset_achievements"); confirm_reset=false; }
            ImGui::SameLine();
            if(ImGui::Button("Cancel")) { kickle_action("kickle.cancel_achievement_reset"); confirm_reset=false; }
        }
        ImGui::Separator();
        int earned=0,total=0;
        for(size_t i=0;i<g_game_item_count;++i) if(!strcmp(g_game_items[i].section,"Achievements")) {
            ++total; earned+=kickle_value(g_game_items[i].key)!=0;
        }
        ImGui::Text("%d / %d unlocked locally",earned,total);
        ImGui::TextWrapped("Assists are allowed. Regular challenges exclude Boss Rush, which has its own achievement. No website account or connection is used.");
        ImGui::Spacing();
        for(size_t i=0;i<g_game_item_count;++i) {
            const auto &item=g_game_items[i];
            if(strcmp(item.section,"Achievements")) continue;
            bool unlocked=kickle_value(item.key)!=0;
            ImGui::PushID(item.key);
                        int badge_id=kickle_badge_index(item.key);
            if(badge_id>=0 && g_kickle_badges[badge_id][unlocked ? 1 : 0].id) {
                ImGui::Image(tid(g_kickle_badges[badge_id][unlocked ? 1 : 0]),ImVec2(64,64));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextWrapped("%s",item.label);
            ImGui::TextColored(unlocked ? ImVec4(.4f,1.f,.5f,1) : ImVec4(.75f,.8f,.9f,1),"%s",unlocked ? "Unlocked" : "Locked");
            if(item.description && item.description[0]) {
                ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.88f,.93f,1.f,1.f));
                ImGui::TextWrapped("Challenge: %s",item.description);
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup(); ImGui::Separator(); ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0,24));
    }
    ImGui::EndChild();
    ImGui::SetCursorPosX(310);
    ImGui::TextDisabled("D-pad / Stick: Navigate    A: Select    Mouse: Click");
    ImGui::SameLine(); ImGui::TextDisabled("v0.0.4");
    ImGui::End();
    // A launcher-wide option, anchored to the bottom-left of the screen.
    ImGui::SetNextWindowPos(ImVec2(16,size.y-16),ImGuiCond_Always,ImVec2(0,1));
    ImGui::Begin("##kickle-boot-option",nullptr,ImGuiWindowFlags_NoDecoration|
        ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_AlwaysAutoResize);
    bool skip=m->s.skip_launcher!=0;
    bool skip_disabled=!launcher_model_can_launch(m) && !skip;
    if(skip_disabled) ImGui::BeginDisabled();
    if(ImGui::Checkbox("Skip launcher next time",&skip))
        m->s.skip_launcher=skip ? 1 : 0;
    if(skip_disabled) ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Boot directly into the loaded ROM on future launches.");
    ImGui::End();
    ImGui::PopStyleVar(3); ImGui::PopStyleColor(6);
    draw_standalone_builtin_rom_picker(m,th);
}




