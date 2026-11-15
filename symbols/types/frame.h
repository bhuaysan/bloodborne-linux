/* Ghidra category /bb/frame: written by tools/ghidra-sync, see symbols/README.md */

#pragma pack(push, 1)

struct FD4TaskData;
struct FlipperFrameRecord;
struct SprjFlipper;

struct FD4TaskData {
    /* 0x0000 */ void *vtable;
    /* 0x0008 */ float delta_time;
    /* 0x000c */ undefined1 _pad_0x000c[4];
};
_Static_assert(sizeof(struct FD4TaskData) == 0x10, "FD4TaskData");

struct FlipperFrameRecord {
    /* 0x0000 */ ulonglong duration_us;
    /* 0x0008 */ byte late;
    /* 0x0009 */ undefined1 _pad_0x0009[7];
};
_Static_assert(sizeof(struct FlipperFrameRecord) == 0x10, "FlipperFrameRecord");

struct SprjFlipper {
    /* 0x0000 */ void *vtable;
    /* 0x0008 */ uint flip_mode;
    /* 0x000c */ uint flip_mode_heavy;
    /* 0x0010 */ int present_interval;
    /* 0x0014 */ byte tearing;
    /* 0x0015 */ undefined1 _pad_0x0015[3];
    /* 0x0018 */ float target_frame_time;
    /* 0x001c */ undefined1 _pad_0x001c[4];
    /* 0x0020 */ ulonglong prev_time_us;
    /* 0x0028 */ ulonglong now_time_us;
    /* 0x0030 */ ulonglong draw_time_us[6];
    /* 0x0060 */ struct FlipperFrameRecord records[32];
    /* 0x0260 */ uint record_index;
    /* 0x0264 */ float delta_time;
    /* 0x0268 */ uint foreground_skip_max;
    /* 0x026c */ uint background_skip_max;
    /* 0x0270 */ byte late;
    /* 0x0271 */ byte clear_skip_once;
    /* 0x0272 */ byte no_skip;
    /* 0x0273 */ byte skip_wait;
    /* 0x0274 */ byte request_0x274;
    /* 0x0275 */ byte heavy_last;
    /* 0x0276 */ byte heavy;
    /* 0x0277 */ undefined1 _pad_0x0277[1];
    /* 0x0278 */ float recent_delta[16];
    /* 0x02b8 */ float fps;
    /* 0x02bc */ int foreground_skip_max_debug;
    /* 0x02c0 */ int background_skip_max_debug;
    /* 0x02c4 */ byte force_default_mode;
    /* 0x02c5 */ byte draw_time_live;
    /* 0x02c6 */ undefined1 _pad_0x02c6[2];
};
_Static_assert(sizeof(struct SprjFlipper) == 0x2c8, "SprjFlipper");

#pragma pack(pop)
