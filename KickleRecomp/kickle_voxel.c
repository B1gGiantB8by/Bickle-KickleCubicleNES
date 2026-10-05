#include "kickle_voxel.h"
#include "kickle_voxel_adapter.h"
#include "cyc_render.h"
#include "../nesrecomp/runner/include/voxel_renderer.h"
#include <SDL.h>
#include <string.h>

unsigned char kickle_voxel_ctrl;
unsigned char kickle_voxel_oam[256], kickle_voxel_pal[32], kickle_voxel_chr[8192];
int kickle_voxel_enabled, kickle_voxel_pitch = 59, kickle_voxel_yaw = 2, kickle_voxel_zoom = 100;
static uint32_t picture[426 * 240];
/* SCORE/TIME use both tile rows at y=8 and y=16. Keep the entire
 * HUD outside the projected terrain, including the lower glyph halves. */
enum { BOARD_TOP = 20, BOARD_ROWS = 27, HUD_ROWS = 24 };
static uint8_t tiles[32 * BOARD_ROWS], palettes[32 * BOARD_ROWS];
static uint16_t patterns[32 * BOARD_ROWS];
static float heights[32 * BOARD_ROWS];
static int8_t decorations[32 * BOARD_ROWS];
static uint8_t sprite_groups[64];
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* Fixed oblique camera matching the selected preset. */
void kickle_voxel_controls(void) { }
static float tile_height(uint8_t tile, int x, int y, void *ctx)
{
    (void)tile; (void)ctx; return heights[y * 32 + x];
}
static void tile_pixels(uint32_t *out, int stride, uint8_t tile, int x, int y, void *ctx)
{
    (void)tile; (void)ctx;
    uint32_t pixels[64];
    for (int i = 0; i < 64; ++i) pixels[i] = cyc_render_color(0);
    int cell = y * 32 + x;
    cyc_render_tile(pixels, 8, 8, 0, 0, patterns[cell], palettes[cell], 0, NULL, NULL);
    if (decorations[cell]) {
        /* Use CHR transparency, not RGB equality with the water. Ice uses
         * some of those same blue colours for its opaque block faces. */
        for (int row = 0; row < 8; ++row) {
            unsigned lo = cyc_render_chr(patterns[cell] + row);
            unsigned hi = cyc_render_chr(patterns[cell] + row + 8);
            for (int col = 0; col < 8; ++col) {
                int bit = 7-col;
                if (!(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)))
                    pixels[row * 8 + col] = 0;
            }
        }
    }
    for (int row = 0; row < 8; ++row) memcpy(out + row * stride, pixels + row * 8, 8 * sizeof(uint32_t));
}
static float sprite_ground(int min_x, int min_y, int max_x, int max_y,
                           float sampled, void *ctx)
{
    (void)min_y; (void)ctx;
    /* The exclusive bottom of a metasprite can fall in the water tile
     * immediately below its platform. Sample inside its footprint instead. */
    int row = clamp((max_y - BOARD_TOP - 4) / 8, 0, BOARD_ROWS - 1);
    for (int x = clamp(min_x / 8, 0, 31);
         x <= clamp((max_x - 1) / 8, 0, 31); ++x)
        if (heights[row * 32 + x] > sampled) sampled = heights[row * 32 + x];
    return sampled + 0.5f;
}
static int tile_billboard(uint8_t tile, int x, int y, int *columns, int *rows, void *ctx)
{
    (void)tile; (void)ctx;
    *columns = 2; *rows = 2;
    return decorations[y * 32 + x];
}
static void group_sprites(void)
{
    int height = (kickle_voxel_ctrl & 0x20) ? 16 : 8;
    /* OAM captures show coherent consecutive rectangular actor records:
     * player 3x2, tall enemies 2x2, bags/blobs 2x1. Group records by their
     * geometry, independently of animation CHR numbering and palette flips. */
    for (int i = 0; i < 64;) {
        int count = 1;
        int ax = kickle_voxel_oam[i * 4 + 3];
        int ay = kickle_voxel_oam[i * 4];
        /* A contiguous goal-bag row is one visual strip. Separate fully
         * camera-facing cards overlap at diagonal yaw and hide each other. */
        int bag_count = 0;
        while (bag_count < 6 && i + bag_count + 1 < 64) {
            int j = i + bag_count;
            int tile = kickle_voxel_oam[j * 4 + 1];
            if (tile < 0x28 || tile > 0x2e || (tile & 1) ||
                kickle_voxel_oam[j * 4 + 3] != ax + bag_count * 8 ||
                kickle_voxel_oam[(j+1) * 4 + 3] != ax + (bag_count+1)*8 ||
                kickle_voxel_oam[j * 4] != ay ||
                kickle_voxel_oam[(j+1) * 4] != ay ||
                kickle_voxel_oam[(j+1) * 4 + 1] != tile + 2) break;
            bag_count += 2;
        }
        if (bag_count > 2) {
            for (int j = 0; j < bag_count; ++j) sprite_groups[i+j] = (uint8_t)i;
            i += bag_count;
            continue;
        }
        const int widths[] = {3, 2, 2}, rows[] = {2, 2, 1};
        for (int shape = 0; shape < 3; ++shape) {
            int n = widths[shape] * rows[shape];
            if (i + n > 64) continue;
            int matches = 1;
            for (int j = 0; j < n; ++j) {
                int x = kickle_voxel_oam[(i+j)*4+3];
                int y = kickle_voxel_oam[(i+j)*4];
                if (x != ax + (j % widths[shape])*8 ||
                    y != ay + (j / widths[shape])*height) matches = 0;
            }
            if (matches) { count = n; break; }
        }
        for (int j = 0; j < count; ++j) sprite_groups[i+j] = (uint8_t)i;
        i += count;
    }
}
static int sprite_connect(int a, int b, void *ctx)
{
    (void)ctx;
    return sprite_groups[a] == sprite_groups[b] ? 1 : -1;
}
static void orient_player(void)
{
    /* Normal player records consist of three head pieces and three body
     * pieces. Choose front/back/profile art in camera coordinates while
     * leaving the machine's direction and controller input untouched. */
    for (int i = 0; i + 5 < 64; ++i) {
        int x = kickle_voxel_oam[i*4+3], y = kickle_voxel_oam[i*4];
        if (y >= 0xef) continue;
        int head = kickle_voxel_oam[i*4+1];
        int flipped = !!(kickle_voxel_oam[i*4+2] & 0x40);
        int base = head - (flipped ? 4 : 0);
        if (base != 0x41 && base != 0x4d && base != 0x59) continue;
        int valid = 1;
        for (int j = 0; j < 6; ++j)
            if (kickle_voxel_oam[(i+j)*4+3] != x+(j%3)*8 ||
                kickle_voxel_oam[(i+j)*4] != y+(j/3)*16) valid = 0;
        if (!valid) continue;
        int direction = base == 0x41 ? 0 : base == 0x4d ? 180 : flipped ? 90 : -90;
        int relative = direction - 2;
        while (relative > 180) relative -= 360;
        while (relative < -180) relative += 360;
        int target, target_flip;
        if (relative >= -45 && relative <= 45) { target = 0x41; target_flip = flipped; }
        else if (relative >= 135 || relative <= -135) { target = 0x4d; target_flip = flipped; }
        else { target = 0x59; target_flip = relative > 0; }
        /* Keep native animation when its view is already correct. */
        if (target == base && (base != 0x59 || target_flip == flipped)) { i += 5; continue; }
        if (target == base) {
            /* Mirror the complete native pose, including its walking legs.
             * Replacing it with the idle profile loses the direction cue. */
            unsigned char pose[24];
            memcpy(pose, kickle_voxel_oam + i*4, sizeof(pose));
            for (int j = 0; j < 6; ++j) {
                int src = (j/3)*3 + 2-j%3;
                kickle_voxel_oam[(i+j)*4+1] = pose[src*4+1];
                kickle_voxel_oam[(i+j)*4+2] = pose[src*4+2] ^ 0x40;
            }
            i += 5;
            continue;
        }
        for (int j = 0; j < 6; ++j) {
            int col = target_flip ? 2-j%3 : j%3;
            kickle_voxel_oam[(i+j)*4+1] = (uint8_t)(target + (j/3)*6 + col*2);
            kickle_voxel_oam[(i+j)*4+2] =
                (kickle_voxel_oam[(i+j)*4+2] & ~0x40) | (target_flip ? 0x40 : 0);
        }
        i += 5;
    }
}
const uint32_t *kickle_voxel_present(void *ctx, int *w, int *h)
{
    (void)ctx;
    if (!kickle_voxel_enabled) return kickle_flat_present(w, h);
    const uint32_t *native = cyc_frame_argb();
    /* Restrict the experimental profile to the gameplay HUD (SCORE). */
    int lit = 0;
    for (int y = 8; y < 16; ++y)
        for (int x = 8; x < 48; ++x)
            lit += (native[y * 256 + x] & 0xffffff) == 0xffffff;
    if (lit < 15) return kickle_flat_present(w, h);
    for (int y = 0; y < BOARD_ROWS; ++y) {
        int sy = y * 8 + BOARD_TOP;
        int scroll_x = cyc_render_line_scroll_x(sy), scroll_y = cyc_render_line_scroll_y(sy);
        for (int x = 0; x < 32; ++x) {
            int px = (scroll_x + x * 8 + 1) & 511, py = scroll_y % 480;
            unsigned base = 0x2000 + (px / 256) * 0x400 + (py / 240) * 0x800;
            int tx = (px % 256) / 8, ty = (py % 240) / 8;
            int cell = y * 32 + x;
            tiles[cell] = cyc_render_nametable((uint16_t)(base + ty * 32 + tx));
            /* Vertical shake can wrap the status rows into the room. Those
             * rows belong exclusively to the screen overlay. */
            if (ty < 2) {
                ty = 2;
                tx = 0;
                tiles[cell] = cyc_render_nametable((uint16_t)(base + ty*32 + tx));
            }
            unsigned attr = cyc_render_nametable((uint16_t)(base + 0x3c0 + (ty / 4) * 8 + tx / 4));
            palettes[cell] = (attr >> ((ty & 2) * 2 + (tx & 2))) & 3;
            patterns[cell] = cyc_render_line_bg_table(sy) + tiles[cell] * 16;
        }
    }
    /* Water shoreline/shadow patterns are still water, even when they do not
     * occur at the screen edges. Only light platform pixels raise terrain. */
    memset(decorations, 0, sizeof(decorations));
    for (int y = 0; y < BOARD_ROWS; ++y) for (int x = 0; x < 32; ++x) {
        uint32_t pixels[64];
        tile_pixels(pixels, 8, tiles[y * 32 + x], x, y, NULL);
        int light = 0;
        for (int p = 0; p < 64; ++p) {
            unsigned r = (pixels[p] >> 16) & 255;
            unsigned g = (pixels[p] >> 8) & 255;
            unsigned b = pixels[p] & 255;
            light += r >= 140 && g >= 140 && b >= 140;
        }
        heights[y * 32 + x] = light >= 16 ? 3.0f : 0.0f;
    }
    /* Gold obstacles are background metatiles, not OAM actors. Reconstruct
     * their four pieces into one upright card rather than painting them flat. */
    for (int y = 0; y < BOARD_ROWS - 1; ++y) for (int x = 0; x < 31; ++x) {
        int i = y * 32 + x;
        if (decorations[i] || decorations[i + 1] ||
            decorations[i + 32] || decorations[i + 33]) continue;
        /* The room uses column-major 2x2 metatiles for obstacles/items.
         * These identities also cover feather stems and ice highlights that
         * cannot reliably be recognised from their palette colours. */
        int root = tiles[i];
        int object = root == 0x00 || root == 0x70 || root == 0x74 || root == 0x78 ||
                     root == 0x7c || root == 0x80 || root == 0x84 ||
                     root == 0x88 || root == 0xc4 || root == 0xc8 || root == 0xf0 ||
                     root == 0xf4 || root == 0xf8 || root == 0xfc;
        int complete = tiles[i+1] == root+2 && tiles[i+32] == root+1 &&
                       tiles[i+33] == root+3;
        if (!object || !complete) continue;
        decorations[i] = 1;
        decorations[i + 1] = decorations[i + 32] = decorations[i + 33] = -1;
        heights[i] = heights[i + 1] = heights[i + 32] = heights[i + 33] = 3.0f;
    }
    kickle_voxel_ctrl = (cyc_render_line_sprite16(100) ? 0x20 : 0) |
                        (cyc_render_line_sprite_table(100) ? 8 : 0);
    memcpy(kickle_voxel_oam, cyc_render_oam(), 256);
    orient_player();
    group_sprites();
    for (int i = 0; i < 32; ++i) kickle_voxel_pal[i] = cyc_render_palette(i);
    for (int i = 0; i < 8192; ++i) kickle_voxel_chr[i] = cyc_render_chr(i);
    memset(picture, 0, sizeof(picture));
    for (int y = 0; y < 240; ++y) memcpy(picture + y * 426 + 85, native + y * 256, 256 * sizeof(uint32_t));
    /* The native HUD can contain actors walking at the top of the room.
     * Rebuild its background tiles so only SCORE/TIME/lives remain here. */
    for (int y = 0; y < HUD_ROWS; ++y) {
        /* Status text stays in its normal screen position during room shake. */
        int scroll_x = cyc_render_line_scroll_x(y), scroll_y = (y + 476) % 480;
        for (int x = 0; x < 256; ++x) {
            int px = (scroll_x + x) & 511;
            unsigned base = 0x2000 + (px/256)*0x400 + (scroll_y/240)*0x800;
            int tx = (px%256)/8, ty = (scroll_y%240)/8;
            uint8_t tile = cyc_render_nametable(base + ty*32 + tx);
            uint8_t attr = cyc_render_nametable(base + 0x3c0 + (ty/4)*8 + tx/4);
            unsigned pattern = cyc_render_line_bg_table(y) + tile*16 + (scroll_y&7);
            int bit = 7-(px&7);
            int colour = ((cyc_render_chr(pattern)>>bit)&1) |
                         (((cyc_render_chr(pattern+8)>>bit)&1)<<1);
            int pal = (attr >> ((ty&2)*2 + (tx&2))) & 3;
            picture[y*426+85+x] = cyc_render_color(colour ? pal*4+colour : 0);
        }
    }
    /* The level starts underneath the last four HUD scanlines in the NES
     * picture. Keep the glyphs, but replace that overlapping scenery with
     * the water pattern from the empty left edge of the same scanline. */
    for (int y = 20; y < HUD_ROWS; ++y)
        for (int x = 0; x < 256; ++x)
            picture[y * 426 + 85 + x] = native[y * 256 + (x & 7)];
    NesVoxelScene scene = {
        .framebuffer = picture, .output_width = 426, .output_height = 240,
        .source_x = 85, .source_y = BOARD_TOP, .source_width = 256, .source_height = BOARD_ROWS*8,
        .tiles = tiles, .tile_columns = 32, .tile_rows = BOARD_ROWS, .tile_stride = 32, .tile_size = 8,
        .tile_height = tile_height, .tile_pixels = tile_pixels,
        .terrain_offset_x = 1.0f,
        .tile_billboard = tile_billboard,
        .elevation_degrees = 59.0f, .yaw_degrees = 2.0f,
        .camera_distance = 460.0f,
        .camera_center_y = 0.55f,
        .sprite_scale = 1.0f, .draw_oam_sprites = 1, .preserve_top_rows = HUD_ROWS,
        .sprite_ground = sprite_ground, .sprite_face_camera_pitch = 1,
        .sprite_connect = sprite_connect, .sprite_group_max_width = 48,
        .sprite_depth_bias = 0.5f, .clip_sprites_to_source = 0,
        .sky_top = 0xff071731, .sky_bottom = 0xff143a66,
    };
    if (!nes_voxel_render(&scene)) return kickle_flat_present(w, h);
    *w = 426; *h = 240; return picture;
}
