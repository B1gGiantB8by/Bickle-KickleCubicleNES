/* Presentation-only title menu. $D1 is the native idle timer, $D2 is
 * Start (8) / Password (4), $D3 the native previous-button latch.
 * The title loop at bank 5:$8125 owns these bytes. No ROM bytes change. */
enum { TM_ROOT, TM_OPTIONS, TM_DISPLAY, TM_AUDIO, TM_CHEATS, TM_MODES,
       TM_LEVEL, TM_CONTROLS, TM_QUIT };
static int tm_page, tm_sel, tm_seen, tm_handoff, tm_repeat;
static uint8_t tm_prev;
typedef struct { const char *label, *key; int page, min, max, step; } TitleItem;
#define PAGE(label,p) {label,NULL,p,0,0,0}
#define VALUE(label,key,lo,hi,step) {label,key,-1,lo,hi,step}
static const TitleItem tm_root[] = {
    PAGE("START",-2), PAGE("PASSWORD",-3), PAGE("OPTIONS",TM_OPTIONS),
    PAGE("ADDITIONAL MODES",TM_MODES), PAGE("ACHIEVEMENTS",-6),
    PAGE("QUIT",TM_QUIT) };
static const TitleItem tm_options[] = {
    PAGE("DISPLAY",TM_DISPLAY), PAGE("AUDIO",TM_AUDIO), PAGE("CHEATS",TM_CHEATS),
    PAGE("LEVEL SELECT",TM_LEVEL), PAGE("CONTROLS",TM_CONTROLS),
    PAGE("LOAD SAVE STATE",-7), PAGE("BACK",TM_ROOT) };
static const TitleItem tm_display[] = {
    VALUE("SCREEN","display.fullscreen",0,2,1),
    VALUE("WINDOW SCALE","display.window_scale",1,8,1),
    VALUE("INTEGER SCALE","graphics.integer_scale",0,1,1),
    VALUE("SMOOTHING","graphics.linear_filter",0,1,1),
    VALUE("WIDESCREEN EXP.","kickle.widescreen",0,1,1),
    VALUE("BEZEL MODE","kickle.bezel",0,1,1),
    VALUE("FILTER","kickle.filter",0,4,1),
    VALUE("LAUNCHER","launcher.enabled",0,1,1), PAGE("BACK",TM_OPTIONS) };
static const TitleItem tm_audio[] = {
    VALUE("AUDIO","audio.enabled",0,1,1), VALUE("VOLUME","audio.volume",0,100,5), PAGE("BACK",TM_OPTIONS) };
static const TitleItem tm_cheats[] = {
    VALUE("INFINITE LIVES","kickle.infinite_lives",0,1,1),
    VALUE("INVINCIBILITY","kickle.invincibility",0,1,1),
    VALUE("FREEZE TIMER","kickle.freeze_timer",0,1,1), PAGE("BACK",TM_OPTIONS) };
static const TitleItem tm_modes[] = {
    {"NORMAL GAME","kickle.start_mode",-4}, {"BOSS RUSH","kickle.start_boss_rush",-4},
    {"1 LIFE EXPERIMENTAL","kickle.start_hardcore",-4}, PAGE("BACK",TM_ROOT) };
static const TitleItem tm_level[] = {
    VALUE("WORLD","kickle.world",0,4,1), VALUE("LEVEL","kickle.level",1,30,1),
    {"START LEVEL","kickle.start_level",-4}, PAGE("BACK",TM_OPTIONS) };
static const TitleItem tm_quit[] = { PAGE("CANCEL",TM_ROOT), PAGE("QUIT TO DESKTOP",-5) };
static const char *const tm_headers[] = {"", "OPTIONS", "DISPLAY", "AUDIO", "CHEATS",
    "ADDITIONAL MODES", "LEVEL SELECT", "CONTROLS", "QUIT GAME?"};
static const TitleItem *title_items(int *count)
{
#define LIST(p,a) case p: *count=sizeof(a)/sizeof(a[0]); return a
    switch(tm_page) {
        LIST(TM_ROOT,tm_root); LIST(TM_OPTIONS,tm_options); LIST(TM_DISPLAY,tm_display);
        LIST(TM_AUDIO,tm_audio); LIST(TM_CHEATS,tm_cheats); LIST(TM_MODES,tm_modes);
        LIST(TM_LEVEL,tm_level); LIST(TM_QUIT,tm_quit);
        default: *count=0; return NULL;
    }
#undef LIST
}
static int title_logo_showing(void)
{
    return cyc_render_nametable(0x20C8)==0 && cyc_render_nametable(0x20E8)==1;
}
static int title_showing(void)
{
    /* Require both title strings and its original CHR/logo identities.
     * Gameplay/password screens reuse the font but not this nametable. */
    static const uint8_t start[]={0xF8,0xF9,0xE6,0xF7,0xF9};
    static const uint8_t password[]={0xF5,0xE6,0xF8,0xF8,0xFC,0xF4,0xF7,0xE9};
    static const uint8_t resume[]={0xE8,0xF4,0xF3,0xF9,0xEE,0xF3,0xFA,0xEA};
    for(int i=0;i<5;++i) if(cyc_render_nametable(0x228C+i)!=start[i]) return 0;
    int pass=1, cont=1;
    for(int i=0;i<8;++i) {
        int tile=cyc_render_nametable(0x22CC+i);
        pass &= tile==password[i]; cont &= tile==resume[i];
    }
    return (pass || cont) && title_logo_showing();
}
static void title_menu_reset(void)
{
    tm_page=TM_ROOT; tm_sel=tm_seen=tm_handoff=tm_repeat=0;
    tm_prev=0;
}
static void title_go(int page) { tm_page=page; tm_sel=0; tm_repeat=0; }
static int title_get(const char *key, int *value)
{
    RecompRuntimeUiItem item={.key=key};
    if(!strncmp(key,"kickle.",7)) return checkbox_get(NULL,&item,value);
    return cyc_ui_setting_get(key,value);
}
static void title_change(const TitleItem *it,int dir)
{
    int value;
    if(!title_get(it->key,&value)) return;
    int maximum=it->max;
    if(!strcmp(it->key,"kickle.level")) maximum=level_limits[selected_world];
    value+=dir*it->step;
    if(value>maximum) value=it->min;
    if(value<it->min) value=maximum;
    if(!strncmp(it->key,"kickle.",7)) {
        RecompRuntimeUiItem item={.key=it->key};
        if(checkbox_set(NULL,&item,value)) cyc_ui_save_settings();
    } else cyc_ui_setting_set(it->key,value);
}
static void title_menu_frame(void)
{
    uint8_t buttons=hw_get_controller(0), pressed=buttons & ~tm_prev;
    tm_prev=buttons;
    int showing=title_showing();
    if(!showing) {
        if(!title_logo_showing()) tm_seen=tm_handoff=0;
        /* Let the cinematic finish normally. The launcher must not queue
         * game-start taps for a normal launch; Start can still skip it. */
        return;
    }
    if(pending_level || reset_queued || tm_handoff) return;
    if(!tm_seen) { title_go(TM_ROOT); tm_seen=1; }
    /* Keep the native menu animation/music running, but prevent its idle
     * timeout and navigation from selecting a game behind a settings page. */
    cyc_mod_poke(0xD1,0);
    cyc_set_controller(0,0); cyc_set_controller(1,0);
    tm_repeat=(buttons&0x0F) ? tm_repeat+1 : 0;
    if(tm_repeat>=24 && tm_repeat%6==0) pressed|=buttons&0x0F;
    if(pressed&0x40) {
        if(tm_page==TM_ROOT) return;
        title_go(tm_page==TM_OPTIONS || tm_page==TM_MODES || tm_page==TM_QUIT ? TM_ROOT : TM_OPTIONS);
        return;
    }
    if(tm_page==TM_CONTROLS) return;
    int count; const TitleItem *list=title_items(&count);
    if(pressed&0x08) tm_sel=(tm_sel+count-1)%count;
    if(pressed&0x24) tm_sel=(tm_sel+1)%count;
    const TitleItem *it=&list[tm_sel];
    if(it->page==-1) {
        if(pressed&0x02) title_change(it,-1);
        if(pressed&0x91) title_change(it,1);
        return;
    }
    if(!(pressed&0x90)) return;
    if(it->page==TM_CONTROLS) { cyc_ui_open_controls(); return; }
    if(it->page>=0) { title_go(it->page); return; }
    if(it->page==-2 || it->page==-3) {
        cyc_mod_poke(0xD2,it->page==-2 ? 8 : 4);
        cyc_mod_poke(0xD3,0);
        cyc_set_controller(0,0x10);
        tm_handoff=1;
    } else if(it->page==-4) {
        RecompRuntimeUiItem item={.key=it->key};
        if(!strcmp(it->key,"kickle.start_mode")) selected_hardcore=0;
        if(level_action(NULL,&item)) tm_handoff=1;
    } else if(it->page==-5) cyc_ui_quit();
    else if(it->page==-6) cyc_ui_open_achievements();
    else if(it->page==-7) cyc_ui_load_save_state();
}
/* Glyphs come from Kickle's active title CHR bank; no copied font asset. */
static int tm_stride = 256;
static void title_text(uint32_t *fb,int x,int y,const char *text,uint32_t color)
{
    for(;*text && x<=tm_stride-8;++text,x+=8) {
        unsigned char c=(unsigned char)*text;
        if(c>='a' && c<='z') c-=32;
        int tile=c>='A' && c<='Z' ? 0xE6+c-'A' : c>='0' && c<='9' ? 0xDC+c-'0' : -1;
        for(int dy=0;dy<8;++dy) {
            unsigned bits=0;
            if(tile>=0) {
                unsigned p=cyc_render_line_bg_table(200)+tile*16+dy;
                /* Kickle stores its font strokes as colour zero and its
                 * surrounding tile as colour one: decode the zero pixels. */
                bits=(uint8_t)~(cyc_render_chr(p)|cyc_render_chr(p+8));
            } else if(c=='-' && dy==3) bits=0x7C;
            else if(c=='.' && dy==6) bits=0x18;
            else if(c==':' && (dy==2 || dy==5)) bits=0x18;
            else if(c=='!' && (dy<4 || dy==6)) bits=0x18;
            else if(c=='/' && dy<7) bits=0x02<<dy;
            else if(c==',' && (dy==5 || dy==6)) bits=dy==5 ? 0x18 : 0x10;
            for(int dx=0;dx<8;++dx) if(bits&(0x80>>dx)) fb[(y+dy)*tm_stride+x+dx]=color;
        }
    }
}
static const uint32_t *title_menu_present(void *ctx,int *w,int *h)
{
    if(!title_logo_showing() || tm_handoff || pending_level) return kickle_voxel_present(ctx,w,h);
    static uint32_t picture[CYC_VIDEO_MAX_WIDTH*240];
    int width=cyc_video_width(), origin=(width-256)/2;
    int card=width>=336 ? 320 : width-16;
    int left=(width-card)/2, right=left+card;
    tm_stride=width;
    const uint32_t *native=cyc_frame_argb();
    uint32_t bg=native[140*256], white=0xFF193C68, blue=0xFF9B174E;
    for(int y=0;y<240;++y) {
        for(int x=0;x<width;++x) picture[y*width+x]=native[y*256];
        memcpy(picture+y*width+origin,native+y*256,256*sizeof(uint32_t));
    }
    if(!title_showing()) { *w=width; *h=240; return picture; }
    /* Retain the cinematic's pink field and animated logo. A light card
     * echoes the logo's white interior without changing the whole scene. */
    for(int y=140;y<232;++y) for(int x=0;x<width;++x) picture[y*width+x]=bg;
    for(int y=140;y<232;++y) for(int x=left;x<right;++x) {
        if((y<143 || y>=229) && (x<left+3 || x>=right-3)) continue;
        picture[y*width+x]=(y==140 || y==231 || x==left || x==right-1) ? 0xFFCF739D : 0xFFFFF4E8;
    }
    if(tm_page!=TM_ROOT) title_text(picture,(width-(int)strlen(tm_headers[tm_page])*8)/2,140,tm_headers[tm_page],blue);
    if(tm_page==TM_CONTROLS) {
        title_text(picture,left+16,160,"DPAD OR ARROWS  MOVE",white);
        title_text(picture,left+16,174,"A OR START     CHOOSE",white);
        title_text(picture,left+16,188,"B              BACK",white);
        title_text(picture,left+16,204,"REMAP IN LAUNCHER",blue);
    } else {
        int count; const TitleItem *list=title_items(&count);
        for(int i=0;i<count;++i) {
            int y=(tm_page==TM_ROOT ? 148 : count>=7 ? 152 : 156)+i*(count>=9 ? 8 : count>=8 ? 9 : count==7 ? 10 : 12);
            const TitleItem *it=&list[i];
            if(i==tm_sel) for(int dy=-2;dy<9;++dy) for(int x=left;x<right;++x)
                picture[(y+dy)*width+x]=0xFFFFD5E5;
            const char *label=tm_page==TM_ROOT && i==1 && (cyc_mod_peek(0xDC)&0x80) ? "CONTINUE" : it->label;
            title_text(picture,left+16,y,label,i==tm_sel ? blue : white);
            if(i==tm_sel) for(int dy=0;dy<7;++dy) for(int dx=0;dx<=3-abs(dy-3);++dx) picture[(y+dy)*width+left+4+dx]=blue;
            if(it->page==-1) {
                int value; char text[24];
                if(!title_get(it->key,&value)) snprintf(text,sizeof(text),"N A");
                else if(!strcmp(it->key,"display.fullscreen")) {
                    const char *names[]={"WINDOWED","BORDERLESS","EXCLUSIVE"};
                    snprintf(text,sizeof(text),"%s",names[value]);
                }
                else if(!strcmp(it->key,"kickle.world")) {
                    const char *names[]={"GARDEN","FRUIT","CAKE","TOY","SPECIAL"};
                    snprintf(text,sizeof(text),"%s",names[value]);
                }
                else if(!strcmp(it->key,"kickle.filter")) snprintf(text,sizeof(text),"%d",value);
                else if(!strcmp(it->key,"kickle.bezel") && enabled) snprintf(text,sizeof(text),"N A");
                else if(!strcmp(it->key,"launcher.enabled")) snprintf(text,sizeof(text),"%s",value ? "ENABLED" : "DISABLED");
                else if(it->max==1) snprintf(text,sizeof(text),"%s",value ? "ON" : "OFF");
                else snprintf(text,sizeof(text),"%d",value);
                title_text(picture,right-8-(int)strlen(text)*8,y,text,white);
            }
        }
        if(tm_page==TM_DISPLAY) {
            char text[28]; snprintf(text,sizeof(text),"FILTER %s",kickle_filter_names[kickle_filter_mode]);
            title_text(picture,left+8,224,text,blue);
        }
    }
    if(tm_page!=TM_DISPLAY) title_text(picture,left+8,224,tm_page==TM_ROOT ? "A START CHOOSE   DPAD MOVE" : "B BACK   DPAD CHANGE",blue);
    *w=width; *h=240; return picture;
}
