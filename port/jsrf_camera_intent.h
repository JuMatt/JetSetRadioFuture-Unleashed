/* Pure host camera math. LT/LB and targeting remain native. */
#ifndef JSRF_CAMERA_INTENT_H
#define JSRF_CAMERA_INTENT_H
#include <math.h>
#include <string.h>
#define JCI_PI 3.14159265358979323846f
typedef struct {
    float view, elevation, control;
    int valid, owned;
} JciState;
typedef struct {
    float dt, native_view, native_elevation, rx, ry;
    int right_active;
} JciInput;
static float jci_wrap(float x) { return x-2.f*JCI_PI*floorf((x+JCI_PI)/(2.f*JCI_PI)); }
static void jci_reset(JciState *s) { memset(s,0,sizeof *s); }
static void jci_step(JciState *s,const JciInput *i)
{
    float dt=fminf(.1f,fmaxf(0.f,i->dt));
    if(!s->valid) {
        s->view=s->control=jci_wrap(i->native_view);
        s->elevation=i->native_elevation;s->valid=1;
    }
    if(i->right_active) {
        float delta=i->rx*dt;
        s->owned=1;
        s->view=jci_wrap(s->view+delta);
        s->elevation=fminf(1.2f,fmaxf(-.30f,s->elevation-i->ry*dt));
    } else if(!s->owned) {
        s->view=jci_wrap(i->native_view);s->elevation=i->native_elevation;
    }
    /* Manual ownership has no LS or time transition. The bridge relinquishes
     * it on native LT reset, or yields to guarded native/protected contexts.
     * Free-skating controls always use the actually shown manual view. */
    s->control=s->view;
}
#endif
