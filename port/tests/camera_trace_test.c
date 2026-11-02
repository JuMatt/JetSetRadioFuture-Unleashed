#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define RECOMP_TLS _Thread_local
ptrdiff_t g_xbox_mem_offset;
#include "../jsrf_camera_trace.inc"

static void put(uint32_t address, uint32_t value)
{
    memcpy((uint8_t *)g_xbox_mem_offset + address, &value, 4);
}
static uint64_t memory_hash(void)
{
    const uint8_t *memory = (const uint8_t *)g_xbox_mem_offset;
    uint64_t value = 1469598103934665603ull;
    for (unsigned i = 0; i < (64u << 20); i++) value = (value ^ memory[i]) * 1099511628211ull;
    return value;
}
int main(int argc, char **argv)
{
    uint32_t status, call;
    uint64_t before;
    const uint32_t player = 0x40000, input = 0x60000, camera = 0x50000, target = 0x70000;
    assert(argc == 2 || argc == 3 || argc == 4);
    g_xbox_mem_offset = (ptrdiff_t)calloc(1, 64u << 20);
    assert(g_xbox_mem_offset);
    put(0x22FCE0, 0x20000);
    put(0x20000 + 0x98 + 2 * 4, player); put(0x20000 + 0x98 + 3 * 4, camera);
    assert(jct_object(2) == player && jct_object(3) == camera);
    assert(jct_object(0xFFFFFFFFu) == 0 && jct_object(0x40000000u) == 0);
    assert(jct_read(0) == 0 && jct_read((64u << 20) - 2u) == 0);
    put(0x251F78 + 4 + 7 * 4, target); put(target + 0x10, 0x107);
    assert(jct_target(0x107, &status) == target && status == 1);
    assert(jct_target(0xFFFFFFFFu, &status) == 0 && status == 0);
    put(target + 0x54, target);
    assert(jct_target(0x207, &status) == 0 && status == 3);
    put(target + 0x54, (64u << 20) - 4);
    assert(jct_target(0x207, &status) == 0 && status == 2);
    put(target + 0x54, 0);
    assert(jct_target(0x207, &status) == 0 && status == 0);
    put(player + 0x24, input); put(camera + 0x24, player);
    put(input + 0x8AC, 1); put(0x215770, 2); put(0x215774, 0); /* index != slot */
    put(input + 0x8D4, 2); put(input + 0x8D8, 3);
    put(input + 0x8BC, 0x5678); put(input + 0x8C0, 0x3E800000);
    put(input + 0x1170, 0xBAD); put(input + 0x1174, 0xBAD);
    put(player + 0x1170, 0xA5A5); put(player + 0x1174, 0xB6B6);
    put(player + 0xFD4, 1); put(player + 0xFD8, 0x107); put(player + 0xE58, 9);
    put(camera + 0x208, 9); put(camera + 0x228, 0x1234); put(camera + 0x234, 0x4321); put(camera + 0x238, 1);
    put(0x251E18, 0x3F000000); put(0x251DE8, 0x100); put(0x251DE4, 0x100);
    before = memory_hash();
    assert(_putenv_s("JSRF_CAMERA_TRACE_FILE", argv[1]) == 0);
    if(argc>=3) assert(_putenv_s("JSRF_CAMERA_TRACE_ARM_FILE", argv[2])==0);
    if(argc==4) assert(_putenv_s("JSRF_CAMERA_TRACE_TRIGGER","rail")==0);
    call = jct_begin_call();
    SetLastError(0x12345678); errno = EDOM;
    jct_note_pad(12000, 0, 10000, 0, 100, 0);
    if(argc>=3) {
        assert(!jct_started && !jct_collecting);
        if(argc==4) jct_request_rail_capture();
        else {FILE *arm=fopen(argv[2],"wb");assert(arm);fclose(arm);}
        for(int n=0;n<200&&!jct_collecting;n++)Sleep(10);
        assert(jct_started&&jct_collecting);
    }
    SetLastError(0x12345678); errno=EDOM;
    jct_sample(2, 0, input, 0, call);
    assert(GetLastError() == 0x12345678 && errno == EDOM);
    jct_delivered_pad(12000, 0, 0, 0, 100, 0);
    jct_sample(2, 1, input, 0, call);
    call = jct_begin_call(); jct_sample(1, 0, player, 0, call); jct_sample(1, 1, player, 0, call);
    call = jct_begin_call(); jct_sample(3, 1, camera, 5, call); jct_sample(3, 2, camera, 5, call);
    jct_pad_failed();
    assert(memory_hash() == before);
    /* Raw polling after a scene was unloaded must not use cached actors. */
    jct_pad.player=0xFFFFFF00u;jct_pad.input=0xFFFFFF00u;jct_pad.camera=0xFFFFFF00u;
    jct_note_pad(0,0,12000,0,0,0);assert(memory_hash()==before);
    put(input+0x8AC,4);put(0x215770+4*4,4);put(0x251F20,0);
    before=memory_hash();call=jct_begin_call();jct_sample(2,0,input,0,call);jct_sample(2,1,input,0,call);
    assert(jct_pad.player==player&&jct_pad.input==input&&jct_pad.camera==camera);
    assert(memory_hash()==before);
    if(argc==3) {
        EnterCriticalSection(&jct_lock);jct_begin=GetTickCount64()-120001u;LeaveCriticalSection(&jct_lock);
        for(int n=0;n<200&&jct_collecting;n++)Sleep(10);assert(!jct_collecting);
        FILE *arm=fopen(argv[2],"wb");assert(arm);fclose(arm);
        for(int n=0;n<200&&!jct_collecting;n++)Sleep(10);assert(jct_collecting);
    }
    InterlockedExchange(&jct_collecting, 0);
    { LONG attempts = jct_captures; jct_sample(2, 1, input, 0, jct_begin_call()); assert(jct_captures == attempts); }
    puts("PASS: bounded object/target resolvers, non-identity pad mapping, read-only guest memory, host error preservation, pad failure and finite collection gate");
    /* The normal exit handler flushes the background writer. */
    return 0;
}
