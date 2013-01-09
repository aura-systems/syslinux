#include <sys/ansi.h>
#include <sys/io.h>
#include <fs.h>
#include <bios.h>
#include <com32.h>
#include <graphics.h>
#include <syslinux/memscan.h>
#include <syslinux/firmware.h>

#include <sys/vesa/vesa.h>
#include <sys/vesa/video.h>
#include <sys/vesa/debug.h>
#include <minmax.h>

struct firmware *firmware = NULL;

extern struct ansi_ops bios_ansi_ops;

#define BIOS_CURXY ((struct curxy *)0x450)	/* Array for each page */
#define BIOS_ROWS (*(uint8_t *)0x484)	/* Minus one; if zero use 24 (= 25 lines) */
#define BIOS_COLS (*(uint16_t *)0x44A)
#define BIOS_PAGE (*(uint8_t *)0x462)

static void bios_set_mode(uint16_t mode)
{
    syslinux_force_text_mode();
}

static void bios_get_mode(int *cols, int *rows)
{
    *rows = BIOS_ROWS ? BIOS_ROWS + 1 : 25;
    *cols = BIOS_COLS;
}

static uint16_t cursor_type;	/* Saved cursor pattern */

static void bios_get_cursor(int *x, int *y)
{
    com32sys_t ireg, oreg;

    memset(&ireg, 0, sizeof(ireg));

    ireg.eax.b[1] = 0x03;
    ireg.ebx.b[1] = BIOS_PAGE;
    __intcall(0x10, &ireg, &oreg);
    cursor_type = oreg.ecx.w[0];
    *x = oreg.edx.b[0];
    *y = oreg.edx.b[1];
}

static void bios_erase(int x0, int y0, int x1, int y1, uint8_t attribute)
{
    static com32sys_t ireg;

    ireg.eax.w[0] = 0x0600;	/* Clear window */
    ireg.ebx.b[1] = attribute;
    ireg.ecx.b[0] = x0;
    ireg.ecx.b[1] = y0;
    ireg.edx.b[0] = x1;
    ireg.edx.b[1] = y1;
    __intcall(0x10, &ireg, NULL);
}

static void bios_showcursor(const struct term_state *st)
{
    static com32sys_t ireg;
    uint16_t cursor = st->cursor ? cursor_type : 0x2020;

    ireg.eax.b[1] = 0x01;
    ireg.ecx.w[0] = cursor;
    __intcall(0x10, &ireg, NULL);
}

static void bios_set_cursor(int x, int y, bool visible)
{
    const int page = BIOS_PAGE;
    struct curxy xy = BIOS_CURXY[page];
    static com32sys_t ireg;

    (void)visible;

    if (xy.x != x || xy.y != y) {
	ireg.eax.b[1] = 0x02;
	ireg.ebx.b[1] = page;
	ireg.edx.b[1] = y;
	ireg.edx.b[0] = x;
	__intcall(0x10, &ireg, NULL);
    }
}

static void bios_write_char(uint8_t ch, uint8_t attribute)
{
    static com32sys_t ireg;

    ireg.eax.b[1] = 0x09;
    ireg.eax.b[0] = ch;
    ireg.ebx.b[1] = BIOS_PAGE;
    ireg.ebx.b[0] = attribute;
    ireg.ecx.w[0] = 1;
    __intcall(0x10, &ireg, NULL);
}

static void bios_scroll_up(uint8_t cols, uint8_t rows, uint8_t attribute)
{
    static com32sys_t ireg;

    ireg.eax.w[0] = 0x0601;
    ireg.ebx.b[1] = attribute;
    ireg.ecx.w[0] = 0;
    ireg.edx.b[1] = rows;
    ireg.edx.b[0] = cols;
    __intcall(0x10, &ireg, NULL);	/* Scroll */
}

static void bios_beep(void)
{
    static com32sys_t ireg;

    ireg.eax.w[0] = 0x0e07;
    ireg.ebx.b[1] = BIOS_PAGE;
    __intcall(0x10, &ireg, NULL);
}

struct output_ops bios_output_ops = {
	.erase = bios_erase,
	.write_char = bios_write_char,
	.showcursor = bios_showcursor,
	.set_cursor = bios_set_cursor,
	.scroll_up = bios_scroll_up,
	.beep = bios_beep,
	.get_mode = bios_get_mode,
	.set_mode = bios_set_mode,
	.get_cursor = bios_get_cursor,
};

extern char bios_getchar(char *);
extern int bios_pollchar(void);

struct input_ops bios_input_ops = {
	.getchar = bios_getchar,
	.pollchar = bios_pollchar,
};

static const char *syslinux_ipappend_string_list[32];
bool bios_ipappend_strings(char **list, int *count)
{
    static com32sys_t reg;
    int i;

    reg.eax.w[0] = 0x000f;
    __intcall(0x22, &reg, &reg);

    if (reg.eflags.l & EFLAGS_CF)
	return false;

    for (i = 0; i < reg.ecx.w[0]; i++) {
	syslinux_ipappend_string_list[i] =
	    MK_PTR(reg.es,
		   *(uint16_t *) MK_PTR(reg.es, reg.ebx.w[0] + i * 2));
    }

    *list = syslinux_ipappend_string_list;
    *count = reg.ecx.w[0];

    return true;
}

static void bios_get_serial_console_info(uint16_t *iobase, uint16_t *divisor,
					 uint16_t *flowctl)
{
    *iobase = SerialPort;
    *divisor = BaudDivisor;

    *flowctl = FlowOutput | FlowInput | (FlowIgnore << 4);

    if (!DisplayCon)
	*flowctl |= (0x80 << 8);
}

void *__syslinux_adv_ptr;
size_t __syslinux_adv_size;

void bios_adv_init(void)
{
    static com32sys_t reg;

    reg.eax.w[0] = 0x0025;
    __intcall(0x22, &reg, &reg);

    reg.eax.w[0] = 0x001c;
    __intcall(0x22, &reg, &reg);
    __syslinux_adv_ptr = MK_PTR(reg.es, reg.ebx.w[0]);
    __syslinux_adv_size = reg.ecx.w[0];
}

int bios_adv_write(void)
{
    static com32sys_t reg;

    reg.eax.w[0] = 0x001d;
    __intcall(0x22, &reg, &reg);
    return (reg.eflags.l & EFLAGS_CF) ? -1 : 0;
}

struct adv_ops bios_adv_ops = {
	.init = bios_adv_init,
	.write = bios_adv_write,
};


static int __constfunc is_power_of_2(unsigned int x)
{
    return x && !(x & (x - 1));
}

static int vesacon_paged_mode_ok(const struct vesa_mode_info *mi)
{
    int i;

    if (!is_power_of_2(mi->win_size) ||
	!is_power_of_2(mi->win_grain) || mi->win_grain > mi->win_size)
	return 0;		/* Impossible... */

    for (i = 0; i < 2; i++) {
	if ((mi->win_attr[i] & 0x05) == 0x05 && mi->win_seg[i])
	    return 1;		/* Usable window */
    }

    return 0;			/* Nope... */
}

static int bios_vesacon_set_mode(struct vesa_info *vesa_info, int *px, int *py,
				 enum vesa_pixel_format *bestpxf)
{
    com32sys_t rm;
    uint16_t mode, bestmode, *mode_ptr;
    struct vesa_info *vi;
    struct vesa_general_info *gi;
    struct vesa_mode_info *mi;
    enum vesa_pixel_format pxf;
    int x = *px, y = *py;
    int err = 0;

    /* Allocate space in the bounce buffer for these structures */
    vi = lzalloc(sizeof *vi);
    if (!vi) {
	err = 10;		/* Out of memory */
	goto exit;
    }
    gi = &vi->gi;
    mi = &vi->mi;

    memset(&rm, 0, sizeof rm);

    gi->signature = VBE2_MAGIC;	/* Get VBE2 extended data */
    rm.eax.w[0] = 0x4F00;	/* Get SVGA general information */
    rm.edi.w[0] = OFFS(gi);
    rm.es = SEG(gi);
    __intcall(0x10, &rm, &rm);

    if (rm.eax.w[0] != 0x004F) {
	err = 1;		/* Function call failed */
	goto exit;
    }
    if (gi->signature != VESA_MAGIC) {
	err = 2;		/* No magic */
	goto exit;
    }
    if (gi->version < 0x0102) {
	err = 3;		/* VESA 1.2+ required */
	goto exit;
    }

    /* Copy general info */
    memcpy(&vesa_info->gi, gi, sizeof *gi);

    /* Search for the proper mode with a suitable color and memory model... */

    mode_ptr = GET_PTR(gi->video_mode_ptr);
    bestmode = 0;
    *bestpxf = PXF_NONE;

    while ((mode = *mode_ptr++) != 0xFFFF) {
	mode &= 0x1FF;		/* The rest are attributes of sorts */

	debug("Found mode: 0x%04x\r\n", mode);

	memset(mi, 0, sizeof *mi);
	rm.eax.w[0] = 0x4F01;	/* Get SVGA mode information */
	rm.ecx.w[0] = mode;
	rm.edi.w[0] = OFFS(mi);
	rm.es = SEG(mi);
	__intcall(0x10, &rm, &rm);

	/* Must be a supported mode */
	if (rm.eax.w[0] != 0x004f)
	    continue;

	debug
	    ("mode_attr 0x%04x, h_res = %4d, v_res = %4d, bpp = %2d, layout = %d (%d,%d,%d)\r\n",
	     mi->mode_attr, mi->h_res, mi->v_res, mi->bpp, mi->memory_layout,
	     mi->rpos, mi->gpos, mi->bpos);

	/* Must be an LFB color graphics mode supported by the hardware.

	   The bits tested are:
	   4 - graphics mode
	   3 - color mode
	   1 - mode information available (mandatory in VBE 1.2+)
	   0 - mode supported by hardware
	 */
	if ((mi->mode_attr & 0x001b) != 0x001b)
	    continue;

	/* Must be the chosen size */
	if (mi->h_res != x || mi->v_res != y)
	    continue;

	/* We don't support multibank (interlaced memory) modes */
	/*
	 *  Note: The Bochs VESA BIOS (vbe.c 1.58 2006/08/19) violates the
	 * specification which states that banks == 1 for unbanked modes;
	 * fortunately it does report bank_size == 0 for those.
	 */
	if (mi->banks > 1 && mi->bank_size) {
	    debug("bad: banks = %d, banksize = %d, pages = %d\r\n",
		  mi->banks, mi->bank_size, mi->image_pages);
	    continue;
	}

	/* Must be either a flat-framebuffer mode, or be an acceptable
	   paged mode */
	if (!(mi->mode_attr & 0x0080) && !vesacon_paged_mode_ok(mi)) {
	    debug("bad: invalid paged mode\r\n");
	    continue;
	}

	/* Must either be a packed-pixel mode or a direct color mode
	   (depending on VESA version ); must be a supported pixel format */
	pxf = PXF_NONE;		/* Not usable */

	if (mi->bpp == 32 &&
	    (mi->memory_layout == 4 ||
	     (mi->memory_layout == 6 && mi->rpos == 16 && mi->gpos == 8 &&
	      mi->bpos == 0)))
	    pxf = PXF_BGRA32;
	else if (mi->bpp == 24 &&
		 (mi->memory_layout == 4 ||
		  (mi->memory_layout == 6 && mi->rpos == 16 && mi->gpos == 8 &&
		   mi->bpos == 0)))
	    pxf = PXF_BGR24;
	else if (mi->bpp == 16 &&
		 (mi->memory_layout == 4 ||
		  (mi->memory_layout == 6 && mi->rpos == 11 && mi->gpos == 5 &&
		   mi->bpos == 0)))
	    pxf = PXF_LE_RGB16_565;
	else if (mi->bpp == 15 &&
		 (mi->memory_layout == 4 ||
		  (mi->memory_layout == 6 && mi->rpos == 10 && mi->gpos == 5 &&
		   mi->bpos == 0)))
	    pxf = PXF_LE_RGB15_555;

	if (pxf < *bestpxf) {
	    debug("Best mode so far, pxf = %d\r\n", pxf);

	    /* Best mode so far... */
	    bestmode = mode;
	    *bestpxf = pxf;

	    /* Copy mode info */
	    memcpy(&vesa_info->mi, mi, sizeof *mi);
	}
    }

    if (*bestpxf == PXF_NONE) {
	err = 4;		/* No mode found */
	goto exit;
    }

    mi = &vesa_info->mi;
    mode = bestmode;

    /* Now set video mode */
    rm.eax.w[0] = 0x4F02;	/* Set SVGA video mode */
    if (mi->mode_attr & 0x0080)
	mode |= 0x4000;		/* Request linear framebuffer if supported */
    rm.ebx.w[0] = mode;
    __intcall(0x10, &rm, &rm);
    if (rm.eax.w[0] != 0x004F) {
	err = 9;		/* Failed to set mode */
	goto exit;
    }

exit:
    if (vi)
	lfree(vi);

    return err;
}

static void set_window_pos(struct win_info *wi, size_t win_pos)
{
    static com32sys_t ireg;

    wi->win_pos = win_pos;

    if (wi->win_num < 0)
	return;			/* This should never happen... */

    ireg.eax.w[0] = 0x4F05;
    ireg.ebx.b[0] = wi->win_num;
    ireg.edx.w[0] = win_pos >> wi->win_gshift;

    __intcall(0x10, &ireg, NULL);
}

static void bios_vesacon_screencpy(size_t dst, const char * src,
				   size_t bytes, struct win_info *wi)
{
    size_t win_pos, win_off;
    size_t win_size = wi->win_size;
    size_t omask = win_size - 1;
    char *win_base = wi->win_base;
    size_t l;

    while (bytes) {
	win_off = dst & omask;
	win_pos = dst & ~omask;

	if (__unlikely(win_pos != wi->win_pos))
	    set_window_pos(wi, win_pos);

	l = min(bytes, win_size - win_off);
	memcpy(win_base + win_off, src, l);

	bytes -= l;
	src += l;
	dst += l;
    }
}

static int bios_font_query(uint8_t **font)
{
    com32sys_t rm;

    /* Get BIOS 8x16 font */

    rm.eax.w[0] = 0x1130;	/* Get Font Information */
    rm.ebx.w[0] = 0x0600;	/* Get 8x16 ROM font */
    __intcall(0x10, &rm, &rm);
    *font = MK_PTR(rm.es, rm.ebp.w[0]);

    return 16;

}
struct vesa_ops bios_vesa_ops = {
	.set_mode  = bios_vesacon_set_mode,
	.screencpy = bios_vesacon_screencpy,
	.font_query = bios_font_query,
};

static uint32_t min_lowmem_heap = 65536;
extern char __lowmem_heap[];
uint8_t KbdFlags;		/* Check for keyboard escapes */
__export uint8_t KbdMap[256];	/* Keyboard map */

__export uint16_t PXERetry;

static inline void check_escapes(void)
{
	com32sys_t ireg, oreg;

	ireg.eax.b[1] = 0x02;	/* Check keyboard flags */
	__intcall(0x16, &ireg, &oreg);

	KbdFlags = oreg.eax.b[0];

	/* Ctrl->skip 386 check */
	if (oreg.eax.b[0] & 0x04) {
		/*
		 * Now check that there is sufficient low (DOS) memory
		 *
		 * NOTE: Linux doesn't use all of real_mode_seg, but we use
		 * the same segment for COMBOOT images, which can use all 64K.
		 */
		uint16_t mem;

		__intcall(0x12, &ireg, &oreg);

		mem = ((uint32_t)__lowmem_heap) + min_lowmem_heap + 1023;
		mem = mem >> 10;

		if (mem < oreg.eax.w[0]) {
			char buf[256];

			snprintf(buf, sizeof(buf),
				 "It appears your computer has only "
				 "%dK of low (\"DOS\") RAM.\n"
				 "This version of Syslinux needs "
				 "%dK to boot.  "
				 "If you get this\nmessage in error, "
				 "hold down the Ctrl key while booting, "
				 "and I\nwill take your word for it.\n",
				 oreg.eax.w[0], mem);
			writestr(buf);
			kaboom();
		}
	}
}

extern uint32_t BIOS_timer_next;
extern uint32_t timer_irq;
static inline void bios_timer_init(void)
{
	unsigned long next;
	uint32_t *hook = (uint32_t *)BIOS_timer_hook;

	next = *hook;
	BIOS_timer_next = next;
	*hook = (uint32_t)&timer_irq;
}

extern uint8_t bios_free_mem;

void bios_init(void)
{
	int i;

	/* Initialize timer */
	bios_timer_init();

	for (i = 0; i < 256; i++)
		KbdMap[i] = i;

	bios_adjust_screen();

	/* Init the memory subsystem */
	bios_free_mem = (uint16_t *)0x413;
	mem_init();

	/* CPU-dependent initialization and related checks. */
	check_escapes();
}

extern void *bios_malloc(size_t, enum heap, size_t);
extern void *bios_realloc(void *, size_t);
extern void bios_free(void *);

struct mem_ops bios_mem_ops = {
	.malloc = bios_malloc,
	.realloc = bios_realloc,
	.free = bios_free,
	.scan_memory = bios_scan_memory,
};

struct firmware bios_fw = {
	.init = bios_init,
	.adjust_screen = bios_adjust_screen,
	.cleanup = bios_cleanup_hardware,
	.disk_init = bios_disk_init,
	.o_ops = &bios_output_ops,
	.i_ops = &bios_input_ops,
	.ipappend_strings = bios_ipappend_strings,
	.get_serial_console_info = bios_get_serial_console_info,
	.adv_ops = &bios_adv_ops,
	.vesa = &bios_vesa_ops,
	.mem = &bios_mem_ops,
};

void syslinux_register_bios(void)
{
	firmware = &bios_fw;
}