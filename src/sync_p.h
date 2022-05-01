// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#define DEBUG_FLAG DEBUG_SYNC

#include "sync.h"
#include "sync_p_enum.h"

BIT_ENUM(
	S_DEAD,         // ephemeral: the entry was killed and should be ignored
	S_EXPIRE,       // the entry is being expired (near side message removal scheduled)
	S_EXPIRED,      // the entry is expired (near side message removal confirmed)
	S_NEXPIRE,      // temporary: new expiration state
	S_PENDING,      // the entry is new and awaits propagation (possibly a retry)
	S_DUMMY(2),     // f/n message is only a placeholder
	S_SKIPPED,      // pre-1.4 legacy: the entry was not propagated (message is too big)
	S_GONE(2),      // ephemeral: f/n message has been expunged
	S_DEL(2),       // ephemeral: f/n message would be subject to non-selective expunge
	S_DELETE,       // ephemeral: flags propagation is a deletion
	S_UPGRADE,      // ephemeral: upgrading placeholder, do not apply MaxSize
	S_PURGE,        // ephemeral: placeholder is being nuked
	S_PURGED,       // ephemeral: placeholder was nuked
)

// This is the persistent status of the sync record, with regard to the journal.
#define S_LOGGED (S_EXPIRE | S_EXPIRED | S_PENDING | S_DUMMY(F) | S_DUMMY(N) | S_SKIPPED)

typedef struct sync_rec {
	struct sync_rec *next;
	/* string_list_t *keywords; */
	uint uid[2];
	message_t *msg[2];
	ushort status;
	uchar flags, pflags, aflags[2], dflags[2];
	char tuid[TUIDL];
} sync_rec_t;

static_assert_bits(F, sync_rec_t, flags);
static_assert_bits(S, sync_rec_t, status);

typedef struct {
	int t[2];
	void (*cb)( int sts, void *aux ), *aux;
	char *dname, *jname, *nname, *lname, *box_name[2];
	FILE *jfp, *nfp;
	sync_rec_t *srecs, **srecadd;
	channel_conf_t *chan;
	store_t *ctx[2];
	driver_t *drv[2];
	const char *orig_name[2];
	message_t *msgs[2], *new_msgs[2];
	uint_array_alloc_t trashed_msgs[2];
	int state[2], lfd, ret, existing, replayed, any_expiring;
	uint ref_count, nsrecs, opts[2];
	uint new_pending[2], flags_pending[2], trash_pending[2];
	uint maxuid[2];     // highest UID that was already propagated
	uint oldmaxuid[2];  // highest UID that was already propagated before this run
	uint newmaxuid[2];  // highest UID that is currently being propagated
	uint uidval[2];     // UID validity value
	uint newuidval[2];  // UID validity obtained from driver
	uint finduid[2];    // TUID lookup makes sense only for UIDs >= this
	uint maxxfuid;      // highest expired UID on far side
	uchar good_flags[2], bad_flags[2], can_crlf[2];
} sync_vars_t;

int prepare_state( sync_vars_t *svars );
int lock_state( sync_vars_t *svars );
int load_state( sync_vars_t *svars );
void save_state( sync_vars_t *svars );
void delete_state( sync_vars_t *svars );

void ATTR_PRINTFLIKE(2, 3) jFprintf( sync_vars_t *svars, const char *msg, ... );

#define JLOG_(pre_commit, log_fmt, log_args, dbg_fmt, ...) \
	do { \
		if (pre_commit && !(DFlags & FORCEJOURNAL)) { \
			debug( "-> (log: " log_fmt ") (" dbg_fmt ")\n", __VA_ARGS__ ); \
		} else { \
			debug( "-> log: " log_fmt " (" dbg_fmt ")\n", __VA_ARGS__ ); \
			jFprintf( svars, log_fmt "\n", deparen(log_args) ); \
		} \
	} while (0)
#define JLOG3(pre_commit, log_fmt, log_args, dbg_fmt) \
	JLOG_(pre_commit, log_fmt, log_args, dbg_fmt, deparen(log_args))
#define JLOG4(pre_commit, log_fmt, log_args, dbg_fmt, dbg_args) \
	JLOG_(pre_commit, log_fmt, log_args, dbg_fmt, deparen(log_args), deparen(dbg_args))
#define JLOG_SEL(_1, _2, _3, _4, x, ...) x
#define JLOG(...) JLOG_SEL(__VA_ARGS__, JLOG4, JLOG3, NO_JLOG2, NO_JLOG1)(0, __VA_ARGS__)
#define PC_JLOG(...) JLOG_SEL(__VA_ARGS__, JLOG4, JLOG3, NO_JLOG2, NO_JLOG1)(1, __VA_ARGS__)

void assign_uid( sync_vars_t *svars, sync_rec_t *srec, int t, uint uid );

#define ASSIGN_UID(srec, t, nuid, ...) \
	do { \
		JLOG( "%c %u %u %u", ("<>"[t], srec->uid[F], srec->uid[N], nuid), __VA_ARGS__ ); \
		assign_uid( svars, srec, t, nuid ); \
	} while (0)

void assign_tuid( sync_vars_t *svars, sync_rec_t *srec );
int match_tuids( sync_vars_t *svars, int t, message_t *msgs );

sync_rec_t *upgrade_srec( sync_vars_t *svars, sync_rec_t *srec, int t );

typedef struct copy_vars {
	void (*cb)( int sts, uint uid, struct copy_vars *vars );
	void *aux;
	sync_rec_t *srec; /* also ->tuid */
	message_t *msg;
	msg_data_t data;
	int minimal;
} copy_vars_t;

char *copy_msg_convert( int in_cr, int out_cr, copy_vars_t *vars );
