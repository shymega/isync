// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#include "sync_p.h"
#include "sync_c_enum.h"

channel_conf_t global_conf;
channel_conf_t *channels;
group_conf_t *groups;

uint BufferLimit = 10 * 1024 * 1024;

int new_total[2], new_done[2];
int flags_total[2], flags_done[2];
int trash_total[2], trash_done[2];
int expunge_total[2], expunge_done[2];

static void sync_ref( sync_vars_t *svars ) { ++svars->ref_count; }
static void sync_deref( sync_vars_t *svars );
static int check_cancel( sync_vars_t *svars );

#define AUX &svars->t[t]
#define INV_AUX &svars->t[t^1]
#define DECL_SVARS \
	int t; \
	sync_vars_t *svars
#define INIT_SVARS(aux) \
	t = *(int *)aux; \
	svars = (sync_vars_t *)(((char *)(&((int *)aux)[-t])) - offsetof(sync_vars_t, t))
#define DECL_INIT_SVARS(aux) \
	int t = *(int *)aux; \
	sync_vars_t *svars = (sync_vars_t *)(((char *)(&((int *)aux)[-t])) - offsetof(sync_vars_t, t))

/* operation dependencies:
   select(x): -
   load(x): select(x)
   new(F), new(N), flags(F), flags(N): load(F) & load(N)
   find_new(x): new(x)
   trash(x): flags(x)
   close(x): trash(x) & flags(!x) & find_new(x) & new(!x) // with expunge
   cleanup: close(F) & close(N)
*/

BIT_ENUM(
	ST_PRESENT,
	ST_CONFIRMED,
	ST_SELECTED,
	ST_FIND_OLD,
	ST_LOADED,
	ST_SENT_FLAGS,
	ST_SENDING_NEW,
	ST_SENT_NEW,
	ST_FIND_NEW,
	ST_FOUND_NEW,
	ST_SENT_TRASH,
	ST_TRASH_BAD,
	ST_CLOSING,
	ST_CLOSED,
	ST_SENT_CANCEL,
	ST_CANCELED,
)

static uchar
sanitize_flags( uchar tflags, sync_vars_t *svars, int t )
{
	if (Verbosity >= TERSE) {
		// We complain only once per flag per store - even though _theoretically_
		// each mailbox can support different flags according to the IMAP spec.
		uchar bflags = tflags & ~(svars->good_flags[t] | svars->bad_flags[t]);
		if (bflags) {
			notice( "Notice: %s store does not support flag(s) '%s'; not propagating.\n",
			        str_fn[t], fmt_flags( bflags ).str );
			svars->bad_flags[t] |= bflags;
		}
	}
	return tflags & svars->good_flags[t];
}


enum {
	COPY_OK,
	COPY_GONE,
	COPY_NOGOOD,
	COPY_CANCELED,
	COPY_FAIL,
};

static void msg_fetched( int sts, void *aux );

static void
copy_msg( copy_vars_t *vars )
{
	DECL_INIT_SVARS(vars->aux);

	t ^= 1;
	vars->data.flags = vars->msg->flags;
	vars->data.date = svars->chan->use_internal_date ? -1 : 0;
	svars->drv[t]->fetch_msg( svars->ctx[t], vars->msg, &vars->data, vars->minimal, msg_fetched, vars );
}

static void msg_stored( int sts, uint uid, void *aux );

static void
msg_fetched( int sts, void *aux )
{
	copy_vars_t *vars = (copy_vars_t *)aux;
	sync_rec_t *srec = vars->srec;
	DECL_SVARS;
	int scr, tcr;

	switch (sts) {
	case DRV_OK:
		INIT_SVARS(vars->aux);
		if (check_cancel( svars )) {
			free( vars->data.data );
			vars->cb( COPY_CANCELED, 0, vars );
			return;
		}

		if (srec && (srec->status & S_UPGRADE)) {
			vars->data.flags = (srec->pflags | srec->aflags[t]) & ~srec->dflags[t];
			if (srec->aflags[t] || srec->dflags[t]) {
				JLOG( "$ %u %u %u %u", (srec->uid[F], srec->uid[N], srec->aflags[t], srec->dflags[t]),
				      "%sing upgrade with flags: +%s -%s",
				      (str_hl[t], fmt_flags( srec->aflags[t] ).str, fmt_flags( srec->dflags[t] ).str) );
			}
		} else {
			vars->data.flags = sanitize_flags( vars->data.flags, svars, t );
			if (srec) {
				if (srec->status & S_DUMMY(t))
					vars->data.flags &= ~F_FLAGGED;
				if (vars->data.flags) {
					srec->pflags = vars->data.flags;
					JLOG( "%% %u %u %u", (srec->uid[F], srec->uid[N], srec->pflags),
					      "%sing with flags %s", (str_hl[t], fmt_lone_flags( srec->pflags ).str) );
				}
			}
		}

		scr = svars->can_crlf[t^1];
		tcr = svars->can_crlf[t];
		if (srec || scr != tcr) {
			const char *err;
			if ((err = copy_msg_convert( scr, tcr, vars ))) {
				error( "Error: message %u from %s %s; skipping.\n", vars->msg->uid, str_fn[t^1], err );
				svars->ret |= SYNC_FAIL;
				vars->cb( COPY_NOGOOD, 0, vars );
				return;
			}
		}

		svars->drv[t]->store_msg( svars->ctx[t], &vars->data, !srec, msg_stored, vars );
		break;
	case DRV_CANCELED:
		vars->cb( COPY_CANCELED, 0, vars );
		break;
	case DRV_MSG_BAD:
		if (vars->msg->status & M_DEAD) {
			// The message was expunged under our feet; this is no error.
			vars->cb( COPY_GONE, 0, vars );
		} else {
			INIT_SVARS(vars->aux);
			// Driver already reported error.
			svars->ret |= SYNC_FAIL;
			vars->cb( COPY_NOGOOD, 0, vars );
		}
		break;
	default:  // DRV_BOX_BAD
		vars->cb( COPY_FAIL, 0, vars );
		break;
	}
}

static void
msg_stored( int sts, uint uid, void *aux )
{
	copy_vars_t *vars = (copy_vars_t *)aux;
	DECL_SVARS;

	switch (sts) {
	case DRV_OK:
		vars->cb( COPY_OK, uid, vars );
		break;
	case DRV_CANCELED:
		vars->cb( COPY_CANCELED, 0, vars );
		break;
	case DRV_MSG_BAD:
		INIT_SVARS(vars->aux);
		// Driver already reported error, but we still need to report the source.
		error( "Error: %s refuses to store message %u from %s.\n",
		       str_fn[t], vars->msg->uid, str_fn[t^1] );
		svars->ret |= SYNC_FAIL;
		vars->cb( COPY_NOGOOD, 0, vars );
		break;
	default:  // DRV_BOX_BAD
		vars->cb( COPY_FAIL, 0, vars );
		break;
	}
}


static void sync_bail( sync_vars_t *svars );
static void sync_bail2( sync_vars_t *svars );
static void sync_bail3( sync_vars_t *svars );
static void cancel_done( void *aux );

static void
cancel_sync( sync_vars_t *svars )
{
	int state1 = svars->state[1];
	for (int t = 0; ; t++) {
		if (svars->ret & SYNC_BAD(t)) {
			cancel_done( AUX );
		} else if (!(svars->state[t] & ST_SENT_CANCEL)) {
			/* ignore subsequent failures from in-flight commands */
			svars->state[t] |= ST_SENT_CANCEL;
			svars->drv[t]->cancel_cmds( svars->ctx[t], cancel_done, AUX );
		}
		if (t || (state1 & ST_CANCELED))
			break;
	}
}

static void
cancel_done( void *aux )
{
	DECL_INIT_SVARS(aux);

	svars->state[t] |= ST_CANCELED;
	if (svars->state[t^1] & ST_CANCELED) {
		if (svars->nfp) {
			Fclose( svars->nfp, 0 );
			Fclose( svars->jfp, 0 );
		}
		sync_bail( svars );
	}
}

static void
store_bad( void *aux )
{
	DECL_INIT_SVARS(aux);

	svars->drv[t]->cancel_store( svars->ctx[t] );
	svars->ret |= SYNC_BAD(t);
	cancel_sync( svars );
}

static int
check_cancel( sync_vars_t *svars )
{
	return (svars->state[F] | svars->state[N]) & (ST_SENT_CANCEL | ST_CANCELED);
}

static int
check_ret( int sts, void *aux )
{
	if (sts == DRV_CANCELED)
		return 1;
	DECL_INIT_SVARS(aux);
	if (sts == DRV_BOX_BAD) {
		svars->ret |= SYNC_FAIL;
		cancel_sync( svars );
		return 1;
	}
	return 0;
}

#define SVARS_CHECK_RET \
	if (check_ret( sts, aux )) \
		return; \
	DECL_INIT_SVARS(aux)

// After drv->cancel_cmds() on our side, commands may still complete
// successfully, while the other side is already dead.
#define SVARS_CHECK_RET_CANCEL \
	SVARS_CHECK_RET; \
	if (check_cancel( svars )) \
		return

#define SVARS_CHECK_RET_VARS(type) \
	type *vars = (type *)aux; \
	if (check_ret( sts, vars->aux )) { \
		free( vars ); \
		return; \
	} \
	DECL_INIT_SVARS(vars->aux)

static void
message_expunged( message_t *msg, void *aux )
{
	DECL_INIT_SVARS(aux);
	(void)svars;

	if (msg->srec) {
		msg->srec->status |= S_GONE(t);
		msg->srec->msg[t] = NULL;
		msg->srec = NULL;
	}
	if (msg->status & M_EXPUNGE) {
		expunge_done[t]++;
		stats();
	}
}

static void box_confirmed( int sts, uint uidvalidity, void *aux );
static void box_confirmed2( sync_vars_t *svars, int t );
static void box_deleted( int sts, void *aux );
static void box_created( int sts, void *aux );
static void box_opened( int sts, uint uidvalidity, void *aux );
static void box_opened2( sync_vars_t *svars, int t );
static void load_box( sync_vars_t *svars, int t, uint minwuid, uint_array_t mexcs );

void
sync_boxes( store_t *ctx[], const char * const names[], int present[], channel_conf_t *chan,
            void (*cb)( int sts, void *aux ), void *aux )
{
	sync_vars_t *svars;
	int t;

	svars = nfzalloc( sizeof(*svars) );
	svars->t[1] = 1;
	svars->ref_count = 1;
	svars->cb = cb;
	svars->aux = aux;
	svars->ctx[0] = ctx[0];
	svars->ctx[1] = ctx[1];
	svars->chan = chan;
	svars->lfd = -1;
	svars->uidval[0] = svars->uidval[1] = UIDVAL_BAD;
	svars->srecadd = &svars->srecs;

	for (t = 0; t < 2; t++) {
		svars->orig_name[t] =
			(!names[t] || (ctx[t]->conf->map_inbox && !strcmp( ctx[t]->conf->map_inbox, names[t] ))) ?
				"INBOX" : names[t];
		if (!ctx[t]->conf->flat_delim[0]) {
			svars->box_name[t] = nfstrdup( svars->orig_name[t] );
		} else if (map_name( svars->orig_name[t], -1, &svars->box_name[t], 0, "/", ctx[t]->conf->flat_delim ) < 0) {
			error( "Error: canonical mailbox name '%s' contains flattened hierarchy delimiter\n", svars->orig_name[t] );
		  bail3:
			svars->ret = SYNC_FAIL;
			sync_bail3( svars );
			return;
		}
		svars->drv[t] = ctx[t]->driver;
		svars->drv[t]->set_callbacks( ctx[t], message_expunged, store_bad, AUX );
		svars->can_crlf[t] = (svars->drv[t]->get_caps( svars->ctx[t] ) / DRV_CRLF) & 1;
	}
	/* Both boxes must be fully set up at this point, so that error exit paths
	 * don't run into uninitialized variables. */
	for (t = 0; t < 2; t++) {
		switch (svars->drv[t]->select_box( ctx[t], svars->box_name[t] )) {
		case DRV_STORE_BAD:
			store_bad( AUX );
			return;
		case DRV_BOX_BAD:
			goto bail3;
		}
	}

	if (!prepare_state( svars )) {
		svars->ret = SYNC_FAIL;
		sync_bail2( svars );
		return;
	}
	if (!load_state( svars )) {
		svars->ret = SYNC_FAIL;
		sync_bail( svars );
		return;
	}

	sync_ref( svars );
	for (t = 0; ; t++) {
		info( "Opening %s box %s...\n", str_fn[t], svars->orig_name[t] );
		if (present[t] == BOX_ABSENT)
			box_confirmed2( svars, t );
		else
			svars->drv[t]->open_box( ctx[t], box_confirmed, AUX );
		if (t || check_cancel( svars ))
			break;
	}
	sync_deref( svars );
}

static void
box_confirmed( int sts, uint uidvalidity, void *aux )
{
	if (sts == DRV_CANCELED)
		return;
	DECL_INIT_SVARS(aux);
	if (check_cancel( svars ))
		return;

	if (sts == DRV_OK) {
		svars->state[t] |= ST_PRESENT;
		svars->newuidval[t] = uidvalidity;
	}
	box_confirmed2( svars, t );
}

static void
box_confirmed2( sync_vars_t *svars, int t )
{
	svars->state[t] |= ST_CONFIRMED;
	if (!(svars->state[t^1] & ST_CONFIRMED))
		return;

	sync_ref( svars );
	for (t = 0; ; t++) {
		if (!(svars->state[t] & ST_PRESENT)) {
			if (!(svars->state[t^1] & ST_PRESENT)) {
				if (!svars->existing) {
					error( "Error: channel %s: both far side %s and near side %s cannot be opened.\n",
					       svars->chan->name, svars->orig_name[F], svars->orig_name[N] );
				  bail:
					svars->ret = SYNC_FAIL;
				} else {
					/* This can legitimately happen if a deletion propagation was interrupted.
					 * We have no place to record this transaction, so we just assume it.
					 * Of course this bears the danger of clearing the state if both mailboxes
					 * temorarily cannot be opened for some weird reason (while the stores can). */
					delete_state( svars );
				}
			  done:
				sync_bail( svars );
				break;
			}
			if (svars->existing) {
				if (!(svars->chan->ops[t^1] & OP_REMOVE)) {
					error( "Error: channel %s: %s box %s cannot be opened.\n",
					       svars->chan->name, str_fn[t], svars->orig_name[t] );
					goto bail;
				}
				if (svars->drv[t^1]->confirm_box_empty( svars->ctx[t^1] ) != DRV_OK) {
					warn( "Warning: channel %s: %s box %s cannot be opened and %s box %s is not empty.\n",
					      svars->chan->name, str_fn[t], svars->orig_name[t], str_fn[t^1], svars->orig_name[t^1] );
					goto done;
				}
				info( "Deleting %s box %s...\n", str_fn[t^1], svars->orig_name[t^1] );
				svars->drv[t^1]->delete_box( svars->ctx[t^1], box_deleted, INV_AUX );
			} else {
				if (!(svars->chan->ops[t] & OP_CREATE)) {
					box_opened( DRV_BOX_BAD, UIDVAL_BAD, AUX );
				} else {
					info( "Creating %s box %s...\n", str_fn[t], svars->orig_name[t] );
					svars->drv[t]->create_box( svars->ctx[t], box_created, AUX );
				}
			}
		} else {
			box_opened2( svars, t );
		}
		if (t || check_cancel( svars ))
			break;
	}
	sync_deref( svars );
}

static void
box_deleted( int sts, void *aux )
{
	SVARS_CHECK_RET_CANCEL;
	delete_state( svars );
	svars->drv[t]->finish_delete_box( svars->ctx[t] );
	sync_bail( svars );
}

static void
box_created( int sts, void *aux )
{
	SVARS_CHECK_RET_CANCEL;
	svars->drv[t]->open_box( svars->ctx[t], box_opened, AUX );
}

static void
box_opened( int sts, uint uidvalidity, void *aux )
{
	if (sts == DRV_CANCELED)
		return;
	DECL_INIT_SVARS(aux);
	if (check_cancel( svars ))
		return;

	if (sts == DRV_BOX_BAD) {
		error( "Error: channel %s: %s box %s cannot be opened.\n",
		       svars->chan->name, str_fn[t], svars->orig_name[t] );
		svars->ret = SYNC_FAIL;
		sync_bail( svars );
	} else {
		svars->newuidval[t] = uidvalidity;
		box_opened2( svars, t );
	}
}

static void
box_opened2( sync_vars_t *svars, int t )
{
	store_t *ctx[2];
	channel_conf_t *chan;
	sync_rec_t *srec;
	uint_array_alloc_t mexcs;
	uint opts[2], fails, minwuid;

	svars->state[t] |= ST_SELECTED;
	if (!(svars->state[t^1] & ST_SELECTED))
		return;
	ctx[0] = svars->ctx[0];
	ctx[1] = svars->ctx[1];
	chan = svars->chan;

	fails = 0;
	for (t = 0; t < 2; t++)
		if (svars->uidval[t] != UIDVAL_BAD && svars->uidval[t] != svars->newuidval[t])
			fails++;
	// If only one side changed UIDVALIDITY, we will try to re-approve it further down.
	if (fails == 2) {
		error( "Error: channel %s: UIDVALIDITY of both far side %s and near side %s changed.\n",
		       svars->chan->name, svars->orig_name[F], svars->orig_name[N]);
	  bail:
		svars->ret = SYNC_FAIL;
		sync_bail( svars );
		return;
	}

	if (!lock_state( svars ))
		goto bail;

	int any_dummies[2] = { 0, 0 };
	int any_purges[2] = { 0, 0 };
	int any_upgrades[2] = { 0, 0 };
	int any_old[2] = { 0, 0 };
	int any_new[2] = { 0, 0 };
	int any_tuids[2] = { 0, 0 };
	if (svars->replayed || ((chan->ops[F] | chan->ops[N]) & OP_UPGRADE)) {
		for (srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			if (srec->status & S_DUMMY(F))
				any_dummies[F]++;
			else if (srec->status & S_DUMMY(N))
				any_dummies[N]++;
			else if (srec->status & S_SKIPPED)
				any_dummies[!srec->uid[F] ? F : N]++;
			if (!svars->replayed)
				continue;
			if ((shifted_bit(srec->status, S_EXPIRE, S_EXPIRED) ^ srec->status) & S_EXPIRED)
				svars->any_expiring = 1;
			if (srec->status & S_PURGE) {
				any_purges[srec->uid[F] ? F : N]++;
			} else if (srec->status & S_PENDING) {
				t = !srec->uid[F] ? F : N;
				if (srec->status & S_UPGRADE)
					any_upgrades[t]++;
				else if (srec->uid[t^1] <= svars->maxuid[t^1])
					any_old[t]++;
				else
					any_new[t]++;
				if (srec->tuid[0])
					any_tuids[t]++;
			}
		}
	}

	opts[F] = opts[N] = 0;
	if (fails)
		opts[F] = opts[N] = OPEN_PAIRED | OPEN_PAIRED_IDS;
	for (t = 0; t < 2; t++) {
		if (any_purges[t]) {
			debug( "resuming %d %s purge(s)\n", any_purges[t], str_fn[t] );
			opts[t] |= OPEN_SETFLAGS;
		}
		if (any_tuids[t]) {
			debug( "finding %d %sed message(s)\n", any_tuids[t], str_hl[t] );
			opts[t] |= OPEN_NEW | OPEN_FIND;
			svars->state[t] |= ST_FIND_OLD;
		}
		if (chan->ops[t] & (OP_GONE | OP_FLAGS)) {
			opts[t] |= OPEN_SETFLAGS;
			opts[t^1] |= OPEN_PAIRED;
			if (chan->ops[t] & OP_FLAGS)
				opts[t^1] |= OPEN_FLAGS;
		}
		if (!any_dummies[t] && (chan->ops[t] & OP_UPGRADE)) {
			chan->ops[t] &= ~OP_UPGRADE;
			debug( "no %s dummies; masking Upgrade\n", str_fn[t] );
		}
		if ((chan->ops[t] & (OP_OLD | OP_NEW | OP_UPGRADE)) || any_old[t] || any_new[t] || any_upgrades[t]) {
			opts[t] |= OPEN_APPEND;
			if ((chan->ops[t] & OP_OLD) || any_old[t]) {
				debug( "resuming %s of %d old message(s)\n", str_hl[t], any_old[t] );
				opts[t^1] |= OPEN_OLD;
				if (chan->stores[t]->max_size != UINT_MAX)
					opts[t^1] |= OPEN_OLD_SIZE;
			}
			if ((chan->ops[t] & OP_NEW) || any_new[t]) {
				debug( "resuming %s of %d new message(s)\n", str_hl[t], any_new[t] );
				opts[t^1] |= OPEN_NEW;
				if (chan->stores[t]->max_size != UINT_MAX)
					opts[t^1] |= OPEN_NEW_SIZE;
			}
			if ((chan->ops[t] & OP_UPGRADE) || any_upgrades[t]) {
				debug( "resuming %s of %d upgrade(s)\n", str_hl[t], any_upgrades[t] );
				if (chan->ops[t] & OP_UPGRADE)
					opts[t] |= OPEN_PAIRED | OPEN_FLAGS | OPEN_SETFLAGS;
				opts[t^1] |= OPEN_PAIRED;
			}
			if ((chan->ops[t] | chan->ops[t^1]) & OP_EXPUNGE)  // Don't propagate doomed msgs
				opts[t^1] |= OPEN_FLAGS;
		}
		if (chan->ops[t] & (OP_EXPUNGE | OP_EXPUNGE_SOLO)) {
			opts[t] |= OPEN_EXPUNGE;
			if (chan->ops[t] & OP_EXPUNGE_SOLO) {
				opts[t] |= OPEN_OLD | OPEN_NEW | OPEN_FLAGS | OPEN_UID_EXPUNGE;
				opts[t^1] |= OPEN_OLD;
			} else if (chan->stores[t]->trash) {
				if (!chan->stores[t]->trash_only_new)
					opts[t] |= OPEN_OLD;
				opts[t] |= OPEN_NEW | OPEN_FLAGS | OPEN_UID_EXPUNGE;
			} else if (chan->stores[t^1]->trash && chan->stores[t^1]->trash_remote_new) {
				opts[t] |= OPEN_NEW | OPEN_FLAGS | OPEN_UID_EXPUNGE;
			}
		}
	}
	// While only new messages can cause expiration due to displacement,
	// updating flags can cause expiration of already overdue messages.
	// The latter would also apply when the expired box is the source,
	// but it's more natural to treat it as read-only in that case.
	// OP_UPGRADE makes sense only for legacy S_SKIPPED entries.
	int xt = chan->expire_side;
	if ((chan->ops[xt] & (OP_OLD | OP_NEW | OP_UPGRADE | OP_FLAGS)) && chan->max_messages)
		svars->any_expiring = 1;
	if (svars->any_expiring) {
		opts[xt] |= OPEN_PAIRED | OPEN_FLAGS;
		if (any_dummies[xt])
			opts[xt^1] |= OPEN_PAIRED | OPEN_FLAGS;
		else if (chan->ops[xt] & (OP_OLD | OP_NEW | OP_UPGRADE))
			opts[xt^1] |= OPEN_FLAGS;
	}
	for (t = 0; t < 2; t++) {
		svars->opts[t] = svars->drv[t]->prepare_load_box( ctx[t], opts[t] );
		if (opts[t] & ~svars->opts[t] & OPEN_UID_EXPUNGE) {
			if (chan->ops[t] & OP_EXPUNGE_SOLO) {
				error( "Error: Store %s does not support ExpungeSolo.\n",
				       svars->chan->stores[t]->name );
				goto bail;
			}
			if (!ctx[t]->racy_trash) {
				ctx[t]->racy_trash = 1;
				notice( "Notice: Trashing in Store %s is prone to race conditions.\n",
				        svars->chan->stores[t]->name );
			}
		}
	}

	ARRAY_INIT( &mexcs );
	if ((svars->opts[xt^1] & OPEN_PAIRED) && !(svars->opts[xt^1] & OPEN_OLD) && chan->max_messages) {
		/* When messages have been expired on one side, the other side's fetch is split into
		 * two ranges: The bulk fetch which corresponds with the most recent messages, and an
		 * exception list of messages which would have been expired if they weren't important. */
		debug( "preparing %s selection - max expired %s uid is %u\n",
		       str_fn[xt^1], str_fn[xt^1], svars->maxxfuid );
		/* First, find out the lower bound for the bulk fetch. */
		minwuid = svars->maxxfuid + 1;
		/* Next, calculate the exception fetch. */
		for (srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			if (!srec->uid[xt^1])
				continue;  // No message; other state is irrelevant
			if (srec->uid[xt^1] >= minwuid)
				continue;  // Message is in non-expired range
			if ((svars->opts[xt^1] & OPEN_NEW) && srec->uid[xt^1] > svars->maxuid[xt^1])
				continue;  // Message is in expired range, but new range overlaps that
			if (!srec->uid[xt] && !(srec->status & S_PENDING))
				continue;  // Only actually paired up messages matter
			// The pair is alive, but outside the bulk range
			*uint_array_append( &mexcs ) = srec->uid[xt^1];
		}
		sort_uint_array( mexcs.array );
	} else {
		minwuid = 1;
	}
	sync_ref( svars );
	load_box( svars, xt^1, minwuid, mexcs.array );
	if (!check_cancel( svars ))
		load_box( svars, xt, 1, (uint_array_t){ NULL, 0 } );
	sync_deref( svars );
}

static uint
get_seenuid( sync_vars_t *svars, int t )
{
	uint seenuid = 0;
	for (sync_rec_t *srec = svars->srecs; srec; srec = srec->next)
		if (!(srec->status & S_DEAD) && seenuid < srec->uid[t])
			seenuid = srec->uid[t];
	return seenuid;
}

static void box_loaded( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux );

static void
load_box( sync_vars_t *svars, int t, uint minwuid, uint_array_t mexcs )
{
	uint maxwuid = 0, pairuid = UINT_MAX;

	if (svars->opts[t] & OPEN_NEW) {
		if (svars->opts[t] & OPEN_OLD) {
			svars->opts[t] |= OPEN_PAIRED;
			minwuid = 1;
		} else if (!(svars->opts[t] & OPEN_PAIRED) || (minwuid > svars->maxuid[t] + 1)) {
			minwuid = svars->maxuid[t] + 1;
		}
		maxwuid = UINT_MAX;
		if (svars->opts[t] & OPEN_PAIRED_IDS)  // Implies OPEN_PAIRED
			pairuid = get_seenuid( svars, t );
	} else if (svars->opts[t] & (OPEN_PAIRED | OPEN_OLD)) {
		uint seenuid = get_seenuid( svars, t );
		if (svars->opts[t] & OPEN_OLD) {
			minwuid = 1;
			maxwuid = svars->maxuid[t];
			if (maxwuid < seenuid) {
				if (svars->opts[t] & OPEN_PAIRED)
					maxwuid = seenuid;
			} else {
				svars->opts[t] |= OPEN_PAIRED;
			}
		} else {  // OPEN_PAIRED
			maxwuid = seenuid;
		}
	} else {
		minwuid = UINT_MAX;
	}
	info( "Loading %s box...\n", str_fn[t] );
	svars->drv[t]->load_box( svars->ctx[t], minwuid, maxwuid, svars->finduid[t], pairuid, svars->maxuid[t], mexcs, box_loaded, AUX );
}

typedef struct {
	sync_rec_t *srec;
	uchar flags;
} alive_srec_t;

static int
cmp_srec_far( const void *a, const void *b )
{
	uint au = (*(const alive_srec_t *)a).srec->uid[F];
	uint bu = (*(const alive_srec_t *)b).srec->uid[F];
	assert( au && bu );
	assert( au != bu );
	return au > bu ? 1 : -1;  // Can't subtract, the result might not fit into signed int.
}

static int
cmp_srec_near( const void *a, const void *b )
{
	uint au = (*(const alive_srec_t *)a).srec->uid[N];
	uint bu = (*(const alive_srec_t *)b).srec->uid[N];
	assert( au && bu );
	assert( au != bu );
	return au > bu ? 1 : -1;  // Can't subtract, the result might not fit into signed int.
}

typedef struct {
	void *aux;
	sync_rec_t *srec;
	int aflags, dflags;
} flag_vars_t;

static void flags_set( int sts, void *aux );
static void flags_set_p2( sync_vars_t *svars, sync_rec_t *srec, int t );
static void msgs_flags_set( sync_vars_t *svars, int t );
static void msg_copied( int sts, uint uid, copy_vars_t *vars );
static void msgs_copied( sync_vars_t *svars, int t );

static void
box_loaded( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux )
{
	sync_rec_t *srec, **srecmap;
	message_t *tmsg;
	flag_vars_t *fv;
	int no[2], del[2];
	uchar sflags, nflags, aflags, dflags;
	uint hashsz, idx;

	SVARS_CHECK_RET_CANCEL;
	svars->state[t] |= ST_LOADED;
	svars->msgs[t] = msgs;
	info( "%s: %d messages, %d recent\n", str_fn[t], total_msgs, recent_msgs );

	if (svars->state[t] & ST_FIND_OLD) {
		debug( "matching previously copied messages on %s\n", str_fn[t] );
		for (; msgs && msgs->uid < svars->finduid[t]; msgs = msgs->next) {}
		match_tuids( svars, t, msgs );
	}

	debug( "matching messages on %s against sync records\n", str_fn[t] );
	hashsz = bucketsForSize( svars->nsrecs * 3 );
	srecmap = nfzalloc( hashsz * sizeof(*srecmap) );
	for (srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		uint uid = srec->uid[t];
		if (!uid)
			continue;
		idx = (uint)(uid * 1103515245U) % hashsz;
		while (srecmap[idx])
			if (++idx == hashsz)
				idx = 0;
		srecmap[idx] = srec;
	}
	for (tmsg = svars->msgs[t]; tmsg; tmsg = tmsg->next) {
		if (tmsg->srec) /* found by TUID */
			continue;
		uint uid = tmsg->uid;
		idx = (uint)(uid * 1103515245U) % hashsz;
		while ((srec = srecmap[idx])) {
			if (srec->uid[t] == uid)
				goto found;
			if (++idx == hashsz)
				idx = 0;
		}
		continue;
	  found:
		tmsg->srec = srec;
		srec->msg[t] = tmsg;
	}
	free( srecmap );

	if (!(svars->state[t^1] & ST_LOADED))
		return;

	for (t = 0; t < 2; t++) {
		if (svars->uidval[t] != UIDVAL_BAD && svars->uidval[t] != svars->newuidval[t]) {
			// This code checks whether the messages with known UIDs are actually the
			// same messages, as recognized by their Message-IDs.
			unsigned need = 0, got = 0;
			debug( "trying to re-approve uid validity of %s\n", str_fn[t] );
			for (srec = svars->srecs; srec; srec = srec->next) {
				if (srec->status & S_DEAD)
					continue;
				need++;
				if (!srec->msg[t])
					continue;  // Message disappeared.
				// Present paired messages require re-validation.
				if (!srec->msg[t]->msgid)
					continue;  // Messages without ID are useless for re-validation.
				if (!srec->msg[t^1])
					continue;  // Partner disappeared.
				if (!srec->msg[t^1]->msgid || strcmp( srec->msg[F]->msgid, srec->msg[N]->msgid )) {
					error( "Error: channel %s, %s box %s: UIDVALIDITY genuinely changed (at UID %u).\n",
					       svars->chan->name, str_fn[t], svars->orig_name[t], srec->uid[t] );
				  uvchg:
					svars->ret |= SYNC_FAIL;
					cancel_sync( svars );
					return;
				}
				got++;
			}
			// We encountered no messages that contradict the hypothesis that the
			// UIDVALIDITY change was spurious.
			// If we got enough messages confirming the hypothesis, we just accept it.
			// If there aren't quite enough messages, we check that at least 80% of
			// those previously present are still there and confirm the hypothesis;
			// this also covers the case of a box that was already empty.
			if (got < 20 && got * 5 < need * 4) {
				// Too few confirmed messages. This is very likely in the drafts folder.
				// A proper fallback would be fetching more headers (which potentially need
				// normalization) or the message body (which should be truncated for sanity)
				// and comparing.
				error( "Error: channel %s, %s box %s: Unable to recover from UIDVALIDITY change.\n",
				       svars->chan->name, str_fn[t], svars->orig_name[t] );
				goto uvchg;
			}
			notice( "Notice: channel %s, %s box %s: Recovered from change of UIDVALIDITY.\n",
			        svars->chan->name, str_fn[t], svars->orig_name[t] );
			svars->uidval[t] = UIDVAL_BAD;
		}
	}

	if (svars->uidval[F] == UIDVAL_BAD || svars->uidval[N] == UIDVAL_BAD) {
		svars->uidval[F] = svars->newuidval[F];
		svars->uidval[N] = svars->newuidval[N];
		JLOG( "| %u %u", (svars->uidval[F], svars->uidval[N]), "new UIDVALIDITYs" );
	}

	svars->oldmaxuid[F] = svars->newmaxuid[F];
	svars->oldmaxuid[N] = svars->newmaxuid[N];

	info( "Synchronizing...\n" );
	int xt = svars->chan->expire_side;
	for (t = 0; t < 2; t++)
		svars->good_flags[t] = (uchar)svars->drv[t]->get_supported_flags( svars->ctx[t] );

	int any_new[2] = { 0, 0 };

	debug( "synchronizing old entries\n" );
	for (srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		debug( "pair (%u,%u)\n", srec->uid[F], srec->uid[N] );
		assert( !srec->tuid[0] );
		// no[] means that a message is known to be not there.
		no[F] = !srec->msg[F] && (svars->opts[F] & OPEN_PAIRED);
		no[N] = !srec->msg[N] && (svars->opts[N] & OPEN_PAIRED);
		if (no[F] && no[N]) {
			// It does not matter whether one side was already known to be missing
			// (never stored [skipped or failed] or expunged [possibly expired]) -
			// now both are missing, so the entry is superfluous.
			srec->status = S_DEAD;
			JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "both missing" );
		} else {
			// del[] means that a message becomes known to have been expunged.
			del[F] = no[F] && srec->uid[F];
			del[N] = no[N] && srec->uid[N];

			sync_rec_t *nsrec = srec;
			for (t = 0; t < 2; t++) {
				// Do this before possibly upgrading that side.
				if (srec->msg[t] && (srec->msg[t]->flags & F_DELETED))
					srec->status |= S_DEL(t);
				// Flagging the message on the target side causes an upgrade of the dummy.
				// We do this first in a separate loop, so flag propagation sees the upgraded
				// state for both sides. After a journal replay, that would be the case anyway.
				if ((svars->chan->ops[t] & OP_UPGRADE) && (srec->status & S_DUMMY(t)) && srec->uid[t^1] && srec->msg[t]) {
					sflags = srec->msg[t]->flags;
					if (sflags & F_FLAGGED) {
						sflags &= ~(F_SEEN | F_FLAGGED) | (srec->flags & F_SEEN);  // As below.
						// We save away the dummy's flags, because after an
						// interruption it may be already gone.
						srec->pflags = sflags;
						JLOG( "^ %u %u %u", (srec->uid[F], srec->uid[N], srec->pflags),
						      "upgrading %s placeholder, dummy's flags %s",
						      (str_fn[t], fmt_lone_flags( srec->pflags ).str) );
						nsrec = upgrade_srec( svars, srec, t );
					}
				}
			}
			for (t = 0; t < 2; t++) {
				if (srec->status & S_UPGRADE) {
					// Such records hold orphans by definition, so the del[] cases are irrelevant.
					if (srec->uid[t]) {
						// Direction towards the source message.
						// The placeholder was already detached, so use its saved flags instead.
						sflags = srec->pflags;
						goto doflags;
					}
					// Direction towards the copy.
					if (srec->msg[t^1]) {
						// Flag propagation along placeholder upgrades must be explicitly requested,
						// and is, at the source, handled like any other flag update.
						sflags = srec->msg[t^1]->flags;
						goto doflags;
					}
					debug( "  no %s\n", str_fn[t^1] );
				} else if (del[t]) {
					// The target was newly expunged, so there is nothing to update.
					// The deletion is propagated in the opposite iteration.
					srec->status |= S_GONE(t);
				} else if (!srec->uid[t]) {
					// The target was never stored, or was previously expunged, so there
					// is nothing to update.
					// Note: the opposite UID must be valid, as otherwise the entry would
					// have been pruned already.
				} else if (del[t^1]) {
					// The source was newly expunged, so possibly propagate the deletion.
					// The target may be in an unknown state (not fetched).
					if ((t != xt) && (srec->status & (S_EXPIRE | S_EXPIRED))) {
						/* Don't propagate deletion resulting from expiration. */
						if (~srec->status & (S_EXPIRE | S_EXPIRED)) {
							// An expiration was interrupted, but the message was expunged since.
							srec->status |= S_EXPIRE | S_EXPIRED;  // Override failed unexpiration attempts.
							JLOG( "~ %u %u %u", (srec->uid[F], srec->uid[N], srec->status), "forced expiration commit" );
						}
						JLOG( "%c %u %u 0", ("<>"[xt], srec->uid[F], srec->uid[N]),
						      "%s expired, orphaning %s", (str_fn[xt], str_fn[xt^1]) );
						srec->uid[xt] = 0;
					} else {
						if (srec->msg[t] && (srec->msg[t]->status & M_FLAGS) &&
						    // Ignore deleted flag, as that's what we'll change ourselves ...
						    (((srec->msg[t]->flags & ~F_DELETED) != (srec->flags & ~F_DELETED)) ||
						     // ... except for undeletion, as that's the opposite.
						     (!(srec->msg[t]->flags & F_DELETED) && (srec->flags & F_DELETED))))
							notice( "Notice: conflicting changes in (%u,%u)\n", srec->uid[F], srec->uid[N] );
						if (svars->chan->ops[t] & OP_GONE) {
							debug( "  %sing delete\n", str_hl[t] );
							srec->aflags[t] = F_DELETED;
							srec->status |= S_DELETE;
						} else {
							debug( "  not %sing delete\n", str_hl[t] );
						}
					}
				} else if (!srec->msg[t^1]) {
					// We have no source to work with, because it was never stored,
					// it was previously expunged, or we did not fetch it.
					debug( "  no %s\n", str_fn[t^1] );
				} else {
					// We have a source. The target may be in an unknown state.
					sflags = srec->msg[t^1]->flags;

				  doflags:
					if (svars->chan->ops[t] & OP_FLAGS) {
						sflags = sanitize_flags( sflags, svars, t );
						if ((t != xt) && (srec->status & (S_EXPIRE | S_EXPIRED))) {
							/* Don't propagate deletion resulting from expiration. */
							debug( "  near side expiring\n" );
							sflags &= ~F_DELETED;
						}
						if (srec->status & S_DUMMY(t^1)) {
							// From placeholders, don't propagate:
							// - Seen, because the real contents were obviously not seen yet.
							//   However, we do propagate un-seeing.
							// - Flagged, because it's just a request to upgrade
							sflags &= ~(F_SEEN | F_FLAGGED) | (srec->flags & F_SEEN);
						} else if (srec->status & S_DUMMY(t)) {
							// Don't propagate Flagged to placeholders, as that would be
							// misunderstood as a request to upgrade next time around. We
							// could replace the placeholder with one with(out) the flag
							// notice line (we can't modify the existing one due to IMAP
							// semantics), but that seems like major overkill, esp. as the
							// user likely wouldn't even notice the change. So the flag
							// won't be seen until the placeholder is upgraded - tough luck.
							sflags &= ~F_FLAGGED;
						}
						srec->aflags[t] = sflags & ~srec->flags;
						srec->dflags[t] = ~sflags & srec->flags;
						if (srec->aflags[t] || srec->dflags[t]) {
							debug( "  %sing flags: +%s -%s\n", str_hl[t],
							       fmt_flags( srec->aflags[t] ).str, fmt_flags( srec->dflags[t] ).str );
						}
					}
				}
			}
			srec = nsrec;  // Minor optimization: skip freshly created placeholder entry.
		}
	}

	for (t = 0; t < 2; t++) {
		debug( "synchronizing new messages on %s\n", str_fn[t^1] );
		int topping = 1;
		for (tmsg = svars->msgs[t^1]; tmsg; tmsg = tmsg->next) {
			if (tmsg->status & M_DEAD)
				continue;
			srec = tmsg->srec;
			if (srec) {
				// This covers legacy (or somehow corrupted) state files which
				// failed to track maxuid properly.
				// Note that this doesn't work in the presence of skipped or
				// failed messages. We could start keeping zombie entries, but
				// this wouldn't help with legacy state files.
				if (topping && svars->newmaxuid[t^1] < tmsg->uid)
					svars->newmaxuid[t^1] = tmsg->uid;

				if (srec->status & S_SKIPPED) {
					// Pre-1.4 legacy only: The message was skipped due to being too big.
					if (!(svars->chan->ops[t] & OP_UPGRADE))  // OP_OLD would be somewhat logical, too.
						continue;
					// The message size was not queried, so this won't be dummified below.
					srec->status = S_PENDING | S_DUMMY(t);
					JLOG( "_ %u %u", (srec->uid[F], srec->uid[N]), "placeholder only - was previously skipped" );
				} else {
					if (!(srec->status & S_PENDING)) {
						if (srec->uid[t])
							continue;  // Nothing to do - the message is paired
						if (!(svars->chan->ops[t] & OP_OLD)) {
							// This was reported as 'no <opposite>' already.
							// debug( "not re-propagating orphaned message %u\n", tmsg->uid );
							continue;
						}
						if (t != xt || !(srec->status & S_EXPIRED)) {
							// Orphans are essentially deletion propagation transactions which
							// were interrupted midway through by not expunging the target. We
							// don't re-propagate these, as it would be illogical, and also
							// make a mess of placeholder upgrades.
							debug( "ignoring orphaned message %u\n", tmsg->uid );
							continue;
						}
						if ((!(tmsg->flags & F_FLAGGED) &&
						     ((tmsg->flags & F_SEEN) || svars->chan->expire_unread > 0))) {
							debug( "not re-propagating tracked expired message %u\n", tmsg->uid );
							continue;
						}
						assert( !(srec->status & S_LOGGED) );
						srec->status |= S_PENDING;
						JLOG( "~ %u %u " stringify(S_PENDING), (srec->uid[F], srec->uid[N]),
						      "re-propagate tracked expired message" );
					} else {
						// Propagation was scheduled, but we got interrupted
						debug( "unpropagated old message %u\n", tmsg->uid );

						if (srec->status & S_UPGRADE) {
							if (((svars->chan->ops[t] & OP_EXPUNGE) &&
							     ((srec->pflags | srec->aflags[t]) & ~srec->dflags[t] & F_DELETED)) ||
							    ((svars->chan->ops[t^1] & OP_EXPUNGE) &&
							     ((srec->msg[t^1]->flags | srec->aflags[t^1]) & ~srec->dflags[t^1] & F_DELETED))) {
								// We can't just kill the entry, as we may be propagating flags
								// (in particular, F_DELETED) towards the real message.
								// No dummy is actually present, but pretend there is, so the
								// real message is considered new when trashing.
								srec->status = (srec->status & ~(S_PENDING | S_UPGRADE)) | S_DUMMY(t);
								JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
								      "canceling placeholder upgrade - would be expunged anyway" );
								continue;
							}
							// Prevent the driver from "completing" the flags, as we'll ignore them anyway.
							tmsg->status |= M_FLAGS;
							any_new[t] = 1;
							continue;
						}
					}
				}
			} else {
				// The 1st unknown message which should be known marks the end
				// of the synced range; more known messages may follow (from an
				// unidirectional sync in the opposite direction).
				if (t != xt || tmsg->uid > svars->maxxfuid)
					topping = 0;

				const char *what;
				if (tmsg->uid <= svars->maxuid[t^1]) {
					// The message should be already paired. It's not, so it was:
					// - attempted, but failed
					// - ignored, as it would have been expunged anyway
					// - paired, but subsequently expired and pruned
					if (!(svars->chan->ops[t] & OP_OLD)) {
						debug( "not propagating old message %u\n", tmsg->uid );
						continue;
					}
					if (topping) {
						// The message is below the boundary of the bulk range.
						// We'll sync it only if it has become important meanwhile.
						if (!(tmsg->flags & F_FLAGGED) &&
						    ((tmsg->flags & F_SEEN) || svars->chan->expire_unread > 0)) {
							debug( "not re-propagating untracked expired message %u\n", tmsg->uid );
							continue;
						}
						what = "untracked expired message";
					} else {
						what = "old message";
					}
				} else {
					if (!(svars->chan->ops[t] & OP_NEW)) {
						debug( "not propagating new message %u\n", tmsg->uid );
						continue;
					}
					what = "new message";
				}

				srec = nfzalloc( sizeof(*srec) );
				*svars->srecadd = srec;
				svars->srecadd = &srec->next;
				svars->nsrecs++;
				srec->status = S_PENDING;
				srec->uid[t^1] = tmsg->uid;
				srec->msg[t^1] = tmsg;
				tmsg->srec = srec;
				if (svars->newmaxuid[t^1] < tmsg->uid)
					svars->newmaxuid[t^1] = tmsg->uid;
				JLOG( "+ %u %u", (srec->uid[F], srec->uid[N]), "%s", what );
			}
			if (((svars->chan->ops[t] | svars->chan->ops[t^1]) & OP_EXPUNGE) && (tmsg->flags & F_DELETED)) {
				// Yes, we may nuke fresh entries, created only for newmaxuid tracking.
				// It would be lighter on the journal to log a (compressed) skip, but
				// this rare case does not justify additional complexity.
				JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "killing - would be expunged anyway" );
				tmsg->srec = NULL;
				srec->status = S_DEAD;
				continue;
			}
			if (tmsg->size > svars->chan->stores[t]->max_size && !(srec->status & (S_DUMMY(F) | S_DUMMY(N)))) {
				srec->status |= S_DUMMY(t);
				JLOG( "_ %u %u", (srec->uid[F], srec->uid[N]), "placeholder only - too big" );
			}
			any_new[t] = 1;
		}
	}

	if (svars->any_expiring) {
		/* Expire excess messages. Important (flagged, unread, or unpropagated) messages
		 * older than the first not expired message are not counted towards the total. */
		// Note: When this branch is entered, we have loaded all expire-side messages.
		debug( "preparing message expiration\n" );
		alive_srec_t *arecs = nfmalloc( sizeof(*arecs) * svars->nsrecs );
		int alive = 0;
		for (srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			// We completely ignore unpaired expire-side messages, as we cannot expire
			// them without data loss; consequently, we also don't count them.
			// Note that we also ignore expire-side messages we're currently propagating,
			// which delays expiration of some messages by one cycle. Otherwise, we'd
			// have to sequence flag updating after message propagation to avoid a race
			// with external expunging, and that seems unreasonably expensive.
			if (!srec->uid[xt^1])
				continue;
			if (!(srec->status & S_PENDING)) {
				// We ignore unpaired far-side messages, as there is obviously nothing
				// to expire in the first place.
				if (!srec->msg[xt])
					continue;
				nflags = srec->msg[xt]->flags;
				if (srec->status & S_DUMMY(xt)) {
					if (!srec->msg[xt^1])
						continue;
					// We need to pull in the real Flagged and Seen even if flag
					// propagation was not requested, as the placeholder's ones are
					// useless (except for un-seeing).
					// This results in the somewhat weird situation that messages
					// which are not visibly flagged remain unexpired.
					sflags = srec->msg[xt^1]->flags;
					aflags = (sflags & ~srec->flags) & (F_SEEN | F_FLAGGED);
					dflags = (~sflags & srec->flags) & F_SEEN;
					nflags = (nflags & (~(F_SEEN | F_FLAGGED) | (srec->flags & F_SEEN)) & ~dflags) | aflags;
				}
				nflags = (nflags | srec->aflags[xt]) & ~srec->dflags[xt];
			} else {
				if (srec->status & S_UPGRADE) {
					// The dummy's F & S flags are mostly masked out anyway,
					// but we may be pulling in the real ones.
					nflags = (srec->pflags | srec->aflags[xt]) & ~srec->dflags[xt];
				} else {
					nflags = srec->msg[xt^1]->flags;
				}
			}
			if (!(nflags & F_DELETED) || (srec->status & (S_EXPIRE | S_EXPIRED))) {
				// The message is not deleted, or it is, but only due to being expired.
				arecs[alive++] = (alive_srec_t){ srec, nflags };
			}
		}
		// Sort such that the messages which have been in the
		// complete store longest expire first.
		qsort( arecs, alive, sizeof(*arecs), (xt == F) ? cmp_srec_near : cmp_srec_far );
		int todel = alive - svars->chan->max_messages;
		debug( "%d alive messages, %d excess - expiring\n", alive, todel );
		int unseen = 0;
		for (int sri = 0; sri < alive; sri++) {
			srec = arecs[sri].srec;
			nflags = arecs[sri].flags;
			if ((nflags & F_FLAGGED) ||
			    !((nflags & F_SEEN) || ((void)(todel > 0 && unseen++), svars->chan->expire_unread > 0))) {
				// Important messages are always fetched/kept.
				debug( "  pair(%u,%u) is important\n", srec->uid[F], srec->uid[N] );
				todel--;
			} else if (todel > 0 ||
			           ((srec->status & (S_EXPIRE | S_EXPIRED)) == (S_EXPIRE | S_EXPIRED)) ||
			           ((srec->status & (S_EXPIRE | S_EXPIRED)) && (srec->msg[xt]->flags & F_DELETED))) {
				/* The message is excess or was already (being) expired. */
				srec->status |= S_NEXPIRE;
				debug( "  expiring pair(%u,%u)\n", srec->uid[F], srec->uid[N] );
				todel--;
			}
		}
		debug( "%d excess messages remain\n", todel );
		if (svars->chan->expire_unread < 0 && unseen * 2 > svars->chan->max_messages) {
			error( "%s: %d unread messages in excess of MaxMessages (%d).\n"
			       "Please set ExpireUnread to decide outcome. Skipping mailbox.\n",
			       svars->orig_name[xt], unseen, svars->chan->max_messages );
			svars->ret |= SYNC_FAIL;
			cancel_sync( svars );
			return;
		}
		for (int sri = 0; sri < alive; sri++) {
			srec = arecs[sri].srec;
			if (!(srec->status & S_PENDING)) {
				uchar nex = (srec->status / S_NEXPIRE) & 1;
				if (nex != ((srec->status / S_EXPIRED) & 1)) {
					/* The record needs a state change ... */
					if (nex != ((srec->status / S_EXPIRE) & 1)) {
						/* ... and we need to start a transaction. */
						srec->status = (srec->status & ~S_EXPIRE) | (nex * S_EXPIRE);
						JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
						      "expire %u - begin", nex );
					} else {
						/* ... but the "right" transaction is already pending. */
						debug( "-> pair(%u,%u): expire %u (pending)\n", srec->uid[F], srec->uid[N], nex );
					}
				} else {
					/* Note: the "wrong" transaction may be pending here,
					 * e.g.: S_NEXPIRE = 0, S_EXPIRE = 1, S_EXPIRED = 0. */
				}
			} else {
				if (srec->status & S_NEXPIRE) {
					srec->status = S_EXPIRE | S_EXPIRED;
					JLOG( "~ %u %u %u", (srec->uid[F], srec->uid[N], srec->status), "expire unborn" );
					// If we have so many new messages that some of them are instantly expired,
					// but some are still propagated because they are important, we need to
					// ensure explicitly that the bulk fetch limit is upped.
					if (svars->maxxfuid < srec->uid[xt^1])
						svars->maxxfuid = srec->uid[xt^1];
					srec->msg[xt^1]->srec = NULL;
				}
			}
		}
		free( arecs );
	}

	sync_ref( svars );

	debug( "synchronizing flags\n" );
	for (srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		for (t = 0; t < 2; t++) {
			if (!srec->uid[t])
				continue;
			if (srec->status & S_GONE(t)) {
				// The message was expunged. No need to call flags_set(), because:
				// - for S_DELETE and S_PURGE, the entry will be pruned due to both sides being gone
				// - for regular flag propagations, there is nothing to do
				// - expirations were already handled above
				continue;
			}
			aflags = srec->aflags[t];
			dflags = srec->dflags[t];
			if (srec->status & (S_DELETE | S_PURGE)) {
				if (!aflags) {
					// This deletion propagation goes the other way round, or
					// this deletion of a dummy happens on the other side.
					continue;
				}
			} else {
				/* The trigger is an expiration transaction being ongoing ... */
				if ((t == xt) && ((shifted_bit(srec->status, S_EXPIRE, S_EXPIRED) ^ srec->status) & S_EXPIRED)) {
					// ... but the actual action derives from the wanted state -
					// so that canceled transactions are rolled back as well.
					if (srec->status & S_NEXPIRE)
						aflags |= F_DELETED;
					else
						dflags |= F_DELETED;
				}
			}
			if ((svars->chan->ops[t] & OP_EXPUNGE) &&
			    (((srec->msg[t] ? srec->msg[t]->flags : 0) | aflags) & ~dflags & F_DELETED) &&
			    (!svars->ctx[t]->conf->trash || svars->ctx[t]->conf->trash_only_new))
			{
				/* If the message is going to be expunged, don't propagate anything but the deletion. */
				srec->aflags[t] &= F_DELETED;
				aflags &= F_DELETED;
				srec->dflags[t] = dflags = 0;
			}
			if (srec->msg[t] && (srec->msg[t]->status & M_FLAGS)) {
				/* If we know the target message's state, optimize away non-changes. */
				aflags &= ~srec->msg[t]->flags;
				dflags &= srec->msg[t]->flags;
			}
			if (aflags | dflags) {
				flags_total[t]++;
				stats();
				svars->flags_pending[t]++;
				fv = nfmalloc( sizeof(*fv) );
				fv->aux = AUX;
				fv->srec = srec;
				fv->aflags = aflags;
				fv->dflags = dflags;
				svars->drv[t]->set_msg_flags( svars->ctx[t], srec->msg[t], srec->uid[t], aflags, dflags, flags_set, fv );
				if (check_cancel( svars ))
					goto out;
			} else {
				flags_set_p2( svars, srec, t );
			}
		}
	}
	for (t = 0; t < 2; t++) {
		svars->drv[t]->commit_cmds( svars->ctx[t] );
		svars->state[t] |= ST_SENT_FLAGS;
		msgs_flags_set( svars, t );
		if (check_cancel( svars ))
			goto out;
	}

	debug( "propagating new messages\n" );
	for (t = 0; t < 2; t++) {
		if (any_new[t]) {
			// fsync'ing the UIDNEXT bump is not strictly necessary, but advantageous.
			svars->finduid[t] = svars->drv[t]->get_uidnext( svars->ctx[t] );
			JLOG( "F %d %u", (t, svars->finduid[t]), "save UIDNEXT of %s", str_fn[t] );
			svars->new_msgs[t] = svars->msgs[t^1];
		} else {
			svars->state[t] |= ST_SENT_NEW;
		}
	}
	if (any_new[F] | any_new[N]) {
		// TUID assignment needs to be fsync'd, as otherwise a system crash may
		// lead to the newly propagated messages becoming duplicated.
		// Of course, we could assign each TUID only after fetching the message
		// and fsync it separately, but that would be horribly inefficient.
		for (srec = svars->srecs; srec; srec = srec->next)
			if (srec->status & S_PENDING)
				assign_tuid( svars, srec );
		if (UseFSync && svars->jfp)
			fdatasync( fileno( svars->jfp ) );
	}
	for (t = 0; t < 2; t++) {
		msgs_copied( svars, t );
		if (check_cancel( svars ))
			goto out;
	}

  out:
	sync_deref( svars );
}

static void
msg_copied( int sts, uint uid, copy_vars_t *vars )
{
	DECL_INIT_SVARS(vars->aux);
	sync_rec_t *srec = vars->srec;
	switch (sts) {
	case COPY_OK:
		if (!uid)  // Stored to a non-UIDPLUS mailbox
			svars->state[t] |= ST_FIND_NEW;
		else
			ASSIGN_UID( srec, t, uid, "%sed message", str_hl[t] );
		break;
	case COPY_NOGOOD:
	case COPY_GONE:
		srec->status = S_DEAD;
		JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "%s failed", str_hl[t] );
		break;
	default:  // COPY_FAIL
		cancel_sync( svars );
		FALLTHROUGH
	case COPY_CANCELED:
		free( vars );
		return;
	}
	free( vars );
	new_done[t]++;
	stats();
	svars->new_pending[t]--;
	if (check_cancel( svars ))
		return;
	msgs_copied( svars, t );
}

static void msgs_found_new( int sts, message_t *msgs, void *aux );
static void msgs_new_done( sync_vars_t *svars, int t );
static void sync_close( sync_vars_t *svars, int t );

static void
msgs_copied( sync_vars_t *svars, int t )
{
	message_t *tmsg;
	sync_rec_t *srec;
	copy_vars_t *cv;

	if (svars->state[t] & ST_SENDING_NEW)
		return;

	sync_ref( svars );

	if (!(svars->state[t] & ST_SENT_NEW)) {
		for (tmsg = svars->new_msgs[t]; tmsg; tmsg = tmsg->next) {
			if (tmsg->status & M_DEAD)
				continue;
			if ((srec = tmsg->srec) && (srec->status & S_PENDING)) {
				if (svars->drv[t]->get_memory_usage( svars->ctx[t] ) >= BufferLimit) {
					svars->new_msgs[t] = tmsg;
					goto out;
				}
				new_total[t]++;
				stats();
				svars->new_pending[t]++;
				svars->state[t] |= ST_SENDING_NEW;
				cv = nfmalloc( sizeof(*cv) );
				cv->cb = msg_copied;
				cv->aux = AUX;
				cv->srec = srec;
				cv->msg = tmsg;
				cv->minimal = (srec->status & S_DUMMY(t));
				copy_msg( cv );
				svars->state[t] &= ~ST_SENDING_NEW;
				if (check_cancel( svars ))
					goto out;
			}
		}
		svars->state[t] |= ST_SENT_NEW;
	}

	if (svars->new_pending[t])
		goto out;

	sync_close( svars, t^1 );
	if (check_cancel( svars ))
		goto out;

	if (svars->state[t] & ST_FIND_NEW) {
		debug( "finding just copied messages on %s\n", str_fn[t] );
		svars->drv[t]->find_new_msgs( svars->ctx[t], svars->finduid[t], msgs_found_new, AUX );
	} else {
		msgs_new_done( svars, t );
	}

  out:
	sync_deref( svars );
}

static void
msgs_found_new( int sts, message_t *msgs, void *aux )
{
	SVARS_CHECK_RET;
	debug( "matching just copied messages on %s\n", str_fn[t] );
	int num_lost = match_tuids( svars, t, msgs );
	if (num_lost)
		warn( "Warning: lost track of %d %sed message(s)\n", num_lost, str_hl[t] );
	if (check_cancel( svars ))
		return;
	msgs_new_done( svars, t );
}

static void
msgs_new_done( sync_vars_t *svars, int t )
{
	svars->state[t] |= ST_FOUND_NEW;
	sync_close( svars, t );
}

static void
flags_set( int sts, void *aux )
{
	SVARS_CHECK_RET_VARS(flag_vars_t);
	sync_rec_t *srec = vars->srec;
	switch (sts) {
	case DRV_OK:
		if (vars->aflags & F_DELETED)
			srec->status |= S_DEL(t);
		else if (vars->dflags & F_DELETED)
			srec->status &= ~S_DEL(t);
		flags_set_p2( svars, srec, t );
		break;
	}
	free( vars );
	flags_done[t]++;
	stats();
	svars->flags_pending[t]--;
	if (check_cancel( svars ))
		return;
	msgs_flags_set( svars, t );
}

static void
flags_set_p2( sync_vars_t *svars, sync_rec_t *srec, int t )
{
	if (srec->status & S_PURGE) {
		JLOG( "P %u %u", (srec->uid[F], srec->uid[N]), "deleted dummy" );
		srec->status = (srec->status & ~S_PURGE) | S_PURGED;
	} else if (!(srec->status & S_DELETE)) {
		uchar nflags = (srec->flags | srec->aflags[t]) & ~srec->dflags[t];
		if (srec->flags != nflags) {
			JLOG( "* %u %u %u", (srec->uid[F], srec->uid[N], nflags), "%sed flags %s; were %s",
			      (str_hl[t], fmt_lone_flags( nflags ).str, fmt_lone_flags( srec->flags ).str) );
			srec->flags = nflags;
		}
		if (t == svars->chan->expire_side) {
			uchar ex = (srec->status / S_EXPIRE) & 1;
			uchar exd = (srec->status / S_EXPIRED) & 1;
			if (ex != exd) {
				uchar nex = (srec->status / S_NEXPIRE) & 1;
				if (nex == ex) {
					if (nex && svars->maxxfuid < srec->uid[t^1])
						svars->maxxfuid = srec->uid[t^1];
					srec->status = (srec->status & ~S_EXPIRED) | (nex * S_EXPIRED);
					JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
					      "expired %d - commit", nex );
				} else {
					srec->status = (srec->status & ~S_EXPIRE) | (nex * S_EXPIRE);
					JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
					      "expire %d - cancel", nex );
				}
			}
		}
	}
}

typedef struct {
	void *aux;
	message_t *msg;
} trash_vars_t;

static void msg_trashed( int sts, void *aux );
static void msg_rtrashed( int sts, uint uid, copy_vars_t *vars );

static void
msgs_flags_set( sync_vars_t *svars, int t )
{
	message_t *tmsg;
	sync_rec_t *srec;
	trash_vars_t *tv;
	copy_vars_t *cv;

	if (!(svars->state[t] & ST_SENT_FLAGS) || svars->flags_pending[t])
		return;

	sync_ref( svars );

	sync_close( svars, t^1 );
	if (check_cancel( svars ))
		goto out;

	int only_solo;
	if (svars->chan->ops[t] & OP_EXPUNGE_SOLO)
		only_solo = 1;
	else if (svars->chan->ops[t] & OP_EXPUNGE)
		only_solo = 0;
	else
		goto skip;
	int xt = svars->chan->expire_side;
	int expunge_other = (svars->chan->ops[t^1] & OP_EXPUNGE);
	// Driver-wise, this makes sense only if (svars->opts[t] & OPEN_UID_EXPUNGE),
	// but the trashing loop uses the result as well.
	debug( "preparing expunge of %s on %s, %sexpunging %s\n",
	       only_solo ? "solo" : "all", str_fn[t], expunge_other ? "" : "NOT ", str_fn[t^1] );
	for (tmsg = svars->msgs[t]; tmsg; tmsg = tmsg->next) {
		if (tmsg->status & M_DEAD)
			continue;
		if (!(tmsg->flags & F_DELETED)) {
			//debug( "  message %u is not deleted\n", tmsg->uid );  // Too noisy
			continue;
		}
		debugn( "  message %u ", tmsg->uid );
		if (only_solo) {
			if ((srec = tmsg->srec)) {
				if (!srec->uid[t^1]) {
					debugn( "(solo) " );
				} else if (srec->status & S_GONE(t^1)) {
					debugn( "(orphaned) " );
				} else if (expunge_other && (srec->status & S_DEL(t^1))) {
					debugn( "(orphaning) " );
				} else if (t == xt && (srec->status & (S_EXPIRE | S_EXPIRED))) {
					// Expiration overrides mirroring, as otherwise the combination
					// makes no sense at all.
					debugn( "(expire) " );
				} else {
					debug( "is not solo\n" );
					continue;
				}
				if (srec->status & S_PENDING) {
					debug( "is being paired\n" );
					continue;
				}
			} else {
				debugn( "(isolated) " );
			}
		}
		debug( "- expunging\n" );
		tmsg->status |= M_EXPUNGE;
	}

	int remote, only_new;
	if (svars->ctx[t]->conf->trash) {
		only_new = svars->ctx[t]->conf->trash_only_new;
		debug( "trashing %s on %s locally\n", only_new ? "new" : "all", str_fn[t] );
		remote = 0;
	} else if (svars->ctx[t^1]->conf->trash && svars->ctx[t^1]->conf->trash_remote_new) {
		debug( "trashing new on %s remotely\n", str_fn[t] );
		only_new = 1;
		remote = 1;
	} else {
		goto skip;
	}
	for (tmsg = svars->msgs[t]; tmsg; tmsg = tmsg->next) {
		if (tmsg->status & M_DEAD)
			continue;
		if (!(tmsg->status & M_EXPUNGE)) {
			//debug( "  message %u is not being expunged\n", tmsg->uid );  // Too noisy
			continue;
		}
		debugn( "  message %u ", tmsg->uid );
		if ((srec = tmsg->srec)) {
			if (t == xt && (srec->status & (S_EXPIRE | S_EXPIRED))) {
				// Don't trash messages that are deleted only due to expiring.
				// However, this is an unlikely configuration to start with ...
				debug( "is expired\n" );
				continue;
			}
			if (srec->status & S_DUMMY(t)) {
				// This is mostly academical, as trashing being done on the side
				// where placeholders reside is rather unlikely.
				debug( "is dummy\n" );
				continue;
			}
			if (srec->status & S_PURGED) {
				// As above.
				debug( "is deleted dummy\n" );
				continue;
			}
			if (only_new && !(srec->status & (S_DUMMY(t^1) | S_SKIPPED))) {
				debug( "is not new\n" );
				continue;
			}
		}
		if (find_uint_array( svars->trashed_msgs[t].array, tmsg->uid )) {
			debug( "was already trashed\n" );
			continue;
		}
		debug( "- trashing\n" );
		trash_total[t]++;
		stats();
		svars->trash_pending[t]++;
		if (!remote) {
			tv = nfmalloc( sizeof(*tv) );
			tv->aux = AUX;
			tv->msg = tmsg;
			svars->drv[t]->trash_msg( svars->ctx[t], tmsg, msg_trashed, tv );
		} else {
			cv = nfmalloc( sizeof(*cv) );
			cv->cb = msg_rtrashed;
			cv->aux = INV_AUX;
			cv->srec = NULL;
			cv->msg = tmsg;
			cv->minimal = 0;
			copy_msg( cv );
		}
		if (check_cancel( svars ))
			goto out;
	}
  skip:
	svars->state[t] |= ST_SENT_TRASH;
	sync_close( svars, t );

  out:
	sync_deref( svars );
}

static void
msg_trashed( int sts, void *aux )
{
	SVARS_CHECK_RET_VARS(trash_vars_t);
	switch (sts) {
	case DRV_OK:
		JLOG( "T %d %u", (t, vars->msg->uid), "trashed on %s", str_fn[t] );
		break;
	case DRV_MSG_BAD:
		if (vars->msg->status & M_DEAD)
			break;
		// Driver already reported error.
		svars->ret |= SYNC_FAIL;
		if (svars->opts[t] & OPEN_UID_EXPUNGE)
			vars->msg->status &= ~M_EXPUNGE;
		else
			svars->state[t] |= ST_TRASH_BAD;
		break;
	}
	free( vars );
	trash_done[t]++;
	stats();
	svars->trash_pending[t]--;
	if (check_cancel( svars ))
		return;
	sync_close( svars, t );
}

static void
msg_rtrashed( int sts, uint uid ATTR_UNUSED, copy_vars_t *vars )
{
	DECL_INIT_SVARS(vars->aux);
	t ^= 1;
	switch (sts) {
	case COPY_OK:
		JLOG( "T %d %u", (t, vars->msg->uid), "trashed remotely on %s", str_fn[t^1] );
		break;
	case COPY_GONE:
		break;
	case COPY_NOGOOD:
		if (svars->opts[t] & OPEN_UID_EXPUNGE)
			vars->msg->status &= ~M_EXPUNGE;
		else
			svars->state[t] |= ST_TRASH_BAD;
		break;
	default:  // COPY_FAIL
		cancel_sync( svars );
		FALLTHROUGH
	case COPY_CANCELED:
		free( vars );
		return;
	}
	free( vars );
	trash_done[t]++;
	stats();
	svars->trash_pending[t]--;
	if (check_cancel( svars ))
		return;
	sync_close( svars, t );
}

static void box_closed( int sts, int reported, void *aux );
static void box_closed_p2( sync_vars_t *svars, int t );

static void
sync_close( sync_vars_t *svars, int t )
{
	if ((~svars->state[t] & (ST_FOUND_NEW|ST_SENT_TRASH)) || svars->trash_pending[t] ||
	    (~svars->state[t^1] & (ST_SENT_NEW | ST_SENT_FLAGS)) || svars->new_pending[t^1] || svars->flags_pending[t^1])
		return;

	if (svars->state[t] & ST_CLOSING)
		return;
	svars->state[t] |= ST_CLOSING;

	if ((svars->chan->ops[t] & (OP_EXPUNGE | OP_EXPUNGE_SOLO)) && !(DFlags & FAKEEXPUNGE)
	    && !(svars->state[t] & ST_TRASH_BAD)) {
		if (Verbosity >= TERSE || (DFlags & EXT_EXIT)) {
			if (svars->opts[t] & OPEN_UID_EXPUNGE) {
				for (message_t *tmsg = svars->msgs[t]; tmsg; tmsg = tmsg->next) {
					if (tmsg->status & M_DEAD)
						continue;
					if (tmsg->status & M_EXPUNGE)
						expunge_total[t]++;
				}
			} else {
				for (sync_rec_t *srec = svars->srecs; srec; srec = srec->next) {
					if (srec->status & S_DEAD)
						continue;
					if (srec->status & S_DEL(t))
						expunge_total[t]++;
				}
			}
			stats();
		}

		debug( "expunging %s\n", str_fn[t] );
		svars->drv[t]->close_box( svars->ctx[t], box_closed, AUX );
	} else {
		box_closed_p2( svars, t );
	}
}

static void
box_closed( int sts, int reported, void *aux )
{
	SVARS_CHECK_RET_CANCEL;
	if (!reported) {
		for (sync_rec_t *srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			// Note that this logic is somewhat optimistic - theoretically, it's
			// possible that a message was concurrently undeleted before we tried
			// to expunge it. Such a message would be subsequently re-propagated
			// by a refresh, and in the extremely unlikely case of this happening
			// on both sides, we'd even get a duplicate. That's why this is only
			// a fallback.
			if (srec->status & S_DEL(t)) {
				srec->status |= S_GONE(t);
				expunge_done[t]++;
			}
		}
		stats();
	}
	box_closed_p2( svars, t );
}

static void
box_closed_p2( sync_vars_t *svars, int t )
{
	sync_rec_t *srec;

	svars->state[t] |= ST_CLOSED;
	if (!(svars->state[t^1] & ST_CLOSED))
		return;

	// All logging done in this function is merely for the journal replay
	// autotest - the operations are idempotent, and we're about to commit
	// the new state right afterwards anyway. Therefore, it would also
	// make no sense to cover it by the interrupt-resume autotest (which
	// would also add unreasonable complexity, as the maxuid bumps and entry
	// purge must be consistent).

	if (DFlags & KEEPJOURNAL)
		printf( "### %d steps, %d entries ###\n", -JLimit, JCount );

	for (t = 0; t < 2; t++) {
		// Committing maxuid is delayed until all messages were propagated, to
		// ensure that all pending messages are still loaded next time in case
		// of interruption - in particular skipping messages would otherwise
		// up the limit too early.
		svars->maxuid[t] = svars->newmaxuid[t];
		if (svars->maxuid[t] != svars->oldmaxuid[t])
			PC_JLOG( "N %d %u", (t, svars->maxuid[t]), "up maxuid of %s", str_fn[t] );
	}

	debug( "purging obsolete entries\n" );
	int xt = svars->chan->expire_side;
	for (srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		if ((srec->status & S_EXPIRED) &&
		    (!srec->uid[xt] || (srec->status & S_GONE(xt))) &&
		    svars->maxuid[xt^1] >= srec->uid[xt^1] && svars->maxxfuid >= srec->uid[xt^1]) {
		    PC_JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "killing expired" );
			srec->status = S_DEAD;
		} else if (!srec->uid[N] || (srec->status & S_GONE(N))) {
			if (!srec->uid[F] || (srec->status & S_GONE(F))) {
				PC_JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "killing" );
				srec->status = S_DEAD;
			} else if (srec->uid[N] && (srec->status & S_DEL(F))) {
				PC_JLOG( "> %u %u 0", (srec->uid[F], srec->uid[N]), "orphaning" );
				srec->uid[N] = 0;
			}
		} else if (srec->uid[F] && (srec->status & S_GONE(F)) && (srec->status & S_DEL(N))) {
			PC_JLOG( "< %u %u 0", (srec->uid[F], srec->uid[N]), "orphaning" );
			srec->uid[F] = 0;
		}
	}

	save_state( svars );

	sync_bail( svars );
}

static void
sync_bail( sync_vars_t *svars )
{
	sync_rec_t *srec, *nsrec;

	free( svars->trashed_msgs[F].array.data );
	free( svars->trashed_msgs[N].array.data );
	for (srec = svars->srecs; srec; srec = nsrec) {
		nsrec = srec->next;
		free( srec );
	}
	if (svars->lfd >= 0) {
		unlink( svars->lname );
		close( svars->lfd );
	}
	sync_bail2( svars );
}

static void
sync_bail2( sync_vars_t *svars )
{
	free( svars->lname );
	free( svars->nname );
	free( svars->jname );
	free( svars->dname );
	sync_bail3( svars );
}

static void
sync_bail3( sync_vars_t *svars )
{
	free( svars->box_name[F] );
	free( svars->box_name[N] );
	sync_deref( svars );
}

static void
sync_deref( sync_vars_t *svars )
{
	if (!--svars->ref_count) {
		void (*cb)( int sts, void *aux ) = svars->cb;
		void *aux = svars->aux;
		int ret = svars->ret;
		free( svars );
		cb( ret, aux );
	}
}
