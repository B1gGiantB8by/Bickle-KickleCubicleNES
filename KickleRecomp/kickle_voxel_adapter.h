#pragma once
#include "hw_internal.h"
extern unsigned char kickle_voxel_ctrl;
extern unsigned char kickle_voxel_oam[256], kickle_voxel_pal[32], kickle_voxel_chr[8192];
#define g_ppuctrl kickle_voxel_ctrl
#define g_ppu_oam kickle_voxel_oam
#define g_ppu_pal kickle_voxel_pal
#define g_chr_ram kickle_voxel_chr
#define g_nes_palette hw_palette_argb
