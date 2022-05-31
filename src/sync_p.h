// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#define DEBUG_FLAG DEBUG_SYNC

#include "sync.h"
#include "sync_p_enum.h"

// This is the (mostly) persistent status of the sync record.
// Most of these bits are actually mutually exclusive. It is a
// bitfield to allow for easy testing for multiple states.
BIT_ENUM(
	S_DEAD,         // ephemeral: the entry was killed and should be ignored
	S_EXPIRE,       // the entry is being expired (near side message removal scheduled)
	S_EXPIRED,      // the entry is expired (near side message removal confirmed)
	S_PENDING,      // the entry is new and awaits propagation (possibly a retry)
	S_DUMMY(2),     // f/n message is only a placeholder
	S_SKIPPED,      // pre-1.4 legacy: the entry was not propagated (message is too big)
)

// Ephemeral working set.
BIT_ENUM(
	W_NEXPIRE,      // temporary: new expiration state
	W_DELETE,       // ephemeral: flags propagation is a deletion
	W_DEL(2),       // ephemeral: f/n message would be subject to expunge
	W_UPGRADE,      // ephemeral: upgrading placeholder, do not apply MaxSize
	W_PURGE,        // ephemeral: placeholder is being nuked
)

typedef struct sync_rec {
	struct sync_rec *next;
	/* string_list_t *keywords; */
	uint uid[2];
	message_t *msg[2];
	uchar status, wstate, flags, pflags, aflags[2], dflags[2];
	char tuid[TUIDL];
} sync_rec_t;

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
	int state[2], lfd, ret, existing, replayed;
	uint ref_count, nsrecs, opts[2];
	uint new_pending[2], flags_pending[2], trash_pending[2];
	uint maxuid[2];     // highest UID that was already propagated
	uint oldmaxuid[2];  // highest UID that was already propagated before this run
	uint uidval[2];     // UID validity value
	uint newuidval[2];  // UID validity obtained from driver
	uint finduid[2];    // TUID lookup makes sense only for UIDs >= this
	uint maxxfuid;      // highest expired UID on far side
	uint oldmaxxfuid;   // highest expired UID on far side before this run
	uchar good_flags[2], bad_flags[2];
} sync_vars_t;

int prepare_state( sync_vars_t *svars );
int lock_state( sync_vars_t *svars );
int load_state( sync_vars_t *svars );
void save_state( sync_vars_t *svars );
void delete_state( sync_vars_t *svars );

void ATTR_PRINTFLIKE(2, 3) jFprintf( sync_vars_t *svars, const char *msg, ... );

#define JLOG_(log_fmt, log_args, dbg_fmt, ...) \
	do { \
		debug( "-> log: " log_fmt " (" dbg_fmt ")\n", __VA_ARGS__ ); \
		jFprintf( svars, log_fmt "\n", deparen(log_args) ); \
	} while (0)
#define JLOG3(log_fmt, log_args, dbg_fmt) \
	JLOG_(log_fmt, log_args, dbg_fmt, deparen(log_args))
#define JLOG4(log_fmt, log_args, dbg_fmt, dbg_args) \
	JLOG_(log_fmt, log_args, dbg_fmt, deparen(log_args), deparen(dbg_args))
#define JLOG_SEL(_1, _2, _3, _4, x, ...) x
#define JLOG(...) JLOG_SEL(__VA_ARGS__, JLOG4, JLOG3, NO_JLOG2, NO_JLOG1)(__VA_ARGS__)

void assign_uid( sync_vars_t *svars, sync_rec_t *srec, int t, uint uid );

#define ASSIGN_UID(srec, t, nuid, ...) \
	do { \
		JLOG( "%c %u %u %u", ("<>"[t], srec->uid[F], srec->uid[N], nuid), __VA_ARGS__ ); \
		assign_uid( svars, srec, t, nuid ); \
	} while (0)

void assign_tuid( sync_vars_t *svars, sync_rec_t *srec );
int match_tuids( sync_vars_t *svars, int t, message_t *msgs );

sync_rec_t *upgrade_srec( sync_vars_t *svars, sync_rec_t *srec );
