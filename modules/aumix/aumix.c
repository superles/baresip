/**
 * @file aumix.c N-1 audio source and play (e.g. for centralized conferences)
 *
 * Copyright (C) 2021 Sebastian Reimers
 */
#include <re.h>
#include <rem.h>
#include <baresip.h>

enum { PTIME = 20, SRATE = 48000, CH = 2 };

static struct auplay *auplay;
static struct ausrc *ausrc;
static struct list auplayl;
static struct list ausrcl;
static struct aumix *aumix;

struct ausrc_st {
	struct le le;
	struct ausrc_prm prm;
	ausrc_read_h *rh;
	struct auplay_st *st_play;
	void *arg;
	const char *device;
};

struct auplay_st {
	struct le le;
	struct auplay_prm prm;
	auplay_write_h *wh;
	int16_t *sampv;
	struct aumix_source *aumix_src;
	struct ausrc_st *st_src;
	void *arg;
	uint64_t ts;
	const char *device;
};


static void mix_handler(const int16_t *sampv, size_t sampc, void *arg)
{
	struct auplay_st *st_play = arg;
	struct auframe af;

	auframe_init(&af, st_play->st_src->prm.fmt, (int16_t *)sampv, sampc,
		     st_play->st_src->prm.srate, st_play->prm.ch);
	af.timestamp = st_play->ts;
	st_play->st_src->rh(&af, st_play->st_src->arg);

	auframe_init(&af, st_play->prm.fmt, st_play->sampv, sampc,
		     st_play->prm.srate, st_play->prm.ch);
	af.timestamp = st_play->ts;
	st_play->wh(&af, st_play->arg);

	aumix_source_put(st_play->aumix_src, st_play->sampv, sampc);

	st_play->ts += PTIME * 1000;
}


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;
	if (st->st_play && st->st_play->aumix_src)
		aumix_source_enable(st->st_play->aumix_src, false);
	list_unlink(&st->le);
}


static int src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct media_ctx **ctx, struct ausrc_prm *prm,
		     const char *device, ausrc_read_h *rh, ausrc_error_h *errh,
		     void *arg)
{
	struct ausrc_st *st;
	struct le *le;
	(void)ctx;
	(void)errh;

	if (!stp || !as || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm = *prm;
	st->rh	= rh;
	st->arg = arg;
	st->device = device;

	/* setup if auplay is started before ausrc */
	for (le = list_head(&auplayl); le; le = le->next) {
		struct auplay_st *st_play = le->data;

		/* compare struct audio arg */
		if (st->arg == st_play->arg) {
			st_play->st_src = st;
			st->st_play = st_play;

			// aumix_source_enable(st_play->aumix_src, true);
			break;
		}
	}

	list_append(&ausrcl, &st->le, st);

	*stp = st;

	return 0;
}


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	mem_deref(st->aumix_src);
	mem_deref(st->sampv);
	list_unlink(&st->le);
}


static int play_alloc(struct auplay_st **stp, const struct auplay *ap,
		      struct auplay_prm *prm, const char *device,
		      auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;
	struct le *le;

	if (!stp || !ap || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->sampv = mem_zalloc((SRATE * CH * PTIME / 1000) * sizeof(int16_t),
			       NULL);
	if (!st->sampv) {
		err = ENOMEM;
		goto out;
	}

	st->prm = *prm;
	st->wh	= wh;
	st->arg = arg;
	st->device = device;

	err = aumix_source_alloc(&st->aumix_src, aumix, mix_handler, st);
	if (err)
		goto out;

	/* setup if ausrc is started before auplay */
	for (le = list_head(&ausrcl); le; le = le->next) {
		struct ausrc_st *st_src = le->data;

		/* compare struct audio arg */
		if (st->arg == st_src->arg) {
			st_src->st_play = st;
			st->st_src = st_src;

			// aumix_source_enable(st->aumix_src, true);
			break;
		}
	}

	list_append(&auplayl, &st->le, st);

out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


/* aumix_enable device,true/false */
static int source_enable(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct le *le;
	struct pl r, device = pl_null, enable_pl = pl_null;
	bool enable;
	int err;

	pl_set_str(&r, carg->prm);
	err = re_regex(r.p, r.l, "[^,]+,[~]*", &device, &enable_pl);
	IF_ERR_RETURN(err);

	str_bool(&enable, enable_pl.p);

	info("aumix_enable %r %d", device, enable);

	LIST_FOREACH(&auplayl, le) {
		struct auplay_st *st = le->data;
		if (!str_cmp(st->device, device.p))
			continue;
		if (enable)
			aumix_source_enable(st->aumix_src, true);
		else 
			aumix_source_enable(st->aumix_src, false);
	}

	return err;
}


static int mix_debug(struct re_printf *pf, void *arg)
{
	(void)arg;

}


static const struct cmd cmdv[] = {
	{"aumix_enable", 0, CMD_PRM, "Enable/Disable aumix source", source_enable},
	{"aumix_debug", 'z', 0, "Debug aumix", mix_debug}
};


static int module_init(void)
{
	int err;

	err = ausrc_register(&ausrc, baresip_ausrcl(), "aumix", src_alloc);
	IF_ERR_GOTO_OUT(err);

	err = auplay_register(&auplay, baresip_auplayl(), "aumix", play_alloc);
	IF_ERR_GOTO_OUT(err)

	err = aumix_alloc(&aumix, SRATE, CH, PTIME);
	IF_ERR_GOTO_OUT(err);

	list_init(&auplayl);
	list_init(&ausrcl);

out:
	return err;
}


static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);
	aumix  = mem_deref(aumix);
	list_flush(&auplayl);
	list_flush(&ausrcl);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(aumix) = {
	"aumix",
	"audio",
	module_init,
	module_close,
};
