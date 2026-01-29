/*
 * PicoDrive Web Platform Layer
 * Emscripten/asm.js interface for running in browser
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <pico/pico_types.h>
#include <pico/pico.h>
#include <pico/pico_int.h>
#include <pico/state.h>

/* Video output buffer - RGB565 format */
/* Use fixed 320x240 buffer, same as libretro */
#define VOUT_MAX_WIDTH 320
#define VOUT_MAX_HEIGHT 240

static unsigned short vout_buf[VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT];
static int vout_width = VOUT_MAX_WIDTH;
static int vout_height = VOUT_MAX_HEIGHT;
static int vout_offset = 0;  /* Byte offset for start_line */

/* Store current video mode parameters for mode changes */
static int vm_current_start_line = -1;
static int vm_current_line_count = -1;
static int vm_current_start_col = -1;
static int vm_current_col_count = -1;

/* Audio output buffer */
#define SND_RATE 44100
#define SND_MAX_SAMPLES (SND_RATE / 50 * 2)  /* stereo, 50fps minimum */
static short snd_buffer[SND_MAX_SAMPLES * 2];

/* Input state - Genesis/MD format: MXYZ SACB RLDU */
static unsigned short input_state[2] = {0, 0};

/* ROM data */
static unsigned char *rom_data = NULL;
static unsigned int rom_size = 0;

/* Emulator state */
static int emu_initialized = 0;
static int game_loaded = 0;

/* Platform functions required by PicoDrive core */

void lprintf(const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
#ifdef __EMSCRIPTEN__
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    EM_ASM({
        console.log(UTF8ToString($0));
    }, buf);
#else
    vprintf(fmt, vl);
#endif
    va_end(vl);
}

/* Dummy MP3 functions - not supported in web build */
int mp3_get_bitrate(void *f, int size) { return 0; }
void mp3_start_play(void *f, int pos) {}
void mp3_update(int *buffer, int length, int stereo) {}

/* Cache flush - not needed for web */
void cache_flush_d_inval_i(void *start_addr, void *end_addr) {}

/* Memory allocation */
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
    void *ret = calloc(1, size);
    return ret;
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
    void *ret = realloc(ptr, newsize);
    return ret;
}

void plat_munmap(void *ptr, size_t size)
{
    if (ptr)
        free(ptr);
}

void *plat_mem_get_for_drc(size_t size)
{
    return NULL;  /* No DRC in web build */
}

int plat_mem_set_exec(void *ptr, size_t size)
{
    return 0;
}

/* Video mode change callback - called by the core when video mode changes */
void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count)
{
    lprintf("emu_video_mode_change: start_line=%d, lines=%d, start_col=%d, cols=%d\n",
            start_line, line_count, start_col, col_count);

    /* Store current video mode for 32X startup re-init */
    vm_current_start_line = start_line;
    vm_current_line_count = line_count;
    vm_current_start_col = start_col;
    vm_current_col_count = col_count;

    /* Match libretro exactly */
    vout_width = col_count;
    vout_height = line_count;

    /* Clear the entire buffer */
    memset(vout_buf, 0, VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);

    /* Set output buffer - use vout_width * 2 as pitch (same as libretro) */
    PicoDrawSetOutBuf(vout_buf, vout_width * 2);
    lprintf("emu_video_mode_change: DrawLineDestIncrement=%d after SetOutBuf\n", DrawLineDestIncrement);

    /* Calculate offset to where visible content starts (same as libretro) */
    vout_offset = vout_width * start_line * 2;

    /* Sanity checks (same as libretro) */
    if (vout_height > VOUT_MAX_HEIGHT)
        vout_height = VOUT_MAX_HEIGHT;
    if (vout_offset > vout_width * (VOUT_MAX_HEIGHT - 1) * 2)
        vout_offset = vout_width * (VOUT_MAX_HEIGHT - 1) * 2;

#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (typeof window.onVideoModeChange === 'function') {
            window.onVideoModeChange($0, $1);
        }
    }, vout_width, vout_height);
#endif

    /* Force palette refresh */
    Pico.m.dirtyPal = 1;
}

/* 32X startup callback */
void emu_32x_startup(void)
{
    lprintf("emu_32x_startup called, AHW=0x%x\n", PicoIn.AHW);

    /* Match libretro: use_32x_line_mode=0 for 32X */
    PicoDrawSetOutFormat(PDF_RGB555, 0);

    /* Re-apply video mode change if we have valid mode info */
    if ((vm_current_start_line != -1) && (vm_current_line_count != -1) &&
        (vm_current_start_col != -1) && (vm_current_col_count != -1)) {
        lprintf("emu_32x_startup: re-applying video mode %dx%d\n",
                vm_current_col_count, vm_current_line_count);
        emu_video_mode_change(vm_current_start_line, vm_current_line_count,
                              vm_current_start_col, vm_current_col_count);
    } else {
        lprintf("emu_32x_startup: setting default buffer\n");
        PicoDrawSetOutBuf(vout_buf, vout_width * 2);
    }
}

/* Audio write callback */
static void snd_write(int len)
{
#ifdef __EMSCRIPTEN__
    /* len is in bytes, we have 16-bit stereo samples */
    int samples = len / 4;
    EM_ASM({
        if (typeof window.onAudioWrite === 'function') {
            var ptr = $0;
            var samples = $1;
            var audioData = new Int16Array(samples * 2);
            for (var i = 0; i < samples * 2; i++) {
                audioData[i] = HEAP16[(ptr >> 1) + i];
            }
            window.onAudioWrite(audioData);
        }
    }, PicoIn.sndOut, samples);
#endif
}

/* ========== Exported API ========== */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_init(void)
{
    if (emu_initialized)
        return 1;

    lprintf("pico_init: initializing PicoDrive\n");

    /* Initialize emulator */
    PicoInit();

    /* Configure default options - enable all features */
    PicoIn.opt = POPT_EN_STEREO | POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80 |
                 POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX |
                 POPT_ACC_SPRITES | POPT_EN_32X | POPT_EN_PWM |
                 POPT_DIS_32C_BORDER;  /* Disable 32-column border for proper rendering */

    /* Auto-detect region from ROM header (like all other emulators) */
    PicoIn.regionOverride = 0;  /* 0 = auto-detect from ROM */
    /* Region priority when ROM supports multiple regions: US, EU, JP */
    PicoIn.autoRgnOrder = 0x184;

    /* Setup audio */
    PicoIn.sndRate = SND_RATE;
    PicoIn.sndOut = snd_buffer;
    PicoIn.writeSound = snd_write;

    /* Setup video - use accurate renderer with RGB555 output */
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(vout_buf, vout_width * 2);

    /* Setup input - 6 button pad for both players */
    PicoSetInputDevice(0, PICO_INPUT_PAD_6BTN);
    PicoSetInputDevice(1, PICO_INPUT_PAD_6BTN);

    emu_initialized = 1;
    lprintf("pico_init: done\n");

    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void pico_exit(void)
{
    if (!emu_initialized)
        return;

    if (rom_data) {
        free(rom_data);
        rom_data = NULL;
        rom_size = 0;
    }

    PicoExit();
    emu_initialized = 0;
    game_loaded = 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
unsigned char *pico_get_rom_buffer(unsigned int size)
{
    /* Allocate or reallocate ROM buffer */
    if (rom_data) {
        free(rom_data);
    }

    rom_data = (unsigned char *)malloc(size);
    rom_size = size;

    lprintf("pico_get_rom_buffer: allocated %u bytes\n", size);
    return rom_data;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_load_rom(const char *filename)
{
    if (!emu_initialized) {
        lprintf("pico_load_rom: emulator not initialized\n");
        return 0;
    }

    if (!rom_data || rom_size == 0) {
        lprintf("pico_load_rom: no ROM data\n");
        return 0;
    }

    /* Unload previous game if any */
    if (game_loaded) {
        PicoCartUnload();
        game_loaded = 0;
    }

    lprintf("pico_load_rom: loading %s (%u bytes)\n", filename, rom_size);

    /* Load the ROM */
    enum media_type_e media_type = PicoLoadMedia(filename, rom_data, rom_size,
                                                  NULL, NULL, NULL, NULL);

    if (media_type <= 0) {
        lprintf("pico_load_rom: failed, error %d\n", media_type);
        return 0;
    }

    lprintf("pico_load_rom: media_type=%d, AHW=0x%x, regionOverride=%d\n",
            media_type, PicoIn.AHW, PicoIn.regionOverride);

    /* Log ROM header region info for debugging */
    {
        char region_str[8] = {0};
        int support = 0;
        int i;
        memcpy(region_str, Pico.rom + 0x1f0, 4);

        /* Parse region codes like PicoDetectRegion does */
        for (i = 0; i < 4; i++) {
            int c = region_str[i];
            if (c <= ' ') continue;
            if (c == 'J' || c == 'j') support |= 1;  /* Japan NTSC */
            else if (c == 'U' || c == 'u') support |= 4;  /* USA */
            else if (c == 'E' || c == 'e') support |= 8;  /* Europe */
            else if (c >= '0' && c <= '9') support |= (c - '0');
            else if (c >= 'A' && c <= 'F') support |= (c - 'A' + 10);
        }

        lprintf("pico_load_rom: ROM region header='%s' (supports: %s%s%s)\n",
                region_str,
                (support & 1) ? "Japan " : "",
                (support & 4) ? "USA " : "",
                (support & 8) ? "Europe " : "");
        lprintf("pico_load_rom: regionOverride=%d, autoRgnOrder=0x%x\n",
                PicoIn.regionOverride, PicoIn.autoRgnOrder);
        lprintf("pico_load_rom: is 32X=%d, hw=0x%02x, pal=%d\n",
                (PicoIn.AHW & 2) ? 1 : 0, Pico.m.hardware, Pico.m.pal);

        /* Warn if there's a region mismatch */
        if (PicoIn.regionOverride) {
            int hw_region = (Pico.m.hardware & 0xc0);
            int matches = 0;
            if (hw_region == 0x00 && (support & 1)) matches = 1;  /* Japan NTSC */
            if (hw_region == 0x40 && (support & 2)) matches = 1;  /* Japan PAL */
            if (hw_region == 0x80 && (support & 4)) matches = 1;  /* USA */
            if (hw_region == 0xc0 && (support & 8)) matches = 1;  /* Europe */
            if (!matches && support != 0) {
                lprintf("pico_load_rom: WARNING! Region mismatch - game may show region lock error\n");
                lprintf("pico_load_rom: Hardware region=0x%02x but ROM supports=0x%x\n",
                        hw_region, support);
            }
        }
    }

    /* Prepare emulation loop (PicoLoadMedia already called PicoPower internally) */
    PicoLoopPrepare();

    /* Initialize sound for the detected region (PAL/NTSC) */
    memset(snd_buffer, 0, sizeof(snd_buffer));
    PsndRerate(0);

    /* Apply renderer - match libretro's apply_renderer() */
    PicoIn.opt &= ~(POPT_ALT_RENDERER | POPT_EN_SOFTSCALE);
    PicoIn.opt |= POPT_DIS_32C_BORDER;
    /* Match libretro: always use_32x_line_mode=0 */
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(vout_buf, vout_width * 2);
    lprintf("pico_load_rom: DrawLineDestIncrement=%d after SetOutBuf\n", DrawLineDestIncrement);

    game_loaded = 1;
    lprintf("pico_load_rom: success, pal=%d, hw=0x%02x (VERSION reg=0x%02x)\n",
            Pico.m.pal, Pico.m.hardware, Pico.m.hardware | 0x20);
    lprintf("pico_load_rom: opt=0x%08x, EN_32X=%d\n",
            PicoIn.opt, (PicoIn.opt & POPT_EN_32X) ? 1 : 0);

    /* Force 32X startup for .32x ROM files */
    if ((PicoIn.opt & POPT_EN_32X) && !(PicoIn.AHW & PAHW_32X)) {
        /* Check if filename ends with .32x (case insensitive) */
        int len = strlen(filename);
        if (len >= 4) {
            const char *ext = filename + len - 4;
            if ((ext[0] == '.' || ext[0] == '.') &&
                (ext[1] == '3') &&
                (ext[2] == '2') &&
                (ext[3] == 'x' || ext[3] == 'X')) {
                lprintf("pico_load_rom: forcing 32X startup for .32x ROM\n");
                lprintf("pico_load_rom: Pico32xMem before startup = %p\n", (void*)Pico32xMem);
                Pico32xStartup();
                lprintf("pico_load_rom: 32X started, AHW=0x%x, Pico32xMem=%p\n", PicoIn.AHW, (void*)Pico32xMem);

                /* Debug: Check BIOS HLE memory */
                if (Pico32xMem) {
                    u32 *pl = (u32 *)&Pico32xMem->sh2_rom_m;
                    lprintf("pico_load_rom: BIOS HLE pl[0]=0x%08x, pl[1]=0x%08x\n", pl[0], pl[1]);
                    lprintf("pico_load_rom: BIOS HLE w[0]=0x%04x, w[1]=0x%04x\n",
                            Pico32xMem->sh2_rom_m.w[0], Pico32xMem->sh2_rom_m.w[1]);
                    lprintf("pico_load_rom: msh2.read32_map=%p\n", (void*)msh2.read32_map);
                }

                /* Reset SH2 processors to read PC/SP from HLE BIOS */
                p32x_reset_sh2s();
                lprintf("pico_load_rom: SH2 reset done, msh2.pc=0x%08x, ssh2.pc=0x%08x\n",
                        msh2.pc, ssh2.pc);
            }
        }
    }

    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void pico_reset(void)
{
    if (game_loaded) {
        PicoReset();
        lprintf("pico_reset: done\n");
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void pico_set_input(int pad, unsigned short buttons)
{
    if (pad >= 0 && pad < 2) {
        input_state[pad] = buttons;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
static int frame_count = 0;

void pico_run_frame(void)
{
    if (!game_loaded)
        return;

    /* Update input */
    PicoIn.pad[0] = input_state[0];
    PicoIn.pad[1] = input_state[1];

    /* Debug: Log input every 60 frames if buttons pressed */
    if (frame_count % 60 == 0) {
        if (input_state[0] != 0) {
            lprintf("Frame %d: input[0]=0x%04x\n", frame_count, input_state[0]);
        }
    }

    /* Run one frame */
    PicoFrame();

    frame_count++;
    /* Debug: Log every 300 frames (approx 5 seconds) */
    if (frame_count % 300 == 0) {
        lprintf("Frame %d: vout=%dx%d, offset=%d, pal=%d, AHW=0x%x, DrawLineDestIncrement=%d\n",
                frame_count, vout_width, vout_height, vout_offset, Pico.m.pal, PicoIn.AHW, DrawLineDestIncrement);
    }
    /* Log first few frames with buffer contents */
    if (frame_count <= 3) {
        lprintf("Frame %d: DrawLineDestIncrement=%d, DrawLineDestBase=%p, vout_buf=%p\n",
                frame_count, DrawLineDestIncrement, (void*)DrawLineDestBase, (void*)vout_buf);
        /* Check buffer contents at various lines */
        unsigned short *visible_start = (unsigned short *)((char *)vout_buf + vout_offset);
        int line0_nonzero = 0, line50_nonzero = 0, line100_nonzero = 0, line200_nonzero = 0;
        for (int x = 0; x < vout_width; x++) {
            if (visible_start[0 * vout_width + x] != 0) line0_nonzero++;
            if (visible_start[50 * vout_width + x] != 0) line50_nonzero++;
            if (visible_start[100 * vout_width + x] != 0) line100_nonzero++;
            if (vout_height > 200 && visible_start[200 * vout_width + x] != 0) line200_nonzero++;
        }
        lprintf("Frame %d: Buffer check - line0=%d nonzero, line50=%d, line100=%d, line200=%d\n",
                frame_count, line0_nonzero, line50_nonzero, line100_nonzero, line200_nonzero);
        /* Also check some specific pixel values */
        lprintf("Frame %d: visible_start[0]=0x%04x, [160]=0x%04x, [%d]=0x%04x\n",
                frame_count, visible_start[0], visible_start[160],
                100*vout_width + 160, visible_start[100*vout_width + 160]);
    }

    /* Log when 32X starts up (AHW changes) */
    static unsigned int last_ahw = 0;
    if (PicoIn.AHW != last_ahw) {
        lprintf("Frame %d: AHW changed from 0x%x to 0x%x\n", frame_count, last_ahw, PicoIn.AHW);
        last_ahw = PicoIn.AHW;
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
unsigned short *pico_get_video_buffer(void)
{
    /* Return pointer to visible area (with offset for start_line) */
    return (unsigned short *)((char *)vout_buf + vout_offset);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_video_width(void)
{
    return vout_width;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_video_height(void)
{
    return vout_height;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_is_pal(void)
{
    return Pico.m.pal;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *pico_get_rom_name(void)
{
    if (!game_loaded)
        return "";

    /* Return the game name from the ROM header */
    static char name[49];
    memcpy(name, media_id_header + 0x20, 48);
    name[48] = '\0';

    /* Trim trailing spaces */
    int i;
    for (i = 47; i >= 0 && name[i] == ' '; i--)
        name[i] = '\0';

    return name;
}

/* Input button definitions for JavaScript */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_up(void)    { return 1 << 0; }  /* GBTN_UP */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_down(void)  { return 1 << 1; }  /* GBTN_DOWN */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_left(void)  { return 1 << 2; }  /* GBTN_LEFT */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_right(void) { return 1 << 3; }  /* GBTN_RIGHT */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_b(void)     { return 1 << 4; }  /* GBTN_B */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_c(void)     { return 1 << 5; }  /* GBTN_C */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_a(void)     { return 1 << 6; }  /* GBTN_A */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_start(void) { return 1 << 7; }  /* GBTN_START */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_z(void)     { return 1 << 8; }  /* GBTN_Z */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_y(void)     { return 1 << 9; }  /* GBTN_Y */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_x(void)     { return 1 << 10; } /* GBTN_X */

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_button_mode(void)  { return 1 << 11; } /* GBTN_MODE */

/* Region override API - values: 0=Auto, 1=Japan NTSC, 2=Japan PAL, 4=USA, 8=Europe */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void pico_set_region(int region)
{
    PicoIn.regionOverride = region;
    lprintf("pico_set_region: set to %d\n", region);

    /* If a game is loaded, re-detect region and reset */
    if (game_loaded) {
        PicoDetectRegion();
        PicoLoopPrepare();
        PsndRerate(0);
        lprintf("pico_set_region: applied, pal=%d, hw=0x%02x\n", Pico.m.pal, Pico.m.hardware);
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_region(void)
{
    /* Return current hardware region: 0x80=USA, 0xc0=Europe, 0x00=Japan NTSC, 0x40=Japan PAL */
    return Pico.m.hardware & 0xc0;
}

/* Save state support - based on libretro implementation */
#define STATE_MAX_SIZE (512 * 1024)  /* 512KB should be enough for any state */
static unsigned char *state_buffer = NULL;
static size_t state_size = 0;

/* State file callbacks */
struct state_context {
    const unsigned char *load_buf;
    unsigned char *save_buf;
    size_t size;
    size_t pos;
};

static size_t state_read_cb(void *p, size_t size, size_t nmemb, void *file)
{
    struct state_context *ctx = (struct state_context *)file;
    size_t bsize = size * nmemb;

    if (ctx->pos + bsize > ctx->size) {
        bsize = ctx->size - ctx->pos;
        if ((int)bsize <= 0)
            return 0;
    }

    memcpy(p, ctx->load_buf + ctx->pos, bsize);
    ctx->pos += bsize;
    return bsize;
}

static size_t state_write_cb(void *p, size_t size, size_t nmemb, void *file)
{
    struct state_context *ctx = (struct state_context *)file;
    size_t bsize = size * nmemb;

    if (ctx->pos + bsize > ctx->size) {
        bsize = ctx->size - ctx->pos;
        if ((int)bsize <= 0)
            return 0;
    }

    memcpy(ctx->save_buf + ctx->pos, p, bsize);
    ctx->pos += bsize;
    return bsize;
}

static size_t state_skip_cb(void *p, size_t size, size_t nmemb, void *file)
{
    struct state_context *ctx = (struct state_context *)file;
    size_t bsize = size * nmemb;
    ctx->pos += bsize;
    return bsize;
}

static size_t state_eof_cb(void *file)
{
    struct state_context *ctx = (struct state_context *)file;
    return ctx->pos >= ctx->size;
}

static int state_seek_cb(void *file, long offset, int whence)
{
    struct state_context *ctx = (struct state_context *)file;

    switch (whence) {
    case SEEK_SET:
        ctx->pos = offset;
        break;
    case SEEK_CUR:
        ctx->pos += offset;
        break;
    case SEEK_END:
        ctx->pos = ctx->size + offset;
        break;
    }
    return (int)ctx->pos;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_state_save(void)
{
    if (!game_loaded) {
        lprintf("pico_state_save: no game loaded\n");
        return 0;
    }

    /* Allocate buffer if needed */
    if (!state_buffer) {
        state_buffer = (unsigned char *)malloc(STATE_MAX_SIZE);
        if (!state_buffer) {
            lprintf("pico_state_save: failed to allocate buffer\n");
            return 0;
        }
    }

    struct state_context ctx = { 0 };
    ctx.save_buf = state_buffer;
    ctx.size = STATE_MAX_SIZE;
    ctx.pos = 0;

    int ret = PicoStateFP(&ctx, 1, NULL, state_write_cb, NULL, state_seek_cb);
    if (ret != 0) {
        lprintf("pico_state_save: PicoStateFP failed with %d\n", ret);
        return 0;
    }

    state_size = ctx.pos;
    lprintf("pico_state_save: saved %zu bytes\n", state_size);
    return 1;
}

/* Get pointer to state buffer (for JS to read after save) */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
unsigned char *pico_get_state_buffer(void)
{
    return state_buffer;
}

/* Get size of saved state */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_get_state_size(void)
{
    return (int)state_size;
}

/* Allocate/get buffer for loading state (JS writes data here before calling load) */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
unsigned char *pico_get_state_load_buffer(int size)
{
    if (!state_buffer) {
        state_buffer = (unsigned char *)malloc(STATE_MAX_SIZE);
        if (!state_buffer) {
            lprintf("pico_get_state_load_buffer: failed to allocate buffer\n");
            return NULL;
        }
    }
    state_size = size;
    return state_buffer;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_state_load(void)
{
    if (!game_loaded) {
        lprintf("pico_state_load: no game loaded\n");
        return 0;
    }

    if (!state_buffer || state_size == 0) {
        lprintf("pico_state_load: no save state available\n");
        return 0;
    }

    struct state_context ctx = { 0 };
    ctx.load_buf = state_buffer;
    ctx.size = state_size;
    ctx.pos = 0;

    int ret = PicoStateFP(&ctx, 0, state_read_cb, NULL, state_eof_cb, state_seek_cb);
    if (ret != 0) {
        lprintf("pico_state_load: PicoStateFP failed with %d\n", ret);
        return 0;
    }

    lprintf("pico_state_load: loaded %zu bytes\n", state_size);
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int pico_state_exists(void)
{
    return (state_buffer != NULL && state_size > 0) ? 1 : 0;
}

/* Main entry point - called by Emscripten on startup */
int main(int argc, char *argv[])
{
    lprintf("main: starting\n");
    pico_init();
    return 0;
}
