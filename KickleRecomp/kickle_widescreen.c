#include "cyc_host_extras.h"
#include "cyc_render.h"
#include "cyc_video.h"
#include "cyc_mod.h"
#include "cyc_run.h"
#include "cyc_ui.h"
#include "kickle_voxel.h"
#include "cyc_png.h"
#include "../nesrecomp/runner/include/mod_function_hooks.h"
#include "../nesrecomp/runner/include/mod_savestate.h"
#include "recomp_runtime_ui.h"
#include "hw_internal.h"
#include "gale_data.h"
#include "cpu6502.h"
#include <string.h>

/* Fixed puzzle boards have no offscreen actors to simulate. Extend only
 * matching water rows on both sides; keep maps and title screens native. */
static int mode;
static int enabled = 1;
int kickle_bezel_enabled;
static int have_water, uncertain_frames, toy_water_ready;
static int selected_world, selected_level = 1, pending_level, reset_queued, boot_frame;
static int skip_title_requested;
static int infinite_lives, invincibility, freeze_timer, boss_rush;
static int selected_hardcore, hardcore_status; /* 0 normal, 1 running, 2 failed, 3 cleared */
static int achievement_hook(uint16_t address);
static void title_menu_frame(void);
static void title_menu_reset(void);
static bool assists_allowed(void *ctx) { (void)ctx; return hardcore_status == 0; }
/* Suppress only the death bit at verified collision stores. Preserve all
 * other flags and run the original STA instruction with its normal timing. */

static void apply_invincibility(void)
{
    /* Loading/rewinding RAM bypasses the collision-store hooks. B1 bit 1
     * requests player death; preserve completion and all other scene flags. */
    if(invincibility && !hardcore_status)
        cyc_mod_poke(0xB1,cyc_mod_peek(0xB1)&0xFD);
}
static int invincibility_hook(uint16_t addr)
{
    (void)addr;
    if(invincibility && !hardcore_status) cpu.a &= 0xFD;
    return 0;
}
/* Ignore dangerous occupancy only in the player's current/destination
 * hazard checks. Leave the board and enemies intact, and let movement
 * and sprite drawing finish instead of taking the early death branch. */
static int invincibility_hazard_hook(uint16_t addr)
{
    (void)addr;
    if(invincibility && !hardcore_status) cpu.a &= 0x79;
    return 0;
}
/* A frozen enemy can become solid underneath an invincible player.
 * Treat that occupied current cell as floor in player checks only; the
 * actual block and all destination blocks retain their normal collision. */
static int invincibility_overlap_hook(uint16_t addr)
{
    if(!invincibility || hardcore_status || (cpu.a & 0xFE)!=0x0C) return 0;
    if(addr==0xA29B ||
       (cyc_mod_peek(0x0F)==((cyc_mod_peek(0x0402)+8)&0xF0) &&
        cyc_mod_peek(0x0E)==((cyc_mod_peek(0x0404)+8)&0xF0)))
        cpu.a=1;
    return 0;
}
/* Both collision death (B1 bit 1) and the alternate hazard condition
 * (86 bit 5) converge here, before the player is marked dead. Skip to
 * the normal drawing tail: restarting the update repeats the collision,
 * while returning immediately omits the player sprite for that frame. */
static int invincibility_player_death_hook(uint16_t addr)
{
    (void)addr;
    if(invincibility && !hardcore_status) {
        apply_invincibility();
        cpu.pc = 0xA361; /* Draw the living player, then return normally. */
    }
    return 0;
}
/* $CEEE is the normal countdown routine. Completion converts the remaining
 * time through $CF97 directly, so skipping this entry does not stall scoring. */
static int freeze_timer_hook(uint16_t addr)
{
    (void)addr;
    return freeze_timer && !hardcore_status;
}
static int hardcore_death_hook(uint16_t addr)
{
    achievement_hook(addr);
    (void)addr;
    if(hardcore_status) {
        cyc_mod_poke(0xB6,0);
        if(hardcore_status==1) {
            hardcore_status=2;
            cyc_ui_set_toast("1 Life, 1 Credit", "Run ended. Start a new attempt from Additional Modes.");
        }
    }
    return 0;
}
static int boss_entry_pending;
static int gale_mode, selected_gale;
static void apply_gale_mode(void)
{
    /* Retain the old state field for compatibility, but retired remix
     * settings and snapshots must never reactivate the patch. */
    gale_mode = selected_gale = 0;
    if(!hw_cart.prg || hw_cart.prg_len != 131072) return;
    for(unsigned i=0;i<sizeof(gale_changes)/sizeof(gale_changes[0]);++i)
        hw_cart.prg[gale_changes[i].offset] = gale_mode ? gale_changes[i].gale : gale_changes[i].original;
}
static int boss_presentation;
static int boss_input_check, boss_scene_check, combat_check_frames;
static int room_template_ready;
static uint16_t room_pattern[256 * 240];
static uint8_t room_palette[256 * 240], room_bit[256 * 240];
static void snapshot_room(void);
void kickle_motion_reset(void);
/* Rendering/boot state must travel with the machine when rewinding. User
 * preferences (cheats, camera and display settings) remain current. */
static int kickle_state_get(uint8_t *buf, int cap)
{
    int values[] = {2, boss_presentation, boss_rush, boss_entry_pending,
        selected_world, selected_level, pending_level, reset_queued, boot_frame, gale_mode};
    if(cap < (int)sizeof(values)) return -1;
    memcpy(buf,values,sizeof(values));
    return sizeof(values);
}
static int kickle_state_validate(const uint8_t *buf, int len)
{
    int version;
    if(!buf && len==0) return 1; /* states made before this extension */
    if(len != 9*(int)sizeof(int) && len != 10*(int)sizeof(int)) return 0;
    memcpy(&version,buf,sizeof(version));
    return (version==1 && len==9*(int)sizeof(int)) || (version==2 && len==10*(int)sizeof(int));
}
static int kickle_state_set(const uint8_t *buf, int len)
{
    if (enabled) kickle_bezel_enabled=0;
    title_menu_reset();
    kickle_motion_reset();
    int v[10]={0};
    if(!kickle_state_validate(buf,len)) return 0;
    memcpy(v,buf,len);
    boss_presentation=v[1]; boss_rush=v[2]; boss_entry_pending=v[3];
    selected_world=v[4]; selected_level=v[5]; pending_level=v[6];
    reset_queued=v[7]; boot_frame=v[8];
    gale_mode=selected_gale=v[9]!=0;
    apply_gale_mode();
    apply_invincibility();

    toy_water_ready=have_water=uncertain_frames=room_template_ready=0;
    return 1;
}
static int boss_phase_hook(uint16_t addr)
{
    achievement_hook(addr);
    if (addr == 0x9A43 || addr == 0x9AB5 || addr == 0x9F22 || addr == 0x89FF) {
        boss_presentation = (addr == 0x9F22 || addr == 0x89FF) ? 3 : 2;
        snapshot_room();
        return 0;
    }
    boss_presentation = addr == 0x889A ? 1 : addr == 0x9EC2 ? 3 : 2;
    if (addr == 0x9889 || addr == 0x889A || addr == 0x89FF) room_template_ready = 0;
    /* The vine/pumpkin cinematic leaves E8 bit 5 set. E192 branches
     * straight to RTS at E27D while that bit is set, so direction changes
     * animate but never move the player. Release only that cinematic lock
     * at the native combat setup; retain death, pause and other flags. */
    if (addr == 0x9AD3) combat_check_frames = 0;
    if (addr == 0x9AD3 && boss_rush)
        cyc_mod_poke(0xE8, cyc_mod_peek(0xE8) & 0xDF);
    toy_water_ready = have_water = uncertain_frames = 0;
    if (boss_input_check) fprintf(stderr,"boss phase %04x player=%02x C1=%02x DC=%02x B1=%02x\n",addr,cyc_mod_peek(0x400),cyc_mod_peek(0xC1),cyc_mod_peek(0xDC),cyc_mod_peek(0xB1));
    return 0;
}
static const int level_limits[] = {17, 17, 16, 17, 30};
#include "kickle_achievements.inc"
#include "kickle_filters.inc"
/* High-refresh presentation retired; keep reset hooks harmless for old states. */
int kickle_motion_active(void) { return 0; }
void kickle_motion_reset(void) {}
void kickle_motion_draw(SDL_Renderer *r,const uint32_t *p,int w,int h,float a) { (void)r; (void)p; (void)w; (void)h; (void)a; }
static const char *const world_names[] = {"Garden Land", "Fruit Land", "Cake Land", "Toy Land", "Special Zones"};
static int level_hook(uint16_t addr)
{
    boss_presentation = 0;
    if (!pending_level) {
        if (boss_rush) {
            int world = cyc_mod_peek(0x010A);
            if (world < 4) {
                cyc_mod_poke(0x010B, (uint8_t)(level_limits[world] - 1));
                cyc_mod_poke(0x010C, (uint8_t)(level_limits[world] - 1));
                boss_entry_pending = 1;
            } else boss_rush = 0;
        }
        return 0;
    }
    fprintf(stderr, "level-select: hook $%04X world=%d level=%d\n", addr, selected_world, selected_level);
    cyc_mod_poke(0x010A, (uint8_t)selected_world);
    int stage = boss_rush ? level_limits[selected_world] - 1 : selected_level - 1;
    boss_entry_pending = boss_rush;
    cyc_mod_poke(0x010B, (uint8_t)stage);
    cyc_mod_poke(0x010C, (uint8_t)stage);
    cyc_mod_poke(0x0101, selected_world == 3 ? 0x35 : 0x3b);
    if(hardcore_status) cyc_mod_poke(0xB6,0);
    pending_level = 0;
    return 0;
}
/* A fresh title-screen start normally jumps to $8306, which builds a
 * regular-world room. Special Game requires $8314 -> $846C so its board,
 * palette, and spawn are initialized by the same path as normal progression. */
static int special_start_hook(uint16_t addr)
{
    (void)addr;
    if(cyc_mod_peek(0x010A)==4) cpu.pc=0x8314;
    return 0;
}
static int boss_room_hook(uint16_t addr)
{
    (void)addr;
    if (!boss_rush || !boss_entry_pending) return 0;
    /* Finish loading the final room before entering its completion script.
     * The script owns the boss arena setup and the bank/CHR transitions.
     * Jumping to the post-room index bypasses these prerequisites. */
    if (cyc_mod_peek(0x0400) != 2) return 0;
    boss_entry_pending = 0;
    cyc_mod_poke(0xB1, cyc_mod_peek(0xB1) | 1);
    /* Preserve native completion, including the world-specific boss branch. */
    return 0;
}
static void level_frame(void *ctx)
{
    (void)ctx;
    title_menu_frame();
    kickle_voxel_controls();
    apply_invincibility();

    if(hardcore_status) {
        infinite_lives=invincibility=freeze_timer=boss_rush=0;
        cyc_mod_poke(0xB6,0); /* no score/item extra lives */
        if(hardcore_status==2) cyc_set_controller(0,0);
    }
    static int diagnostic_frame;
    if (boss_scene_check && boss_presentation == 2 && cyc_mod_peek(0x400) == 1) {
        if (++combat_check_frames == 120) cyc_mod_poke(0xB1,cyc_mod_peek(0xB1)|1);
    }
    if (boss_input_check && boss_presentation == 2 && cyc_mod_peek(0x400) == 1)
        cyc_set_controller(0, (diagnostic_frame % 120 < 60 ? 0x01 : 0x02) | (boss_scene_check ? 0x80 : 0));
    if (boss_presentation) ++diagnostic_frame;
    if (boss_input_check && boss_presentation && diagnostic_frame % 30 == 0) {
        fprintf(stderr,"boss player:");
        for(int i=0;i<16;++i) fprintf(stderr," %02x",cyc_mod_peek(0x400+i));
        fprintf(stderr," C1=%02x DC=%02x B1=%02x E8=%02x input=%02x\n",cyc_mod_peek(0xC1),cyc_mod_peek(0xDC),cyc_mod_peek(0xB1),cyc_mod_peek(0xE8),cyc_mod_peek(0x86));
    }
    if (reset_queued) {
        reset_queued = 0;
        if (enabled) kickle_bezel_enabled=0;
    title_menu_reset();
        cyc_power_on(0);
        cyc_run_power_on();
        apply_gale_mode();

        toy_water_ready = have_water = uncertain_frames = 0;
        boot_frame = 0;
        if (skip_title_requested) {
            skip_title_requested = 0;
            /* Keep native initialization hidden. Boss Rush also waits for
             * the final room's completion hook, so the old title and world
             * map cannot be presented before the boss opening starts. */
            int boot_limit = boss_rush ? 6000 : 1800;
            while ((pending_level || (boss_rush && (boss_entry_pending ||
                   (!boss_presentation && cyc_mod_peek(0x0400) != 0x41)))) &&
                   boot_frame < boot_limit) {
                int f = boot_frame++;
                cyc_set_controller(0, (f == 120 || f == 121 || f == 300 ||
                                      f == 301 || f == 600 || f == 601) ? 0x10 : 0);
                cyc_run_frame();
            }
            cyc_set_controller(0, 0);
        }
    }
    if (pending_level) {
        /* Override input only for the four automatic Start frames. The host
         * supplies live controller input on every other frame, even if the
         * loader hook has not fired yet. */
        int f = boot_frame++;
        if (f == 120 || f == 121 || f == 300 || f == 301 || f == 600 || f == 601)
            cyc_set_controller(0, 0x10);
        if (boot_frame > 1800) pending_level = 0;
    }
    /* $B6 is the spare-life counter used by both puzzle and boss deaths
     * ($8679/$8BB7). Keep a reserve while allowing normal death/respawn. */
    if (!hardcore_status && infinite_lives && cyc_mod_peek(0xB6) < 9) cyc_mod_poke(0xB6, 9);
    if(hardcore_status==1 && !pending_level && cyc_mod_peek(0x010A)>=4) {
        hardcore_status=3;
        cyc_ui_set_toast("1 Life, 1 Credit", "Clear! All four worlds completed without a death.");
    }
    if (boss_rush && !pending_level && cyc_mod_peek(0x010A) >= 4) {
        boss_rush = 0;
        cyc_ui_set_toast("Boss Rush", "All four bosses cleared");
    }
}
static const CycHostOption level_options[] = {
    { "--boss-scene-check", false, "Diagnostic: capture boss victory scenes" },
    { "--boss-input-check", false, "Diagnostic: exercise boss movement" },
    { "--boss-rush", true, "Start Boss Rush at world 1-4" },
    { "--start-level", true, "Start at WORLD:LEVEL (world 1-4)" },
    { "--voxel-capture", true, "Capture voxel diagnostics at frame 1800 to PATH" },
    { "--voxel-camera", true, "Diagnostic camera PITCH:YAW:ZOOM" },
    { "--voxel-capture-frame", true, "Diagnostic capture frame (default 1800)" },
};
static const char *voxel_capture;
static int voxel_capture_frame = 1800;
static void capture_frame(void *ctx)
{
    achievement_frame();
    static int frame;
    if (!voxel_capture || ++frame != voxel_capture_frame) return;
    kickle_voxel_enabled = 1;
    int w, h;
    const uint32_t *pixels = kickle_voxel_present(ctx, &w, &h);
    cyc_write_png(voxel_capture, pixels, w, h);
    uint32_t atlas[128 * 512];
    for (int i = 0; i < 128 * 512; ++i) atlas[i] = cyc_render_color(0);
    for (int pal = 0; pal < 4; ++pal) for (int tile = 0; tile < 256; ++tile)
        cyc_render_tile(atlas, 128, 512, (tile % 16) * 8,
                        pal * 128 + (tile / 16) * 8,
                        cyc_render_line_bg_table(100) + tile * 16, pal, 0, NULL, NULL);
    cyc_write_png("voxel-atlas.png", atlas, 128, 512);
    uint32_t sprites[128*128];
    memset(sprites, 0, sizeof(sprites));
    for (int t = 0; t < 128; ++t) {
        int tile = t*2;
        cyc_render_tile(sprites,128,128,(t%16)*8,(t/16)*16,0x1000+tile*16,5,0,NULL,NULL);
        cyc_render_tile(sprites,128,128,(t%16)*8,(t/16)*16+8,0x1000+(tile+1)*16,5,0,NULL,NULL);
    }
    cyc_write_png("voxel-sprites.png",sprites,128,128);
    FILE *map = fopen("voxel-tiles.txt", "w");
    if (map) {
        for (int y = 0; y < 32; ++y)
            fprintf(map, "scroll %d: %d %d\n", y, cyc_render_line_scroll_x(y), cyc_render_line_scroll_y(y));
        for (int y = 24; y < 240; y += 8) {
            fprintf(map, "y=%03d:", y);
            int sx = cyc_render_line_scroll_x(y), sy = cyc_render_line_scroll_y(y) % 480;
            for (int x = 0; x < 256; x += 8) {
                int px = (sx+x)&511;
                unsigned base = 0x2000+(px/256)*0x400+(sy/240)*0x800;
                fprintf(map," %02X",cyc_render_nametable(base+(sy%240)/8*32+(px%256)/8));
            }
            fprintf(map,"\n");
        }
        fclose(map);
    }
    FILE *f = fopen("voxel-oam.txt", "w");
    const uint8_t *oam = cyc_render_oam();
    if (f) {
        for (int i = 0; i < 64; ++i)
            fprintf(f, "%d: x=%d y=%d tile=%02X attr=%02X\n", i, oam[i*4+3], oam[i*4]+1, oam[i*4+1], oam[i*4+2]);
        fclose(f);
    }
}
static bool level_option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--boss-scene-check")) { boss_scene_check = boss_input_check = 1; return true; }
    if (!strcmp(name, "--boss-input-check")) { boss_input_check = 1; return true; }
    if (!strcmp(name, "--boss-rush")) {
        int world;
        if (sscanf(value, "%d", &world) != 1 || world < 1 || world > 4) return false;
        selected_world = world - 1; selected_level = 1;
        skip_title_requested = 1;
        boss_rush = pending_level = reset_queued = 1;
        return true;
    }
    if (!strcmp(name, "--voxel-capture")) { voxel_capture = value; return true; }
    if (!strcmp(name, "--voxel-capture-frame")) {
        return sscanf(value, "%d", &voxel_capture_frame) == 1 && voxel_capture_frame > 0;
    }
    if (!strcmp(name, "--voxel-camera")) {
        int pitch, yaw, zoom;
        if (sscanf(value, "%d:%d:%d", &pitch, &yaw, &zoom) != 3 ||
            pitch < 20 || pitch > 85 || yaw < -180 || yaw > 180 || zoom < 50 || zoom > 180)
            return false;
        kickle_voxel_pitch = pitch; kickle_voxel_yaw = yaw; kickle_voxel_zoom = zoom;
        return true;
    }
    int world, level;
    if (strcmp(name, "--start-level") || sscanf(value, "%d:%d", &world, &level) != 2 ||
        world < 1 || world > 5 || level < 1 || level > level_limits[world - 1]) return false;
    selected_world = world - 1; selected_level = level;
    pending_level = reset_queued = 1;
    return true;
}
static uint32_t last_water[32 * 16];
static uint16_t toy_water_pattern[32 * 16];
static uint8_t toy_water_palette[32 * 16], toy_water_bit[32 * 16];
static void toy_water_sample(int capture, int source_x, int source_y)
{
    for(int y=0;y<16;++y) for(int x=0;x<32;++x) {
        int i=((y+source_y)%16)*32+(x+source_x)%32;
        if(capture) {
            int sy=source_y+y;
            int px=(cyc_render_line_scroll_x(sy)+(x%16)+source_x)&511;
            int py=cyc_render_line_scroll_y(sy)%480;
            unsigned base=0x2000+(px/256)*0x400+(py/240)*0x800;
            int tx=(px%256)/8, ty=(py%240)/8;
            unsigned tile=cyc_render_nametable(base+ty*32+tx);
            unsigned attr=cyc_render_nametable(base+0x3c0+(ty/4)*8+tx/4);
            toy_water_pattern[i]=cyc_render_line_bg_table(sy)+tile*16+(py&7);
            toy_water_palette[i]=(attr>>(((ty&2)<<1)|(tx&2)))&3;
            toy_water_bit[i]=7-(px&7);
        }
        unsigned pattern=toy_water_pattern[i], bit=toy_water_bit[i];
        unsigned color=((cyc_render_chr(pattern)>>bit)&1)
            | (((cyc_render_chr(pattern+8)>>bit)&1)<<1);
        last_water[i]=cyc_render_color(color ? toy_water_palette[i]*4+color : 0);
    }
}
const uint32_t *kickle_flat_present(int *w, int *h)
{
    static uint32_t cleaned[2048 * 240];
    const uint32_t *frame = cyc_render_present(w, h);
    const uint32_t *native = cyc_frame_argb();
    if (*w > 2048 || *h != 240) return frame;
    {
        /* Present all submitted sprites in every scene without the NES
         * eight-sprites-per-row display limit. Keep PPU/game timing intact
         * and run the transition border cleanup after drawing sprites. */
        static uint8_t background[2048 * 240];
        int x0=(*w-256)/2;
        memcpy(cleaned,frame,(size_t)*w * *h * sizeof(uint32_t));
        memset(background,0,(size_t)*w * *h);
        const uint8_t *opaque=cyc_frame_bg_opaque();
        for (int y=0; y<240; ++y)
            memcpy(background+y * *w+x0,opaque+y*256,256);
        cyc_render_sprites(cleaned,*w,*h,x0,background,NULL,NULL);
        frame = cleaned;
    }
    /* These outer nametable columns contain wipe scratch tiles. Keep them
     * hidden throughout native puzzle presentation, including transition
     * boundaries where there are no black bands to detect yet. */
    if (!enabled && !boss_presentation) {
        int x0=(*w-256)/2;
        if (frame != cleaned)
            memcpy(cleaned,frame,(size_t)*w * *h * sizeof(uint32_t));
        for (int y=0;y<240;++y) for (int x=0;x<8;++x) {
            cleaned[y * *w+x0+x]=0xff000000;
            cleaned[y * *w+x0+248+x]=0xff000000;
        }
        return cleaned;
    }
    if (boss_presentation) return frame;
    int black = 0, matching = 0, hud = 0, water = 0;
    for (int y = 8; y < 232; ++y) {
        for (int x = 1; x < 255; ++x)
            black += !(native[y*256+x] & 0xffffff);
        matching += !memcmp(native+y*256+8, native+y*256+232, 8*sizeof(uint32_t));
        uint32_t c = native[y*256+8];
        water += ((c>>16)&255) < 80 && ((c>>8)&255) < 160 && (c&255) > 140;
    }
    for (int y = 8; y < 16; ++y) for (int x = 8; x < 48; ++x)
        hud += (native[y*256+x] & 0xffffff) == 0xffffff;
    int clearing = black > 224*254*95/100;
    int blue_samples = 0;
    for (int y=32; y<232; y+=8) for (int x=16; x<240; x+=8) {
        uint32_t c=native[y*256+x];
        int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
        blue_samples += b>r+32 && b>g+32;
    }
    /* During upload/balloon arrival the outer tile columns are temporary.
     * They need not match each other and can contain platform fragments.
     * The compositor clears have_water on maps; the HUD ends this cleanup. */
    int rebuilding = !clearing && hud<15 && blue_samples>=224 && have_water;
    /* Native-width wipes leave black horizontal bands around the surviving
     * board. Detect that final picture, not live RAM or a particular edge
     * color: scanline snapshots can belong to the frame before RAM advances.
     * This applies equally to closing and opening wipes, including the fully
     * black midpoint. Normal puzzle play has no such black horizontal band. */
    int black_rows=0;
    for(int y=0;y<240;++y) {
        int row_black=0;
        for(int x=8;x<248;++x) row_black+=!(native[y*256+x]&0xffffff);
        black_rows+=row_black>=232;
    }
    int respawn_gutter = *w==256 &&
        (clearing || (black_rows>=2 && blue_samples>=8));
    if (!clearing && !rebuilding && !respawn_gutter) return frame;
    if (frame != cleaned)
        memcpy(cleaned, frame, (size_t)*w * *h * sizeof(uint32_t));
    int x0 = (*w-256)/2;
    for (int y = 0; y < 240; ++y) {
        if(respawn_gutter) {
            for(int x=0;x<8;++x) {
                cleaned[y * *w+x0+x]=0xff000000;
                cleaned[y * *w+x0+248+x]=0xff000000;
            }
        } else if (clearing) {
            cleaned[y * *w + x0] = 0xff000000;
            cleaned[y * *w + x0 + 255] = 0xff000000;
        } else {
            /* The inner edge samples can contain platforms during balloon
             * arrival. Use the compositor's repeated water patch instead of
             * copying room geometry into the transition border. */
            const uint32_t *water_row = last_water + (y % 16) * 32;
            for (int x = 0; x < 8; ++x) {
                cleaned[y * *w+x0+x] = water_row[x];
                cleaned[y * *w+x0+248+x] = water_row[(248+x)%32];
            }
        }
    }
    return cleaned;
}
/* Store pristine room tile identities before dialogue or placed ice changes
 * the nametable. Decode through the current CHR mapping as the arena changes. */
static unsigned room_pixel_entry(int x, int y, int snapshot)
{
    int cell = y * 256 + x;
    if (snapshot) {
        int px = (cyc_render_line_scroll_x(y) + x) & 511;
        int py = cyc_render_line_scroll_y(y) % 480;
        unsigned base = 0x2000 + (px / 256) * 0x400 + (py / 240) * 0x800;
        int tx = (px % 256) / 8, ty = (py % 240) / 8;
        unsigned tile = cyc_render_nametable(base + ty * 32 + tx);
        unsigned attr = cyc_render_nametable(base + 0x3c0 + (ty / 4) * 8 + tx / 4);
        room_pattern[cell] = cyc_render_line_bg_table(y) + tile * 16 + (py & 7);
        room_palette[cell] = (attr >> (((ty & 2) << 1) | (tx & 2))) & 3;
        room_bit[cell] = 7 - (px & 7);
    }
    unsigned pattern = room_pattern[cell], bit = room_bit[cell];
    unsigned color = ((cyc_render_chr(pattern) >> bit) & 1) |
                     (((cyc_render_chr(pattern + 8) >> bit) & 1) << 1);
    return color ? room_palette[cell] * 4 + color : 0;
}
static void snapshot_room(void)
{
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 256; ++x)
        room_pixel_entry(x, y, 1);
    room_template_ready = 1;
}
static int compose(uint32_t *out, int w, int h, int x0,
                   const uint32_t *native, void *ctx)
{
    (void)ctx;
    if (!enabled) return 0;
    if (boss_presentation) {
        /* Boss arenas and princess rooms keep their original visible width.
         * Black margins make the native collision boundary unambiguous. */
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) out[y*w+x] = 0xff000000u;
            memcpy(out+y*w+x0,native+y*256,256*sizeof(uint32_t));
        }
        return 1;
    }
    /* Respawn transitions are almost entirely black, with heart sprites.
     * The native leftmost column can retain water while the screen clears.
     * Hide only that column on this narrowly detected transition. */
    int black = 0;
    for (int y = 8; y < h - 8; ++y)
        for (int x = 1; x < 256; ++x)
            black += (native[y * 256 + x] & 0x00ffffffu) == 0;
    if (black > (h - 16) * 255 * 95 / 100) {
        uncertain_frames = 0;
        for (int y = 0; y < h; ++y) {
            uint32_t *dst = out + y * w;
            for (int x = 0; x < w; ++x) dst[x] = 0xff000000u;
            memcpy(dst + x0 + 1, native + y * 256 + 1, 255 * sizeof(uint32_t));
        }
        return 1;
    }
    /* World maps have a long neutral frame near the native left edge.
     * Fade-in palettes turn white into gray; recognize both shades. Reject
     * it before repeated border rows can be mistaken for a water patch,
     * including palette/upload frames where the map interior is still black. */
    int map_frame = 0;
    for (int x=4; x<24; ++x) {
        int white = 0;
        for (int y=32; y<h-16; ++y)
             {
                uint32_t c=native[y*256+x];
                int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
                white += r>=64 && r==g && g==b;
            }
        int pink = 0;
        /* The map's pink outline is outside (left of) its white border.
         * Pink floor tiles inside a white platform edge are not a map. */
        for (int y=32; y<h-16; ++y) for (int dx=-3; dx<0; ++dx) {
            uint32_t c=native[y*256+x+dx];
            int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
            pink += r>g+64 && b>g+48 && g*2<r;
        }
        if (white>=128 && pink>=128) { map_frame=1; break; }
    }
    if (map_frame) {
        toy_water_ready=have_water=uncertain_frames=0;
        for (int y=0; y<h; ++y) {
            for (int x=0; x<w; ++x) out[y*w+x]=0xff000000u;
            memcpy(out+y*w+x0,native+y*256,256*sizeof(uint32_t));
        }
        return 1;
    }
    /* Cake Land's first board has only a narrow gutter and scattered holes.
     * Its shadows are not a complete water repeat. Use the water layout from
     * this board's clean completion presentation, tinted by the live gutter.
     * Detect the pink floor as well as the stage so maps and transitions do
     * not inherit the extension merely because their stage index is retained. */
    if(cyc_mod_peek(0x010A)==2 && cyc_mod_peek(0x010B)==0) {
        int pink_floor=0;
        for(int y=32;y<h-8;++y) for(int x=16;x<240;++x) {
            uint32_t c=native[y*256+x];
            int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
            pink_floor+=r>180 && b>180 && r>g+16 && b>g+16;
        }
        if(pink_floor>2048) {
            uint32_t colors[32]={0}; int counts[32]={0}, n=0;
            for(int y=32;y<h-8;++y) for(int x=0;x<8;++x) {
                uint32_t c=native[y*256+x];
                int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
                if(!(b>r+32 && b>g+32)) continue;
                int i=0; while(i<n && colors[i]!=c) ++i;
                if(i==n) { if(n==32) continue; colors[n++]=c; }
                ++counts[i];
            }
            int base=-1, fleck=-1;
            for(int i=0;i<n;++i) if(base<0 || counts[i]>counts[base]) base=i;
            for(int i=0;i<n;++i) if(i!=base && (fleck<0 || counts[i]>counts[fleck])) fleck=i;
            if(base>=0 && fleck>=0 && counts[base]>512 && counts[fleck]>16) {
                static const uint32_t pattern[16]={
                    0x30003000u,0x00030003u,0x00600060u,0,0,0x00600060u,
                    0x80018001u,0,0x00060006u,0x06000600u,0,0x00c000c0u,
                    0,0x80018001u,0x00300030u,0
                };
                for(int y=0;y<16;++y) for(int x=0;x<32;++x)
                    last_water[y*32+x]=colors[(pattern[y]>>x)&1 ? fleck : base];
                have_water=1; uncertain_frames=0;
                for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                    int nx=x-x0;
                    out[y*w+x]=nx>=0 && nx<256 ? native[y*256+nx]
                        : last_water[(y%16)*32+((nx%32+32)%32)];
                }
                return 1;
            }
        }
    }
    /* Toy Land 3 repeats platform skirts at both edges. The band between
     * the HUD and the board is clean water; sample that intact band instead
     * of treating the repeating skirts as the surrounding water pattern. */
    if(cyc_mod_peek(0x010A)==3 || cyc_mod_peek(0x010A)==4) {
        int green_floor=0;
        for(int y=32;y<h-8;y+=4) for(int x=16;x<240;x+=4) {
            uint32_t c=native[y*256+x];
            int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
            green_floor+=(g>160 && g>r+64 && g>b+64) ||
                (cyc_mod_peek(0x010A)==4 && r>160 && b>160 && r>g+64 && b>g+64);
        }
        /* Special Zones use several board palettes. The earlier map/black
         * transition checks and clean-water sampling guard this path; do
         * not require their platforms to be green or magenta. */
        int dense_board=green_floor>128 ||
            (cyc_mod_peek(0x010A)==4 && cyc_mod_peek(0x010C)<30);
        /* Only accept the live gutter's water colors. Moving ice and blue
         * floor objects can otherwise satisfy the sparse two-color test. */
        uint32_t live_colors[32]={0}, live_base=0, live_fleck=0;
        int live_counts[32]={0}, live_n=0, live_best=-1, live_second=-1;
        for(int sy=16;sy<h-8;++sy) for(int sx=0;sx<4;++sx) {
            if(!(cyc_render_line_mask(sy)&2)) continue;
            uint32_t c=native[sy*256+sx];
            int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
            if(!(b>r+32 && b>g+32)) continue;
            int i=0; while(i<live_n && live_colors[i]!=c) ++i;
            if(i==live_n) { if(live_n==32) continue; live_colors[live_n++]=c; }
            ++live_counts[i];
        }
        for(int i=0;i<live_n;++i)
            if(live_best<0 || live_counts[i]>live_counts[live_best]) live_best=i;
        for(int i=0;i<live_n;++i)
            if(i!=live_best && (live_second<0 || live_counts[i]>live_counts[live_second]))
                live_second=i;
        if(live_best>=0) live_base=live_colors[live_best];
        if(live_second>=0) live_fleck=live_colors[live_second];
        int source_x=-1, source_y=-1;
        /* Find an intact sparse water patch, rather than assuming the same
         * HUD gap is free on every layout or scroll frame. Dense ripple rows
         * and platform shadows must not qualify even when they are blue. */
        for(int sy=16;sy+16<=h-8 && source_x<0;sy+=8)
            for(int sx=0;sx+16<=256 && source_x<0;sx+=8) {
                /* Masked left-edge pixels show the backdrop, not this tile. */
                if(sx<8 && !(cyc_render_line_mask(sy)&2)) continue;
                uint32_t colors[2]={0}; int counts[2]={0}, n=0, clean=1;
                for(int dy=0;dy<16 && clean;++dy) for(int dx=0;dx<16;++dx) {
                    uint32_t c=native[(sy+dy)*256+sx+dx];
                    int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
                    if(!(b>r+32 && b>g+32) ||
                       (cyc_mod_peek(0x010A)==4 && live_best>=0 &&
                        c!=live_base && (live_second<0 || c!=live_fleck))) {
                        clean=0; break;
                    }
                    int i=0; while(i<n && colors[i]!=c) ++i;
                    if(i==n) { if(n==2) { clean=0; break; } colors[n++]=c; }
                    ++counts[i];
                }
                if(!clean || n!=2) continue;
                int fleck=counts[0]<counts[1] ? 0 : 1;
                int sparse=counts[fleck]>=8 && counts[fleck]<=48;
                if(cyc_mod_peek(0x010A)==4 && live_best>=0 && colors[1-fleck]!=live_base) sparse=0;
                for(int dy=0;dy<16;++dy) {
                    int light=0;
                    for(int dx=0;dx<16;++dx)
                        light+=native[(sy+dy)*256+sx+dx]==colors[fleck];
                    if(light>4) sparse=0;
                }
                if(sparse) { source_x=sx; source_y=sy; }
            }
        if(source_x>=0 && (dense_board || toy_water_ready)) {
            if(cyc_mod_peek(0x010A)==4) {
                /* Rendered pixels include the exact fetch-time CHR bank and
                 * palette. Re-decoding end-of-frame references can resolve
                 * to platform tiles on Special Zone split-screen frames. */
                for(int dy=0;dy<16;++dy) for(int dx=0;dx<32;++dx)
                    last_water[((source_y+dy)%16)*32+(source_x+dx)%32]=
                        native[(source_y+dy)*256+source_x+(dx%16)];
            } else toy_water_sample(1, source_x, source_y);
            toy_water_ready=have_water=1; uncertain_frames=0;
            for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                int nx=x-x0;
                out[y*w+x]=nx>=0 && nx<256 ? native[y*256+nx]
                    : last_water[(y%16)*32+((nx%32+32)%32)];
            }
            return 1;
        }
        if(dense_board && (!toy_water_ready || cyc_mod_peek(0x010A)==4)) {
            /* Dense boards expose some water phases only as blank gutter
             * tiles. Accept those only in the dominant live water color,
             * excluding the solid blue shadows along platform edges. */
            uint32_t gutter_colors[32]={0}, water_base=0;
            int gutter_counts[32]={0}, gutter_n=0, gutter_best=-1;
            for(int sy=0;sy<h-8;++sy) for(int sx=0;sx<4;++sx) {
                uint32_t c=native[sy*256+sx];
                int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
                if(!(b>r+32 && b>g+32)) continue;
                int i=0; while(i<gutter_n && gutter_colors[i]!=c) ++i;
                if(i==gutter_n) { if(gutter_n==32) continue; gutter_colors[gutter_n++]=c; }
                ++gutter_counts[i];
            }
            for(int i=0;i<gutter_n;++i)
                if(gutter_best<0 || gutter_counts[i]>gutter_counts[gutter_best]) gutter_best=i;
            if(gutter_best>=0) water_base=gutter_colors[gutter_best];
            int tile_x[4],tile_y[4],complete=1;
            for(int phase=0;phase<4;++phase) {
                tile_x[phase]=tile_y[phase]=-1;
                int blank_x=-1, blank_y=-1;
                for(int sy=(phase/2)*8;sy+8<=h-8 && tile_x[phase]<0;sy+=16)
                    for(int sx=(phase%2)*8;sx+8<=256 && tile_x[phase]<0;sx+=16) {
                        if(sx<8 && !(cyc_render_line_mask(sy)&2)) continue;
                        uint32_t colors[2]={0}; int counts[2]={0},n=0,clean=1;
                        for(int dy=0;dy<8 && clean;++dy) for(int dx=0;dx<8;++dx) {
                            uint32_t c=native[(sy+dy)*256+sx+dx];
                            int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
                            if(!(b>r+32 && b>g+32) ||
                       (cyc_mod_peek(0x010A)==4 && live_best>=0 &&
                        c!=live_base && (live_second<0 || c!=live_fleck))) {
                        clean=0; break;
                    }
                            int i=0; while(i<n && colors[i]!=c) ++i;
                            if(i==n) { if(n==2) { clean=0; break; } colors[n++]=c; }
                            ++counts[i];
                        }
                        if(!clean) continue;
                        if(n==1 && gutter_best>=0 && colors[0]==water_base) {
                            if(blank_x<0) { blank_x=sx; blank_y=sy; }
                            continue;
                        }
                        if(n!=2) continue;
                        int fleck=counts[0]<counts[1] ? 0 : 1;
                        int sparse=counts[fleck]>=2 && counts[fleck]<=12;
                        if(cyc_mod_peek(0x010A)==4 && live_best>=0 && colors[1-fleck]!=live_base) sparse=0;
                        for(int dy=0;dy<8;++dy) {
                            int light=0;
                            for(int dx=0;dx<8;++dx) light+=native[(sy+dy)*256+sx+dx]==colors[fleck];
                            if(light>2) sparse=0;
                        }
                        if(sparse) { tile_x[phase]=sx; tile_y[phase]=sy; }
                    }
                if(tile_x[phase]<0 && blank_x>=0) {
                    tile_x[phase]=blank_x; tile_y[phase]=blank_y;
                }
                /* Dense boards may expose only the first horizontal half in
                 * their gutter. Repeat that clean half rather than rejecting
                 * the entire board or sampling its platform borders. */
                if(tile_x[phase]<0 && phase%2 && tile_x[phase-1]>=0) {
                    tile_x[phase]=tile_x[phase-1];
                    tile_y[phase]=tile_y[phase-1];
                }
                if(tile_x[phase]<0) complete=0;
            }
            if(complete) {
                for(int phase=0;phase<4;++phase) for(int dy=0;dy<8;++dy) for(int dx=0;dx<8;++dx) {
                    int sy=tile_y[phase]+dy;
                    int px=(cyc_render_line_scroll_x(sy)+tile_x[phase]+dx)&511;
                    int py=cyc_render_line_scroll_y(sy)%480;
                    unsigned base=0x2000+(px/256)*0x400+(py/240)*0x800;
                    int tx=(px%256)/8,ty=(py%240)/8;
                    unsigned tile=cyc_render_nametable(base+ty*32+tx);
                    unsigned attr=cyc_render_nametable(base+0x3c0+(ty/4)*8+tx/4);
                    for(int repeat=0;repeat<2;++repeat) {
                        int i=((phase/2)*8+dy)*32+(phase%2)*8+dx+repeat*16;
                        if(cyc_mod_peek(0x010A)==4)
                            last_water[i]=native[sy*256+tile_x[phase]+dx];
                        toy_water_pattern[i]=cyc_render_line_bg_table(sy)+tile*16+(py&7);
                        toy_water_palette[i]=(attr>>(((ty&2)<<1)|(tx&2)))&3;
                        toy_water_bit[i]=7-(px&7);
                    }
                }
                toy_water_ready=have_water=1;
            }
        }
        /* While actors cover the clean band, retain its tile references. Once
         * clear begins exposing water again, the path above refreshes their
         * scroll phase and tile mapping before drawing this frame. */
        if(toy_water_ready && have_water) {
            if(cyc_mod_peek(0x010A)!=4) toy_water_sample(0, 0, 0);
            uncertain_frames=0;
            for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                int nx=x-x0;
                out[y*w+x]=nx>=0 && nx<256 ? native[y*256+nx]
                    : last_water[(y%16)*32+((nx%32+32)%32)];
            }
            return 1;
        }
    }
    /* Keep the original edge detector as the primary path. Water palettes
     * vary, so normal boards must not depend on absolute RGB thresholds. */
    int matching = 0;
    for (int y = 32; y < h - 8; ++y)
        if (!memcmp(native+y*256,native+y*256+224,32*sizeof(uint32_t)))
            ++matching;
    int best_x = 0, best_y = -1, best_count = 0;
    for (int candidate = 32; candidate + 16 <= h - 8; candidate += 16) {
        int count = 0;
        for (int y = 32; y + 16 <= h - 8; y += 16) {
            int equal = 1;
            for (int dy = 0; dy < 16; ++dy) {
                const uint32_t *a = native+(candidate+dy)*256;
                const uint32_t *b = native+(y+dy)*256;
                if (memcmp(a,b,32*sizeof(uint32_t)) ||
                    memcmp(a,b+224,32*sizeof(uint32_t))) {
                    equal = 0; break;
                }
            }
            count += equal;
        }
        if (count > best_count) { best_count=count; best_y=candidate; }
    }
    int stable = matching >= 64 && best_count >= 3;
    /* Interior water remains visible during arrival and completion even when
     * stage indices advance or edge strips contain geometry. Map frames were
     * excluded above; require repeated textured water before refreshing it. */
    int puzzle_water_visible = 0;
    {
        int blue_samples = 0;
        for (int y=32; y<h-8; y+=8) for (int x=0; x<256; x+=8) {
            uint32_t c=native[y*256+x];
            int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
            blue_samples += b>r+32 && b>g+32;
        }
        /* Player state changes with animation, death and motion. The surrounding
         * water identifies arrival and gameplay; the map frame is excluded above. */
        int hud=0;
        for (int y=8; y<16; ++y) for (int x=8; x<48; ++x)
            hud+=(native[y*256+x]&0xffffffu)==0xffffffu;
        int gutter_blue=0;
        for(int y=32;y<h-8;++y) for(int x=0;x<8;++x) {
            uint32_t c=native[y*256+x];
            int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
            gutter_blue+=b>r+32 && b>g+32;
        }
        puzzle_water_visible = blue_samples>=256 || (blue_samples>=32 && hud>=15)
            || gutter_blue>=(h-40)*8*9/10;
    }
    if (!stable && puzzle_water_visible) {
        /* Dense boards expose water in isolated tiles rather than full patches.
         * Learn its colors from the narrow left gutter and gather each phase. */
        uint32_t colors[64]={0}, patch[512];
        int counts[64]={0}, n=0;
        for (int y=32; y<h-8; ++y) for (int x=0; x<8; ++x) {
            uint32_t c=native[y*256+x];
            int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
            if (!(b>r+32 && b>g+32)) continue;
            int i=0; while(i<n && colors[i]!=c) ++i;
            if(i==n) { if(n==64) continue; colors[n++]=c; }
            ++counts[i];
        }
        int base=0, flecks=-1, complete=n>=2;
        for(int i=1;i<n;++i) if(counts[i]>counts[base]) base=i;
        for(int i=0;i<n;++i) if(i!=base && (flecks<0 || counts[i]>counts[flecks])) flecks=i;
        for(int py=0;py<16 && complete;py+=8) for(int px=0;px<32;px+=8) {
            uint32_t candidates[32][64];
            int votes[32]={0}, candidate_count=0;
            for(int sy=32+py;sy+8<=h-8;sy+=16)
                for(int sx=px;sx+8<=256;sx+=32) {
                    uint32_t tile[64];
                    int clean=1, light=0;
                    for(int dy=0;dy<8;++dy) for(int dx=0;dx<8;++dx) {
                        uint32_t c=native[(sy+dy)*256+sx+dx];
                        tile[dy*8+dx]=c;
                        if(c!=colors[base] && (flecks<0 || c!=colors[flecks])) clean=0;
                        light+=c!=colors[base];
                    }
                    /* Whole textured tiles exclude actors, lips and solid shadows.
                     * Vote across repeats instead of assembling unrelated rows. */
                    if(!clean || light<2 || light>16) continue;
                    int i=0;
                    while(i<candidate_count && memcmp(candidates[i],tile,sizeof(tile))) ++i;
                    if(i==candidate_count) {
                        if(candidate_count==32) continue;
                        memcpy(candidates[candidate_count++],tile,sizeof(tile));
                    }
                    ++votes[i];
                }
            int best=-1;
            for(int i=0;i<candidate_count;++i) if(best<0 || votes[i]>votes[best]) best=i;
            if(best<0 || votes[best]<2) { complete=0; break; }
            for(int dy=0;dy<8;++dy)
                memcpy(patch+(py+dy)*32+px,candidates[best]+dy*8,8*sizeof(uint32_t));
        }
        if(complete) { memcpy(last_water,patch,sizeof(patch)); stable=1; best_x=-2; }
    }
    if (!stable && puzzle_water_visible) {
        /* Narrow edge gutters can be clean even when a 32-pixel sample
         * reaches a platform. Combine the two complementary pattern halves. */
        for (int cy=32; cy+16<=h-8; cy+=16) {
            int blue=0, textured=0, count=0;
            uint32_t first=native[cy*256];
            for (int dy=0; dy<16; ++dy) for (int dx=0; dx<32; ++dx) {
                int sx=dx<16 ? dx : 240+dx-16;
                uint32_t c=native[(cy+dy)*256+sx];
                int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
                blue+=b>r+32 && b>g+32;
                textured|=c!=first;
            }
            if (blue!=512 || !textured) continue;
            for (int sy=32; sy+16<=h-8; sy+=16) {
                int equal=1;
                for (int dy=0; dy<16; ++dy)
                    if (memcmp(native+(cy+dy)*256,native+(sy+dy)*256,16*sizeof(uint32_t)) ||
                        memcmp(native+(cy+dy)*256+240,native+(sy+dy)*256+240,16*sizeof(uint32_t))) {
                        equal=0; break;
                    }
                count+=equal;
            }
            if (count>=3 && count>best_count) {
                best_count=count; best_x=-1; best_y=cy; stable=1;
            }
        }
    }
    if (!stable && puzzle_water_visible) {
        /* Water uses a base color and flecks. Blue platform shadows also
         * pass an RGB test, so identify the dominant water colors first. */
        uint32_t colors[64] = {0};
        int counts[64] = {0}, color_count = 0;
        for (int y=32; y<h-8; ++y) for (int x=0; x<256; ++x) {
            uint32_t c=native[y*256+x];
            int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
            if (!(b>r+32 && b>g+32)) continue;
            int i=0;
            while (i<color_count && colors[i]!=c) ++i;
            if (i==color_count) {
                if (color_count==64) continue;
                colors[color_count++]=c;
            }
            ++counts[i];
        }
        int base=0, flecks=-1;
        for (int i=1; i<color_count; ++i)
            if (counts[i]>counts[base]) base=i;
        for (int i=0; i<color_count; ++i)
            if (i!=base && (flecks<0 || counts[i]>counts[flecks])) flecks=i;
        best_count = 0;
        for (int cy = 32; cy + 16 <= h - 8; cy += 4)
            for (int cx = 0; cx + 32 <= 256; cx += 8) {
                int blue = 0, textured = 0;
                uint32_t first = native[cy*256+cx];
                for (int dy = 0; dy < 16; ++dy) for (int dx = 0; dx < 32; ++dx) {
                    uint32_t c = native[(cy+dy)*256+cx+dx];
                    blue += c==colors[base] || (flecks>=0 && c==colors[flecks]);
                    textured |= c != first;
                }
                /* Repeated level rows can be half water and half platforms or
                 * actors. Only a nearly pure water patch may fill the margins. */
                if (blue != 512 || !textured) continue;
                int count = 0;
                for (int sy = 32 + cy % 16; sy + 16 <= h - 8; sy += 16)
                    for (int sx = cx % 32; sx + 32 <= 256; sx += 32) {
                        int equal = 1;
                        for (int dy = 0; dy < 16; ++dy)
                            if (memcmp(native+(cy+dy)*256+cx,
                                       native+(sy+dy)*256+sx,32*sizeof(uint32_t))) {
                                equal = 0; break;
                            }
                        count += equal;
                    }
                if (count > best_count) { best_count=count; best_x=cx; best_y=cy; }
            }
        stable = best_count >= 3;
    }
    if (stable) {
        /* Preserve native pattern phase even for an offset sample. */
        for (int dy = 0; dy < 16; ++dy) for (int dx = 0; dx < 32; ++dx)
            if (best_x==-1)
                last_water[((best_y+dy)%16)*32+dx] =
                    native[(best_y+dy)*256+(dx<16 ? dx : 240+dx-16)];
            else if (best_x>=0)
                last_water[((best_y+dy)%16)*32+(best_x+dx)%32] =
                    native[(best_y+dy)*256+best_x+dx];
        have_water = 1;
        uncertain_frames = 0;
    } else {
        /* Palette/upload transitions can invalidate the edge comparison for
         * a few frames. Preserve the previous extension for at most 4 frames,
         * then let genuine map/menu changes return to native presentation. */
        if (have_water && puzzle_water_visible) {
            /* Keep the established patch across actor overlap. Do not mask the
             * native edge columns: they contain real geometry on this board. */
            uncertain_frames = 0;
            stable = 1;
        } else if (!have_water || ++uncertain_frames > 4) return 0;
    }
    for (int y = 0; y < h; ++y) {
        const uint32_t *row = native + y * 256;
        const uint32_t *water = last_water + (y % 16) * 32;
        uint32_t *dst = out + y * w;
        for (int x = 0; x < w; ++x) {
            int nx = x - x0;
            /* During uncertain frames also cover the transient eight-pixel
             * edge bands; the board remains untouched. */
            int edge = !stable && (nx < 8 || nx >= 248);
            dst[x] = nx >= 0 && nx < 256 && !edge ? row[nx]
                : water[(nx % 32 + 32) % 32];
        }
    }
    return 1;
}
static int get_mode(void *ctx) { (void)ctx; return mode; }
static bool set_mode(void *ctx, int value)
{
    (void)ctx;
    if (value < 0 || value > 2) return false;
    mode = value;
    cyc_video_set_mode(!enabled ? NES_VIDEO_STOCK : value == RECOMP_RUNTIME_UI_VIEW_FIXED_16_9 ? NES_VIDEO_16_9
                       : value == RECOMP_RUNTIME_UI_VIEW_ADAPTIVE ? NES_VIDEO_FIT : NES_VIDEO_STOCK);
    return true;
}
int kickle_bezel_active(void) { return kickle_bezel_enabled && !enabled; }
static int display_item_enabled(void *ctx, const RecompRuntimeUiItem *item)
{
    (void)ctx;
    return strcmp(item->key, "kickle.bezel") || !enabled;
}
static int checkbox_get(void *ctx, const RecompRuntimeUiItem *item, int *value)
{
    (void)ctx;
    if(achievement_value(item,value)) return 1;
    if(!strcmp(item->key,"kickle.bezel")) { *value=kickle_bezel_enabled; return 1; }
    if(!strcmp(item->key,"kickle.filter")) { *value=kickle_filter_mode; return 1; }
    if(!strcmp(item->key,"kickle.achievements_enabled")) { *value=achievements_enabled; return 1; }
    if (!strcmp(item->key, "kickle.freeze_timer")) { *value=freeze_timer; return 1; }
    if (!strcmp(item->key, "kickle.invincibility")) { *value=invincibility; return 1; }
    if (!strcmp(item->key, "kickle.hardcore")) { *value=selected_hardcore; return 1; }
    if (!strcmp(item->key, "kickle.game_mode")) { *value=selected_gale; return 1; }
    if (!strcmp(item->key, "kickle.infinite_lives")) { *value = infinite_lives; return 1; }
    if (!strcmp(item->key, "kickle.boss_rush")) { *value = boss_rush; return 1; }
    if (!strcmp(item->key, "kickle.voxel")) { *value = kickle_voxel_enabled; return 1; }
    if (!strcmp(item->key, "kickle.pitch")) { *value = kickle_voxel_pitch; return 1; }
    if (!strcmp(item->key, "kickle.yaw")) { *value = kickle_voxel_yaw; return 1; }
    if (!strcmp(item->key, "kickle.zoom")) { *value = kickle_voxel_zoom; return 1; }
    if (!strcmp(item->key, "kickle.world")) { *value = selected_world; return 1; }
    if (!strcmp(item->key, "kickle.level")) { *value = selected_level; return 1; }
    if (strcmp(item->key, "kickle.widescreen")) return 0;
    *value = enabled;
    return 1;
}
static int checkbox_set(void *ctx, const RecompRuntimeUiItem *item, int value)
{

    if(!strcmp(item->key,"kickle.bezel")) { if (value && enabled) return 0; kickle_bezel_enabled=value!=0; return 1; }
    if(!strcmp(item->key,"kickle.filter")) {
        if(value<0 || value>4)return 0;
        kickle_filter_mode=value;

        return 1;
    }
    if(!strcmp(item->key,"kickle.achievements_enabled")) {
        achievements_enabled=value!=0;
        achievement_reset();
        if(!achievements_enabled) cyc_ui_achievement(NULL,NULL,0);
        return 1;
    }
    if(hardcore_status && (strstr(item->key,"infinite_lives") || strstr(item->key,"invincibility") || strstr(item->key,"freeze_timer") || strstr(item->key,"boss_rush") || strstr(item->key,"game_mode") || strstr(item->key,"hardcore") || strstr(item->key,"world") || strstr(item->key,"level"))) {
        cyc_ui_set_toast("Hardcore run", "Locked. End this run before changing modes or cheats.");
        return 0;
    }
    if(!strcmp(item->key,"kickle.freeze_timer")) { freeze_timer=value!=0; return 1; }
    if(!strcmp(item->key,"kickle.invincibility")) { invincibility=value!=0; return 1; }
    if(!strcmp(item->key,"kickle.hardcore")) { selected_hardcore=value!=0; if(selected_hardcore) boss_rush=0; return 1; }
    if (!strcmp(item->key, "kickle.game_mode")) { if(value<0 || value>1) return 0; selected_gale=value; return 1; }
    if (!strcmp(item->key, "kickle.infinite_lives")) { infinite_lives = value != 0; return 1; }
    if (!strcmp(item->key, "kickle.boss_rush")) { boss_rush = value != 0; return 1; }
    if (!strcmp(item->key, "kickle.voxel")) { kickle_voxel_enabled = value != 0; return 1; }
    if (!strcmp(item->key, "kickle.pitch")) { if (value < 20 || value > 85) return 0; kickle_voxel_pitch = value; return 1; }
    if (!strcmp(item->key, "kickle.yaw")) { if (value < -180 || value > 180) return 0; kickle_voxel_yaw = value; return 1; }
    if (!strcmp(item->key, "kickle.zoom")) { if (value < 50 || value > 180) return 0; kickle_voxel_zoom = value; return 1; }
    if (!strcmp(item->key, "kickle.world")) {
        if (value < 0 || value > 4) return 0;
        selected_world = value;
        if (selected_level > level_limits[value]) selected_level = level_limits[value];
        return 1;
    }
    if (!strcmp(item->key, "kickle.level")) {
        if (value < 1 || value > level_limits[selected_world]) return 0;
        selected_level = value; return 1;
    }
    if (strcmp(item->key, "kickle.widescreen")) return 0;
    enabled = value != 0;
    if (enabled) kickle_bezel_enabled=0;
    if (enabled && mode == RECOMP_RUNTIME_UI_VIEW_NATIVE) mode = RECOMP_RUNTIME_UI_VIEW_FIXED_16_9;
    set_mode(ctx, mode);
    return 1;
}
static void load_setting(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    if (!strcmp(key, "GaleFestival")) gale_mode=selected_gale=0;
    if (!strcmp(key, "AchievementsEnabled")) achievements_enabled=value[0]=='1';
    if (!strcmp(key,"BezelMode")) kickle_bezel_enabled=value[0]=='1';
    if (!strcmp(key,"DisplayFilter")) { int v=atoi(value); if(v>=0 && v<=4) kickle_filter_mode=v; }
    if (!strcmp(key, "WidescreenEnabled")) enabled = value[0] == '1';
    /* Hidden experimental mode: old preferences must not enable an option
     * that players can no longer turn off in the menus. */
    if (!strcmp(key, "IsometricEnabled") || !strcmp(key, "VoxelEnabled")) kickle_voxel_enabled = 0;
    if (!strcmp(key, "Invincibility")) invincibility = value[0] == '1';
    if (!strcmp(key, "FreezeTimer")) freeze_timer = value[0] == '1';
    if (!strcmp(key, "InfiniteLives")) infinite_lives = value[0] == '1';
    int camera_value;
    if (sscanf(value, "%d", &camera_value) == 1) {
    }
}
static void save_setting(void *ctx, FILE *f)
{
    (void)ctx;
    fprintf(f, "WidescreenEnabled = %d\n", enabled);
    fprintf(f, "AchievementsEnabled = %d\n", achievements_enabled);
    fprintf(f,"BezelMode = %d\n",kickle_bezel_enabled);
    fprintf(f,"DisplayFilter = %d\n",kickle_filter_mode);
    fprintf(f, "FreezeTimer = %d\n", freeze_timer);
    fprintf(f, "InfiniteLives = %d\n", infinite_lives);
    fprintf(f, "Invincibility = %d\n", invincibility);
    fprintf(f, "IsometricEnabled = %d\n", kickle_voxel_enabled);
    fprintf(f, "GaleFestival = %d\n", gale_mode);
}
static const RecompRuntimeUiItem items[] = {
    { .key="kickle.bezel", .section="Display Options", .label="Bezel Mode", .description="Larger native game screen with a fitted TV frame. Available when widescreen is off.", .type=RECOMP_RUNTIME_UI_BOOL, .maximum=1, .step=1 },
    { .key="kickle.start_mode", .section="Additional Modes", .label="Start normal game",
      .description="Start a fresh Garden Land level 1 run.", .type=RECOMP_RUNTIME_UI_ACTION },
    { .key="kickle.filter", .section="Display Options", .label="Display Filter", .description="Off, CRT Soft, LCD Grid, Sharp or Warm Composite. Adapted from the shared SNES shader presets.", .type=RECOMP_RUNTIME_UI_CHOICE, .minimum=0, .maximum=4, .step=1, .choices=kickle_filter_names, .choice_count=5 },
    { .key="kickle.freeze_timer", .section="Cheats", .label="Freeze Timer",
      .description="Stop the level countdown at its current value. Completion time bonuses still count down normally. Unavailable in hardcore mode.", .type=RECOMP_RUNTIME_UI_BOOL, .maximum=1, .step=1 },
    { .key="kickle.invincibility", .section="Cheats", .label="Invincibility",
      .description="Prevent lethal enemy and hazard collisions.", .type=RECOMP_RUNTIME_UI_BOOL, .maximum=1, .step=1 },
    { .key="kickle.hardcore", .section="Additional Modes", .label="1 Life, 1 Credit Clear (Experimental)",
      .description="Experimental: a complete run has not been verified. Bugs may interrupt or end an attempt. One death ends the run. No cheats, extra lives, rewind, fast-forward or save/load states.", .type=RECOMP_RUNTIME_UI_BOOL, .maximum=1, .step=1 },
    { .key="kickle.start_hardcore", .section="Additional Modes", .label="Start hardcore attempt",
      .description="Restart from Garden Land level 1 with all restrictions enabled.", .type=RECOMP_RUNTIME_UI_ACTION },
    { .key="kickle.end_hardcore", .section="Additional Modes", .label="End hardcore run",
      .description="Abandon this attempt and start a fresh normal run.", .type=RECOMP_RUNTIME_UI_ACTION },
    { .key = "kickle.infinite_lives", .section = "Cheats", .label = "Infinite Lives",
      .description = "Keep nine spare lives. Death and respawning still work normally.",
      .type = RECOMP_RUNTIME_UI_BOOL, .minimum = 0, .maximum = 1, .step = 1 },
    { .key = "kickle.boss_rush", .section = "Additional Modes", .label = "Boss Rush active",
      .description = "Skip puzzle stages between bosses. Turn off to return to normal progression.",
      .type = RECOMP_RUNTIME_UI_BOOL, .minimum = 0, .maximum = 1, .step = 1 },
    { .key = "kickle.start_boss_rush", .section = "Additional Modes", .label = "Start Boss Rush",
      .description = "Start a fresh run: Garden, Fruit, Cake, then Toy boss. Infinite Lives is optional.",
      .type = RECOMP_RUNTIME_UI_ACTION },
#if 0 /* Restore this row when Isometric mode is ready for the menus. */
    { .key = "kickle.voxel", .section = "Display Options", .label = "Enable Isometric mode",
      .description = "Fixed raised-board view. Original gameplay controls remain active.",
      .type = RECOMP_RUNTIME_UI_BOOL, .minimum = 0, .maximum = 1, .step = 1 },
#endif
    { .key = "kickle.widescreen", .section = "Display Options", .label = "Enable widescreen (Experimental)",
      .description = "Extend the surrounding water while preserving the original puzzle board. Experimental: visual issues may still occur.",
      .type = RECOMP_RUNTIME_UI_BOOL, .minimum = 0, .maximum = 1, .step = 1 },
    { .key = "kickle.world", .section = "Level select", .label = "World",
      .type = RECOMP_RUNTIME_UI_CHOICE, .minimum = 0, .maximum = 4,
      .choices = world_names, .choice_count = 5, .step = 1 },
    { .key = "kickle.level", .section = "Level select", .label = "Level",
      .description = "Garden/Fruit/Toy: 1–17; Cake: 1–16; Special Zones: 1–30. Starts a fresh run.",
      .type = RECOMP_RUNTIME_UI_INT, .minimum = 1, .maximum = 30, .step = 1 },
    { .key = "kickle.start_level", .section = "Level select", .label = "Start selected level",
      .description = "Starts a fresh run and closes this menu automatically.",
      .type = RECOMP_RUNTIME_UI_ACTION },
#define ACH(n,t,d,p) { .key="kickle.achievement." #n, .section="Achievements", .label=t, .description=d " (" #p "G)", .type=RECOMP_RUNTIME_UI_BOOL, .maximum=1, .step=1 },
#include "kickle_achievement_list.inc"
#undef ACH
    { .key="kickle.achievements_enabled", .section="Achievement Options", .label="Enable Achievements", .description="Allow local unlocks, cards and sounds.", .type=RECOMP_RUNTIME_UI_BOOL, .maximum=1, .step=1 },
    { .key="kickle.reset_achievements", .section="Achievement Options", .label="Reset Achievements", .description="Erase local unlocks and attempt progress after confirmation.", .type=RECOMP_RUNTIME_UI_ACTION },
    { .key="kickle.reset_title", .section="System", .label="Reset to Title Screen", .description="End the current run and return to the game's title screen.", .type=RECOMP_RUNTIME_UI_ACTION },
};
static int level_action(void *ctx, const RecompRuntimeUiItem *item)
{
    (void)ctx;
    if(!strcmp(item->key,"kickle.reset_title")) {
        hardcore_status=selected_hardcore=boss_rush=boss_entry_pending=0;
        pending_level=skip_title_requested=0;
        boss_presentation=0;
        achievement_reset();
        reset_queued=1;
        if(cyc_ui_menu_open()) cyc_ui_toggle_menu();
        return 1;
    }
    if(!strcmp(item->key,"kickle.reset_achievements")) {
            achievement_reset_pending=0;
            achievement_earned=0;
            memset(achievement_dates,0,sizeof(achievement_dates));
            achievement_reset(); achievement_save();
            cyc_ui_achievement(NULL,NULL,0);
            cyc_ui_set_toast("Achievements reset", "All challenges can be earned again.");
        return 1;
    }
    if(!strcmp(item->key,"kickle.end_hardcore")) {
        hardcore_status=selected_hardcore=0;
        boss_rush=0; selected_world=0; selected_level=1;
        pending_level=reset_queued=1;

        if(cyc_ui_menu_open()) cyc_ui_toggle_menu();
        return 1;
    }
    if(!strcmp(item->key,"kickle.start_hardcore")) selected_hardcore=1;
    if(hardcore_status && strcmp(item->key,"kickle.start_mode") && strcmp(item->key,"kickle.start_hardcore")) {
        cyc_ui_set_toast("Hardcore run", "Level select and Boss Rush are locked during this attempt.");
        return 0;
    }
    if (!strcmp(item->key, "kickle.start_mode") || !strcmp(item->key,"kickle.start_hardcore")) {
        hardcore_status=selected_hardcore ? 1 : 0;
        if(hardcore_status) infinite_lives=invincibility=freeze_timer=0;

        gale_mode=selected_gale; boss_rush=0; selected_world=0; selected_level=1;
        pending_level=reset_queued=1;
        if(cyc_ui_menu_open()) cyc_ui_toggle_menu();
        cyc_ui_set_toast("Game mode",gale_mode ? "Starting Gale Festival" : "Starting Original");
        return 1;
    }
    if (!strcmp(item->key, "kickle.start_boss_rush")) {
        skip_title_requested = 1;
        gale_mode=selected_gale;
        boss_rush = 1;
        selected_world = 0;
        selected_level = 1;
        pending_level = reset_queued = 1;
        if (cyc_ui_menu_open()) cyc_ui_toggle_menu();
        cyc_ui_set_toast("Boss Rush", "Starting the Garden boss");
        return 1;
    }
    if (strcmp(item->key, "kickle.start_level")) return 0;
    skip_title_requested = 1;
    gale_mode = selected_gale;
    boss_rush = 0;
    if (selected_level > level_limits[selected_world]) selected_level = level_limits[selected_world];
    pending_level = reset_queued = 1;
    if (cyc_ui_menu_open()) cyc_ui_toggle_menu();
    cyc_ui_set_toast("Level select", "Starting a fresh run at the selected world and level");
    return 1;
}
#include "kickle_title_menu.inc"
static const RecompRuntimeUiCallbacks callbacks = {
    .get_value = checkbox_get, .set_value = checkbox_set, .is_enabled = display_item_enabled,
    .run_action = level_action,
};
static void power_on(void *ctx)
{
    if (enabled) kickle_bezel_enabled=0;
    title_menu_reset();
    achievement_reset();
    apply_gale_mode();

    toy_water_ready = have_water = uncertain_frames = boss_presentation = 0;
    cyc_render_set_compositor(compose, NULL);
    set_mode(ctx, boss_input_check ? RECOMP_RUNTIME_UI_VIEW_FIXED_16_9 : mode);
}
const CycHostExtras *cyc_host_extras(void)
{
    static int registered;
    if (!registered) {
        registered = 1;
        achievement_register();
        nes_mod_register_function_entry_plugin("kickle.invincibility.current-ice",0xA29B,invincibility_overlap_hook);
        nes_mod_set_function_hook_enabled("kickle.invincibility.current-ice",1);
        nes_mod_register_function_entry_plugin("kickle.invincibility.move-ice",0xA17A,invincibility_overlap_hook);
        nes_mod_set_function_hook_enabled("kickle.invincibility.move-ice",1);
        nes_mod_register_function_entry_plugin("kickle.invincibility.start-ice",0xA329,invincibility_overlap_hook);
        nes_mod_set_function_hook_enabled("kickle.invincibility.start-ice",1);
        nes_mod_register_function_entry_plugin("kickle.invincibility.current-hazard",0xA08B,invincibility_hazard_hook);
        nes_mod_set_function_hook_enabled("kickle.invincibility.current-hazard",1);
        nes_mod_register_function_entry_plugin("kickle.invincibility.destination-hazard",0xA183,invincibility_hazard_hook);
        nes_mod_set_function_hook_enabled("kickle.invincibility.destination-hazard",1);
        nes_mod_register_function_entry_plugin("kickle.invincibility.player-death",0xA099,invincibility_player_death_hook);
        nes_mod_set_function_hook_enabled("kickle.invincibility.player-death",1);
        nes_mod_register_function_entry_plugin("kickle.freeze-timer.countdown",0xCEEE,freeze_timer_hook);
        nes_mod_set_function_hook_enabled("kickle.freeze-timer.countdown",1);
        static const uint16_t hits[]={0xA2C2,0x86C1,0x9C4D,0x9DAD,0xB74F,0xB799,0xB7E6,0x8AB0,0x8439};
        for(unsigned i=0;i<sizeof(hits)/sizeof(hits[0]);++i) {
            char id[64]; snprintf(id,sizeof(id),"kickle.invincibility.hit-%u",i);
            nes_mod_register_function_entry_plugin(id,hits[i],invincibility_hook);
            nes_mod_set_function_hook_enabled(id,1);
        }
        nes_mod_register_function_entry_plugin("kickle.hardcore.puzzle-death",0x8679,hardcore_death_hook);
        nes_mod_set_function_hook_enabled("kickle.hardcore.puzzle-death",1);
        nes_mod_register_function_entry_plugin("kickle.hardcore.boss-death",0x8BB7,hardcore_death_hook);
        nes_mod_set_function_hook_enabled("kickle.hardcore.boss-death",1);
        nes_mod_register_savestate_hook("kickle.presentation",kickle_state_get,kickle_state_set);
        nes_mod_register_savestate_validator("kickle.presentation",kickle_state_validate);
        nes_mod_register_function_entry_plugin("kickle.boss.room-visible",0x9A43,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.room-visible",1);
        nes_mod_register_function_entry_plugin("kickle.boss.clean-room",0x9AB5,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.clean-room",1);
        nes_mod_register_function_entry_plugin("kickle.princess.clean-room",0x9F22,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.princess.clean-room",1);
        nes_mod_register_function_entry_plugin("kickle.boss.world-transition",0x9EC2,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.world-transition",1);
        nes_mod_register_function_entry_plugin("kickle.boss.end",0x89FF,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.end",1);
        nes_mod_register_function_entry_plugin("kickle.boss.cinematic",0x889A,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.cinematic",1);
        nes_mod_register_function_entry_plugin("kickle.boss.arena",0x9889,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.arena",1);
        nes_mod_register_function_entry_plugin("kickle.boss.combat",0x9AD3,boss_phase_hook);
        nes_mod_set_function_hook_enabled("kickle.boss.combat",1);
        nes_mod_register_function_entry_plugin("kickle.select.special-start",0x8306,special_start_hook);
        nes_mod_set_function_hook_enabled("kickle.select.special-start",1);
        nes_mod_register_function_entry_plugin("kickle.select.world-init", 0x82BB, level_hook);
        nes_mod_set_function_hook_enabled("kickle.select.world-init", 1);
        nes_mod_register_function_entry_plugin("kickle.rush.room-ready", 0x83D2, boss_room_hook);
        nes_mod_set_function_hook_enabled("kickle.rush.room-ready", 1);
    }
    static const CycHostExtras extras = {
        .present = title_menu_present,
        .view_modes = RECOMP_RUNTIME_UI_VIEW_MODE_NATIVE |
                      RECOMP_RUNTIME_UI_VIEW_MODE_FIXED_16_9 |
                      RECOMP_RUNTIME_UI_VIEW_MODE_ADAPTIVE,
        .get_view_mode = get_mode, .set_view_mode = set_mode,
        .menu_items = items, .menu_item_count = sizeof(items) / sizeof(items[0]), .menu_callbacks = &callbacks,
        .frame_begin = level_frame,
        .frame_end = capture_frame,
        .options = level_options, .option_count = sizeof(level_options)/sizeof(level_options[0]), .option = level_option,
        .load_setting = load_setting, .save_settings = save_setting,
        .power_on = power_on,
        .assists_allowed = assists_allowed,
    };
    return &extras;
}






