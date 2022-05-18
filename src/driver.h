// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#ifndef DRIVER_H
#define DRIVER_H

#include "config.h"
#include "driver_enum.h"

typedef struct driver driver_t;

#define FAIL_TEMP   0  /* Retry immediately (also: no error) */
#define FAIL_WAIT   1  /* Retry after some time (if at all) */
#define FAIL_FINAL  2  /* Don't retry until store reconfiguration */

#define STORE_CONF \
	struct store_conf *next; \
	char *name; \
	driver_t *driver; \
	const char *flat_delim; \
	const char *map_inbox; \
	const char *trash; \
	uint max_size;  /* off_t is overkill */ \
	char trash_remote_new, trash_only_new;

typedef struct store_conf {
	STORE_CONF
} store_conf_t;

extern store_conf_t *stores;

/* For message->flags */
// Keep the MESSAGE_FLAGS in sync (grep that)!
/* The order is according to alphabetical maildir flag sort */
BIT_ENUM(
	F_DRAFT,      // Draft
	F_FLAGGED,    // Flagged
	F_FORWARDED,  // Passed
	F_ANSWERED,   // Replied
	F_SEEN,       // Seen
	F_DELETED,    // Trashed
)

extern const char MsgFlags[F__NUM_BITS];
void make_flags( uchar flags, char *buf );

/* For message->status */
BIT_ENUM(
	M_RECENT,   // unsyncable flag; maildir_*() depend on this being bit 0
	M_DEAD,     // expunged
	M_FLAGS,    // flags are valid
	// The following are only for IMAP FETCH response parsing
	M_DATE,
	M_SIZE,
	M_BODY,
	M_HEADER,
)

#define TUIDL 12

#define MESSAGE(message) \
	message *next; \
	struct sync_rec *srec; \
	char *msgid;  /* owned */ \
	/* string_list_t *keywords; */ \
	uint size;  /* zero implies "not fetched" */ \
	uint uid; \
	uchar flags, status; \
	char tuid[TUIDL];

typedef struct message {
	MESSAGE(struct message)
} message_t;

static_assert_bits(F, message_t, flags);
static_assert_bits(M, message_t, status);

// For driver_t->prepare_load_box(), which may amend the passed flags.
// The drivers don't use the first two, but may set them if loading the
// particular range is required to handle some other flag; note that these
// ranges may overlap.
BIT_ENUM(
	OPEN_OLD,         // Paired messages *in* this store.
	OPEN_NEW,         // Messages (possibly) not yet propagated *from* this store.
	OPEN_FIND,
	OPEN_FLAGS,       // Note that fetch_msg() gets the flags regardless.
	OPEN_NEW_SIZE,
	OPEN_OLD_IDS,
	OPEN_APPEND,
	OPEN_SETFLAGS,
	OPEN_EXPUNGE,
)

#define UIDVAL_BAD ((uint)-1)

#define STORE(store) \
	store *next; \
	driver_t *driver; \
	store##_conf *conf;  /* foreign */

typedef struct store {
	STORE(struct store)
} store_t;

typedef struct {
	char *data;
	uint len;
	time_t date;
	uchar flags;
} msg_data_t;

static_assert_bits(F, msg_data_t, flags);

#define DRV_OK          0
/* Message went missing, or mailbox is full, etc. */
#define DRV_MSG_BAD     1
/* Something is wrong with the current mailbox - probably it is somehow inaccessible. */
#define DRV_BOX_BAD     2
/* Failed to connect store. */
#define DRV_STORE_BAD   3
/* The command has been cancel_cmds()d or cancel_store()d. */
#define DRV_CANCELED    4

/* All memory belongs to the driver's user, unless stated otherwise. */
// If the driver is NOT DRV_ASYNC, memory owned by the driver returned
// through callbacks MUST remain valid until a related subsequent command
// is invoked, as the proxy driver may deliver these pointers with delay.

/*
   This flag says that the driver CAN store messages with CRLFs,
   not that it must. The lack of it OTOH implies that it CANNOT,
   and as CRLF is the canonical format, we convert.
*/
#define DRV_CRLF        1
/*
   This flag says that the driver will act upon (Verbosity >= VERBOSE).
*/
#define DRV_VERBOSE     2
/*
   This flag says that the driver operates asynchronously.
*/
#define DRV_ASYNC       4

#define LIST_INBOX      1
#define LIST_PATH       2
#define LIST_PATH_MAYBE 4

#define xint uint  // For auto-generation of appropriate printf() formats.

struct driver {
	/* Return driver capabilities. */
	xint (*get_caps)( store_t *ctx );

	/* Parse configuration. */
	int (*parse_store)( conffile_t *cfg, store_conf_t **storep );

	/* Close remaining server connections. All stores must be discarded first. */
	void (*cleanup)( void );

	/* Allocate a store with the given configuration. This is expected to
	 * return quickly, and must not fail. */
	store_t *(*alloc_store)( store_conf_t *conf, const char *label );

	/* When this callback is invoked (at most once per store), the store is fubar;
	 * call cancel_store() to dispose of it. */
	void (*set_bad_callback)( store_t *ctx, void (*cb)( void *aux ), void *aux );

	/* Open/connect the store. This may recycle existing server connections. */
	void (*connect_store)( store_t *ctx,
	                       void (*cb)( int sts, void *aux ), void *aux );

	/* Discard the store. Underlying server connection may be kept alive. */
	void (*free_store)( store_t *ctx );

	/* Discard the store after a bad_callback. The server connections will be closed.
	 * Pending commands will have their callbacks synchronously invoked with DRV_CANCELED. */
	void (*cancel_store)( store_t *ctx );

	/* List the mailboxes in this store. Flags are ORed LIST_* values.
	 * The returned box list remains owned by the driver. */
	void (*list_store)( store_t *ctx, int flags,
	                    void (*cb)( int sts, string_list_t *boxes, void *aux ), void *aux );

	/* Invoked before open_box(), this informs the driver which box is to be opened. */
	int (*select_box)( store_t *ctx, const char *name );

	/* Get the selected box' on-disk path, if applicable, null otherwise. */
	const char *(*get_box_path)( store_t *ctx );

	/* Create the selected mailbox. */
	void (*create_box)( store_t *ctx,
	                    void (*cb)( int sts, void *aux ), void *aux );

	/* Open the selected mailbox.
	 * Note that this should not directly complain about failure to open. */
	void (*open_box)( store_t *ctx,
	                  void (*cb)( int sts, uint uidvalidity, void *aux ), void *aux );

	/* Return the minimal UID the next stored message will have. */
	uint (*get_uidnext)( store_t *ctx );

	/* Return the flags that can be stored in the selected mailbox. */
	xint (*get_supported_flags)( store_t *ctx );

	/* Confirm that the open mailbox is empty. */
	int (*confirm_box_empty)( store_t *ctx );

	/* Delete the open mailbox. The mailbox is expected to be empty.
	 * Subfolders of the mailbox are *not* deleted.
	 * Some artifacts of the mailbox may remain, but they won't be
	 * recognized as a mailbox any more. */
	void (*delete_box)( store_t *ctx,
	                    void (*cb)( int sts, void *aux ), void *aux );

	/* Remove the last artifacts of the open mailbox, as far as possible. */
	int (*finish_delete_box)( store_t *ctx );

	/* Invoked before load_box(), this informs the driver which operations (OP_*)
	 * will be performed on the mailbox. The driver may extend the set by implicitly
	 * needed or available operations. Returns this possibly extended set. */
	xint (*prepare_load_box)( store_t *ctx, xint opts );

	/* Load the message attributes needed to perform the requested operations.
	 * Consider only messages with UIDs between minuid and maxuid (inclusive)
	 * and those named in the excs array (smaller than minuid).
	 * The driver takes ownership of the excs array.
	 * Messages starting with finduid need to have the TUID populated when OPEN_FIND is set.
	 * Messages up to pairuid need to have the Message-Id populated when OPEN_OLD_IDS is set.
	 * Messages up to newuid need to have the size populated when OPEN_OLD_SIZE is set;
	 * likewise messages above newuid when OPEN_NEW_SIZE is set.
	 * The returned message list remains owned by the driver. */
	void (*load_box)( store_t *ctx, uint minuid, uint maxuid, uint finduid, uint pairuid, uint newuid, uint_array_t excs,
	                  void (*cb)( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux ), void *aux );

	/* Fetch the contents and flags of the given message from the current mailbox.
	 * If minimal is non-zero, fetch only a placeholder for the requested message -
	 * ideally, this is precisely the header, but it may be more. */
	void (*fetch_msg)( store_t *ctx, message_t *msg, msg_data_t *data, int minimal,
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Store the given message to either the current mailbox or the trash folder.
	 * If the new copy's UID can be immediately determined, return it, otherwise 0. */
	void (*store_msg)( store_t *ctx, msg_data_t *data, int to_trash,
	                   void (*cb)( int sts, uint uid, void *aux ), void *aux );

	/* Index the messages which have newly appeared in the mailbox, including their
	 * temporary UID headers. This is needed if store_msg() does not guarantee returning
	 * a UID; otherwise the driver needs to implement only the OPEN_FIND flag.
	 * The returned message list remains owned by the driver. */
	void (*find_new_msgs)( store_t *ctx, uint newuid,
	                       void (*cb)( int sts, message_t *msgs, void *aux ), void *aux );

	/* Add/remove the named flags to/from the given message. The message may be either
	 * a pre-fetched one (in which case the in-memory representation is updated),
	 * or it may be identifed by UID only.
	 * The operation may be delayed until commit_cmds() is called. */
	void (*set_msg_flags)( store_t *ctx, message_t *msg, uint uid, int add, int del,
	                       void (*cb)( int sts, void *aux ), void *aux );

	/* Move the given message from the current mailbox to the trash folder.
	 * This may expunge the original message immediately, but it needn't to. */
	void (*trash_msg)( store_t *ctx, message_t *msg,
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Expunge deleted messages from the current mailbox and close it.
	 * There is no need to explicitly close a mailbox if no expunge is needed. */
	void (*close_box)( store_t *ctx,
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Cancel queued commands which are not in flight yet; they will have their
	 * callbacks invoked with DRV_CANCELED. Afterwards, wait for the completion of
	 * the in-flight commands. If the store is canceled before this command completes,
	 * the callback will *not* be invoked. */
	void (*cancel_cmds)( store_t *ctx,
	                     void (*cb)( void *aux ), void *aux );

	/* Commit any pending set_msg_flags() commands. */
	void (*commit_cmds)( store_t *ctx );

	/* Get approximate amount of memory occupied by the driver. */
	uint (*get_memory_usage)( store_t *ctx );

	/* Get the FAIL_* state of the driver. */
	int (*get_fail_state)( store_conf_t *conf );
};

uint count_generic_messages( message_t * );
void free_generic_messages( message_t * );

void parse_generic_store( store_conf_t *store, conffile_t *cfg, const char *type );

store_t *proxy_alloc_store( store_t *real_ctx, const char *label );

#define N_DRIVERS 2
extern driver_t *drivers[N_DRIVERS];
extern driver_t maildir_driver, imap_driver, proxy_driver;

void cleanup_drivers( void );

#endif
