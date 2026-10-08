/* Experimental sprite interpolation. GPU placement uses fractional native
 * pixels; CPU/PPU execution and original framebuffers remain untouched. */
static int kickle_motion_enabled;
static const char *const kickle_motion_names[]={"Off","240 Hz Low Latency","240 Hz Buffered Interpolation","240 Hz Broad Interpolation"};
static int motion_valid;
static uint8_t motion_current[256];
static float motion_from_x[64], motion_from_y[64];
typedef struct {
    int first, count, columns, rows, height, x, y, player;
    uint8_t tile[6], palette[6];
} MotionObject;
static MotionObject motion_objects[64];
static int motion_object_count;
static uint64_t motion_cycle;
static int motion_width, motion_scene, motion_scroll;
static SDL_Texture *motion_base, *motion_atlas;
static SDL_Texture *motion_previous_base, *motion_previous_atlas;
static int motion_has_previous;
static uint32_t motion_pixels[2048*240], motion_tiles[512*16];
static uint32_t motion_old_pose[512*16], motion_blended_pose[512*16];

int kickle_motion_active(void) {
    return kickle_motion_enabled && !kickle_filter_mode && !kickle_voxel_enabled;
}
void kickle_motion_reset(void) { motion_valid=0; motion_has_previous=0; }

/* Kickle submits consecutive rectangular metasprites: the player is 3x2,
 * tall enemies 2x2, and bags/blobs 2x1. Geometry stays stable while animation
 * changes tile numbers, flips and OAM starting slots. Never connect arbitrary
 * touching sprites: two adjacent enemies remain distinct records. */
static int motion_collect_objects(MotionObject *objects) {
    int count=0;
    for(int slot=0;slot<64;) {
        const uint8_t *o=motion_current+slot*4;
        if(o[0]>=0xef) { ++slot; continue; }
        MotionObject *obj=objects+count++;
        memset(obj,0,sizeof(*obj));
        obj->first=slot; obj->count=obj->columns=obj->rows=1;
        obj->height=cyc_render_line_sprite16(o[0]+1)?16:8;
        obj->x=o[3]; obj->y=o[0]+1;
        const int columns[]={3,2,2},rows[]={2,2,1};
        for(int shape=0;shape<3;++shape) {
            int n=columns[shape]*rows[shape], fits=slot+n<=64;
            for(int j=0;fits && j<n;++j) {
                const uint8_t *part=motion_current+(slot+j)*4;
                fits=part[0]<0xef && part[3]==obj->x+(j%columns[shape])*8 &&
                     part[0]+1==obj->y+(j/columns[shape])*obj->height &&
                     cyc_render_line_sprite16(part[0]+1)==(obj->height==16);
            }
            if(fits) {
                obj->count=n; obj->columns=columns[shape]; obj->rows=rows[shape];
                break;
            }
        }
        /* Only the normal player uses this six-piece record in puzzle play.
         * Include walking head/body frames and either mirrored direction. */
        obj->player=obj->count==6 && o[1]>=0x41 && o[1]<=0x5d;
        for(int j=0;j<obj->count;++j) {
            obj->tile[j]=motion_current[(slot+j)*4+1];
            obj->palette[j]=motion_current[(slot+j)*4+2]&0x23;
        }
        slot+=obj->count;
    }
    return count;
}

static void motion_track_objects(int continuous) {
    MotionObject previous[64];
    uint32_t previous_tiles[512*16];
    memcpy(previous_tiles,motion_tiles,sizeof(previous_tiles));
    memset(motion_tiles,0,sizeof(motion_tiles));
    int previous_count=motion_object_count;
    memcpy(previous,motion_objects,sizeof(previous));
    motion_object_count=motion_collect_objects(motion_objects);
    for(int slot=0;slot<64;++slot) {
        motion_from_x[slot]=motion_current[slot*4+3];
        motion_from_y[slot]=motion_current[slot*4]+1;
    }
    if(!continuous) return;
    uint8_t used_old[64]={0},used_new[64]={0};
    /* Assign the strongest whole-object correspondences first. A nearby
     * matching pose is preferred, but changing animation is a small penalty,
     * not a reason to drop the object's position history. */
    for(int assignment=0;assignment<motion_object_count;++assignment) {
        int best_new=-1,best_old=-1,best_score=100000;
        for(int n=0;n<motion_object_count;++n) {
            if(used_new[n]) continue;
            const MotionObject *cur=motion_objects+n;
            /* Goal-bag outlines overlay stationary background artwork.
             * OAM rotation must never turn them into moving objects. */
            if(cur->count==2 && (cur->tile[0]==0x28 || cur->tile[0]==0x2a)) continue;
            for(int p=0;p<previous_count;++p) {
                if(used_old[p]) continue;
                const MotionObject *old=previous+p;
                if(cur->columns!=old->columns || cur->rows!=old->rows ||
                   cur->height!=old->height || cur->player!=old->player) continue;
                int dx=cur->x-old->x,dy=cur->y-old->y;
                int distance=dx*dx+dy*dy;
                if(distance>144) continue; /* spawning/teleports snap */
                int score=distance*8;
                for(int j=0;j<cur->count;++j) {
                    score+=cur->tile[j]!=old->tile[j] ? 3 : 0;
                    score+=cur->palette[j]!=old->palette[j] ? 8 : 0;
                }
                if(score<best_score) { best_score=score; best_new=n; best_old=p; }
            }
        }
        if(best_new<0) break;
        MotionObject *cur=motion_objects+best_new;
        const MotionObject *old=previous+best_old;
        used_new[best_new]=used_old[best_old]=1;
        int dx=cur->x-old->x,dy=cur->y-old->y;
        for(int j=0;j<cur->count;++j) {
            int slot=cur->first+j;
            for(int r=0;r<cur->height;++r)
                memcpy(motion_tiles+r*512+slot*8,previous_tiles+r*512+(old->first+j)*8,8*4);
            /* Preserve this frame's pose exactly. Every component receives
             * the same translation, including newly animated/flipped tiles. */
            motion_from_x[slot]=motion_current[slot*4+3]-dx;
            motion_from_y[slot]=motion_current[slot*4]+1-dy;
        }
    }
}

/* Draw after the ordinary picture and before UI. The base and atlas are
 * uploaded once per simulated frame; intermediate displays only move quads. */
void kickle_motion_draw(SDL_Renderer *renderer,const uint32_t *pic,int w,int h,float alpha) {
    if(w<256 || w>2048 || h!=240) { motion_valid=0; return; }
    /* Cinematics and loading wipes use background/sprite composites that
     * cannot be translated independently. Keep their native presentation. */
    int hud_white=0, center=(w-256)/2;
    for(int y=8;y<16;++y) for(int x=16;x<240;++x) {
        uint32_t c=pic[y*w+center+x];
        if((c&0x00ffffff)==0x00ffffff) ++hud_white;
    }
    if(boss_presentation || cyc_mod_peek(0x0400)!=2 || hud_white<15) {
        motion_valid=0; return;
    }
    uint64_t cycle=cyc_cycle_count();
    int scene=(cyc_mod_peek(0x010A)<<16)|(cyc_mod_peek(0x010C)<<8)|boss_presentation;
    int scroll=cyc_render_line_scroll_y(120)*512+cyc_render_line_scroll_x(120);
    if(!motion_base || motion_width!=w) {
        SDL_DestroyTexture(motion_base);
        SDL_DestroyTexture(motion_previous_base);
        motion_base=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h);
        motion_previous_base=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h);
        if(motion_previous_base) {
            SDL_SetTextureScaleMode(motion_previous_base,SDL_ScaleModeNearest);
            SDL_SetTextureBlendMode(motion_previous_base,SDL_BLENDMODE_BLEND);
        }
        motion_width=w; motion_valid=0;
        if(motion_base) SDL_SetTextureScaleMode(motion_base,SDL_ScaleModeNearest);
    }
    if(!motion_atlas) {
        motion_atlas=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,512,16);
        motion_previous_atlas=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,512,16);
        if(motion_previous_atlas) {
            SDL_SetTextureScaleMode(motion_previous_atlas,SDL_ScaleModeNearest);
            SDL_SetTextureBlendMode(motion_previous_atlas,SDL_BLENDMODE_BLEND);
        }
        if(motion_atlas) {
            SDL_SetTextureBlendMode(motion_atlas,SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(motion_atlas,SDL_ScaleModeNearest);
        }
    }
    if(!motion_base || !motion_atlas) return;
    int x0=(w-256)/2;
    if(!motion_valid || cycle!=motion_cycle) {
        int continuous=motion_valid && cycle>motion_cycle && cycle-motion_cycle<40000 &&
                       scene==motion_scene && scroll==motion_scroll;
        motion_has_previous=continuous;
        if(continuous && motion_previous_base)
            SDL_UpdateTexture(motion_previous_base,NULL,motion_pixels,w*4);
        memcpy(motion_current,cyc_frame_sprite_oam(),256);
        motion_track_objects(continuous);
        /* Tracking remaps the old atlas into the current OAM slot layout. */
        if(continuous && motion_previous_atlas)
            SDL_UpdateTexture(motion_previous_atlas,NULL,motion_tiles,512*4);
        memcpy(motion_old_pose,motion_tiles,sizeof(motion_old_pose));
        memcpy(motion_pixels,pic,(size_t)w*h*4);
        const uint32_t *bg=cyc_frame_background_argb();
        for(int y=0;y<240;++y) {
            memcpy(motion_pixels+y*w+x0,bg+y*256,256*4);
            if(!enabled && !boss_presentation) for(int x=0;x<8;++x) {
                motion_pixels[y*w+x0+x]=0xff000000;
                motion_pixels[y*w+x0+248+x]=0xff000000;
            }
        }
        memcpy(motion_tiles,cyc_frame_sprite_argb(),sizeof(motion_tiles));
        for(int slot=0;slot<64;++slot) {
            const uint8_t *o=motion_current+slot*4;
            if(o[0]>=0xef || !(o[2]&0x20)) continue;
            int y=o[0]+1,x=o[3];
            int rows=cyc_render_line_sprite16(y)?16:8;
            for(int r=0;r<rows && y+r<240;++r)
                for(int c=0;c<8 && x+c<256;++c)
                    if(cyc_frame_bg_opaque()[(y+r)*256+x+c])
                        motion_tiles[r*512+slot*8+c]=0;
        }
        if(SDL_UpdateTexture(motion_base,NULL,motion_pixels,w*4) ||
           SDL_UpdateTexture(motion_atlas,NULL,motion_tiles,512*4)) { motion_valid=0; return; }
        motion_cycle=cycle; motion_scene=scene; motion_scroll=scroll; motion_valid=1;
    }
    if(alpha<0) alpha=0; if(alpha>1) alpha=1;
    /* Low latency uses a bounded one-frame prediction from the latest
     * position. Buffered interpolation instead shows the previous->current
     * interval, trading one game frame of visual delay for no prediction. */
    if(kickle_motion_enabled==1) alpha+=1;
    SDL_Rect src={0,8,w,224}; SDL_FRect dst={0,8,(float)w,224};
    SDL_RenderCopyF(renderer,motion_base,&src,&dst);
    int broad=kickle_motion_enabled==3 && motion_has_previous && motion_previous_base && motion_previous_atlas;
    if(broad) {
        /* Interpolate premultiplied colors, then return to SDL's straight
         * alpha format. Identical opaque pixels remain fully opaque. */
        int weight=(int)(alpha*255), inverse=255-weight;
        for(int i=0;i<512*16;++i) {
            uint32_t a=motion_old_pose[i],b=motion_tiles[i];
            int aa=(a>>24)*inverse,ba=(b>>24)*weight,total=aa+ba;
            uint32_t c=total?((uint32_t)(total/255)<<24):0;
            if(total) for(int shift=0;shift<24;shift+=8)
                c|=(uint32_t)((((a>>shift)&255)*aa+((b>>shift)&255)*ba)/total)<<shift;
            motion_blended_pose[i]=c;
        }
        SDL_UpdateTexture(motion_previous_atlas,NULL,motion_blended_pose,512*4);
        SDL_SetTextureAlphaMod(motion_previous_atlas,255);
        /* Keep score/time crisp. Blend only the playfield, after scene and
         * scroll continuity checks, so loading wipes never leave trails. */
        SDL_Rect field={0,24,w,208}; SDL_FRect target={0,24,(float)w,208};
        SDL_SetTextureAlphaMod(motion_previous_base,(Uint8)((1-alpha)*255));
        SDL_RenderCopyF(renderer,motion_previous_base,&field,&target);
    }
    SDL_Rect old_clip;
    SDL_bool clipped=SDL_RenderIsClipEnabled(renderer);
    SDL_RenderGetClipRect(renderer,&old_clip);
    SDL_Rect clip={x0+(!enabled&&!boss_presentation?8:0),8,
                  !enabled&&!boss_presentation?240:256,224};
    SDL_RenderSetClipRect(renderer,&clip);
    for(int slot=63;slot>=0;--slot) {
        const uint8_t *o=motion_current+slot*4;
        if(o[0]>=0xef) continue;
        int rows=cyc_render_line_sprite16(o[0]+1)?16:8;
        SDL_Rect tile={slot*8,0,8,rows};
        SDL_FRect pos={x0+motion_from_x[slot]+alpha*(o[3]-motion_from_x[slot]),
                      motion_from_y[slot]+alpha*(o[0]+1-motion_from_y[slot]),8,(float)rows};
        if(broad) {
            /* Blend poses at the SAME interpolated position, avoiding two
             * spatially separated copies of a moving character. */
            SDL_RenderCopyF(renderer,motion_previous_atlas,&tile,&pos);
        } else {
            SDL_SetTextureAlphaMod(motion_atlas,255);
            SDL_RenderCopyF(renderer,motion_atlas,&tile,&pos);
        }
    }
    SDL_RenderSetClipRect(renderer,clipped?&old_clip:NULL);
}
