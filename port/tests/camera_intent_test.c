#include <assert.h>
#include <stdio.h>
#include "../jsrf_camera_intent.h"
static void near(float a,float b) {assert(fabsf(jci_wrap(a-b))<.00005f);}
int main(void)
{
    JciState s={0};JciInput i={0};i.dt=1.f/60.f;i.native_elevation=.2f;
    /* Native assistance remains exact until RS claims the camera. */
    for(int n=0;n<300;n++) {
        i.native_view=jci_wrap(.01f*n);jci_step(&s,&i);
        near(s.view,i.native_view);near(s.control,s.view);assert(!s.owned);
    }
    i.right_active=1;i.rx=1.5f;
    for(int n=0;n<20;n++)jci_step(&s,&i);
    assert(s.owned);near(s.control,s.view);
    float view=s.view,elevation=s.elevation;i.right_active=0;i.rx=0;
    /* There is no LS input or return clock in the state machine. Long waits
     * and changing native assistance cannot release a manual view. */
    for(int n=0;n<36000;n++) {
        i.native_view=jci_wrap(.007f*n);i.native_elevation=.001f*(n%600);
        jci_step(&s,&i);near(s.view,view);near(s.control,view);
        assert(s.elevation==elevation&&s.owned);
    }
    /* Renewed manual orbit remains camera-relative and bounds elevation. */
    i.right_active=1;i.rx=.5f;i.ry=1000.f;jci_step(&s,&i);
    near(s.view,view+i.rx*i.dt);near(s.control,s.view);assert(s.elevation==-.30f);
    i.ry=-1000.f;jci_step(&s,&i);assert(s.elevation==1.2f);
    /* The bridge's native LT reset relinquishes ownership; the next native
     * pose and movement reference are exact, then RS can claim again. */
    jci_reset(&s);i.right_active=0;i.rx=i.ry=0;i.native_view=-2.f;i.native_elevation=.3f;
    jci_step(&s,&i);near(s.view,-2.f);near(s.control,-2.f);assert(!s.owned&&s.elevation==.3f);
    i.right_active=1;i.rx=1.f;jci_step(&s,&i);assert(s.owned);near(s.view,-2.f+i.dt);near(s.control,s.view);
    puts("persistent manual ownership / no LS or timer transition / native reset PASS");return 0;
}
