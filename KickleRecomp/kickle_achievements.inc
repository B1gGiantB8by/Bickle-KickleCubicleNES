/* Local challenges. No account, network requests or ROM writes. Earned bits
 * are deliberately outside savestates; attempt counters follow rewinds. */
#include <SDL.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define ACH(n,t,d,p) {t,d,p},
static const struct { const char *title, *description; int points; } achievements[] = {
#include "kickle_achievement_list.inc"
};
#undef ACH
static uint32_t achievement_earned;
static int achievements_enabled = 1, achievement_reset_pending;
static uint64_t achievement_dates[24];
static char achievement_path[1024];
static struct AchievementAttempt {
    uint32_t version, world_mask[4], bosses, rush_bosses, continued;
    uint32_t world, stage, collected, crushed, special_mask;
    uint32_t charges, spins, shots, misses, pending_miss, hit_this_frame;
    uint32_t combat, rush_run, first_charge_kill;
} achievement_attempt;

static void achievement_load(void)
{
    static int loaded;
    if(loaded) return;
    loaded=1;
    char *directory=SDL_GetPrefPath("KickleRecomp","KickleCubicle");
    if(!directory) return;
    snprintf(achievement_path,sizeof(achievement_path),"%soffline-achievements.ini",directory);
    SDL_free(directory);
    FILE *f=fopen(achievement_path,"r");
    if(!f) return;
    unsigned id; unsigned long long date;
    while(fscanf(f,"%u=%llu\n",&id,&date)==2) if(id<24 && date) {
        achievement_earned |= 1u<<id; achievement_dates[id]=date;
    }
    fclose(f);
}
static void achievement_save(void)
{
    if(!achievement_path[0]) return;
    char temporary[1060]; snprintf(temporary,sizeof(temporary),"%s.tmp",achievement_path);
    FILE *f=fopen(temporary,"w");
    if(!f) { fprintf(stderr,"Could not save offline achievements: %s\n",achievement_path); return; }
    for(unsigned i=0;i<24;++i) if(achievement_earned&(1u<<i))
        fprintf(f,"%u=%llu\n",i,(unsigned long long)achievement_dates[i]);
    int ok=!ferror(f);
    if(fclose(f)) ok=0;
    if(ok) {
#ifdef _WIN32
        ok=MoveFileExA(temporary,achievement_path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
        ok=rename(temporary,achievement_path)==0;
#endif
    }
    if(!ok) fprintf(stderr,"Could not finish saving offline achievements: %s\n",achievement_path);
}
static int achievement_gameplay(void)
{
    return cyc_mod_peek(0xD1)!=0x5A && cyc_mod_peek(0xD2)!=4;
}
static void achievement_unlock(unsigned id)
{
    if(!achievements_enabled || id>=24 || (achievement_earned&(1u<<id))) return;
    /* Keep the entire Rush run excluded, including its exit cinematic. */
    if(id!=23 && (boss_rush || achievement_attempt.rush_run)) return;
    if(!achievement_gameplay()) return;
    achievement_earned |= 1u<<id;
    achievement_dates[id]=(uint64_t)time(NULL);
    achievement_save();
    cyc_ui_achievement(achievements[id].title,achievements[id].description,achievements[id].points);
}
static void achievement_reset(void)
{
    memset(&achievement_attempt,0,sizeof(achievement_attempt));
    achievement_attempt.version=1;
    achievement_attempt.world=achievement_attempt.stage=255;
    achievement_attempt.rush_run=boss_rush!=0;
}
static int achievement_state_get(uint8_t *buf,int cap)
{
    if(cap<(int)sizeof(achievement_attempt)) return -1;
    memcpy(buf,&achievement_attempt,sizeof(achievement_attempt));
    return sizeof(achievement_attempt);
}
static int achievement_state_validate(const uint8_t *buf,int len)
{
    uint32_t version;
    if(!buf && !len) return 1;
    if(len!=(int)sizeof(achievement_attempt)) return 0;
    memcpy(&version,buf,sizeof(version)); return version==1;
}
static int achievement_state_set(const uint8_t *buf,int len)
{
    if(!buf && !len) { achievement_reset(); return 1; }
    if(!achievement_state_validate(buf,len)) return 0;
    memcpy(&achievement_attempt,buf,sizeof(achievement_attempt)); return 1;
}
static int achievement_hook(uint16_t address)
{
    struct AchievementAttempt *a=&achievement_attempt;
    if(!achievements_enabled || !achievement_gameplay()) return 0;
    unsigned world=cyc_mod_peek(0x10A), stage=cyc_mod_peek(0x10B);
    if(address==0x8252) { achievement_reset(); return 0; }
    if(address==0x8167) { a->continued=1; a->spins=0; return 0; }
    if(address==0x8679) { a->collected=a->crushed=0; return 0; }
    if(address==0x8351) {
        a->world=world; a->stage=stage; a->collected=a->crushed=0;
        a->combat=0; a->rush_run |= boss_rush!=0;
        return 0;
    }
    if(address==0x9AD3) {
        a->combat=1; a->charges=a->spins=a->shots=a->misses=a->pending_miss=0;
        a->hit_this_frame=a->first_charge_kill=0; a->rush_run |= boss_rush!=0; return 0;
    }
    if(address==0x8BB7) { a->spins=0; a->combat=0; return 0; }
    if(address==0x852C && world<5) {
        if(boss_rush || a->rush_run) return 0;
        achievement_unlock(0);
        int lo=cyc_mod_peek(0xBD), hi=cyc_mod_peek(0xBE);
        int timer=(lo&15)+10*(lo>>4)+100*(hi&15)+1000*(hi>>4);
        if(world==0 && stage==11 && timer>=960) achievement_unlock(5);
        if(world==1 && stage==15 && timer>=940) achievement_unlock(6);
        if(timer==0) achievement_unlock(7);
        if(world<4 && stage<(unsigned)level_limits[world]) a->world_mask[world] |= 1u<<stage;
        if(world==4) {
            /* Special Game uses its own round index at $010C. */
            stage=cyc_mod_peek(0x10C);
            if(stage<30) {
                unsigned block=stage/5, mask=31u<<(block*5);
                if(stage%5==0) a->special_mask &= ~mask;
                a->special_mask |= 1u<<stage;
                if((a->special_mask&mask)==mask) achievement_unlock(17+block);
            }
        }
        return 0;
    }
    if(address==0x98DA && world<4) {
        if(boss_rush || a->rush_run) {
            a->rush_bosses |= 1u<<world;
            if(a->rush_bosses==15) achievement_unlock(23);
        } else {
            unsigned mask=(1u<<level_limits[world])-1;
            a->bosses |= 1u<<world;
            if(a->world_mask[world]==mask) achievement_unlock(13+world);
            if(world==0 && a->first_charge_kill) achievement_unlock(8);
            if(world==1 && a->shots && !a->misses && !a->pending_miss) achievement_unlock(9);
            if(a->bosses==15 && !a->continued) {
                int all=1;
                for(unsigned w=0;w<4;++w) all &= a->world_mask[w]==((1u<<level_limits[w])-1);
                if(all) achievement_unlock(12);
            }
        }
        a->combat=0; return 0;
    }
    if(boss_rush || a->rush_run) return 0;
    if(address==0xA1BA) {
        ++a->collected;
        if(world==0 && stage==2 && a->collected==12) achievement_unlock(1);
        if(world==1 && stage==3 && a->collected==7) achievement_unlock(2);
        if(world==3 && stage==0 && a->collected==9) achievement_unlock(3);
    }
    if(address==0xB651) {
        unsigned pointer=cyc_mod_peek(0x0E)|(cyc_mod_peek(0x0F)<<8);
        if(cyc_mod_peek(pointer)==7 && world==0 && stage==14 && ++a->crushed==3) achievement_unlock(4);
    }
    if(a->combat) {
        unsigned object=cyc_mod_peek(0x8C)|(cyc_mod_peek(0x8D)<<8);
        if(address==0xAC26 && world==0) ++a->charges;
        if(address==0xBBCC && world==2 && ++a->spins==20) achievement_unlock(10);
        if(address==0xB6C2 && world==1) ++a->shots;
        if(address==0xB78F && world==1 && cyc_mod_peek(object+0xD)==1) a->pending_miss++;
        if(address==0xA8F0 && world==1) a->hit_this_frame++;
        if(address==0xA8F0 && world==0 && a->charges==1 &&
           cyc_mod_peek(object+0xD)==1 && cyc_mod_peek(0xCA)==1) a->first_charge_kill=1;
    }
    return 0;
}
static void achievement_frame(void)
{
    struct AchievementAttempt *a=&achievement_attempt;
    a->rush_run |= boss_rush!=0;
    if(!achievements_enabled || !achievement_gameplay() || boss_rush || a->rush_run) return;
    if(a->combat) {
        if(a->pending_miss>a->hit_this_frame) a->misses += a->pending_miss-a->hit_this_frame;
        a->pending_miss=a->hit_this_frame=0;
        if(cyc_mod_peek(0x10A)==3 && cyc_mod_peek(0x10B)==17 &&
           cyc_mod_peek(0x4A9)==5 && cyc_mod_peek(0x4B9)==5 &&
           cyc_mod_peek(0x4C9)==5 && cyc_mod_peek(0x4D9)==5) achievement_unlock(11);
    }
}
static int achievement_value(const RecompRuntimeUiItem *item,int *value)
{
    unsigned id;
    if(sscanf(item->key,"kickle.achievement.%u",&id)!=1 || id>=24) return 0;
    *value=(achievement_earned>>id)&1; return 1;
}
static void achievement_register(void)
{
    achievement_load(); achievement_reset();
    nes_mod_register_savestate_hook("kickle.achievement-attempt",achievement_state_get,achievement_state_set);
    nes_mod_register_savestate_validator("kickle.achievement-attempt",achievement_state_validate);
    static const uint16_t addresses[]={0x8252,0x8167,0x8351,0x852C,0x98DA,0xA1BA,0xB651,0xAC26,0xBBCC,0xB6C2,0xB78F,0xA8F0};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i) {
        char id[64]; snprintf(id,sizeof(id),"kickle.achievement.event-%u",i);
        nes_mod_register_function_entry_plugin(id,addresses[i],achievement_hook);
        nes_mod_set_function_hook_enabled(id,1);
    }
}
