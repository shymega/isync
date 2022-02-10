// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#include "main_p.h"

#define nz(a, b) ((a) ? (a) : (b))

static int ops_any[2], trash_any[2];
static int chans_total, chans_done;
static int boxes_total, boxes_done;

void
stats( void )
{
	char buf[3][64];
	char *cs;
	static int cols = -1;

	if (!(DFlags & PROGRESS))
		return;

	if (cols < 0 && (!(cs = getenv( "COLUMNS" )) || !(cols = atoi( cs ))))
		cols = 80;
	int ll = sprintf( buf[2], "C: %d/%d  B: %d/%d", chans_done, chans_total, boxes_done, boxes_total );
	int cls = (cols - ll - 10) / 2;
	for (int t = 0; t < 2; t++) {
		int l = sprintf( buf[t], "+%d/%d *%d/%d #%d/%d",
		                 new_done[t], new_total[t],
		                 flags_done[t], flags_total[t],
		                 trash_done[t], trash_total[t] );
		if (l > cls)
			buf[t][cls - 1] = '~';
	}
	progress( "\r%s  F: %.*s  N: %.*s", buf[2], cls, buf[0], cls, buf[1] );
}

static void
summary( void )
{
	if (Verbosity < TERSE)
		return;

	if (!boxes_done)
		return;  // Shut up if we errored out early.

	printf( "Processed %d box(es) in %d channel(s)", boxes_done, chans_done );
	for (int t = 2; --t >= 0; ) {
		if (ops_any[t])
			printf( ",\n%sed %d new message(s) and %d flag update(s)",
			        str_hl[t], new_done[t], flags_done[t] );
		if (trash_any[t])
			printf( ",\nmoved %d %s message(s) to trash",
			        trash_done[t], str_fn[t] );
	}
	puts( "." );
}

static int
matches( const char *t, const char *p )
{
	for (;;) {
		if (!*p)
			return !*t;
		if (*p == '*') {
			p++;
			do {
				if (matches( t, p ))
					return 1;
			} while (*t++);
			return 0;
		} else if (*p == '%') {
			p++;
			do {
				if (*t == '/')
					return 0;
				if (matches( t, p ))
					return 1;
			} while (*t++);
			return 0;
		} else {
			if (*p != *t)
				return 0;
			p++, t++;
		}
	}
}


static int
is_inbox( const char *name )
{
	return starts_with( name, -1, "INBOX", 5 ) && (!name[5] || name[5] == '/');
}

static int
cmp_box_names( const void *a, const void *b )
{
	const char *as = *(const char * const *)a;
	const char *bs = *(const char * const *)b;
	int ai = is_inbox( as );
	int bi = is_inbox( bs );
	int di = bi - ai;
	if (di)
		return di;
	return strcmp( as, bs );
}

static char **
filter_boxes( string_list_t *boxes, const char *prefix, string_list_t *patterns )
{
	char **boxarr = NULL;
	uint num = 0, rnum = 0;

	uint pfxl = prefix ? strlen( prefix ) : 0;
	for (; boxes; boxes = boxes->next) {
		if (!starts_with( boxes->string, -1, prefix, pfxl ))
			continue;
		uint fnot = 1, not;
		for (string_list_t *cpat = patterns; cpat; cpat = cpat->next) {
			const char *ps = cpat->string;
			if (*ps == '!') {
				ps++;
				not = 1;
			} else {
				not = 0;
			}
			if (matches( boxes->string + pfxl, ps )) {
				fnot = not;
				break;
			}
		}
		if (!fnot) {
			if (num + 1 >= rnum)
				boxarr = nfrealloc( boxarr, (rnum = (rnum + 10) * 2) * sizeof(*boxarr) );
			boxarr[num++] = nfstrdup( boxes->string + pfxl );
			boxarr[num] = NULL;
		}
	}
	qsort( boxarr, num, sizeof(*boxarr), cmp_box_names );
	return boxarr;
}

static void
merge_actions( channel_conf_t *chan, int ops[], int have, int mask, int def )
{
	if (ops[F] & have) {
		chan->ops[F] &= ~mask;
		chan->ops[F] |= ops[F] & mask;
		chan->ops[N] &= ~mask;
		chan->ops[N] |= ops[N] & mask;
	} else if (!(chan->ops[F] & have)) {
		if (global_conf.ops[F] & have) {
			chan->ops[F] |= global_conf.ops[F] & mask;
			chan->ops[N] |= global_conf.ops[N] & mask;
		} else {
			chan->ops[F] |= def;
			chan->ops[N] |= def;
		}
	}
}

typedef struct box_ent {
	struct box_ent *next;
	char *name;
	int present[2];
} box_ent_t;

typedef struct chan_ent {
	struct chan_ent *next;
	channel_conf_t *conf;
	box_ent_t *boxes;
	int boxlist;
} chan_ent_t;

static chan_ent_t *
add_channel( chan_ent_t ***chanapp, channel_conf_t *chan, int ops[] )
{
	chan_ent_t *ce = nfzalloc( sizeof(*ce) );
	ce->conf = chan;

	merge_actions( chan, ops, XOP_HAVE_TYPE, OP_MASK_TYPE, OP_MASK_TYPE );
	merge_actions( chan, ops, XOP_HAVE_CREATE, OP_CREATE, 0 );
	merge_actions( chan, ops, XOP_HAVE_REMOVE, OP_REMOVE, 0 );
	merge_actions( chan, ops, XOP_HAVE_EXPUNGE, OP_EXPUNGE, 0 );
	debug( "channel ops (%s):\n  far: %s\n  near: %s\n",
	       chan->name, fmt_ops( ops[F] ).str, fmt_ops( ops[N] ).str );

	for (int t = 0; t < 2; t++) {
		if (chan->ops[t] & OP_MASK_TYPE)
			ops_any[t] = 1;
		if ((chan->ops[t] & OP_EXPUNGE) &&
		    (chan->stores[t]->trash ||
		     (chan->stores[t^1]->trash && chan->stores[t^1]->trash_remote_new)))
			trash_any[t] = 1;
	}

	**chanapp = ce;
	*chanapp = &ce->next;
	chans_total++;
	return ce;
}

static chan_ent_t *
add_named_channel( chan_ent_t ***chanapp, char *channame, int ops[] )
{
	box_ent_t *boxes = NULL, **mboxapp = &boxes, *mbox;
	int boxlist = 0;

	char *boxp;
	if ((boxp = strchr( channame, ':' )))
		*boxp++ = 0;
	channel_conf_t *chan;
	for (chan = channels; chan; chan = chan->next)
		if (!strcmp( chan->name, channame ))
			goto gotchan;
	error( "No channel or group named '%s' defined.\n", channame );
	return NULL;
  gotchan:
	if (boxp) {
		if (!chan->patterns) {
			error( "Cannot override mailbox in channel '%s' - no Patterns.\n", channame );
			return NULL;
		}
		boxlist = 1;
		do {
			char *nboxp = strpbrk( boxp, ",\n" );
			size_t boxl;
			if (nboxp) {
				boxl = (size_t)(nboxp - boxp);
				*nboxp++ = 0;
			} else {
				boxl = strlen( boxp );
			}
			mbox = nfmalloc( sizeof(*mbox) );
			if (boxl)
				mbox->name = nfstrndup( boxp, boxl );
			else
				mbox->name = nfstrndup( "INBOX", 5 );
			mbox->present[F] = mbox->present[N] = BOX_POSSIBLE;
			mbox->next = NULL;
			*mboxapp = mbox;
			mboxapp = &mbox->next;
			boxes_total++;
			boxp = nboxp;
		} while (boxp);
	} else {
		if (!chan->patterns)
			boxes_total++;
	}

	chan_ent_t *ce = add_channel( chanapp, chan, ops );
	ce->boxes = boxes;
	ce->boxlist = boxlist;
	return ce;
}

typedef struct {
	int t[2];
	core_vars_t *cvars;
	channel_conf_t *chan;
	driver_t *drv[2];
	store_t *ctx[2];
	chan_ent_t *chanptr;
	box_ent_t *boxptr;
	string_list_t *boxes[2];
	char *names[2];
	int state[2];
	int chan_cben, fnlz_cben, box_cben, box_done;
} main_vars_t;

#define AUX &mvars->t[t]
#define MVARS(aux) \
	int t = *(int *)aux; \
	main_vars_t *mvars = (main_vars_t *)(((char *)(&((int *)aux)[-t])) - offsetof(main_vars_t, t));

static void do_sync_chans( main_vars_t *lvars );

void
sync_chans( core_vars_t *cvars, char **argv )
{
	main_vars_t mvars[1];
	chan_ent_t *chans = NULL, **chanapp = &chans;

	memset( mvars, 0, sizeof(*mvars) );
	mvars->t[1] = 1;
	mvars->cvars = cvars;

	if (!channels) {
		fputs( "No channels defined. Try 'man " EXE "'\n", stderr );
		cvars->ret = 1;
		return;
	}

	if (cvars->all) {
		for (channel_conf_t *chan = channels; chan; chan = chan->next) {
			add_channel( &chanapp, chan, cvars->ops );
			if (!chan->patterns)
				boxes_total++;
		}
	} else {
		for (; *argv; argv++) {
			for (group_conf_t *group = groups; group; group = group->next) {
				if (!strcmp( group->name, *argv )) {
					for (string_list_t *channame = group->channels; channame; channame = channame->next)
						if (!add_named_channel( &chanapp, channame->string, cvars->ops ))
							cvars->ret = 1;
					goto gotgrp;
				}
			}
			if (!add_named_channel( &chanapp, *argv, cvars->ops ))
				cvars->ret = 1;
		  gotgrp: ;
		}
	}
	if (cvars->ret)
		return;
	if (!chans) {
		fputs( "No channel specified. Try '" EXE " -h'\n", stderr );
		cvars->ret = 1;
		return;
	}
	mvars->chanptr = chans;

	if (!cvars->list)
		stats();
	do_sync_chans( mvars );
	main_loop();
	if (!cvars->list) {
		flushn();
		summary();
	}
}

enum {
	ST_FRESH,
	ST_CONNECTED,
	ST_OPEN,
	ST_CANCELING,
	ST_CLOSED,
};

static int
check_cancel( main_vars_t *mvars )
{
	return mvars->state[F] >= ST_CANCELING || mvars->state[N] >= ST_CANCELING;
}

static void store_connected( int sts, void *aux );
static void store_listed( int sts, string_list_t *boxes, void *aux );
static void sync_opened( main_vars_t *mvars, int t );
static void do_sync_boxes( main_vars_t *lvars );
static void done_sync_dyn( int sts, void *aux );
static void done_sync( int sts, void *aux );
static void finalize_sync( main_vars_t *mvars );

static void
store_bad( void *aux )
{
	MVARS(aux)

	mvars->drv[t]->cancel_store( mvars->ctx[t] );
	mvars->state[t] = ST_CLOSED;
	mvars->cvars->ret = 1;
	finalize_sync( mvars );
}

static void
advance_chan( main_vars_t *mvars )
{
	if (!mvars->cvars->list) {
		chans_done++;
		stats();
	}
	chan_ent_t *nchan = mvars->chanptr->next;
	free( mvars->chanptr );
	mvars->chanptr = nchan;
}

static void
do_sync_chans( main_vars_t *mvars )
{
	while (mvars->chanptr) {
		mvars->chan = mvars->chanptr->conf;
		info( "Channel %s\n", mvars->chan->name );
		for (int t = 0; t < 2; t++) {
			int st = mvars->chan->stores[t]->driver->get_fail_state( mvars->chan->stores[t] );
			if (st != FAIL_TEMP) {
				info( "Skipping due to %sfailed %s store %s.\n",
				      (st == FAIL_WAIT) ? "temporarily " : "", str_fn[t], mvars->chan->stores[t]->name );
				goto next;
			}
		}

		uint dcaps[2];
		for (int t = 0; t < 2; t++) {
			mvars->drv[t] = mvars->chan->stores[t]->driver;
			dcaps[t] = mvars->drv[t]->get_caps( NULL );
		}
		const char *labels[2];
		if ((DFlags & DEBUG_DRV) || (dcaps[F] & dcaps[N] & DRV_VERBOSE))
			labels[F] = "F: ", labels[N] = "N: ";
		else
			labels[F] = labels[N] = "";
		for (int t = 0; t < 2; t++) {
			store_t *ctx = mvars->drv[t]->alloc_store( mvars->chan->stores[t], labels[t] );
			if ((DFlags & DEBUG_DRV) || ((DFlags & FORCEASYNC) && !(dcaps[t] & DRV_ASYNC))) {
				mvars->drv[t] = &proxy_driver;
				ctx = proxy_alloc_store( ctx, labels[t] );
			}
			mvars->ctx[t] = ctx;
			mvars->drv[t]->set_bad_callback( ctx, store_bad, AUX );
			mvars->state[t] = ST_FRESH;
		}
		mvars->chan_cben = 0;
		for (int t = 0; ; t++) {
			info( "Opening %s store %s...\n", str_fn[t], mvars->chan->stores[t]->name );
			mvars->drv[t]->connect_store( mvars->ctx[t], store_connected, AUX );
			if (t || check_cancel( mvars ))
				break;
		}
		if (mvars->state[F] != ST_CLOSED || mvars->state[N] != ST_CLOSED) {
			mvars->chan_cben = 1;
			return;
		}

	  next:
		advance_chan( mvars );
	}
	cleanup_drivers();
}

static void
sync_next_chan( main_vars_t *mvars )
{
	if (mvars->chan_cben) {
		advance_chan( mvars );
		do_sync_chans( mvars );
	}
}

static void
store_connected( int sts, void *aux )
{
	MVARS(aux)

	switch (sts) {
	case DRV_CANCELED:
		return;
	case DRV_OK:
		mvars->state[t] = ST_CONNECTED;
		if (check_cancel( mvars ))
			break;
		if (!mvars->chanptr->boxlist && mvars->chan->patterns) {
			int cflags = 0;
			for (string_list_t *cpat = mvars->chan->patterns; cpat; cpat = cpat->next) {
				const char *pat = cpat->string;
				if (*pat != '!') {
					char buf[8];
					int bufl = snprintf( buf, sizeof(buf), "%s%s", nz( mvars->chan->boxes[t], "" ), pat );
					int flags = 0;
					// Partial matches like "INB*" or even "*" are not considered,
					// except implicity when the INBOX lives under Path.
					if (starts_with( buf, bufl, "INBOX", 5 )) {
						char c = buf[5];
						if (!c) {
							// User really wants the INBOX.
							flags |= LIST_INBOX;
						} else if (c == '/') {
							// Flattened sub-folders of INBOX actually end up in Path.
							if (mvars->ctx[t]->conf->flat_delim[0])
								flags |= LIST_PATH;
							else
								flags |= LIST_INBOX;
						} else if (c == '*' || c == '%') {
							// It can be both INBOX and Path, but don't require Path to be configured.
							flags |= LIST_INBOX | LIST_PATH_MAYBE;
						} else {
							// It's definitely not the INBOX.
							flags |= LIST_PATH;
						}
					} else {
						flags |= LIST_PATH;
					}
					debug( "pattern '%s' (effective '%s'): %sPath, %sINBOX\n",
					       pat, buf, (flags & LIST_PATH) ? "" : "no ",  (flags & LIST_INBOX) ? "" : "no ");
					cflags |= flags;
				}
			}
			mvars->drv[t]->list_store( mvars->ctx[t], cflags, store_listed, AUX );
			return;
		}
		sync_opened( mvars, t );
		return;
	default:
		mvars->cvars->ret = 1;
		break;
	}
	finalize_sync( mvars );
}

static void
store_listed( int sts, string_list_t *boxes, void *aux )
{
	MVARS(aux)
	int fail = 0;

	switch (sts) {
	case DRV_CANCELED:
		return;
	case DRV_OK:
		if (check_cancel( mvars ))
			break;
		for (string_list_t *box = boxes; box; box = box->next) {
			if (mvars->ctx[t]->conf->flat_delim[0]) {
				string_list_t *nbox;
				if (map_name( box->string, (char **)&nbox, offsetof(string_list_t, string), mvars->ctx[t]->conf->flat_delim, "/" ) < 0) {
					error( "Error: flattened mailbox name '%s' contains canonical hierarchy delimiter\n", box->string );
					fail = 1;
				} else {
					nbox->next = mvars->boxes[t];
					mvars->boxes[t] = nbox;
				}
			} else {
				add_string_list( &mvars->boxes[t], box->string );
			}
		}
		if (fail) {
			mvars->cvars->ret = 1;
			break;
		}
		if (mvars->ctx[t]->conf->map_inbox) {
			debug( "adding mapped inbox to %s store: %s\n", str_fn[t], mvars->ctx[t]->conf->map_inbox );
			add_string_list( &mvars->boxes[t], mvars->ctx[t]->conf->map_inbox );
		}
		sync_opened( mvars, t );
		return;
	default:
		mvars->cvars->ret = 1;
		break;
	}
	finalize_sync( mvars );
}

static void
sync_opened( main_vars_t *mvars, int t )
{
	mvars->state[t] = ST_OPEN;
	if (mvars->state[t^1] != ST_OPEN)
		return;

	if (!mvars->chanptr->boxlist && mvars->chan->patterns) {
		mvars->chanptr->boxlist = 2;
		char **boxes[2];
		boxes[F] = filter_boxes( mvars->boxes[F], mvars->chan->boxes[F], mvars->chan->patterns );
		boxes[N] = filter_boxes( mvars->boxes[N], mvars->chan->boxes[N], mvars->chan->patterns );
		box_ent_t **mboxapp = &mvars->chanptr->boxes;
		for (int mb = 0, sb = 0; ; ) {
			char *fname = boxes[F] ? boxes[F][mb] : NULL;
			char *nname = boxes[N] ? boxes[N][sb] : NULL;
			if (!fname && !nname)
				break;
			box_ent_t *mbox = nfmalloc( sizeof(*mbox) );
			int cmp;
			if (!(cmp = !fname - !nname) && !(cmp = cmp_box_names( &fname, &nname ))) {
				mbox->name = fname;
				free( nname );
				mbox->present[F] = mbox->present[N] = BOX_PRESENT;
				mb++;
				sb++;
			} else if (cmp < 0) {
				mbox->name = fname;
				mbox->present[F] = BOX_PRESENT;
				mbox->present[N] = (!mb && !strcmp( mbox->name, "INBOX" )) ? BOX_PRESENT : BOX_ABSENT;
				mb++;
			} else {
				mbox->name = nname;
				mbox->present[F] = (!sb && !strcmp( mbox->name, "INBOX" )) ? BOX_PRESENT : BOX_ABSENT;
				mbox->present[N] = BOX_PRESENT;
				sb++;
			}
			mbox->next = NULL;
			*mboxapp = mbox;
			mboxapp = &mbox->next;
			boxes_total++;
		}
		free( boxes[F] );
		free( boxes[N] );
		if (!mvars->cvars->list)
			stats();
	}
	mvars->boxptr = mvars->chanptr->boxes;

	if (mvars->cvars->list && chans_total > 1)
		printf( "%s:\n", mvars->chan->name );
	mvars->box_done = 0;
	do_sync_boxes( mvars );
}

static void
do_sync_boxes( main_vars_t *mvars )
{
	mvars->box_cben = 0;
	for (;;) {
		if (mvars->chanptr->boxlist) {
			box_ent_t *mbox = mvars->boxptr;
			if (!mbox)
				break;
			mvars->boxptr = mbox->next;
			mvars->box_done = 0;
			if (mvars->chan->boxes[F] || mvars->chan->boxes[N]) {
				const char *fpfx = nz( mvars->chan->boxes[F], "" );
				const char *npfx = nz( mvars->chan->boxes[N], "" );
				if (mvars->cvars->list) {
					printf( "%s%s <=> %s%s\n", fpfx, mbox->name, npfx, mbox->name );
					continue;
				}
				nfasprintf( &mvars->names[F], "%s%s", fpfx, mbox->name );
				nfasprintf( &mvars->names[N], "%s%s", npfx, mbox->name );
				sync_boxes( mvars->ctx, (const char * const *)mvars->names, mbox->present, mvars->chan, done_sync_dyn, mvars );
			} else {
				if (mvars->cvars->list) {
					puts( mbox->name );
					continue;
				}
				mvars->names[F] = mvars->names[N] = mbox->name;
				sync_boxes( mvars->ctx, (const char * const *)mvars->names, mbox->present, mvars->chan, done_sync, mvars );
			}
		} else {
			if (mvars->cvars->list) {
				printf( "%s <=> %s\n", nz( mvars->chan->boxes[F], "INBOX" ), nz( mvars->chan->boxes[N], "INBOX" ) );
				break;
			}
			if (mvars->box_done)
				break;
			int present[] = { BOX_POSSIBLE, BOX_POSSIBLE };
			sync_boxes( mvars->ctx, mvars->chan->boxes, present, mvars->chan, done_sync, mvars );
		}
		if (!mvars->box_done) {
			mvars->box_cben = 1;
			return;
		}
	}
	finalize_sync( mvars );
}

static void
done_sync_dyn( int sts, void *aux )
{
	main_vars_t *mvars = (main_vars_t *)aux;

	free( mvars->names[F] );
	free( mvars->names[N] );
	done_sync( sts, aux );
}

static void
done_sync( int sts, void *aux )
{
	main_vars_t *mvars = (main_vars_t *)aux;

	boxes_done++;
	stats();
	if (sts) {
		mvars->cvars->ret = 1;
		if (sts & (SYNC_BAD(F) | SYNC_BAD(N))) {
			if (sts & SYNC_BAD(F))
				mvars->state[F] = ST_CLOSED;
			if (sts & SYNC_BAD(N))
				mvars->state[N] = ST_CLOSED;
		}
	}
	mvars->box_done = 1;
	if (mvars->box_cben)
		do_sync_boxes( mvars );
}

static void sync_finalized( void *aux );

static void
finalize_sync( main_vars_t *mvars )
{
	if (mvars->chanptr->boxlist) {
		box_ent_t *mbox, *nmbox;
		for (nmbox = mvars->chanptr->boxes; (mbox = nmbox); ) {
			nmbox = mbox->next;
			free( mbox->name );
			free( mbox );
		}
		mvars->chanptr->boxes = NULL;
		mvars->chanptr->boxlist = 0;
	}

	mvars->fnlz_cben = 0;
	for (int t = 0; t < 2; t++) {
		free_string_list( mvars->boxes[t] );
		mvars->boxes[t] = NULL;

		if (mvars->state[t] == ST_FRESH || mvars->state[t] == ST_OPEN) {
			mvars->drv[t]->free_store( mvars->ctx[t] );
			mvars->state[t] = ST_CLOSED;
		} else if (mvars->state[t] == ST_CONNECTED) {
			mvars->state[t] = ST_CANCELING;
			mvars->drv[t]->cancel_cmds( mvars->ctx[t], sync_finalized, AUX );
		}
	}
	if (mvars->state[F] != ST_CLOSED || mvars->state[N] != ST_CLOSED) {
		mvars->fnlz_cben = 1;
		return;
	}
	sync_next_chan( mvars );
}

static void
sync_finalized( void *aux )
{
	MVARS(aux)

	mvars->drv[t]->free_store( mvars->ctx[t] );
	mvars->state[t] = ST_CLOSED;
	if (mvars->state[t^1] != ST_CLOSED)
		return;

	if (mvars->fnlz_cben)
		sync_next_chan( mvars );
}
