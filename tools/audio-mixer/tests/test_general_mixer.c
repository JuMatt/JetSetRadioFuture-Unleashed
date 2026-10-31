#include "jsrf_mixer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static void fill(float s[256][2], float value)
{
    for (int i = 0; i < 256; i++)
        s[i][0] = s[i][1] = value;
}
static float stable(unsigned v, float input)
{
    float s[256][2];
    fill(s, input);
    xbox_JsrfMixer_Voice(v, s, 256, 0);
    fill(s, input);
    xbox_JsrfMixer_Voice(v, s, 256, 0);
    return s[255][0];
}
int main(void)
{
    assert(xbox_JsrfMixer_Classify("D:\\Media\\Z_ADX\\BGM\\dj_demo1.adx") == JM_MUSIC);
    assert(xbox_JsrfMixer_Classify("D:/Media/Z_ADX/MUSEN/line.adx") == JM_DIALOGUE);
    assert(xbox_JsrfMixer_Classify("D:/Media/Z_ADX/MUSENJ/line.adx") == JM_DIALOGUE);
    assert(xbox_JsrfMixer_Classify("D:/Media/Z_ADX/DJ/a.adx") == JM_DJ);
    assert(xbox_JsrfMixer_Classify("D:/Media/Z_ADX/DJ_Demo/b.adx") == JM_DJ);
    assert(xbox_JsrfMixer_Classify("D:/Media/Z_ADX/Env/wind.adx") == JM_AMBIENCE);
    assert(xbox_JsrfMixer_Classify("D:/Media/Z_ADX/BGMx/unknown.adx") == JM_OTHER);
    unsigned char groups[256];
    memset(groups, JM_EFFECTS, 256);
    groups[68] = JM_MUSIC;
    groups[69] = JM_DIALOGUE;
    groups[70] = JM_DJ;
    groups[71] = JM_AMBIENCE;
    groups[72] = JM_OTHER;
    xbox_JsrfMixer_Map(groups);
    xbox_JsrfMixer_SetBus(JM_MUSIC, 25, 0);
    xbox_JsrfMixer_SetBus(JM_EFFECTS, 50, 0);
    assert(fabsf(stable(68, 0.8f) - 0.2f) < 0.00001f);
    assert(fabsf(stable(1, 0.8f) - 0.4f) < 0.00001f);
    assert(fabsf(stable(69, 0.8f) - 0.8f) < 0.00001f);
    xbox_JsrfMixer_SetBus(JM_MUSIC, 100, 1);
    assert(stable(68, 0.8f) == 0);
    assert(fabsf(stable(69, 0.8f) - 0.8f) < 0.00001f);
    xbox_JsrfMixer_SetBus(JM_DJ, 0, 0);
    assert(stable(70, 0.8f) == 0);
    assert(fabsf(stable(71, 0.8f) - 0.8f) < 0.00001f);
    xbox_JsrfMixer_SetBus(JM_DIALOGUE, 30, 0);
    assert(fabsf(stable(69, 0.8f) - 0.24f) < 0.00001f);
    assert(fabsf(stable(72, 0.8f) - 0.8f) < 0.00001f);
    float multipass[256][2];
    fill(multipass, 0.2f);
    xbox_JsrfMixer_Voice(68, multipass, 256, 1);
    assert(multipass[255][0] == 0.2f);
    int16_t out[512];
    xbox_JsrfMixer_SetBus(JM_MASTER, 50, 0);
    for (int k = 0; k < 2; k++)
    {
        for (int i = 0; i < 512; i++)
            out[i] = 20000;
        xbox_JsrfMixer_Master(out, 256);
    }
    assert(out[511] == 10000);
    xbox_JsrfMixer_SetBus(JM_MASTER, 100, 1);
    xbox_JsrfMixer_Master(out, 256);
    xbox_JsrfMixer_Master(out, 256);
    for (int i = 0; i < 512; i++)
        assert(out[i] == 0);
    xbox_JsrfMixer_SetBus(JM_MASTER, 100, 0);
    for (int k = 0; k < 2; k++)
    {
        for (int i = 0; i < 512; i++)
            out[i] = 20000;
        xbox_JsrfMixer_Master(out, 256);
    }
    for (int i = 0; i < 512; i++)
        assert(out[i] == 20000);
    for (int b = 0; b < JM_COUNT; b++)
        xbox_JsrfMixer_SetBus(b, 100, 0);
    assert(stable(68, 0.8f) == 0.8f);
    puts("PASS: seven groups, isolated gain/mutes, single Music fader at 25%, master exactly once, "
         "multipass protection, full restoration");
}
