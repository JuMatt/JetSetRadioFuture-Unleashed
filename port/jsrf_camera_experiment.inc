/* Opt-in, portable-test-only bridge. No state is serialized to a game save. */
#include "jsrf_camera_intent.h"
static int rcam_mode(void);
#ifndef JCX_FOCUS_OVERRIDES
__attribute__((weak)) int nv_window_camera_focused(void);
__attribute__((weak)) int nv_window_camera_focused(void) { return 1; }
__attribute__((weak)) uint32_t nv_window_camera_focus_generation(void);
__attribute__((weak)) uint32_t nv_window_camera_focus_generation(void) { return 0; }
#endif
static struct {
    JciState state;
    uint32_t camera, player, input, frame, focus, input_overrides, visual_mode;
    double pad_at, applied_at;
    float rx, ry;
    int pad_valid, right;
    float motion_x, motion_z, motion_speed;
    uint32_t motion_frame;
    int motion_valid, moving;
    uint32_t rail_player,rail_input,rail_camera,rail_focus,rail_frame;
    uint32_t rail_exits,rail_238_before,rail_238_after;
    float rail_view,rail_elevation;
    double rail_at;
    int rail_seen,rail_pending;
} s_jcx;
static int jcx_enabled(void)
{
    static int value = -1;
    if (value < 0) { const char *e = getenv("JSRF_CAMERA_EXPERIMENT"); value = e && !strcmp(e,"1"); }
    return value;
}
static int jcx_range(uint64_t a, uint32_t n) { return a >= 0x10000u && a <= (uint64_t)(64u<<20)-n; }
static uint32_t jcx_u(uint64_t a) { uint32_t v=0; if(jcx_range(a,4)) memcpy(&v,(uint8_t *)g_xbox_mem_offset+a,4); return v; }
static float jcx_f(uint64_t a) { uint32_t b=jcx_u(a); float f; memcpy(&f,&b,4); return f; }
static uint32_t jcx_object(uint32_t id)
{
    uint32_t gm=jcx_u(0x22FCE0u);
    return (int32_t)id >= 0 && jcx_range(gm,0x98) ? jcx_u((uint64_t)gm+0x98u+4ull*id) : 0;
}
static int jcx_slot(uint32_t input)
{
    uint32_t index=jcx_u((uint64_t)input+0x8AC), slot;
    if(index>4) return -1;
    slot=jcx_u(0x215770u+4u*index);
    /* Physical player zero or the selected-controller mirror observed in M1. */
    return slot==0 || (slot==4 && jcx_u(0x251F20u)==0) ? (int)slot : -1;
}
static int jcx_fresh(void) { return s_jcx.pad_valid && jsrf_now()-s_jcx.pad_at < .25 && nv_window_camera_focused(); }
static int jcx_identity(uint32_t input, uint32_t player, uint32_t cam)
{
    return jcx_range(input,0x900) && jcx_range(player,0x1178) && jcx_range(cam,0x240) &&
        jcx_u((uint64_t)player+0x24)==input && jcx_u((uint64_t)cam+0x24)==player &&
        jcx_object(jcx_u((uint64_t)input+0x8D4))==player && jcx_object(jcx_u((uint64_t)input+0x8D8))==cam && jcx_slot(input)>=0;
}
static int jcx_protected(uint32_t input, uint32_t p, uint32_t cam)
{
    uint32_t context=jcx_u((uint64_t)p+0xF00);
    /* A76E0 checks the pointed-to action context, not pointer presence.
     * A normal idle hub has F00!=0 but context+38==0. */
    int context_active=context && (!jcx_range(context,0x3C) || jcx_u((uint64_t)context+0x38));
    (void)cam;
    return s_photo.on || s_photo.want || s_world_frozen || !jcx_fresh() ||
        jcx_u((uint64_t)input+0x8B0) || jcx_u((uint64_t)input+0x4C) ||
        (int32_t)jcx_u((uint64_t)p+0x1170)>0 || (int32_t)jcx_u((uint64_t)p+0x1174)>0 ||
        jcx_u((uint64_t)p+0xF18) || jcx_u((uint64_t)p+0xEC8) ||
        (jcx_u((uint64_t)p+0xE38) && jcx_u((uint64_t)p+0xE3C)) ||
        context_active || jcx_u((uint64_t)p+0xF04);
}
static int jcx_free(uint32_t input, uint32_t player, uint32_t cam)
{
    uint32_t state=jcx_u((uint64_t)input+0x8B4), action=jcx_u((uint64_t)player+0xE64);
    return jcx_identity(input,player,cam) && !jcx_protected(input,player,cam) &&
        (state==0 || state==2) && jcx_u((uint64_t)cam+0x208)==3 && !jcx_u((uint64_t)cam+0x238) && jcx_u((uint64_t)player+0xE58)==3 &&
        !jcx_u((uint64_t)player+0xFD4) && !jcx_u((uint64_t)player+0xE60) && action==0xFFFFFFFFu;
}
static int jcx_orbit(uint32_t input,uint32_t player,uint32_t cam)
{
    /* Visual freedom and free-movement input mapping are separate permissions.
     * The grind orbit never substitutes the rail's native control heading. */
    uint32_t mode=jcx_u((uint64_t)cam+0x208),state=jcx_u((uint64_t)input+0x8B4);
    return jcx_identity(input,player,cam) && !jcx_protected(input,player,cam) &&
        (state==0 || state==2) &&
        ((mode==3 && !jcx_u((uint64_t)cam+0x238)) || mode==5) &&
        jcx_u((uint64_t)player+0xE58)==mode &&
        !jcx_u((uint64_t)player+0xFD4) && !jcx_u((uint64_t)player+0xE60) &&
        jcx_u((uint64_t)player+0xE64)==0xFFFFFFFFu;
}
static void jcx_motion(uint32_t player,float dt)
{
    float x=jcx_f((uint64_t)player+0xCA4),z=jcx_f((uint64_t)player+0xCAC);
    if(!isfinite(x)||!isfinite(z)) {s_jcx.motion_valid=s_jcx.moving=0;s_jcx.motion_speed=0;return;}
    if(s_jcx.motion_valid && s_jcx.motion_frame==s_rcam_frames) return;
    if(s_jcx.motion_valid && dt>0 && s_rcam_frames-s_jcx.motion_frame<=6u) {
        float dx=x-s_jcx.motion_x,dz=z-s_jcx.motion_z,distance=sqrtf(dx*dx+dz*dz);
        if(isfinite(distance) && distance<100.f) {
            s_jcx.motion_speed=distance/dt;
            s_jcx.moving=s_jcx.motion_speed>(s_jcx.moving ? 1.f : 3.f);
        } else {s_jcx.moving=0;s_jcx.motion_speed=0;} /* diagnostic sample only */
    } else {s_jcx.moving=0;s_jcx.motion_speed=0;}
    s_jcx.motion_x=x;s_jcx.motion_z=z;s_jcx.motion_frame=s_rcam_frames;s_jcx.motion_valid=1;
}
static void jcx_reset_core(void)
{
    jci_reset(&s_jcx.state); s_jcx.camera=s_jcx.player=s_jcx.input=0; s_jcx.applied_at=0;
    s_jcx.visual_mode=0;
    s_jcx.motion_valid=s_jcx.moving=0;s_jcx.motion_speed=0;
}
static void jcx_forget(void) {jcx_reset_core();s_jcx.rail_seen=s_jcx.rail_pending=0;}
static void jcx_pad_failed(void) { s_jcx.pad_valid=0; s_jcx.right=0; s_jcx.rail_seen=s_jcx.rail_pending=0; jcx_forget(); }
static void jcx_note_pad(const XBOX_INPUT_STATE *pad)
{
    float x=(float)pad->Gamepad.sThumbRX/32767.f, y=(float)pad->Gamepad.sThumbRY/32767.f;
    float mag=sqrtf(x*x+y*y), dead=s_jcx.right ? .18f : .22f;
    s_jcx.right=mag>=dead;
    if(!s_jcx.right) x=y=0;
    else { float k=(fminf(mag,1.f)-.18f)/(.82f*mag); x*=k; y*=k; }
    s_jcx.rx=RCAM_CURVE(x)*RCAM_YAW; s_jcx.ry=RCAM_CURVE(y)*RCAM_PITCH;
    s_jcx.pad_at=jsrf_now(); s_jcx.pad_valid=1;
}
static int jcx_native_reset(uint32_t input)
{
    int slot=jcx_slot(input);
    return slot>=0 && (jcx_u(0x251DE8u+64u*(unsigned)slot)&0x100u)!=0;
}
static int jcx_native_pose(uint32_t cam,float *yaw,float *elevation)
{
    float x=jcx_f((uint64_t)cam+0x44)-jcx_f((uint64_t)cam+0x5C);
    float y=jcx_f((uint64_t)cam+0x48)-jcx_f((uint64_t)cam+0x60);
    float z=jcx_f((uint64_t)cam+0x4C)-jcx_f((uint64_t)cam+0x64);
    int32_t hd=(int32_t)jcx_u((uint64_t)cam+0x228);
    float h=sqrtf(x*x+z*z);
    if(!isfinite(x)||!isfinite(y)||!isfinite(z)||!isfinite(h)||h<.001f||hd < -32768||hd > 32767) return 0;
    *yaw=(float)hd*(2.f*JCI_PI/65536.f);*elevation=atan2f(y,h);return 1;
}
static void jcx_rail_observe(uint32_t input,uint32_t player,uint32_t cam)
{
    if(!s_jcx.rail_seen) return;
    uint32_t mode=jcx_u((uint64_t)cam+0x208),pm=jcx_u((uint64_t)player+0xE58),state=jcx_u((uint64_t)input+0x8B4);
    if(input!=s_jcx.rail_input || player!=s_jcx.rail_player || cam!=s_jcx.rail_camera ||
       !jcx_identity(input,player,cam) || s_jcx.rail_focus!=nv_window_camera_focus_generation() ||
       !((mode==3||mode==5)&&(pm==3||pm==5)) || (state!=0&&state!=2) ||
       jcx_protected(input,player,cam) || jcx_native_reset(input) || jcx_u((uint64_t)player+0xFD4) ||
       jcx_u((uint64_t)player+0xE60) || jcx_u((uint64_t)player+0xE64)!=0xFFFFFFFFu ||
       s_rcam_frames-s_jcx.rail_frame>6u || jsrf_now()-s_jcx.rail_at>.25) {
        jcx_forget();return;
    }
    if(mode!=5 || pm!=5) {if(!s_jcx.rail_pending)s_jcx.rail_frame=s_rcam_frames;s_jcx.rail_pending=1;}
}
/* Only called after the original camera publisher: the new native pose exists.
 * 238 is native direction-continuity memory, deliberately reset once at this
 * narrowly guarded boundary. 234 and input histories remain native. */
static void jcx_finish_rail_exit(uint8_t *m,uint32_t input,uint32_t player,uint32_t cam)
{
    float yaw,elevation;
    jcx_rail_observe(input,player,cam);
    if(!s_jcx.rail_seen || !s_jcx.rail_pending || jcx_u((uint64_t)cam+0x208)!=3 ||
       jcx_u((uint64_t)player+0xE58)!=3 || !jcx_native_pose(cam,&yaw,&elevation)) return;
    s_jcx.rail_238_before=jcx_u((uint64_t)cam+0x238);
    uint32_t zero=0;memcpy(m+cam+0x238,&zero,4);
    s_jcx.rail_238_after=0;s_jcx.rail_exits++;
    jci_reset(&s_jcx.state);
    /* Leaving a rail does not relinquish manual ownership. Continue from
     * the last actually shown pose even with RS released; LT remains reset. */
    s_jcx.state.valid=s_jcx.state.owned=1;
    s_jcx.state.view=s_jcx.state.control=s_jcx.rail_view;
    s_jcx.state.elevation=s_jcx.rail_elevation;
    s_jcx.motion_valid=s_jcx.moving=0;s_jcx.motion_speed=0;s_jcx.visual_mode=3;
    s_jcx.camera=cam;s_jcx.player=player;s_jcx.input=input;
    s_jcx.focus=nv_window_camera_focus_generation();s_jcx.applied_at=jsrf_now();
    s_jcx.rail_seen=s_jcx.rail_pending=0;
}
static void jcx_prepare_input(uint32_t input)
{
    if(!jcx_enabled()) return;
    uint32_t player=jcx_object(jcx_u((uint64_t)input+0x8D4));
    uint32_t cam=jcx_object(jcx_u((uint64_t)input+0x8D8));
    if(!jcx_identity(input,player,cam)) {s_jcx.rail_seen=s_jcx.rail_pending=0;return;}
    jcx_rail_observe(input,player,cam);
    uint32_t mode=jcx_u((uint64_t)cam+0x208),player_mode=jcx_u((uint64_t)player+0xE58);
    if(rcam_mode()!=1 || mode!=3 || player_mode!=3) {
        /* Do not let the previous normal-camera overlay heading leak into
         * the first native grind/target command. Restore only our own write. */
        int32_t *hd=(int32_t *)((uint8_t *)g_xbox_mem_offset+cam+0x228);
        if(s_rcam.applied && cam==s_rcam.cam && *hd==s_rcam.our_hd) {
            *hd=s_rcam.game_hd;s_rcam.our_hd=s_rcam.game_hd;
        }
    }
    if(rcam_mode()!=1) {
        if(s_jcx.state.owned) s_rcam.yaw=s_rcam.pitch=0;
        jcx_forget();
    }
}
/* At the proved direct-movement branch, never overlay the camera's guest fields. */
static int jcx_input_heading(uint32_t input, uint32_t cam, uint32_t *heading)
{
    uint32_t p=jcx_object(jcx_u((uint64_t)input+0x8D4));
    jcx_prepare_input(input);
    if(jcx_enabled() && jcx_native_reset(input)) { jcx_forget(); return 0; }
    if(!jcx_enabled() || rcam_mode()!=1 || s_jcx.rail_pending || !s_jcx.state.valid || !s_jcx.state.owned || s_jcx.input!=input || s_jcx.camera!=cam ||
       !jcx_free(input,p,cam) || jsrf_now()-s_jcx.applied_at>.25) return 0;
    s_jcx.state.control=s_jcx.state.view;
    *heading=(uint32_t)(int32_t)(int16_t)(uint16_t)lrintf(s_jcx.state.control*(65536.f/(2.f*JCI_PI)));
    s_jcx.input_overrides++;
    return 1;
}
static int jcx_apply(uint8_t *m, uint32_t cam, float dt)
{
    uint32_t p=jcx_u((uint64_t)cam+0x24), input=jcx_u((uint64_t)p+0x24), focus=nv_window_camera_focus_generation();
    float *eye=(float *)(m+cam+0x44), *target=(float *)(m+cam+0x5C), v[3], h, length, elevation, offset, ca,sa;
    int32_t *hd=(int32_t *)(m+cam+0x228);
    JciInput in={0};
    if(!jcx_enabled()) return 0;
    s_rcam.active_until=0;
    int preference=rcam_mode();
    if(preference!=1) {
        /* Clean the custom offset once, not the legacy tracking orbit on
         * every frame. prepare_input performs the same cleanup before forget. */
        if(preference==0 || s_jcx.state.owned) s_rcam.yaw=s_rcam.pitch=0;
        jcx_forget();return preference==0;
    }
    jcx_finish_rail_exit(m,input,p,cam);
    /* Native LT reset/targeting wins even during a native238 transition. No
     * host hold, synthetic edge, or LB behavior exists in this candidate. */
    if(jcx_identity(input,p,cam) && jcx_native_reset(input)) { jcx_forget(); s_rcam.yaw=s_rcam.pitch=0; return 1; }
    if(!jcx_orbit(input,p,cam)) {
        /* A temporary3/5 mismatch can publish a new native pose. If RS remains
         * active, continuity must start from what was actually shown then. */
        if(s_jcx.rail_pending && jcx_native_pose(cam,&s_jcx.rail_view,&s_jcx.rail_elevation)) jcx_reset_core();
        else jcx_forget();
        s_rcam.yaw=s_rcam.pitch=0; return 1;
    }
    if(s_jcx.camera!=cam || s_jcx.player!=p || s_jcx.input!=input || s_jcx.focus!=focus ||
       (s_jcx.applied_at && jsrf_now()-s_jcx.applied_at>.25)) jcx_forget();
    s_jcx.camera=cam; s_jcx.player=p; s_jcx.input=input; s_jcx.focus=focus;
    uint32_t mode=jcx_u((uint64_t)cam+0x208);
    s_jcx.visual_mode=mode;
    for(int i=0;i<3;i++) v[i]=eye[i]-target[i];
    h=sqrtf(v[0]*v[0]+v[2]*v[2]); length=sqrtf(h*h+v[1]*v[1]);
    if(!isfinite(length) || length<.001f || h<.001f) { jcx_forget(); return 1; }
    elevation=atan2f(v[1],h);
    in.dt=dt; in.native_view=(float)(int16_t)*hd*(2.f*JCI_PI/65536.f); in.native_elevation=elevation;
    int slot=jcx_slot(input);
    float mag=jcx_f(0x251E18u+64u*(unsigned)slot);
    if(!isfinite(mag) || mag<0 || mag>1.1f) { jcx_forget(); return 1; }
    in.rx=s_jcx.rx; in.ry=s_jcx.ry; in.right_active=s_jcx.right;
    jcx_motion(p,dt); /* telemetry only: movement never requests a handoff */
    jci_step(&s_jcx.state,&in);
    if(mode==5 && s_jcx.state.owned) {
        jct_request_rail_capture();
        s_jcx.rail_seen=1;s_jcx.rail_pending=0;s_jcx.rail_player=p;s_jcx.rail_input=input;s_jcx.rail_camera=cam;
        s_jcx.rail_focus=focus;s_jcx.rail_frame=s_rcam_frames;
        s_jcx.rail_at=jsrf_now();
        s_jcx.rail_view=s_jcx.state.view;s_jcx.rail_elevation=s_jcx.state.elevation;
    }
    s_jcx.frame=s_rcam_frames; s_jcx.applied_at=jsrf_now(); s_rcam.active_until=jsrf_now()+.25;
    if(!s_jcx.state.owned) {
        /* Native assistance is literal until RS claims manual ownership. */
        s_rcam.yaw=s_rcam.pitch=0; return 1;
    }
    offset=jci_wrap(s_jcx.state.view-in.native_view); ca=cosf(offset); sa=sinf(offset);
    float x=v[0]*ca+v[2]*sa,z=v[2]*ca-v[0]*sa;
    float el=fminf(fmaxf(elevation,RCAM_EL_MAX),fmaxf(fminf(elevation,RCAM_EL_MIN),s_jcx.state.elevation));
    float scale=length*cosf(el)/h;
    memcpy(s_rcam.game_eye,eye,12); s_rcam.game_hd=*hd;
    eye[0]=target[0]+x*scale; eye[1]=target[1]+length*sinf(el); eye[2]=target[2]+z*scale;
    if(jcx_u((uint64_t)cam+0x208)==3)
        *hd=(int32_t)(int16_t)(uint16_t)lrintf(s_jcx.state.view*(65536.f/(2.f*JCI_PI)));
    memcpy(s_rcam.our_eye,eye,12); s_rcam.our_hd=*hd; s_rcam.cam=cam; s_rcam.applied=1; s_rcam.photo=0;
    s_rcam.yaw=offset; s_rcam.pitch=elevation-el; s_rcam.active_until=jsrf_now()+.25;
    return 1;
}
