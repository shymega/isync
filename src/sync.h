// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#ifndef SYNC_H
#define SYNC_H

#include "driver.h"

#define F 0  // far side
#define N 1  // near side

#define OP_NEW             (1<<0)
#define OP_RENEW           (1<<1)
#define OP_DELETE          (1<<2)
#define OP_FLAGS           (1<<3)
#define  OP_MASK_TYPE      (OP_NEW|OP_RENEW|OP_DELETE|OP_FLAGS) /* asserted in the target ops */
#define OP_EXPUNGE         (1<<4)
#define OP_CREATE          (1<<5)
#define OP_REMOVE          (1<<6)
#define XOP_PUSH           (1<<8)
#define XOP_PULL           (1<<9)
#define  XOP_MASK_DIR      (XOP_PUSH|XOP_PULL)
#define XOP_HAVE_TYPE      (1<<10)  // Aka mode; at least one of dir and type
// The following must all have the same bit shift from the corresponding OP_* flags.
#define XOP_HAVE_EXPUNGE   (1<<11)
#define XOP_HAVE_CREATE    (1<<12)
#define XOP_HAVE_REMOVE    (1<<13)

typedef struct channel_conf {
	struct channel_conf *next;
	const char *name;
	store_conf_t *stores[2];
	const char *boxes[2];
	char *sync_state;
	string_list_t *patterns;
	int ops[2];
	int max_messages;  // For near side only.
	signed char expire_unread;
	char use_internal_date;
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

extern const char *str_fn[2], *str_hl[2];

#define SYNC_OK       0 /* assumed to be 0 */
#define SYNC_FAIL     1
#define SYNC_BAD(fn)  (4<<(fn))
#define SYNC_NOGOOD   16 /* internal */
#define SYNC_CANCELED 32 /* internal */

#define BOX_POSSIBLE -1
#define BOX_ABSENT    0
#define BOX_PRESENT   1

/* All passed pointers must stay alive until cb is called. */
void sync_boxes( store_t *ctx[], const char * const names[], int present[], channel_conf_t *chan,
                 void (*cb)( int sts, void *aux ), void *aux );

#endif
