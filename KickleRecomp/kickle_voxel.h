#pragma once
#include <stdint.h>
extern int kickle_voxel_enabled, kickle_voxel_pitch, kickle_voxel_yaw, kickle_voxel_zoom;
const uint32_t *kickle_voxel_present(void *ctx, int *w, int *h);
void kickle_voxel_controls(void);
const uint32_t *kickle_flat_present(int *w, int *h);
