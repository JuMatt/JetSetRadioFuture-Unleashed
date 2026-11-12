#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "../../src/input/xinput_xbox.h"
static ptrdiff_t g_xbox_mem_offset;
static double clock_now=1;
static double jsrf_now(void) { return clock_now; }
static int test_camera_preference=1;
static int rcam_mode(void) { return test_camera_preference; }
static uint32_t s_rcam_frames;
static int s_world_frozen;
static struct { int on,want; } s_photo;
static struct { double active_until; float yaw,pitch,game_eye[3],our_eye[3]; int32_t game_hd,our_hd; uint32_t cam; int applied,photo; } s_rcam;
#define RCAM_CURVE(x) ((x)*sqrtf(fabsf(x)))
#define RCAM_YAW 2.6f
#define RCAM_PITCH 1.4f
#define RCAM_EL_MIN -.30f
#define RCAM_EL_MAX 1.2f
#define JCX_FOCUS_OVERRIDES 1
static int test_focused=1;
static uint32_t test_focus_generation;
int nv_window_camera_focused(void) {return test_focused;}
uint32_t nv_window_camera_focus_generation(void) {return test_focus_generation;}
static void jct_request_rail_capture(void) { }
#include "../jsrf_camera_experiment.inc"
static void put(uint32_t a,uint32_t v) { memcpy((uint8_t *)g_xbox_mem_offset+a,&v,4); }
static void putf(uint32_t a,float f) { memcpy((uint8_t *)g_xbox_mem_offset+a,&f,4); }
static uint64_t hash(void) { uint8_t *m=(uint8_t *)g_xbox_mem_offset;uint64_t h=0;for(unsigned n=0;n<(64u<<20);n++)h=h*31+m[n];return h; }
static uint64_t hash_without_overlay(uint32_t c) {
    uint8_t *m=(uint8_t *)g_xbox_mem_offset;uint64_t h=0;
    for(unsigned n=0;n<(64u<<20);n++) {
        if((n>=c+0x44&&n<c+0x50)||(n>=c+0x228&&n<c+0x22C)||(n>=c+0x238&&n<c+0x23C))continue;
        h=h*31+m[n];
    }
    return h;
}
static void own_rail(uint32_t p,uint32_t n,uint32_t c,XBOX_INPUT_STATE *pad)
{
    jcx_forget();put(c+0x208,5);put(p+0xE58,5);put(c+0x238,1);put(n+0x8B4,0);
    putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,123);s_rcam.applied=0;
    pad->Gamepad.sThumbRX=32767;jcx_note_pad(pad);jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    assert(s_jcx.rail_seen && s_jcx.state.owned);
}
int main(void)
{
    const uint32_t p=0x30000,n=0x40000,c=0x50000,gm=0x20000,slot=4;
    XBOX_INPUT_STATE pad={0}; uint32_t heading;
    g_xbox_mem_offset=(ptrdiff_t)calloc(1,64u<<20);assert(g_xbox_mem_offset);
    _putenv_s("JSRF_CAMERA_EXPERIMENT","1");
    put(0x22FCE0,gm);put(gm+0x98+2*4,p);put(gm+0x98+3*4,c);
    put(p+0x24,n);put(c+0x24,p);put(n+0x8D4,2);put(n+0x8D8,3);
    put(n+0x8AC,4);put(0x215770+4*4,4);put(0x251F20,0);
    put(c+0x208,3);put(p+0xE58,3);put(p+0xE64,0xFFFFFFFFu);
    jcx_note_pad(&pad);assert(jcx_slot(n)==4);assert(jcx_free(n,p,c));
    /* Real hub regression: F00 is allocated while idle; camera1E0 is RGBA
     * interpolation completion, not a protected camera mode. */
    put(p+0xF00,0x70000);put(c+0x1E0,1);assert(jcx_free(n,p,c));
    put(0x70000+0x38,1);assert(!jcx_free(n,p,c));put(0x70000+0x38,0);
    put(p+0x1170,0xFFFFFFFFu);put(p+0x1174,0xFFFFFFFFu);assert(jcx_free(n,p,c));
    put(p+0x1170,1);assert(!jcx_free(n,p,c));put(p+0x1170,0);put(p+0x1174,0);
    put(n+0x8B4,2);assert(jcx_free(n,p,c));put(n+0x8B4,5);assert(!jcx_free(n,p,c));put(n+0x8B4,0);
    put(c+0x238,1);assert(!jcx_free(n,p,c)&&!jcx_orbit(n,p,c));put(c+0x238,0);
    put(p+0xE60,1);assert(!jcx_free(n,p,c));put(p+0xE60,0);
    put(0x251F20,1);assert(jcx_slot(n)==-1);put(0x251F20,0);
    s_jcx.state.valid=s_jcx.state.owned=1;s_jcx.state.view=.5f;s_jcx.state.control=.1f;s_jcx.input=n;s_jcx.camera=c;s_jcx.applied_at=clock_now;
    putf(0x251E18+slot*64,.5f);uint64_t before=hash();
    assert(jcx_input_heading(n,c,&heading));assert((int16_t)heading==(int16_t)lrintf(.5f*65536.f/(2.f*JCI_PI)));assert(hash()==before);
    /* First RS sample reconnects movement to the already-shown view before
     * native input consumption, without waiting for the next publication. */
    pad.Gamepad.sThumbRX=12000;jcx_note_pad(&pad);before=hash();
    assert(jcx_input_heading(n,c,&heading));assert((int16_t)heading==(int16_t)lrintf(.5f*65536.f/(2.f*JCI_PI)));
    assert(s_jcx.state.control==s_jcx.state.view&&hash()==before);
    pad.Gamepad.sThumbRX=0;jcx_note_pad(&pad);
    putf(0x251E18+slot*64,0);assert(jcx_input_heading(n,c,&heading));assert(s_jcx.state.control==s_jcx.state.view);
    /* Exact guest mirror and physical blocks remain untouched by host decisions. */
    put(p+0xFD4,0);put(p+0xE58,3);put(c+0x208,3);
    pad.Gamepad.sThumbRX=7000;jcx_note_pad(&pad);assert(!s_jcx.right);
    pad.Gamepad.sThumbRX=8000;jcx_note_pad(&pad);assert(s_jcx.right);
    pad.Gamepad.sThumbRX=6500;jcx_note_pad(&pad);assert(s_jcx.right);
    pad.Gamepad.sThumbRX=5000;jcx_note_pad(&pad);assert(!s_jcx.right);
    /* Exercise publication, not just the predicate: the captured idle-hub
     * fields must permit a real right-stick eye/heading change. */
    jcx_forget();putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);
    putf(c+0x5C,0);putf(c+0x60,14);putf(c+0x64,0);put(c+0x228,0);put(p+0xCC0,0);
    put(p+0xF00,0x70000);put(c+0x1E0,1);put(0x70000+0x38,0);putf(0x251E18+slot*64,0);
    pad.Gamepad.sThumbRX=32767;jcx_note_pad(&pad);
    assert(jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f)==1);
    assert(s_jcx.state.valid&&s_jcx.state.owned&&s_rcam.applied&&s_rcam.active_until>clock_now);
    assert(jcx_f(c+0x44)<-.1f && (int32_t)jcx_u(c+0x228)>0);
    float held=s_jcx.state.view;
    pad.Gamepad.sThumbRX=0;
    for(int k=0;k<180;k++) {
        memcpy((uint8_t *)g_xbox_mem_offset+c+0x44,s_rcam.game_eye,12);put(c+0x228,(uint32_t)s_rcam.game_hd);
        clock_now+=1./60.;jcx_note_pad(&pad);jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
        assert(fabsf(s_jcx.state.view-held)<.00001f);
    }
    assert(jcx_input_heading(n,c,&heading));assert((int16_t)heading==(int16_t)jcx_u(c+0x228));
    /* Native reset wins before camera238 eligibility; LT/LB are never edited. */
    pad.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER]=255;pad.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]=255;
    XBOX_INPUT_STATE original=pad;jcx_note_pad(&pad);assert(!memcmp(&original,&pad,sizeof pad));
    put(0x251DE8+64*slot,0x100);put(c+0x238,1);before=hash();
    assert(!jcx_input_heading(n,c,&heading));assert(!s_jcx.state.valid);assert(hash()==before);
    s_jcx.state.valid=s_jcx.state.owned=1;before=hash();
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(!s_jcx.state.valid);assert(hash()==before);
    put(0x251DE8+64*slot,0);put(c+0x238,0);
    /* Native assistance stays literal after LT until a new RS sample. */
    pad.Gamepad.sThumbRX=0;jcx_note_pad(&pad);before=hash();
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    assert(hash()==before&&!s_jcx.state.owned);
    /* Real displacement plus held LS must NEVER request a handoff. */
    putf(0x251E18+slot*64,.5f);
    pad.Gamepad.sThumbRX=32767;jcx_note_pad(&pad);
    putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,0);
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    pad.Gamepad.sThumbRX=0;putf(0x251E18+slot*64,.5f);
    float command=s_jcx.state.control;
    float manual_view=s_jcx.state.view;
    for(int k=0;k<600;k++) {
        float native_eye[3]={1.f+k*.01f,18,-38};uint32_t native_heading=100u+k*10u;
        memcpy((uint8_t *)g_xbox_mem_offset+c+0x44,native_eye,12);put(c+0x228,native_heading);
        s_rcam.applied=0;s_rcam_frames++;putf(p+0xCA4,k*.1f);clock_now+=1./60.;jcx_note_pad(&pad);jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
        assert(fabsf(s_jcx.state.control-command)<.00001f);
        assert(s_jcx.state.owned&&s_jcx.state.control==s_jcx.state.view);assert(fabsf(s_jcx.state.view-manual_view)<.00001f);
    }
    /* User regression: release/repress LS while still coasting after orbit.
     * Neither one neutral edge nor repeated gestures may schedule a return. */
    putf(0x251E18+slot*64,0);s_rcam.applied=0;
    putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,0);
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(s_jcx.state.owned);
    putf(0x251E18+slot*64,.5f);command=s_jcx.state.view;
    for(int k=0;k<1800;k++) {
        float native_eye[3]={1.f+k*.01f,18,-38};uint32_t native_heading=100u+k*10u;
        memcpy((uint8_t *)g_xbox_mem_offset+c+0x44,native_eye,12);put(c+0x228,native_heading);
        putf(0x251E18+slot*64,k%180==0?0.f:.5f);putf(p+0xCA4,60.f+k*.1f);
        s_rcam.applied=0;s_rcam_frames++;clock_now+=1./60.;jcx_note_pad(&pad);jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
        assert(fabsf(s_jcx.state.control-command)<.00001f);
        assert(s_jcx.state.owned&&s_rcam.applied&&s_jcx.state.control==s_jcx.state.view);
        assert(fabsf(s_jcx.state.view-manual_view)<.00001f);
    }
    /* A normal overlay heading cannot leak into the first grind command. */
    pad.Gamepad.sThumbRX=32767;putf(0x251E18+slot*64,0);jcx_note_pad(&pad);
    putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,123);
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(s_rcam.applied);
    int32_t native_hd=s_rcam.game_hd;put(p+0xE58,5);put(n+0x8BC,0x3456);
    s_jcx.state.valid=0;jcx_prepare_input(n);assert(jcx_u(c+0x228)==(uint32_t)native_hd);assert(jcx_u(n+0x8BC)==0x3456);s_jcx.state.valid=1;
    /* Mode5 changes only the visual eye: native heading, input and actor stay. */
    put(c+0x208,5);put(c+0x234,0x6789);put(c+0x238,1);assert(jcx_orbit(n,p,c)&&!jcx_free(n,p,c));
    put(n+0x8B4,5);assert(!jcx_orbit(n,p,c));put(n+0x8B4,0);
    unsigned char native_input[0x900],native_player[0x1178];
    memcpy(native_input,(uint8_t *)g_xbox_mem_offset+n,sizeof native_input);memcpy(native_player,(uint8_t *)g_xbox_mem_offset+p,sizeof native_player);
    putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,123);s_rcam.applied=0;
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    assert(jcx_f(c+0x44)<-.1f && jcx_u(c+0x228)==123 && jcx_u(c+0x234)==0x6789 && jcx_u(c+0x238)==1);
    assert(!memcmp(native_input,(uint8_t *)g_xbox_mem_offset+n,sizeof native_input));assert(!memcmp(native_player,(uint8_t *)g_xbox_mem_offset+p,sizeof native_player));
    assert(!jcx_input_heading(n,c,&heading));
    put(c+0x208,3);put(p+0xE58,3);put(c+0x238,1);s_jcx.state.control=-1;
    uint32_t exits=s_jcx.rail_exits;jcx_prepare_input(n);
    assert(s_jcx.rail_pending && s_jcx.state.control==-1 && jcx_u(c+0x238)==1 && s_jcx.rail_exits==exits);
    assert(!jcx_input_heading(n,c,&heading)); /* old view is never consumed as the new reference */
    /* Native publisher is180 degrees away, RS is up. Manual ownership stays,
     * controls match its shown view, and only eye/heading/238 may be changed. */
    pad.Gamepad.sThumbRX=0;jcx_note_pad(&pad);putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,38);put(c+0x228,(uint32_t)-32768);s_rcam.applied=0;
    uint64_t expected=hash_without_overlay(c);float saved_rail=s_jcx.rail_view;
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    assert(hash_without_overlay(c)==expected && s_jcx.rail_exits==exits+1 && !s_jcx.rail_seen && !s_jcx.rail_pending&&jcx_u(c+0x238)==0);
    assert(s_jcx.state.valid&&s_jcx.state.owned&&s_rcam.applied);
    assert(fabsf(jci_wrap(s_jcx.state.view-saved_rail))<.00001f && s_jcx.state.control==s_jcx.state.view);
    assert(jcx_u(c+0x234)==0x6789 && !memcmp(native_input,(uint8_t *)g_xbox_mem_offset+n,sizeof native_input));
    assert(jcx_input_heading(n,c,&heading));assert((int16_t)heading==(int16_t)jcx_u(c+0x228));
    put(c+0x238,1);jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    assert(jcx_u(c+0x238)==1 && s_jcx.rail_exits==exits+1); /* never an ongoing238 bypass */
    /* RS held over the same boundary: preserve the shown pose and attach the
     * movement reference to it. A temporary3/5 publication isn't consumed early. */
    own_rail(p,n,c,&pad);put(c+0x208,3);put(p+0xE58,5);put(c+0x228,16000);s_rcam.applied=0;
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(s_jcx.rail_pending&&!s_jcx.state.valid);
    float last_shown=s_jcx.rail_view;put(p+0xE58,3);put(c+0x228,(uint32_t)-30000);s_rcam.applied=0;
    exits=s_jcx.rail_exits;jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);
    assert(s_jcx.rail_exits==exits+1 && jcx_u(c+0x238)==0 && s_jcx.state.owned);
    assert(fabsf(jci_wrap(s_jcx.state.view-last_shown-s_jcx.rx/60.f))<.00001f && s_jcx.state.control==s_jcx.state.view);
    /* New LT, script, different mode or focus cancels provenance, never238. */
    own_rail(p,n,c,&pad);put(c+0x208,3);put(p+0xE58,3);put(0x251DE8+slot*64,0x100);exits=s_jcx.rail_exits;
    before=hash();jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(hash()==before&&!s_jcx.rail_seen&&jcx_u(c+0x238)==1&&s_jcx.rail_exits==exits);put(0x251DE8+slot*64,0);
    own_rail(p,n,c,&pad);put(c+0x208,3);put(p+0xE58,3);put(p+0xF18,1);
    before=hash();jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(hash()==before&&!s_jcx.rail_seen&&jcx_u(c+0x238)==1);put(p+0xF18,0);
    own_rail(p,n,c,&pad);put(c+0x208,6);put(p+0xE58,6);
    before=hash();jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(hash()==before&&!s_jcx.rail_seen&&jcx_u(c+0x238)==1);
    own_rail(p,n,c,&pad);put(c+0x208,3);put(p+0xE58,3);test_focus_generation++;
    before=hash();jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(hash()==before&&!s_jcx.rail_seen&&jcx_u(c+0x238)==1);
    own_rail(p,n,c,&pad);put(c+0x208,9);put(p+0xE58,9);put(p+0xFD4,1);
    before=hash();jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(hash()==before&&!s_jcx.rail_seen);put(p+0xFD4,0);
    own_rail(p,n,c,&pad);put(c+0x208,3);put(p+0xE58,3);clock_now+=.3;jcx_note_pad(&pad);
    before=hash();jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(hash()==before&&!s_jcx.rail_seen);
    put(c+0x208,3);put(p+0xE58,3);put(c+0x238,0);
    /* Actual horizontal displacement: distinct frames, hysteresis, teleport. */
    jcx_forget();s_rcam_frames=1000;putf(p+0xCA4,0);putf(p+0xCAC,0);jcx_motion(p,1.f/60.f);assert(!s_jcx.moving);
    s_rcam_frames++;putf(p+0xCA4,.1f);jcx_motion(p,1.f/60.f);assert(s_jcx.moving);
    putf(p+0xCA4,500);jcx_motion(p,1.f/60.f);assert(s_jcx.motion_x==.1f); /* duplicate ignored */
    s_rcam_frames++;jcx_motion(p,1.f/60.f);assert(!s_jcx.moving&&s_jcx.motion_speed==0);
    s_rcam_frames++;putf(p+0xCA4,500.04f);jcx_motion(p,1.f/60.f);assert(!s_jcx.moving);
    s_rcam_frames++;putf(p+0xCA4,500.10f);jcx_motion(p,1.f/60.f);assert(s_jcx.moving);
    s_rcam_frames++;putf(p+0xCA4,500.13f);jcx_motion(p,1.f/60.f);assert(s_jcx.moving);
    s_rcam_frames++;jcx_motion(p,1.f/60.f);assert(!s_jcx.moving);
    /* Grinding with neutral or fresh LS never relinquishes manual camera.
     * Native rail input/heading stays untouched. */
    putf(0x251E18+slot*64,0);
    jcx_forget();put(c+0x208,5);put(p+0xE58,5);put(c+0x238,1);jcx_note_pad(&pad);
    putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,321);s_rcam.applied=0;
    jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);pad.Gamepad.sThumbRX=0;
    for(int k=0;k<600;k++) {
        s_rcam_frames++;clock_now+=1./60.;putf(p+0xCA4,501.f+k*.1f);jcx_note_pad(&pad);
        putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,321);s_rcam.applied=0;
        jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(jcx_u(c+0x228)==321);
        assert(s_jcx.state.owned&&s_jcx.state.control==s_jcx.state.view);
    }
    putf(0x251E18+slot*64,.5f);
    for(int k=0;k<210;k++) {
        s_rcam_frames++;clock_now+=1./60.;jcx_note_pad(&pad);
        putf(c+0x44,0);putf(c+0x48,18);putf(c+0x4C,-38);put(c+0x228,321);s_rcam.applied=0;
        jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f);assert(jcx_u(c+0x228)==321);
        assert(s_jcx.state.owned&&s_jcx.state.control==s_jcx.state.view);
    }
    assert(s_jcx.state.owned&&s_rcam.applied);assert(jcx_u(n+0x8BC)==0x3456);
    /* Preserve the existing UI's Off and Tracking choices. Switching out of
     * camera mode restores only our own heading before native input. */
    put(c+0x208,3);put(p+0xE58,3);put(c+0x238,0);
    s_rcam.cam=c;s_rcam.applied=1;s_rcam.game_hd=123;s_rcam.our_hd=456;put(c+0x228,456);
    s_rcam.yaw=.6f;s_rcam.pitch=.2f;test_camera_preference=2;jcx_prepare_input(n);
    assert(jcx_u(c+0x228)==123&&!s_jcx.state.owned&&!jcx_input_heading(n,c,&heading));
    assert(s_rcam.yaw==0&&s_rcam.pitch==0);
    before=hash();assert(jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f)==0&&hash()==before);
    /* Once tracking is native, neither pre-input nor successive apply calls
     * may erase its own accumulated orbit. */
    for(int k=0;k<2;k++) {
        float yaw=.6f+k*.1f,pitch=.2f+k*.1f;s_rcam.yaw=yaw;s_rcam.pitch=pitch;
        jcx_prepare_input(n);assert(s_rcam.yaw==yaw&&s_rcam.pitch==pitch);
        assert(jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f)==0);
        assert(s_rcam.yaw==yaw&&s_rcam.pitch==pitch);
    }
    /* If preference changes without a preceding input callback, apply also
     * performs exactly one custom-owned cleanup. */
    s_jcx.state.valid=s_jcx.state.owned=1;
    assert(jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f)==0);
    assert(!s_jcx.state.owned&&s_rcam.yaw==0&&s_rcam.pitch==0);
    test_camera_preference=0;assert(jcx_apply((uint8_t *)g_xbox_mem_offset,c,1.f/60.f)==1&&hash()==before);
    test_camera_preference=1;
    jcx_pad_failed();assert(!s_jcx.pad_valid&&!s_jcx.state.valid&&!jcx_free(n,p,c));
    puts("coasting LS release/repress stays manual / LT native reset / persistent grind exit / guards PASS");free((void *)g_xbox_mem_offset);return 0;
}
