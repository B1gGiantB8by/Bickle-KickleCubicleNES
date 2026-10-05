/* Fixed CPU adaptations of mstan/snesrecomp's CC0 presets, originally
 * Mega Man X Recomp: CRT Soft, LCD Grid, Sharp, Warm Composite.
 * Four subpixels per NES pixel preserve scanline/grid detail in SDL2.
 * Source: GLSL passes in runner/assets/shaders/_passes (main, 2026-10-05).
 * This affects presentation only, never the PPU or savestates. */
static int kickle_filter_mode;
static const char *const kickle_filter_names[]={"Off","CRT Soft","LCD Grid","Sharp","Warm Composite"};
static float filter_smooth(float a,float b,float v) {
    float t=(v-a)/(b-a); if(t<0)t=0; if(t>1)t=1; return t*t*(3-2*t);
}
static int filter_channel(uint32_t p,int shift) { return (p>>shift)&255; }
SDL_Texture *kickle_filter_texture(SDL_Renderer *renderer,const uint32_t *pic,int w,int h,int *scale)
{
    static SDL_Texture *texture;
    static uint32_t *pixels;
    static size_t capacity;
    static int tw,th;
    static int cached_mode=-1;
    static uint8_t lookup[4][4][4][3][256];
    *scale=1;
    if(!kickle_filter_mode || w<1 || h<1 || w>2048 || h>2048) return NULL;
    /* Scan/grid coefficients repeat every four source columns. Build their
     * color transforms once per preset, instead of per displayed subpixel. */
    if(cached_mode!=kickle_filter_mode) {
        for(int phase=0;phase<4;++phase) for(int sy=0;sy<4;++sy) for(int sx=0;sx<4;++sx) {
            float cx=(sx+.5f)*.25f,cy=(sy+.5f)*.25f,mult=1;
            if(kickle_filter_mode==1) {
                float scan=.72f+.36f*filter_smooth(.18f,.50f,cy)*(1-filter_smooth(.55f,.92f,cy));
                float triad=(phase+cx)*.75f; triad-=(int)triad;
                mult=scan*(triad<=.33f ? 1.04f : .92f);
            } else if(kickle_filter_mode==2) {
                float edge=filter_smooth(.04f,.16f,cx),v=filter_smooth(.04f,.16f,cy);
                if(v<edge)edge=v;
                v=filter_smooth(.84f,.96f,cx); if(v<edge)edge=v;
                v=filter_smooth(.84f,.96f,cy); if(v<edge)edge=v;
                mult=.72f+.33f*edge;
            }
            for(int c=0;c<3;++c) for(int value=0;value<256;++value) {
                const float warmth[]={1.05f,1,.94f};
                float v=value*mult*(kickle_filter_mode==1 ? warmth[c] : 1);
                if(v>255)v=255;
                lookup[phase][sy][sx][c][value]=(uint8_t)(v+.5f);
            }
        }
        cached_mode=kickle_filter_mode;
    }
    int ow=w*4,oh=h*4;
    size_t need=(size_t)ow*oh;
    if(need>capacity) {
        uint32_t *next=realloc(pixels,need*sizeof(*pixels));
        if(!next) return NULL;
        pixels=next; capacity=need;
    }
    if(!texture || tw!=ow || th!=oh) {
        SDL_DestroyTexture(texture);
        texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,ow,oh);
        if(!texture) return NULL;
        tw=ow; th=oh;
        SDL_SetTextureScaleMode(texture,SDL_ScaleModeNearest);
    }
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        uint32_t mid=pic[y*w+x],left=pic[y*w+(x ? x-1 : x)],right=pic[y*w+(x+1<w ? x+1 : x)];
        uint32_t up=pic[(y ? y-1 : y)*w+x],down=pic[(y+1<h ? y+1 : y)*w+x];
        float rgb[3];
        for(int c=0;c<3;++c) {
            int shift=16-c*8;
            float m=filter_channel(mid,shift);
            rgb[c]=m;
            if(kickle_filter_mode==3) {
                float blur=(filter_channel(left,shift)+filter_channel(right,shift)+filter_channel(up,shift)+filter_channel(down,shift))*.25f;
                rgb[c]=m+(m-blur)*.35f;
            } else if(kickle_filter_mode==4) {
                const float warmth[]={1.06f,1.01f,.92f};
                rgb[c]=(filter_channel(left,shift)*.18f+m*.64f+filter_channel(right,shift)*.18f)*warmth[c];
            }
        }
        for(int sy=0;sy<4;++sy) for(int sx=0;sx<4;++sx) {
            uint32_t pixel=0xff000000;
            for(int c=0;c<3;++c) {
                float v=rgb[c];
                if(v<0)v=0; if(v>255)v=255;
                pixel|=(uint32_t)lookup[x&3][sy][sx][c][(int)(v+.5f)]<<(16-c*8);
            }
            pixels[(y*4+sy)*ow+x*4+sx]=pixel;
        }
    }
    if(SDL_UpdateTexture(texture,NULL,pixels,ow*4)) return NULL;
    *scale=4; return texture;
}
