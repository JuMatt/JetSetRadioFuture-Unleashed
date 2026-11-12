/* Local M1 diagnostic only. No guest writes, guest calls or behavior patch.
 * CSV records are queued without waiting; a host thread performs file I/O.
 * Float values remain raw bits so the probe need not do guest FP work. */
#if defined(_WIN32)
#include <errno.h>

enum {
    JCT_SELF, JCT_VALID, JCT_PLAYER, JCT_INPUT, JCT_CAMERA,
    JCT_LX, JCT_LY, JCT_RX, JCT_RY, JCT_LT, JCT_LB, JCT_PAD_VALID,
    JCT_SLOT, JCT_EDGE, JCT_HELD, JCT_PAD_MAG, JCT_PAD_ANGLE,
    JCT_INPUT_DIR, JCT_INPUT_MAG, JCT_INPUT_MASK, JCT_INPUT_68, JCT_INPUT_6C,
    JCT_INPUT_8E0, JCT_INPUT_8E4, JCT_BLOCK_1170, JCT_BLOCK_1174,
    JCT_LOCK, JCT_TARGET_ID, JCT_TARGET_RECORD, JCT_RESOLVE_STATUS,
    JCT_TARGET_C, JCT_TARGET_1C, JCT_TARGET_20,
    JCT_TARGET_X, JCT_TARGET_Y, JCT_TARGET_Z,
    JCT_PLAYER_E58, JCT_ACTION_E64, JCT_CANDIDATE,
    JCT_PLAYER_F18, JCT_PLAYER_EC8, JCT_PLAYER_E38, JCT_PLAYER_E3C,
    JCT_CAM_MODE, JCT_CAM_228, JCT_CAM_234, JCT_CAM_238,
    JCT_EYE_X, JCT_EYE_Y, JCT_EYE_Z, JCT_LOOK_X, JCT_LOOK_Y, JCT_LOOK_Z,
    JCT_REF_PLAYER, JCT_REF_CAMERA, JCT_INPUT_INDEX, JCT_PAD_AGE,
    JCT_DELIVERED_VALID, JCT_DLX, JCT_DLY, JCT_DRX, JCT_DRY, JCT_DLT, JCT_DLB,
    JCT_SELECTED_PAD,JCT_PHYSICAL_EDGE,JCT_PHYSICAL_HELD,JCT_E60,JCT_F00,JCT_F04,JCT_8B0,JCT_8B4,JCT_1E0,
    JCT_HOST_VIEW,JCT_HOST_CONTROL,JCT_HOST_FLAGS,JCT_RELEASE_PENDING,JCT_INPUT_OVERRIDES,
    JCT_HOST_MOVING,JCT_HOST_SPEED,JCT_HOST_VISUAL_MODE,
    JCT_RAIL_SEEN,JCT_RAIL_PENDING,JCT_RAIL_EXITS,JCT_RAIL_238_BEFORE,JCT_RAIL_238_AFTER,
    JCT_FIELD_COUNT
};
static const char *jct_names[JCT_FIELD_COUNT] = {
    "self","valid","player","input","camera",
    "raw_lx","raw_ly","raw_rx","raw_ry","raw_lt","raw_lb","pad_tls_valid",
    "pad_slot","guest_edge","guest_held","pad_mag_bits","pad_angle",
    "input_dir","input_mag_bits","input_mask","input_68","input_6c",
    "input_8e0","input_8e4","player_1170","player_1174",
    "lock_fd4","target_fd8","target_record","resolve_status",
    "target_c","target_1c","target_20",
    "target_x_bits","target_y_bits","target_z_bits",
    "player_e58","action_e64","candidate_380",
    "player_f18","player_ec8","player_e38","player_e3c",
    "cam_mode","cam_228","cam_234","cam_238",
    "eye_x_bits","eye_y_bits","eye_z_bits","look_x_bits","look_y_bits","look_z_bits",
    "ref_player_8d4","ref_camera_8d8","input_index_8ac","pad_age_ms",
    "delivered_valid","delivered_lx","delivered_ly","delivered_rx","delivered_ry","delivered_lt","delivered_lb",
    "selected_physical","physical_edge","physical_held","player_e60","player_f00","player_f04","input_8b0","input_8b4","cam_1e0",
    "host_view_bits","host_control_bits","host_flags","host_release_pending","host_input_overrides",
    "host_moving","host_speed_bits","host_visual_mode",
    "host_rail_seen","host_rail_pending","host_rail_exits","host_rail_238_before","host_rail_238_after"
};
typedef struct {
    uint32_t sequence, capture, call, milliseconds, thread, kind, phase, frame;
    uint32_t value[JCT_FIELD_COUNT];
} JctRecord;
typedef struct {
    int16_t lx, ly, rx, ry;
    uint8_t lt, lb, valid;
    int16_t dlx, dly, drx, dry;
    uint8_t dlt, dlb, delivered_valid;
    ULONGLONG sampled;
    uint32_t call;
    uint32_t player, input, camera;
} JctPad;
static RECOMP_TLS JctPad jct_pad;
static INIT_ONCE jct_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION jct_lock;
static HANDLE jct_thread;
static FILE *jct_file;
static int jct_enabled, jct_started, jct_accept;
static wchar_t jct_arm_path[32768];
static volatile LONG jct_stop, jct_dropped, jct_collecting, jct_captures, jct_calls;
static volatile LONG jct_rail_request,jct_rail_once;
static int jct_rail_enabled;
static ULONGLONG jct_begin;
static unsigned jct_head, jct_tail, jct_sequence, jct_rows;
static uint32_t jct_burst_until;
static uint32_t jct_last_ms[16], jct_last_signature[16];
static JctRecord jct_ring[512];

static int jct_range(uint64_t address, uint32_t size)
{
    return address >= 0x10000u && size <= (64u << 20) &&
           address <= (uint64_t)(64u << 20) - size;
}
static uint32_t jct_read(uint64_t address)
{
    uint32_t value = 0;
    if (jct_range(address, 4))
        memcpy(&value, (const uint8_t *)g_xbox_mem_offset + (uint32_t)address, 4);
    return value;
}
/* The native object table takes an index, not an object pointer. */
static uint32_t jct_object(uint32_t reference)
{
    uint32_t manager;
    uint64_t address;
    if ((int32_t)reference < 0) return 0;
    manager = jct_read(0x22FCE0u);
    address = (uint64_t)manager + 0x98u + (uint64_t)reference * 4u;
    return jct_range(manager, 0x98u) && jct_range(address, 4) ? jct_read(address) : 0;
}
/* Status: 0 sentinel/not found, 1 found, 2 bad range, 3 cycle/hop cap. */
static uint32_t jct_target(uint32_t identity, uint32_t *status)
{
    uint32_t node, visited[256];
    *status = 0;
    if (identity == 0xFFFFFFFFu) return 0;
    node = jct_read(0x251F78u + 4u + 4u * (identity & 255u));
    for (unsigned hop = 0; node && hop < 256; hop++) {
        if (!jct_range(node, 0x58)) { *status = 2; return 0; }
        for (unsigned previous = 0; previous < hop; previous++)
            if (visited[previous] == node) { *status = 3; return 0; }
        visited[hop] = node;
        if (jct_read((uint64_t)node + 0x10) == identity) { *status = 1; return node; }
        node = jct_read((uint64_t)node + 0x54);
    }
    if (node) *status = 3;
    return 0;
}
static DWORD WINAPI jct_writer(void *unused)
{
    unsigned bytes = 0;
    int finished = 0;
    (void)unused;
    fprintf(jct_file, "enqueue_sequence,capture_id,call_id,milliseconds,thread,kind,phase,frame");
    for (unsigned field = 0; field < JCT_FIELD_COUNT; field++) fprintf(jct_file, ",%s", jct_names[field]);
    fprintf(jct_file, "\n");
    fflush(jct_file);
    while (!finished) {
        JctRecord row;
        int have = 0;
        EnterCriticalSection(&jct_lock);
        if (jct_started && GetTickCount64() - jct_begin >= 120000u) { jct_accept = 0; InterlockedExchange(&jct_collecting, 0); }
        if (jct_tail != jct_head) {
            row = jct_ring[jct_tail]; jct_tail = (jct_tail + 1u) % 512u; have = 1;
        } else if (jct_stop || (jct_started && !jct_accept && !jct_arm_path[0])) finished = 1;
        LeaveCriticalSection(&jct_lock);
        /* Explicit arm is handled only by the host writer, outside game hooks.
         * A file can start/rearm a 120-second window; 8 MiB remains a run cap. */
        int rail_arm=jct_rail_enabled && !jct_started && InterlockedExchange(&jct_rail_request,0);
        if (!have && !finished && bytes < 8u*1024u*1024u && (rail_arm ||
            (jct_arm_path[0] && GetFileAttributesW(jct_arm_path) != INVALID_FILE_ATTRIBUTES && DeleteFileW(jct_arm_path)))) {
            EnterCriticalSection(&jct_lock);
            jct_begin=GetTickCount64(); jct_started=1; jct_accept=1;
            memset(jct_last_ms,0,sizeof jct_last_ms); memset(jct_last_signature,0,sizeof jct_last_signature);
            jct_burst_until=1000u; InterlockedExchange(&jct_collecting,1);
            LeaveCriticalSection(&jct_lock);
            fprintf(jct_file,rail_arm ? "# armed on first manual grind orbit\n" : "# explicitly armed/rearmed\n"); fflush(jct_file);
        }
        if (have) {
            int amount = fprintf(jct_file, "%u,%u,%u,%u,%u,%u,%u,%u", row.sequence, row.capture, row.call, row.milliseconds,
                                 row.thread, row.kind, row.phase, row.frame);
            for (unsigned field = 0; field < JCT_FIELD_COUNT; field++)
                amount += fprintf(jct_file, ",%08X", row.value[field]);
            amount += fprintf(jct_file, "\n");
            if (amount > 0) bytes += (unsigned)amount;
            if (++jct_rows % 32u == 0) fflush(jct_file);
            if (bytes >= 8u * 1024u * 1024u || ferror(jct_file)) {
                EnterCriticalSection(&jct_lock); jct_accept = 0; InterlockedExchange(&jct_collecting, 0); jct_tail = jct_head; LeaveCriticalSection(&jct_lock);
                finished=1;
            }
        } else if (!finished) Sleep(40);
    }
    fprintf(jct_file, "# end rows=%u dropped=%ld captures=%ld; 120-second / 8-MiB diagnostic cap\n", jct_rows, jct_dropped, jct_captures);
    fclose(jct_file);
    return 0;
}
static void jct_finish(void)
{
    InterlockedExchange(&jct_stop, 1);
    InterlockedExchange(&jct_collecting, 0);
    if (jct_thread) { WaitForSingleObject(jct_thread, 3000); CloseHandle(jct_thread); }
}
static BOOL CALLBACK jct_initialize(PINIT_ONCE once, void *parameter, void **context)
{
    const char *path = getenv("JSRF_CAMERA_TRACE_FILE");
    const char *arm_path = getenv("JSRF_CAMERA_TRACE_ARM_FILE");
    const char *trigger = getenv("JSRF_CAMERA_TRACE_TRIGGER");
    wchar_t wide[32768];
    (void)once; (void)parameter; (void)context;
    if (!path || !*path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 32768)) return TRUE;
    jct_file = _wfopen(wide, L"wb");
    if (!jct_file) return TRUE;
    jct_rail_enabled=trigger && !strcmp(trigger,"rail");
    if (arm_path && *arm_path) MultiByteToWideChar(CP_UTF8,0,arm_path,-1,jct_arm_path,32768);
    InitializeCriticalSection(&jct_lock);
    jct_accept = 1;
    InterlockedExchange(&jct_collecting, (jct_arm_path[0] || jct_rail_enabled) ? 0 : 1);
    jct_thread = CreateThread(NULL, 0, jct_writer, NULL, 0, NULL);
    if (!jct_thread) { fclose(jct_file); jct_file = NULL; return TRUE; }
    jct_enabled = 1;
    atexit(jct_finish);
    return TRUE;
}
static void jct_queue(JctRecord *row, int arm)
{
    uint32_t signature = 2166136261u;
    const unsigned watched[] = {JCT_LOCK,JCT_TARGET_ID,JCT_CAM_MODE,JCT_CAM_238,JCT_INPUT_MASK,JCT_PAD_VALID,JCT_DELIVERED_VALID,JCT_RAIL_EXITS};
    unsigned key = row->kind * 4u + row->phase;
    for (unsigned i = 0; i < sizeof watched / sizeof watched[0]; i++) signature = (signature ^ row->value[watched[i]]) * 16777619u;
    for (unsigned i = JCT_LX; i <= JCT_RY; i++) signature = (signature ^ ((uint32_t)((int32_t)row->value[i] + 32768) >> 10)) * 16777619u;
    signature = (signature ^ (row->value[JCT_LT] > 64)) * 16777619u;
    signature = (signature ^ (row->value[JCT_LB] > 64)) * 16777619u;
    signature = (signature ^ ((row->value[JCT_INPUT_MAG] & 0x7FFFFFFFu) == 0)) * 16777619u;
    signature = (signature ^ ((row->value[JCT_PAD_MAG] & 0x7FFFFFFFu) == 0)) * 16777619u;
    if (key >= 16) return;
    if (!TryEnterCriticalSection(&jct_lock)) { InterlockedIncrement(&jct_dropped); return; }
    if (!jct_accept || jct_stop) { LeaveCriticalSection(&jct_lock); return; }
    if (!jct_started && arm) { jct_begin = GetTickCount64(); jct_started = 1; }
    if (!jct_started) { LeaveCriticalSection(&jct_lock); return; }
    row->milliseconds = (uint32_t)(GetTickCount64() - jct_begin);
    if (row->milliseconds >= 120000u) { jct_accept = 0; InterlockedExchange(&jct_collecting, 0); LeaveCriticalSection(&jct_lock); return; }
    if (jct_last_signature[key] != signature) jct_burst_until = row->milliseconds + 1000u;
    if (row->milliseconds >= jct_burst_until && row->milliseconds - jct_last_ms[key] < 100u && jct_last_signature[key] == signature) {
        LeaveCriticalSection(&jct_lock); return;
    }
    jct_last_ms[key] = row->milliseconds; jct_last_signature[key] = signature;
    if ((jct_head + 1u) % 512u == jct_tail) { InterlockedIncrement(&jct_dropped); LeaveCriticalSection(&jct_lock); return; }
    row->sequence = ++jct_sequence;
    jct_ring[jct_head] = *row; jct_head = (jct_head + 1u) % 512u;
    LeaveCriticalSection(&jct_lock);
}
static void jct_collect(unsigned kind, unsigned phase, uint32_t self, uint32_t frame, uint32_t call)
{
    JctRecord row = {0};
    uint32_t player = 0, input = 0, camera = 0, slot = 0xFFFFFFFFu, record = 0, status = 0;
    int arm = 0;
    InitOnceExecuteOnce(&jct_once, jct_initialize, NULL, NULL);
    if (!jct_enabled || !InterlockedCompareExchange(&jct_collecting, 0, 0)) return;
    row.capture = (uint32_t)InterlockedIncrement(&jct_captures); row.call = call;
    if (kind == 2) input = self;
    else if (kind == 1) { player = self; if (jct_range(player, 0x1000)) input = jct_read((uint64_t)player + 0x24); }
    else if (kind == 3) {
        camera = self;
        if (jct_range(camera, 0x240)) player = jct_read((uint64_t)camera + 0x24);
        if (jct_range(player, 0x1000)) input = jct_read((uint64_t)player + 0x24);
    }
    /* Pad polling can continue on menus after scene actors have been freed.
     * Raw records must never dereference a cached actor from a previous hook. */
    if (jct_range(input, 0x1178)) {
        uint32_t index = jct_read((uint64_t)input + 0x8AC);
        row.value[JCT_INPUT_INDEX] = index;
        if (index <= 4) { uint32_t mapped = jct_read(0x215770u + (uint64_t)index * 4u); if (mapped <= 4) slot = mapped; }
        if (!player) player = jct_object(jct_read((uint64_t)input + 0x8D4));
        if (!camera) camera = jct_object(jct_read((uint64_t)input + 0x8D8));
    }
    /* Keep unknown identities visible. Slot4 is the native selected-pad mirror. */
    if(kind && slot!=0 && slot!=4 && self!=jct_pad.player && self!=jct_pad.input && self!=jct_pad.camera) {
        static uint32_t unknown_at[4]; uint32_t now=(uint32_t)GetTickCount64();
        if(now-unknown_at[kind]<1000u) return; unknown_at[kind]=now;
    }
    if (slot == 0 || slot == 4) { jct_pad.player = player; jct_pad.input = input; jct_pad.camera = camera; }
    row.kind = kind; row.phase = phase; row.frame = frame; row.thread = GetCurrentThreadId();
    row.value[JCT_SELF] = self; row.value[JCT_PLAYER] = player; row.value[JCT_INPUT] = input; row.value[JCT_CAMERA] = camera;
    row.value[JCT_LX] = (uint32_t)(int32_t)jct_pad.lx; row.value[JCT_LY] = (uint32_t)(int32_t)jct_pad.ly;
    row.value[JCT_RX] = (uint32_t)(int32_t)jct_pad.rx; row.value[JCT_RY] = (uint32_t)(int32_t)jct_pad.ry;
    row.value[JCT_LT] = jct_pad.lt; row.value[JCT_LB] = jct_pad.lb; row.value[JCT_PAD_VALID] = jct_pad.valid;
    row.value[JCT_PAD_AGE] = jct_pad.valid ? (uint32_t)(GetTickCount64() - jct_pad.sampled) : 0xFFFFFFFFu;
    row.value[JCT_DELIVERED_VALID] = jct_pad.delivered_valid;
    row.value[JCT_DLX] = (uint32_t)(int32_t)jct_pad.dlx; row.value[JCT_DLY] = (uint32_t)(int32_t)jct_pad.dly;
    row.value[JCT_DRX] = (uint32_t)(int32_t)jct_pad.drx; row.value[JCT_DRY] = (uint32_t)(int32_t)jct_pad.dry;
    row.value[JCT_DLT] = jct_pad.dlt; row.value[JCT_DLB] = jct_pad.dlb;
    arm = jct_pad.valid && (jct_pad.rx > 6553 || jct_pad.rx < -6553 || jct_pad.ry > 6553 || jct_pad.ry < -6553 || jct_pad.lt > 64 || jct_pad.lb > 64);
    row.value[JCT_SLOT] = slot;
    uint32_t physical=jct_read(0x251F20u);
    row.value[JCT_SELECTED_PAD]=physical;
    if(physical<4) { row.value[JCT_PHYSICAL_EDGE]=jct_read(0x251DE8u+64u*physical);row.value[JCT_PHYSICAL_HELD]=jct_read(0x251DE4u+64u*physical); }
#ifdef JSRF_CAMERA_HOST_TRACE
    memcpy(&row.value[JCT_HOST_VIEW], &s_jcx.state.view,4);memcpy(&row.value[JCT_HOST_CONTROL],&s_jcx.state.control,4);
    row.value[JCT_HOST_FLAGS]=s_jcx.state.valid | (s_jcx.state.owned<<1);
    row.value[JCT_INPUT_OVERRIDES]=s_jcx.input_overrides;
    row.value[JCT_HOST_MOVING]=s_jcx.moving;row.value[JCT_HOST_VISUAL_MODE]=s_jcx.visual_mode;
    memcpy(&row.value[JCT_HOST_SPEED],&s_jcx.motion_speed,4);
    row.value[JCT_RAIL_SEEN]=s_jcx.rail_seen;row.value[JCT_RAIL_PENDING]=s_jcx.rail_pending;row.value[JCT_RAIL_EXITS]=s_jcx.rail_exits;
    row.value[JCT_RAIL_238_BEFORE]=s_jcx.rail_238_before;row.value[JCT_RAIL_238_AFTER]=s_jcx.rail_238_after;
#endif
    if (slot <= 4) {
        row.value[JCT_EDGE] = jct_read(0x251DE8u + 64u * slot);
        row.value[JCT_HELD] = jct_read(0x251DE4u + 64u * slot);
        row.value[JCT_PAD_MAG] = jct_read(0x251E18u + 64u * slot);
        row.value[JCT_PAD_ANGLE] = jct_read(0x251E14u + 64u * slot);
    }
    if (jct_range(input, 0x1178)) {
        static const unsigned offsets[] = {0x8BC,0x8C0,0x8C4,0x68,0x6C,0x8E0,0x8E4};
        row.value[JCT_VALID] |= 1;
        for (unsigned index = 0; index < 7; index++) row.value[JCT_INPUT_DIR + index] = jct_read((uint64_t)input + offsets[index]);
        row.value[JCT_REF_PLAYER] = jct_read((uint64_t)input + 0x8D4); row.value[JCT_REF_CAMERA] = jct_read((uint64_t)input + 0x8D8);
        row.value[JCT_8B0]=jct_read((uint64_t)input+0x8B0);row.value[JCT_8B4]=jct_read((uint64_t)input+0x8B4);
    }
    if (jct_range(player, 0x1178)) {
        static const unsigned offsets[] = {0xE58,0xE64,0x380,0xF18,0xEC8,0xE38,0xE3C};
        row.value[JCT_VALID] |= 2;
        row.value[JCT_BLOCK_1170] = jct_read((uint64_t)player + 0x1170); row.value[JCT_BLOCK_1174] = jct_read((uint64_t)player + 0x1174);
        row.value[JCT_LOCK] = jct_read((uint64_t)player + 0xFD4); row.value[JCT_TARGET_ID] = jct_read((uint64_t)player + 0xFD8);
        row.value[JCT_E60]=jct_read((uint64_t)player+0xE60);row.value[JCT_F00]=jct_read((uint64_t)player+0xF00);row.value[JCT_F04]=jct_read((uint64_t)player+0xF04);
        for (unsigned index = 0; index < 3; index++) row.value[JCT_TARGET_X + index] = jct_read((uint64_t)player + 0xFDC + index * 4u);
        for (unsigned index = 0; index < 7; index++) row.value[JCT_PLAYER_E58 + index] = jct_read((uint64_t)player + offsets[index]);
        record = jct_target(row.value[JCT_TARGET_ID], &status);
        row.value[JCT_TARGET_RECORD] = record; row.value[JCT_RESOLVE_STATUS] = status;
        if (record) { row.value[JCT_VALID] |= 8; row.value[JCT_TARGET_C] = jct_read((uint64_t)record + 0xC); row.value[JCT_TARGET_1C] = jct_read((uint64_t)record + 0x1C); row.value[JCT_TARGET_20] = jct_read((uint64_t)record + 0x20); }
    }
    if (jct_range(camera, 0x240)) {
        static const unsigned offsets[] = {0x208,0x228,0x234,0x238,0x44,0x48,0x4C,0x5C,0x60,0x64};
        row.value[JCT_VALID] |= 4;
        row.value[JCT_1E0]=jct_read((uint64_t)camera+0x1E0);
        for (unsigned index = 0; index < 10; index++) row.value[JCT_CAM_MODE + index] = jct_read((uint64_t)camera + offsets[index]);
    }
    jct_queue(&row, arm);
}
static uint32_t jct_begin_call(void) { return (uint32_t)InterlockedIncrement(&jct_calls); }
static void jct_request_rail_capture(void) {if(!InterlockedCompareExchange(&jct_rail_once,1,0)) InterlockedExchange(&jct_rail_request,1);}
static void jct_sample(unsigned kind, unsigned phase, uint32_t self, uint32_t frame, uint32_t call)
{
    DWORD host_error = GetLastError(); int host_errno = errno;
    jct_collect(kind, phase, self, frame, call);
    errno = host_errno; SetLastError(host_error);
}
static void jct_note_pad(int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t lb)
{
    jct_pad.lx = lx; jct_pad.ly = ly; jct_pad.rx = rx; jct_pad.ry = ry;
    jct_pad.lt = lt; jct_pad.lb = lb; jct_pad.valid = 1;
    jct_pad.sampled = GetTickCount64(); jct_pad.delivered_valid = 0; jct_pad.call = jct_begin_call();
    jct_sample(0, 0, 0, 0, jct_pad.call);
}
static void jct_delivered_pad(int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t lb)
{
    jct_pad.dlx = lx; jct_pad.dly = ly; jct_pad.drx = rx; jct_pad.dry = ry;
    jct_pad.dlt = lt; jct_pad.dlb = lb; jct_pad.delivered_valid = 1;
    jct_sample(0, 1, 0, 0, jct_pad.call);
}
static void jct_pad_failed(void)
{
    jct_pad.valid = 0; jct_pad.delivered_valid = 0;
    jct_sample(0, 2, 0, 0, jct_begin_call());
}
#else
static uint32_t jct_begin_call(void) { return 0; }
static void jct_request_rail_capture(void) { }
static void jct_sample(unsigned kind, unsigned phase, uint32_t self, uint32_t frame, uint32_t call) { (void)kind; (void)phase; (void)self; (void)frame; (void)call; }
static void jct_note_pad(int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t lb) { (void)lx; (void)ly; (void)rx; (void)ry; (void)lt; (void)lb; }
static void jct_delivered_pad(int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t lb) { (void)lx; (void)ly; (void)rx; (void)ry; (void)lt; (void)lb; }
static void jct_pad_failed(void) { }
#endif
