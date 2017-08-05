// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#ifndef SYNC_H
#define SYNC_H

#include "driver.h"
#include "sync_enum.h"

#define F 0  // far side
#define N 1  // near side

BIT_ENUM(
	OP_NEW,
	OP_OLD,
	OP_UPGRADE,
	OP_GONE,
	OP_FLAGS,
	OP_EXPUNGE,
	OP_EXPUNGE_SOLO,
	OP_CREATE,
	OP_REMOVE,

	XOP_PUSH,
	XOP_PULL,
	XOP_HAVE_TYPE,  // Aka mode; have at least one of dir and type (see below)
	// The following must all have the same bit shift from the corresponding OP_* flags.
	XOP_HAVE_EXPUNGE,
	XOP_HAVE_EXPUNGE_SOLO,
	XOP_HAVE_CREATE,
	XOP_HAVE_REMOVE,
	// ... until here.
	XOP_TYPE_NOOP,
	// ... and here again from scratch.
	XOP_EXPUNGE_NOOP,
	XOP_EXPUNGE_SOLO_NOOP,
	XOP_CREATE_NOOP,
	XOP_REMOVE_NOOP,
)

#define OP_DFLT_TYPE (OP_NEW | OP_UPGRADE | OP_GONE | OP_FLAGS)
#define OP_MASK_TYPE (OP_DFLT_TYPE | OP_OLD)  // Asserted in the target side ops
#define XOP_MASK_DIR (XOP_PUSH | XOP_PULL)

DECL_BIT_FORMATTER_FUNCTION(ops, OP)

typedef struct channel_conf {
	struct channel_conf *next;
	const char *name;
	store_conf_t *stores[2];
	const char *boxes[2];
	const char *sync_state;
	string_list_t *patterns;
	int ops[2];
	int max_messages;  // For near side only.
	int expire_side;
	signed char expire_unread;
	char use_internal_date;
	uint max_line_len;
	char cut_lines;
} channel_conf_t;

typedef struct group_conf {
	struct group_conf *next;
	const char *name;
	string_list_t *channels;
} group_conf_t;

extern channel_conf_t global_conf;
extern channel_conf_t *channels;
extern group_conf_t *groups;

extern uint BufferLimit;

extern int new_total[2], new_done[2];
extern int flags_total[2], flags_done[2];
extern int trash_total[2], trash_done[2];
extern int expunge_total[2], expunge_done[2];

extern const char *str_fn[2], *str_hl[2];

#define SYNC_OK       0 /* assumed to be 0 */
#define SYNC_FAIL     1
#define SYNC_BAD(fn)  (4<<(fn))

#define BOX_POSSIBLE -1
#define BOX_ABSENT    0
#define BOX_PRESENT   1

/* All passed pointers must stay alive until cb is called. */
void sync_boxes( store_t *ctx[], const char * const names[], int present[], channel_conf_t *chan,
                 void (*cb)( int sts, void *aux ), void *aux );

#endif
