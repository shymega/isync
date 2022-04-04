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
   close(x): trash(x) & find_new(x) & new(!x) // with expunge
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
	ST_CLOSING,
	ST_CLOSED,
	ST_DID_EXPUNGE,
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
			char bfbuf[16];
			make_flags( bflags, bfbuf );
			notice( "Notice: %s store does not support flag(s) '%s'; not propagating.\n", str_fn[t], bfbuf );
			svars->bad_flags[t] |= bflags;
		}
	}
	return tflags & svars->good_flags[t];
}


typedef struct copy_vars {
	void (*cb)( int sts, uint uid, struct copy_vars *vars );
	void *aux;
	sync_rec_t *srec; /* also ->tuid */
	message_t *msg;
	msg_data_t data;
	int minimal;
} copy_vars_t;

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
copy_msg_bytes( char **out_ptr, const char *in_buf, uint *in_idx, uint in_len, int in_cr, int out_cr )
{
	char *out = *out_ptr;
	uint idx = *in_idx;
	if (out_cr != in_cr) {
		char c;
		if (out_cr) {
			for (; idx < in_len; idx++) {
				if ((c = in_buf[idx]) != '\r') {
					if (c == '\n')
						*out++ = '\r';
					*out++ = c;
				}
			}
		} else {
			for (; idx < in_len; idx++) {
				if ((c = in_buf[idx]) != '\r')
					*out++ = c;
			}
		}
	} else {
		memcpy( out, in_buf + idx, in_len - idx );
		out += in_len - idx;
		idx = in_len;
	}
	*out_ptr = out;
	*in_idx = idx;
}

static int
copy_msg_convert( int in_cr, int out_cr, copy_vars_t *vars, int t )
{
	char *in_buf = vars->data.data;
	uint in_len = vars->data.len;
	uint idx = 0, sbreak = 0, ebreak = 0, break2 = UINT_MAX;
	uint lines = 0, hdr_crs = 0, bdy_crs = 0, app_cr = 0, extra = 0;
	uint add_subj = 0;

	if (vars->srec) {
	  nloop: ;
		uint start = idx;
		uint line_crs = 0;
		while (idx < in_len) {
			char c = in_buf[idx++];
			if (c == '\r') {
				line_crs++;
			} else if (c == '\n') {
				if (!ebreak && starts_with_upper( in_buf + start, (int)(in_len - start), "X-TUID: ", 8 )) {
					extra = (sbreak = start) - (ebreak = idx);
					if (!vars->minimal)
						goto oke;
				} else {
					if (break2 == UINT_MAX && vars->minimal &&
					    starts_with_upper( in_buf + start, (int)(in_len - start), "SUBJECT:", 8 )) {
						break2 = start + 8;
						if (break2 < in_len && in_buf[break2] == ' ')
							break2++;
					}
					lines++;
					hdr_crs += line_crs;
				}
				if (idx - line_crs - 1 == start) {
					if (!ebreak)
						sbreak = ebreak = start;
					if (vars->minimal) {
						in_len = idx;
						if (break2 == UINT_MAX) {
							break2 = start;
							add_subj = 1;
						}
					}
					goto oke;
				}
				goto nloop;
			}
		}
		warn( "Warning: message %u from %s has incomplete header; skipping.\n",
		      vars->msg->uid, str_fn[t^1] );
		free( in_buf );
		return 0;
	  oke:
		app_cr = out_cr && (!in_cr || hdr_crs);
		extra += 8 + TUIDL + app_cr + 1;
	}
	if (out_cr != in_cr) {
		for (; idx < in_len; idx++) {
			char c = in_buf[idx];
			if (c == '\r')
				bdy_crs++;
			else if (c == '\n')
				lines++;
		}
		extra -= hdr_crs + bdy_crs;
		if (out_cr)
			extra += lines;
	}

	uint dummy_msg_len = 0;
	char dummy_msg_buf[180];
	static const char dummy_pfx[] = "[placeholder] ";
	static const char dummy_subj[] = "Subject: [placeholder] (No Subject)";
	static const char dummy_msg[] =
		"Having a size of %s, this message is over the MaxSize limit.%s"
		"Flag it and sync again (Sync mode ReNew) to fetch its real contents.%s";

	if (vars->minimal) {
		char sz[32];

		if (vars->msg->size < 1024000)
			sprintf( sz, "%dKiB", (int)(vars->msg->size >> 10) );
		else
			sprintf( sz, "%.1fMiB", vars->msg->size / 1048576. );
		const char *nl = app_cr ? "\r\n" : "\n";
		dummy_msg_len = (uint)sprintf( dummy_msg_buf, dummy_msg, sz, nl, nl );
		extra += dummy_msg_len;
		extra += add_subj ? strlen(dummy_subj) + app_cr + 1 : strlen(dummy_pfx);
	}

	vars->data.len = in_len + extra;
	if (vars->data.len > INT_MAX) {
		warn( "Warning: message %u from %s is too big after conversion; skipping.\n",
		      vars->msg->uid, str_fn[t^1] );
		free( in_buf );
		return 0;
	}
	char *out_buf = vars->data.data = nfmalloc( vars->data.len );
	idx = 0;
	if (vars->srec) {
		if (break2 < sbreak) {
			copy_msg_bytes( &out_buf, in_buf, &idx, break2, in_cr, out_cr );
			memcpy( out_buf, dummy_pfx, strlen(dummy_pfx) );
			out_buf += strlen(dummy_pfx);
		}
		copy_msg_bytes( &out_buf, in_buf, &idx, sbreak, in_cr, out_cr );

		memcpy( out_buf, "X-TUID: ", 8 );
		out_buf += 8;
		memcpy( out_buf, vars->srec->tuid, TUIDL );
		out_buf += TUIDL;
		if (app_cr)
			*out_buf++ = '\r';
		*out_buf++ = '\n';
		idx = ebreak;

		if (break2 != UINT_MAX && break2 >= sbreak) {
			copy_msg_bytes( &out_buf, in_buf, &idx, break2, in_cr, out_cr );
			if (!add_subj) {
				memcpy( out_buf, dummy_pfx, strlen(dummy_pfx) );
				out_buf += strlen(dummy_pfx);
			} else {
				memcpy( out_buf, dummy_subj, strlen(dummy_subj) );
				out_buf += strlen(dummy_subj);
				if (app_cr)
					*out_buf++ = '\r';
				*out_buf++ = '\n';
			}
		}
	}
	copy_msg_bytes( &out_buf, in_buf, &idx, in_len, in_cr, out_cr );

	if (vars->minimal)
		memcpy( out_buf, dummy_msg_buf, dummy_msg_len );

	free( in_buf );
	return 1;
}

static void
msg_fetched( int sts, void *aux )
{
	copy_vars_t *vars = (copy_vars_t *)aux;
	DECL_SVARS;
	int scr, tcr;

	switch (sts) {
	case DRV_OK:
		INIT_SVARS(vars->aux);
		if (check_cancel( svars )) {
			free( vars->data.data );
			vars->cb( SYNC_CANCELED, 0, vars );
			return;
		}

		vars->msg->flags = vars->data.flags = sanitize_flags( vars->data.flags, svars, t );

		scr = (svars->drv[t^1]->get_caps( svars->ctx[t^1] ) / DRV_CRLF) & 1;
		tcr = (svars->drv[t]->get_caps( svars->ctx[t] ) / DRV_CRLF) & 1;
		if (vars->srec || scr != tcr) {
			if (!copy_msg_convert( scr, tcr, vars, t )) {
				vars->cb( SYNC_NOGOOD, 0, vars );
				return;
			}
		}

		svars->drv[t]->store_msg( svars->ctx[t], &vars->data, !vars->srec, msg_stored, vars );
		break;
	case DRV_CANCELED:
		vars->cb( SYNC_CANCELED, 0, vars );
		break;
	case DRV_MSG_BAD:
		vars->cb( SYNC_NOGOOD, 0, vars );
		break;
	default:  // DRV_BOX_BAD
		vars->cb( SYNC_FAIL, 0, vars );
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
		vars->cb( SYNC_OK, uid, vars );
		break;
	case DRV_CANCELED:
		vars->cb( SYNC_CANCELED, 0, vars );
		break;
	case DRV_MSG_BAD:
		INIT_SVARS(vars->aux);
		(void)svars;
		warn( "Warning: %s refuses to store message %u from %s.\n",
		      str_fn[t], vars->msg->uid, str_fn[t^1] );
		vars->cb( SYNC_NOGOOD, 0, vars );
		break;
	default:  // DRV_BOX_BAD
		vars->cb( SYNC_FAIL, 0, vars );
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
	DECL_SVARS;

	if (sts == DRV_CANCELED)
		return 1;
	INIT_SVARS(aux);
	if (sts == DRV_BOX_BAD) {
		svars->ret |= SYNC_FAIL;
		cancel_sync( svars );
		return 1;
	}
	return check_cancel( svars );
}

#define SVARS_CHECK_RET \
	DECL_SVARS; \
	if (check_ret( sts, aux )) \
		return; \
	INIT_SVARS(aux)

#define SVARS_CHECK_RET_VARS(type) \
	type *vars = (type *)aux; \
	DECL_SVARS; \
	if (check_ret( sts, vars->aux )) { \
		free( vars ); \
		return; \
	} \
	INIT_SVARS(vars->aux)

#define SVARS_CHECK_CANCEL_RET \
	DECL_SVARS; \
	if (sts == SYNC_CANCELED) { \
		free( vars ); \
		return; \
	} \
	INIT_SVARS(vars->aux)

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
		} else if (map_name( svars->orig_name[t], &svars->box_name[t], 0, "/", ctx[t]->conf->flat_delim ) < 0) {
			error( "Error: canonical mailbox name '%s' contains flattened hierarchy delimiter\n", svars->orig_name[t] );
		  bail3:
			svars->ret = SYNC_FAIL;
			sync_bail3( svars );
			return;
		}
		svars->drv[t] = ctx[t]->driver;
		svars->drv[t]->set_bad_callback( ctx[t], store_bad, AUX );
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
	DECL_SVARS;

	if (sts == DRV_CANCELED)
		return;
	INIT_SVARS(aux);
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
	DECL_SVARS;

	if (check_ret( sts, aux ))
		return;
	INIT_SVARS(aux);

	delete_state( svars );
	svars->drv[t]->finish_delete_box( svars->ctx[t] );
	sync_bail( svars );
}

static void
box_created( int sts, void *aux )
{
	DECL_SVARS;

	if (check_ret( sts, aux ))
		return;
	INIT_SVARS(aux);

	svars->drv[t]->open_box( svars->ctx[t], box_opened, AUX );
}

static void
box_opened( int sts, uint uidvalidity, void *aux )
{
	DECL_SVARS;

	if (sts == DRV_CANCELED)
		return;
	INIT_SVARS(aux);
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

	opts[F] = opts[N] = 0;
	if (fails)
		opts[F] = opts[N] = OPEN_OLD|OPEN_OLD_IDS;
	for (t = 0; t < 2; t++) {
		if (chan->ops[t] & (OP_DELETE|OP_FLAGS)) {
			opts[t] |= OPEN_SETFLAGS;
			opts[t^1] |= OPEN_OLD;
			if (chan->ops[t] & OP_FLAGS)
				opts[t^1] |= OPEN_FLAGS;
		}
		if (chan->ops[t] & (OP_NEW|OP_RENEW)) {
			opts[t] |= OPEN_APPEND;
			if (chan->ops[t] & OP_NEW) {
				opts[t^1] |= OPEN_NEW;
				if (chan->stores[t]->max_size != UINT_MAX)
					opts[t^1] |= OPEN_FLAGS | OPEN_NEW_SIZE;
			}
			if (chan->ops[t] & OP_RENEW) {
				opts[t] |= OPEN_OLD|OPEN_FLAGS|OPEN_SETFLAGS;
				opts[t^1] |= OPEN_OLD | OPEN_FLAGS;
			}
			if (chan->ops[t] & OP_EXPUNGE)  // Don't propagate doomed msgs
				opts[t^1] |= OPEN_FLAGS;
		}
		if (chan->ops[t] & OP_EXPUNGE) {
			opts[t] |= OPEN_EXPUNGE;
			if (chan->stores[t]->trash) {
				if (!chan->stores[t]->trash_only_new)
					opts[t] |= OPEN_OLD;
				opts[t] |= OPEN_NEW|OPEN_FLAGS;
			} else if (chan->stores[t^1]->trash && chan->stores[t^1]->trash_remote_new) {
				opts[t] |= OPEN_NEW|OPEN_FLAGS;
			}
		}
	}
	// While only new messages can cause expiration due to displacement,
	// updating flags can cause expiration of already overdue messages.
	// The latter would also apply when the expired box is the source,
	// but it's more natural to treat it as read-only in that case.
	// OP_RENEW makes sense only for legacy S_SKIPPED entries.
	if ((chan->ops[N] & (OP_NEW|OP_RENEW|OP_FLAGS)) && chan->max_messages)
		opts[N] |= OPEN_OLD|OPEN_NEW|OPEN_FLAGS;
	if (svars->replayed) {
		for (srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			if (srec->tuid[0]) {
				if (!srec->uid[F])
					opts[F] |= OPEN_NEW|OPEN_FIND, svars->state[F] |= ST_FIND_OLD;
				else if (!srec->uid[N])
					opts[N] |= OPEN_NEW|OPEN_FIND, svars->state[N] |= ST_FIND_OLD;
				else
					warn( "Warning: sync record (%u,%u) has stray TUID. Ignoring.\n", srec->uid[F], srec->uid[N] );
			}
			if (srec->status & S_PURGE) {
				t = srec->uid[F] ? F : N;
				opts[t] |= OPEN_SETFLAGS;
			}
			if (srec->status & S_UPGRADE) {
				t = !srec->uid[F] ? F : N;
				opts[t] |= OPEN_APPEND;
				opts[t^1] |= OPEN_OLD;
			}
		}
	}
	svars->opts[F] = svars->drv[F]->prepare_load_box( ctx[F], opts[F] );
	svars->opts[N] = svars->drv[N]->prepare_load_box( ctx[N], opts[N] );

	ARRAY_INIT( &mexcs );
	if (svars->opts[F] & OPEN_OLD) {
		if (chan->max_messages) {
			/* When messages have been expired on the near side, the far side fetch is split into
			 * two ranges: The bulk fetch which corresponds with the most recent messages, and an
			 * exception list of messages which would have been expired if they weren't important. */
			debug( "preparing far side selection - max expired far uid is %u\n", svars->maxxfuid );
			/* First, find out the lower bound for the bulk fetch. */
			minwuid = svars->maxxfuid + 1;
			/* Next, calculate the exception fetch. */
			for (srec = svars->srecs; srec; srec = srec->next) {
				if (srec->status & S_DEAD)
					continue;
				if (!srec->uid[F])
					continue;  // No message; other state is irrelevant
				if (srec->uid[F] >= minwuid)
					continue;  // Message is in non-expired range
				if ((svars->opts[F] & OPEN_NEW) && srec->uid[F] >= svars->maxuid[F])
					continue;  // Message is in expired range, but new range overlaps that
				if (!srec->uid[N] && !(srec->status & S_PENDING))
					continue;  // Only actually paired up messages matter
				// The pair is alive, but outside the bulk range
				*uint_array_append( &mexcs ) = srec->uid[F];
			}
			sort_uint_array( mexcs.array );
		} else {
			minwuid = 1;
		}
	} else {
		minwuid = UINT_MAX;
	}
	sync_ref( svars );
	load_box( svars, F, minwuid, mexcs.array );
	if (!check_cancel( svars ))
		load_box( svars, N, (svars->opts[N] & OPEN_OLD) ? 1 : UINT_MAX, (uint_array_t){ NULL, 0 } );
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
		if (minwuid > svars->maxuid[t] + 1)
			minwuid = svars->maxuid[t] + 1;
		maxwuid = UINT_MAX;
		if (svars->opts[t] & OPEN_OLD_IDS)  // Implies OPEN_OLD
			pairuid = get_seenuid( svars, t );
	} else if (svars->opts[t] & OPEN_OLD) {
		maxwuid = get_seenuid( svars, t );
	}
	info( "Loading %s box...\n", str_fn[t] );
	svars->drv[t]->load_box( svars->ctx[t], minwuid, maxwuid, svars->finduid[t], pairuid, svars->maxuid[t], mexcs, box_loaded, AUX );
}

typedef struct {
	void *aux;
	sync_rec_t *srec;
	int aflags, dflags;
} flag_vars_t;

typedef struct {
	uint uid;
	sync_rec_t *srec;
} sync_rec_map_t;

static void flags_set( int sts, void *aux );
static void flags_set_p2( sync_vars_t *svars, sync_rec_t *srec, int t );
static void msgs_flags_set( sync_vars_t *svars, int t );
static void msg_copied( int sts, uint uid, copy_vars_t *vars );
static void msgs_copied( sync_vars_t *svars, int t );

static void
box_loaded( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux )
{
	DECL_SVARS;
	sync_rec_t *srec;
	sync_rec_map_t *srecmap;
	message_t *tmsg;
	flag_vars_t *fv;
	int no[2], del[2], alive, todel;
	uchar sflags, nflags, aflags, dflags;
	uint hashsz, idx;

	if (check_ret( sts, aux ))
		return;
	INIT_SVARS(aux);
	svars->state[t] |= ST_LOADED;
	svars->msgs[t] = msgs;
	info( "%s: %d messages, %d recent\n", str_fn[t], total_msgs, recent_msgs );

	if (svars->state[t] & ST_FIND_OLD) {
		debug( "matching previously copied messages on %s\n", str_fn[t] );
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
		while (srecmap[idx].uid)
			if (++idx == hashsz)
				idx = 0;
		srecmap[idx].uid = uid;
		srecmap[idx].srec = srec;
	}
	for (tmsg = svars->msgs[t]; tmsg; tmsg = tmsg->next) {
		if (tmsg->srec) /* found by TUID */
			continue;
		uint uid = tmsg->uid;
		idx = (uint)(uid * 1103515245U) % hashsz;
		while (srecmap[idx].uid) {
			if (srecmap[idx].uid == uid) {
				srec = srecmap[idx].srec;
				goto found;
			}
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

	svars->oldmaxuid[F] = svars->maxuid[F];
	svars->oldmaxuid[N] = svars->maxuid[N];
	svars->oldmaxxfuid = svars->maxxfuid;

	info( "Synchronizing...\n" );
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
		no[F] = !srec->msg[F] && (svars->opts[F] & OPEN_OLD);
		no[N] = !srec->msg[N] && (svars->opts[N] & OPEN_OLD);
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

			for (t = 0; t < 2; t++) {
				if (srec->msg[t] && (srec->msg[t]->flags & F_DELETED))
					srec->status |= S_DEL(t);
				if (del[t]) {
					// The target was newly expunged, so there is nothing to update.
					// The deletion is propagated in the opposite iteration.
				} else if (!srec->uid[t]) {
					// The target was never stored, or was previously expunged, so there
					// is nothing to update.
					// Note: the opposite UID must be valid, as otherwise the entry would
					// have been pruned already.
				} else if (del[t^1]) {
					// The source was newly expunged, so possibly propagate the deletion.
					// The target may be in an unknown state (not fetched).
					if ((t == F) && (srec->status & (S_EXPIRE|S_EXPIRED))) {
						/* Don't propagate deletion resulting from expiration. */
						JLOG( "> %u %u 0", (srec->uid[F], srec->uid[N]), "near side expired, orphaning far side" );
						srec->uid[N] = 0;
					} else {
						if (srec->msg[t] && (srec->msg[t]->status & M_FLAGS) &&
						    // Ignore deleted flag, as that's what we'll change ourselves ...
						    (((srec->msg[t]->flags & ~F_DELETED) != (srec->flags & ~F_DELETED)) ||
						     // ... except for undeletion, as that's the opposite.
						     (!(srec->msg[t]->flags & F_DELETED) && (srec->flags & F_DELETED))))
							notice( "Notice: conflicting changes in (%u,%u)\n", srec->uid[F], srec->uid[N] );
						if (svars->chan->ops[t] & OP_DELETE) {
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
					if (svars->chan->ops[t] & OP_FLAGS) {
						sflags = sanitize_flags( srec->msg[t^1]->flags, svars, t );
						if ((t == F) && (srec->status & (S_EXPIRE|S_EXPIRED))) {
							/* Don't propagate deletion resulting from expiration. */
							debug( "  near side expiring\n" );
							sflags &= ~F_DELETED;
						}
						if (srec->status & S_DUMMY(t^1)) {
							// For placeholders, don't propagate:
							// - Seen, because the real contents were obviously not seen yet
							// - Flagged, because it's just a request to upgrade
							sflags &= ~(F_SEEN|F_FLAGGED);
						}
						srec->aflags[t] = sflags & ~srec->flags;
						srec->dflags[t] = ~sflags & srec->flags;
						if ((DFlags & DEBUG_SYNC) && (srec->aflags[t] || srec->dflags[t])) {
							char afbuf[16], dfbuf[16]; /* enlarge when support for keywords is added */
							make_flags( srec->aflags[t], afbuf );
							make_flags( srec->dflags[t], dfbuf );
							debug( "  %sing flags: +%s -%s\n", str_hl[t], afbuf, dfbuf );
						}
					}
				}
			}

			sync_rec_t *nsrec = srec;
			if (((srec->status & S_DUMMY(F)) && (svars->chan->ops[F] & OP_RENEW)) ||
			     ((srec->status & S_DUMMY(N)) && (svars->chan->ops[N] & OP_RENEW))) {
				// Flagging the message on either side causes an upgrade of the dummy.
				// We ignore flag resets, because that corner case is not worth it.
				ushort muflags = srec->msg[F] ? srec->msg[F]->flags : 0;
				ushort suflags = srec->msg[N] ? srec->msg[N]->flags : 0;
				if ((muflags | suflags) & F_FLAGGED) {
					t = (srec->status & S_DUMMY(F)) ? F : N;
					// We calculate the flags for the replicated message already now,
					// because after an interruption the dummy may be already gone.
					srec->pflags = ((srec->msg[t]->flags & ~(F_SEEN|F_FLAGGED)) | srec->aflags[t]) & ~srec->dflags[t];
					// Consequently, the srec's flags are committed right away as well.
					srec->flags = (srec->flags | srec->aflags[t]) & ~srec->dflags[t];
					JLOG( "^ %u %u %u %u", (srec->uid[F], srec->uid[N], srec->pflags, srec->flags), "upgrading placeholder" );
					nsrec = upgrade_srec( svars, srec );
				}
			}
			// This is separated, because the upgrade can come from the journal.
			if (srec->status & S_UPGRADE) {
				t = !srec->uid[F] ? F : N;
				tmsg = srec->msg[t^1];
				if ((svars->chan->ops[t] & OP_EXPUNGE) && (srec->pflags & F_DELETED)) {
					JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "killing upgrade - would be expunged anyway" );
					tmsg->srec = NULL;
					srec->status = S_DEAD;
				} else {
					// Pretend that the source message has the adjusted flags of the dummy.
					tmsg->flags = srec->pflags;
					tmsg->status |= M_FLAGS;
					any_new[t] = 1;
				}
			}
			srec = nsrec;  // Minor optimization: skip freshly created placeholder entry.
		}
	}

	for (t = 0; t < 2; t++) {
		debug( "synchronizing new messages on %s\n", str_fn[t^1] );
		for (tmsg = svars->msgs[t^1]; tmsg; tmsg = tmsg->next) {
			srec = tmsg->srec;
			if (srec) {
				if (srec->status & S_SKIPPED) {
					// Pre-1.4 legacy only: The message was skipped due to being too big.
					// We must have already seen the UID, but we might have been interrupted.
					if (svars->maxuid[t^1] < tmsg->uid)
						svars->maxuid[t^1] = tmsg->uid;
					if (!(svars->chan->ops[t] & OP_RENEW))
						continue;
					srec->status = S_PENDING;
					// The message size was not queried, so this won't be dummified below.
					if (!(tmsg->flags & F_FLAGGED)) {
						srec->status |= S_DUMMY(t);
						JLOG( "_ %u %u", (srec->uid[F], srec->uid[N]), "placeholder only - was previously skipped" );
					} else {
						JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
						      "was previously skipped" );
					}
				} else {
					if (!(svars->chan->ops[t] & OP_NEW))
						continue;
					// This catches messages:
					// - that are actually new
					// - whose propagation got interrupted
					// - whose propagation was completed, but not logged yet
					// - that aren't actually new, but a result of syncing, and the instant
					//   maxuid upping was prevented by the presence of actually new messages
					if (svars->maxuid[t^1] < tmsg->uid)
						svars->maxuid[t^1] = tmsg->uid;
					if (!(srec->status & S_PENDING))
						continue;  // Nothing to do - the message is paired or expired
					// Propagation was scheduled, but we got interrupted
					debug( "unpropagated old message %u\n", tmsg->uid );
				}

				if ((svars->chan->ops[t] & OP_EXPUNGE) && (tmsg->flags & F_DELETED)) {
					JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "killing - would be expunged anyway" );
					tmsg->srec = NULL;
					srec->status = S_DEAD;
					continue;
				}
			} else {
				if (!(svars->chan->ops[t] & OP_NEW))
					continue;
				if (tmsg->uid <= svars->maxuid[t^1]) {
					// The message should be already paired. It's not, so it was:
					// - previously paired, but the entry was expired and pruned => ignore
					// - attempted, but failed => ignore (the wisdom of this is debatable)
					// - ignored, as it would have been expunged anyway => ignore (even if undeleted)
					continue;
				}
				svars->maxuid[t^1] = tmsg->uid;
				debug( "new message %u\n", tmsg->uid );

				if ((svars->chan->ops[t] & OP_EXPUNGE) && (tmsg->flags & F_DELETED)) {
					debug( "-> ignoring - would be expunged anyway\n" );
					continue;
				}

				srec = nfzalloc( sizeof(*srec) );
				*svars->srecadd = srec;
				svars->srecadd = &srec->next;
				svars->nsrecs++;
				srec->status = S_PENDING;
				srec->uid[t^1] = tmsg->uid;
				srec->msg[t^1] = tmsg;
				tmsg->srec = srec;
				JLOG( "+ %u %u", (srec->uid[F], srec->uid[N]), "fresh" );
			}
			if (!(tmsg->flags & F_FLAGGED) && tmsg->size > svars->chan->stores[t]->max_size &&
			    !(srec->status & (S_DUMMY(F) | S_DUMMY(N) | S_UPGRADE))) {
				srec->status |= S_DUMMY(t);
				JLOG( "_ %u %u", (srec->uid[F], srec->uid[N]), "placeholder only - too big" );
			}
			any_new[t] = 1;
		}
	}

	if ((svars->chan->ops[N] & (OP_NEW|OP_RENEW|OP_FLAGS)) && svars->chan->max_messages) {
		// Note: When this branch is entered, we have loaded all near side messages.
		/* Expire excess messages. Important (flagged, unread, or unpropagated) messages
		 * older than the first not expired message are not counted towards the total. */
		debug( "preparing message expiration\n" );
		// Due to looping only over the far side, we completely ignore unpaired
		// near-side messages. This is correct, as we cannot expire them without
		// data loss anyway; consequently, we also don't count them.
		// Note that we also ignore near-side messages we're currently propagating,
		// which delays expiration of some messages by one cycle. Otherwise, we'd have
		// to sequence flag propagation after message propagation to avoid a race
		// with 3rd-party expunging, and that seems unreasonably expensive.
		alive = 0;
		for (tmsg = svars->msgs[F]; tmsg; tmsg = tmsg->next) {
			if (tmsg->status & M_DEAD)
				continue;
			// We ignore unpaired far-side messages, as there is obviously nothing
			// to expire in the first place.
			if (!(srec = tmsg->srec))
				continue;
			if (!(srec->status & S_PENDING)) {
				if (!srec->msg[N])
					continue;  // Already expired or skipped.
				nflags = (srec->msg[N]->flags | srec->aflags[N]) & ~srec->dflags[N];
			} else {
				nflags = tmsg->flags;
			}
			if (!(nflags & F_DELETED) || (srec->status & (S_EXPIRE | S_EXPIRED))) {
				// The message is not deleted, or it is, but only due to being expired.
				alive++;
			}
		}
		todel = alive - svars->chan->max_messages;
		debug( "%d alive messages, %d excess - expiring\n", alive, todel );
		alive = 0;
		for (tmsg = svars->msgs[F]; tmsg; tmsg = tmsg->next) {
			if (tmsg->status & M_DEAD)
				continue;
			if (!(srec = tmsg->srec))
				continue;
			if (!(srec->status & S_PENDING)) {
				if (!srec->msg[N])
					continue;
				nflags = (srec->msg[N]->flags | srec->aflags[N]) & ~srec->dflags[N];
			} else {
				nflags = tmsg->flags;
			}
			if (!(nflags & F_DELETED) || (srec->status & (S_EXPIRE|S_EXPIRED))) {
				if ((nflags & F_FLAGGED) ||
				    !((nflags & F_SEEN) || ((void)(todel > 0 && alive++), svars->chan->expire_unread > 0))) {
					// Important messages are always fetched/kept.
					debug( "  pair(%u,%u) is important\n", srec->uid[F], srec->uid[N] );
					todel--;
				} else if (todel > 0 ||
				           ((srec->status & (S_EXPIRE|S_EXPIRED)) == (S_EXPIRE|S_EXPIRED)) ||
				           ((srec->status & (S_EXPIRE|S_EXPIRED)) && (srec->msg[N]->flags & F_DELETED))) {
					/* The message is excess or was already (being) expired. */
					srec->status |= S_NEXPIRE;
					debug( "  pair(%u,%u) expired\n", srec->uid[F], srec->uid[N] );
					if (svars->maxxfuid < srec->uid[F])
						svars->maxxfuid = srec->uid[F];
					todel--;
				}
			}
		}
		debug( "%d excess messages remain\n", todel );
		if (svars->chan->expire_unread < 0 && alive * 2 > svars->chan->max_messages) {
			error( "%s: %d unread messages in excess of MaxMessages (%d).\n"
			       "Please set ExpireUnread to decide outcome. Skipping mailbox.\n",
			       svars->orig_name[N], alive, svars->chan->max_messages );
			svars->ret |= SYNC_FAIL;
			cancel_sync( svars );
			return;
		}
		for (srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			if (!(srec->status & S_PENDING)) {
				if (!srec->msg[N])
					continue;
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
					JLOG( "= %u %u", (srec->uid[F], srec->uid[N]), "expire unborn" );
					// If we have so many new messages that some of them are instantly expired,
					// but some are still propagated because they are important, we need to
					// ensure explicitly that the bulk fetch limit is upped.
					if (svars->maxxfuid < srec->uid[F])
						svars->maxxfuid = srec->uid[F];
					srec->msg[F]->srec = NULL;
					srec->status = S_DEAD;
				}
			}
		}
	}

	sync_ref( svars );

	debug( "synchronizing flags\n" );
	for (srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		for (t = 0; t < 2; t++) {
			if (!srec->uid[t])
				continue;
			aflags = srec->aflags[t];
			dflags = srec->dflags[t];
			if (srec->status & (S_DELETE | S_PURGE)) {
				if (!aflags) {
					// This deletion propagation goes the other way round, or
					// this deletion of a dummy happens on the other side.
					continue;
				}
				if (!srec->msg[t] && (svars->opts[t] & OPEN_OLD)) {
					// The message disappeared. This can happen, because the status may
					// come from the journal, and things could have happened meanwhile.
					continue;
				}
			} else {
				/* The trigger is an expiration transaction being ongoing ... */
				if ((t == N) && ((shifted_bit(srec->status, S_EXPIRE, S_EXPIRED) ^ srec->status) & S_EXPIRED)) {
					// ... but the actual action derives from the wanted state -
					// so that canceled transactions are rolled back as well.
					if (srec->status & S_NEXPIRE)
						aflags |= F_DELETED;
					else
						dflags |= F_DELETED;
				}
			}
			if ((svars->chan->ops[t] & OP_EXPUNGE) && (((srec->msg[t] ? srec->msg[t]->flags : 0) | aflags) & ~dflags & F_DELETED) &&
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
	SVARS_CHECK_CANCEL_RET;
	sync_rec_t *srec = vars->srec;
	switch (sts) {
	case SYNC_OK:
		if (!(srec->status & S_UPGRADE) && vars->msg->flags != srec->flags) {
			srec->flags = vars->msg->flags;
			JLOG( "* %u %u %u", (srec->uid[F], srec->uid[N], srec->flags), "%sed with flags", str_hl[t] );
		}
		if (!uid)  // Stored to a non-UIDPLUS mailbox
			svars->state[t] |= ST_FIND_NEW;
		else
			ASSIGN_UID( srec, t, uid, "%sed message", str_hl[t] );
		break;
	case SYNC_NOGOOD:
		srec->status = S_DEAD;
		JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "%s failed", str_hl[t] );
		break;
	default:
		cancel_sync( svars );
		free( vars );
		return;
	}
	free( vars );
	new_done[t]++;
	stats();
	svars->new_pending[t]--;
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
	msgs_flags_set( svars, t );
}

static void
flags_set_p2( sync_vars_t *svars, sync_rec_t *srec, int t )
{
	if (srec->status & S_PURGE) {
		JLOG( "P %u %u", (srec->uid[F], srec->uid[N]), "deleted dummy" );
		srec->status = (srec->status & ~S_PURGE) | S_PURGED;
	} else if (srec->status & S_DELETE) {
		JLOG( "%c %u %u 0", ("><"[t], srec->uid[F], srec->uid[N]), "%sed deletion", str_hl[t] );
		srec->uid[t^1] = 0;
	} else {
		uchar nflags = (srec->flags | srec->aflags[t]) & ~srec->dflags[t];
		if (srec->flags != nflags) {
			JLOG( "* %u %u %u", (srec->uid[F], srec->uid[N], nflags), "%sed flags; were %u", (str_hl[t], srec->flags) );
			srec->flags = nflags;
		}
		if (t == N) {
			uchar nex = (srec->status / S_NEXPIRE) & 1;
			if (nex != ((srec->status / S_EXPIRED) & 1)) {
				srec->status = (srec->status & ~S_EXPIRED) | (nex * S_EXPIRED);
				JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
				      "expired %d - commit", nex );
			} else if (nex != ((srec->status / S_EXPIRE) & 1)) {
				srec->status = (srec->status & ~S_EXPIRE) | (nex * S_EXPIRE);
				JLOG( "~ %u %u %d", (srec->uid[F], srec->uid[N], srec->status & S_LOGGED),
				      "expire %d - cancel", nex );
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

	if (!(svars->chan->ops[t] & OP_EXPUNGE))
		goto skip;
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
		if (!(tmsg->flags & F_DELETED)) {
			//debug( "  message %u is not deleted\n", tmsg->uid );  // Too noisy
			continue;
		}
		debugn( "  message %u ", tmsg->uid );
		if ((srec = tmsg->srec)) {
			if (t == N && (srec->status & (S_EXPIRE | S_EXPIRED))) {
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
			if (only_new && !(srec->status & (S_PENDING | S_DUMMY(t^1) | S_SKIPPED))) {
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
	trash_vars_t *vars = (trash_vars_t *)aux;
	DECL_SVARS;

	if (sts == DRV_MSG_BAD)
		sts = DRV_BOX_BAD;
	if (check_ret( sts, vars->aux ))
		return;
	INIT_SVARS(vars->aux);
	JLOG( "T %d %u", (t, vars->msg->uid), "trashed on %s", str_fn[t] );
	free( vars );
	trash_done[t]++;
	stats();
	svars->trash_pending[t]--;
	sync_close( svars, t );
}

static void
msg_rtrashed( int sts, uint uid ATTR_UNUSED, copy_vars_t *vars )
{
	SVARS_CHECK_CANCEL_RET;
	switch (sts) {
	case SYNC_OK:
	case SYNC_NOGOOD: /* the message is gone or heavily busted */
		break;
	default:
		cancel_sync( svars );
		free( vars );
		return;
	}
	t ^= 1;
	JLOG( "T %d %u", (t, vars->msg->uid), "trashed remotely on %s", str_fn[t^1] );
	free( vars );
	trash_done[t]++;
	stats();
	svars->trash_pending[t]--;
	sync_close( svars, t );
}

static void box_closed( int sts, void *aux );
static void box_closed_p2( sync_vars_t *svars, int t );

static void
sync_close( sync_vars_t *svars, int t )
{
	if ((~svars->state[t] & (ST_FOUND_NEW|ST_SENT_TRASH)) || svars->trash_pending[t] ||
	    !(svars->state[t^1] & ST_SENT_NEW) || svars->new_pending[t^1])
		return;

	if (svars->state[t] & ST_CLOSING)
		return;
	svars->state[t] |= ST_CLOSING;

	if ((svars->chan->ops[t] & OP_EXPUNGE) /*&& !(svars->state[t] & ST_TRASH_BAD)*/) {
		debug( "expunging %s\n", str_fn[t] );
		svars->drv[t]->close_box( svars->ctx[t], box_closed, AUX );
	} else {
		box_closed_p2( svars, t );
	}
}

static void
box_closed( int sts, void *aux )
{
	SVARS_CHECK_RET;
	svars->state[t] |= ST_DID_EXPUNGE;
	box_closed_p2( svars, t );
}

static void
box_closed_p2( sync_vars_t *svars, int t )
{
	sync_rec_t *srec;

	svars->state[t] |= ST_CLOSED;
	if (!(svars->state[t^1] & ST_CLOSED))
		return;

	// All the journalling done in this function is merely for the autotest -
	// the operations are idempotent, and we're about to commit the new state
	// right afterwards anyway.

	for (t = 0; t < 2; t++) {
		// Committing maxuid is delayed until all messages were propagated, to
		// ensure that all pending messages are still loaded next time in case
		// of interruption - in particular skipping messages would otherwise
		// up the limit too early.
		if (svars->maxuid[t] != svars->oldmaxuid[t])
			JLOG( "N %d %u", (t, svars->maxuid[t]), "up maxuid of %s", str_fn[t] );
	}

	if (((svars->state[F] | svars->state[N]) & ST_DID_EXPUNGE) || svars->chan->max_messages) {
		debug( "purging obsolete entries\n" );
		for (srec = svars->srecs; srec; srec = srec->next) {
			if (srec->status & S_DEAD)
				continue;
			if (!srec->uid[N] || ((srec->status & S_DEL(N)) && (svars->state[N] & ST_DID_EXPUNGE))) {
				if (!srec->uid[F] || ((srec->status & S_DEL(F)) && (svars->state[F] & ST_DID_EXPUNGE)) ||
				    ((srec->status & S_EXPIRED) && svars->maxuid[F] >= srec->uid[F] && svars->maxxfuid >= srec->uid[F])) {
					JLOG( "- %u %u", (srec->uid[F], srec->uid[N]), "killing" );
					srec->status = S_DEAD;
				} else if (srec->uid[N]) {
					JLOG( "> %u %u 0", (srec->uid[F], srec->uid[N]), "orphaning" );
					srec->uid[N] = 0;
				}
			} else if (srec->uid[F] && ((srec->status & S_DEL(F)) && (svars->state[F] & ST_DID_EXPUNGE))) {
				JLOG( "< %u %u 0", (srec->uid[F], srec->uid[N]), "orphaning" );
				srec->uid[F] = 0;
			}
		}
	}

	// This is just an optimization, so it needs no journaling of intermediate states.
	// However, doing it before the entry purge would require ensuring that the
	// exception list includes all relevant messages.
	if (svars->maxxfuid != svars->oldmaxxfuid)
		JLOG( "! %u", svars->maxxfuid, "max expired UID on far side" );

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
