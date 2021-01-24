#include "../../arcan_shmif.h"
#include "../../arcan_tui.h"
#include "../tui_int.h"
#include "../screen/libtsm.h"
#include "../screen/libtsm_int.h"
#include "arcan_ttf.h"
#include "../raster/raster.h"

#include <stdio.h>
#include <inttypes.h>
#include <math.h>

void tui_queue_requests(struct tui_context* tui, bool clipboard, bool ident)
{
/* immediately request a clipboard for cut operations (none received ==
 * running appl doesn't care about cut'n'paste/drag'n'drop support). */
/* and send a timer that will be used for cursor blinking when active */
	if (clipboard)
	arcan_shmif_enqueue(&tui->acon, &(struct arcan_event){
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(SEGREQ),
		.ext.segreq.width = 1,
		.ext.segreq.height = 1,
		.ext.segreq.kind = SEGID_CLIPBOARD,
		.ext.segreq.id = 0xfeedface
	});

/* always request a timer as the _tick callback may need it */
	arcan_shmif_enqueue(&tui->acon, &(struct arcan_event){
		.category = EVENT_EXTERNAL,
		.ext.kind = ARCAN_EVENT(CLOCKREQ),
		.ext.clock.rate = 1,
		.ext.clock.id = 0xabcdef00,
	});

/* ident is only set on crash recovery */
	if (ident){
		if (tui->last_ident.ext.kind != 0)
			arcan_shmif_enqueue(&tui->acon, &tui->last_ident);

		arcan_shmif_enqueue(&tui->acon, &tui->last_state_sz);
	}

	tui_expose_labels(tui);
}

static int parse_color(const char* inv, uint8_t outv[4])
{
	return sscanf(inv, "%"SCNu8",%"SCNu8",%"SCNu8",%"SCNu8,
		&outv[0], &outv[1], &outv[2], &outv[3]);
}

/* possible command line overrides */
static void apply_arg(struct tui_context* src, struct arg_arr* args)
{
	if (!args)
		return;

	const char* val;
	uint8_t ccol[4] = {0x00, 0x00, 0x00, 0xff};
	long vbufv = 0;

	if (!(arg_lookup(args, "fgc", 0, &val)
		&& val && parse_color(val, ccol) >= 3)){
		ccol[0] = 0xff;
		ccol[1] = 0xff;
		ccol[2] = 0xff;
	}
	arcan_tui_set_color(src, TUI_COL_TEXT, ccol);

	if (!(arg_lookup(args, "bgc", 0, &val) && parse_color(val, ccol) >= 3)){
		ccol[0] = 0x00;
		ccol[1] = 0x00;
		ccol[2] = 0x00;
	}
	arcan_tui_set_color(src, TUI_COL_BG, ccol);

	if (!(arg_lookup(args, "cc", 0, &val) && parse_color(val, ccol) >= 3)){
		ccol[0] = 0x00;
		ccol[1] = 0xaa;
		ccol[2] = 0x00;
	}
	arcan_tui_set_color(src, TUI_COL_CURSOR, ccol);

	if (!(arg_lookup(args, "clc", 0, &val) && parse_color(val, ccol) >= 3)){
		ccol[0] = 0x00;
		ccol[1] = 0xaa;
		ccol[2] = 0x00;
	}
	arcan_tui_set_color(src, TUI_COL_ALTCURSOR, ccol);

	if (arg_lookup(args, "bgalpha", 0, &val) && val)
		src->alpha = strtoul(val, NULL, 10);
}

arcan_tui_conn* arcan_tui_open_display(const char* title, const char* ident)
{
	struct arcan_shmif_cont* res = malloc(sizeof(struct arcan_shmif_cont));
	if (!res)
		return NULL;

	*res = arcan_shmif_open_ext(
		SHMIF_ACQUIRE_FATALFAIL, NULL, (struct shmif_open_ext){
			.type = SEGID_TUI,
			.title = title,
			.ident = ident,
		}, sizeof(struct shmif_open_ext)
	);

	if (!res->addr){
		free(res);
		return NULL;
	}

	res->user = (void*) 0xdeadbeef;
	return res;
}

void arcan_tui_destroy(struct tui_context* tui, const char* message)
{
	if (!tui)
		return;

	if (tui->clip_in.vidp)
		arcan_shmif_drop(&tui->clip_in);

	if (tui->clip_out.vidp)
		arcan_shmif_drop(&tui->clip_out);

	if (message)
		arcan_shmif_last_words(&tui->acon, message);

	arcan_shmif_drop(&tui->acon);
	tsm_utf8_mach_free(tui->ucsconv);

	free(tui->base);

	memset(tui, '\0', sizeof(struct tui_context));
	free(tui);
}

static void tsm_log(void* data, const char* file, int line,
	const char* func, const char* subs, unsigned int sev,
	const char* fmt, va_list arg)
{
	fprintf(stderr, "[%d] %s:%d - %s, %s()\n", sev, file, line, subs, func);
	vfprintf(stderr, fmt, arg);
}

/*
 * though we are supposed to be prerolled the colors from our display
 * server connection, it's best to have something that gets activated
 * initially regardless..
 */
static void set_builtin_palette(struct tui_context* ctx)
{
	ctx->colors[TUI_COL_CURSOR] = (struct color){0x00, 0xff, 0x00};
	ctx->colors[TUI_COL_ALTCURSOR] = (struct color){0x00, 0xff, 0x00};
	ctx->colors[TUI_COL_HIGHLIGHT] = (struct color){0x26, 0x8b, 0xd2};
	ctx->colors[TUI_COL_BG] = (struct color){0x2b, 0x2b, 0x2b};
	ctx->colors[TUI_COL_PRIMARY] = (struct color){0x13, 0x13, 0x13};
	ctx->colors[TUI_COL_SECONDARY] = (struct color){0x42, 0x40, 0x3b};
	ctx->colors[TUI_COL_TEXT] = (struct color){0xff, 0xff, 0xff};
	ctx->colors[TUI_COL_LABEL] = (struct color){0xff, 0xff, 0x00};
	ctx->colors[TUI_COL_WARNING] = (struct color){0xaa, 0xaa, 0x00};
	ctx->colors[TUI_COL_ERROR] = (struct color){0xaa, 0x00, 0x00};
	ctx->colors[TUI_COL_ALERT] = (struct color){0xaa, 0x00, 0xaa};
	ctx->colors[TUI_COL_REFERENCE] = (struct color){0x20, 0x30, 0x20};
	ctx->colors[TUI_COL_INACTIVE] = (struct color){0x20, 0x20, 0x20};
}

static bool late_bind(
	arcan_tui_conn* con, struct tui_context* res, bool setup)
{
/*
 * if the connection comes from _open_display, free the intermediate
 * context store here and move it to our tui context
 */
	bool managed = false;
	res->acon = *con;
	if( (uintptr_t)con->user == 0xdeadbeef){
		if (managed)
			free(con);
		managed = true;
	}

/*
 * only in a managed context can we retrieve the initial state truthfully, as
 * it only takes a NEWSEGMENT event, not a context activation like for the
 * primary. Thus derive from the primary in that case, inherit from the parent
 * and then let any dynamic overrides appear as normal.
 */
	struct arcan_shmif_initial* init = NULL;
	if (sizeof(struct arcan_shmif_initial) !=
		arcan_shmif_initial(&res->acon, &init) && managed){
		LOG("initial structure size mismatch, out-of-synch header/shmif lib\n");
		arcan_shmif_drop(&res->acon);
		free(res);
		return NULL;
	}

/* this could have been set already by deriving from a parent */
	if (!res->ppcm){
		if (init){
			res->ppcm = init->density;
		}
		else
			res->ppcm = ARCAN_SHMPAGE_DEFAULT_PPCM;
	}

	tui_fontmgmt_setup(res, init);

	res->acon.hints = SHMIF_RHINT_TPACK | SHMIF_RHINT_VSIGNAL_EV;

/* clipboard, timer callbacks, no IDENT */
	tui_queue_requests(res, true, false);

	arcan_shmif_resize_ext(&res->acon,
		res->acon.w, res->acon.h,
		(struct shmif_resize_ext){
			.vbuf_cnt = -1,
			.abuf_cnt = -1,
			.rows = res->acon.h / res->cell_h,
			.cols = res->acon.w / res->cell_w
		}
	);

	for (size_t i = 0; i < COUNT_OF(init->colors) && i < COUNT_OF(res->colors); i++){
		if (init->colors[i].fg_set)
			memcpy(res->colors[i].rgb, init->colors[i].fg, 3);

		if (init->colors[i].bg_set){
			memcpy(res->colors[i].bg, init->colors[i].bg, 3);
			res->colors[i].bgset = true;
		}
	}

	tui_screen_resized(res);

	if (res->handlers.resized)
		res->handlers.resized(res, res->acon.w, res->acon.h,
			res->cols, res->rows, res->handlers.tag);

	return true;
}

bool arcan_tui_bind(arcan_tui_conn* con, struct tui_context* orphan)
{
	return late_bind(con, orphan, false);
}

struct tui_context* arcan_tui_setup(
	arcan_tui_conn* con,
	struct tui_context* parent,
	const struct tui_cbcfg* cbs,
	size_t cbs_sz, ...)
{
/* empty- con is permitted in order to allow 'late binding' of an orphaned
 * context, a way to pre-manage tui contexts without waiting for a matching
 * subwindow request */
	if (!cbs)
		return NULL;

	struct tui_context* res = malloc(sizeof(struct tui_context));
	if (!res)
		return NULL;
	*res = (struct tui_context){
		.alpha = 0xff,
		.font_sz = 0.0416,
		.flags = TUI_ALTERNATE,
		.cell_w = 8,
		.cell_h = 8
	};

	if (tsm_screen_new(&res->screen, tsm_log, res) < 0){
		LOG("failed to build screen structure\n");
		free(res);
		return NULL;
	}

/*
 * due to handlers being default NULL, all fields are void* / fptr*
 * (and we assume sizeof(void*) == sizeof(fptr*) which is somewhat
 * sketchy, but if that's a concern, subtract the offset of tag),
 * and we force the caller to provide its perceived size of the
 * struct we can expand the interface without breaking old clients
 */
	if (cbs_sz > sizeof(struct tui_cbcfg) || cbs_sz % sizeof(void*) != 0){
		LOG("arcan_tui(), caller provided bad size field\n");
		return NULL;
	}
	memcpy(&res->handlers, cbs, cbs_sz);

	set_builtin_palette(res);
	apply_arg(res, arcan_shmif_args(con));

/* tui_fontmgmt is also responsible for building the raster context */
/* if we have a parent, we should derive settings etc. from there */
	if (parent){
		memcpy(res->colors, parent->colors, sizeof(res->colors));
		res->alpha = parent->alpha;
		res->cursor = parent->cursor;
		res->ppcm = parent->ppcm;

		tui_fontmgmt_setup(res, &(struct arcan_shmif_initial){
			.fonts = {
				{
					.size_mm = parent->font_sz,
					.fd = arcan_shmif_dupfd(parent->font[0]->fd, -1, true)
				},
				{
					.size_mm = parent->font_sz,
					.fd = arcan_shmif_dupfd(parent->font[1]->fd, -1, true)
				}
		}});
	}

	if (0 != tsm_utf8_mach_new(&res->ucsconv)){
		free(res);
		return NULL;
	}

	tsm_screen_set_def_attr(res->screen,
		&(struct tui_screen_attr){
			.fr = res->colors[TUI_COL_TEXT].rgb[0],
			.fg = res->colors[TUI_COL_TEXT].rgb[1],
			.fb = res->colors[TUI_COL_TEXT].rgb[2],
			.br = res->colors[TUI_COL_BG].rgb[0],
			.bg = res->colors[TUI_COL_BG].rgb[1],
			.bb = res->colors[TUI_COL_BG].rgb[2]
		}
	);

/* TEMPORARY: when deprecating tsm any scrollback become the widgets problem */
	tsm_screen_set_max_sb(res->screen, 1000);

	if (con)
		late_bind(con, res, true);

/* allow our own formats to be exposed */
	arcan_tui_announce_io(res, false, NULL, "tui-raw");

	return res;
}
