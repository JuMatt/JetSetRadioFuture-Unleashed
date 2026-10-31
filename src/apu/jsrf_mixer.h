/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef JSRF_EXTERNAL_MIXER_H
#define JSRF_EXTERNAL_MIXER_H
#include <stdint.h>
enum
{
    JM_MASTER,
    JM_MUSIC,
    JM_EFFECTS,
    JM_DIALOGUE,
    JM_DJ,
    JM_AMBIENCE,
    JM_OTHER,
    JM_COUNT
};
int xbox_JsrfMixer_Classify(const char *path);
void xbox_JsrfMixer_SetBus(int bus, int percent, int muted);
void xbox_JsrfMixer_Map(const unsigned char groups[256]);
void xbox_JsrfMixer_Voice(unsigned voice, float samples[][2], int frames, int multipass);
void xbox_JsrfMixer_Master(int16_t *samples, int frames);
float xbox_JsrfMixer_SoftwareGain(void);
void xbox_JsrfMixer_Stats(int bus, int *percent, int *muted, int *active, float *peak,
                          float *effective);
const char *xbox_JsrfMixer_Key(int bus);
#endif
