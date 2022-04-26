// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#include "imap_p.h"

#ifdef DEBUG_IMAP_MSGS
# define dbg(...) print(__VA_ARGS__)
#else
# define dbg(...) do { } while (0)
#endif

imap_message_t *
imap_new_msg( imap_messages_t *msgs )
{
	imap_message_t *msg = nfzalloc( sizeof(*msg) );
	*msgs->tail = msg;
	msgs->tail = &msg->next;
	msgs->count++;
	return msg;
}

void
reset_imap_messages( imap_messages_t *msgs )
{
	free_generic_messages( &msgs->head->gen );
	msgs->head = NULL;
	msgs->tail = &msgs->head;
	msgs->count = 0;
	msgs->cursor_ptr = NULL;
	msgs->cursor_seq = 0;
}

static int
imap_compare_msgs( const void *a_, const void *b_ )
{
	const imap_message_t *a = *(const imap_message_t * const *)a_;
	const imap_message_t *b = *(const imap_message_t * const *)b_;

	if (a->uid < b->uid)
		return -1;
	if (a->uid > b->uid)
		return 1;
	return 0;
}

void
imap_ensure_relative( imap_messages_t *msgs )
{
	if (msgs->cursor_ptr)
		return;
	uint count = msgs->count;
	if (!count)
		return;
	if (count > 1) {
		imap_message_t **t = nfmalloc( sizeof(*t) * count );

		imap_message_t *m = msgs->head;
		for (uint i = 0; i < count; i++) {
			t[i] = m;
			m = m->next;
		}

		qsort( t, count, sizeof(*t), imap_compare_msgs );

		imap_message_t *nm = t[0];
		msgs->head = nm;
		nm->prev = NULL;
		uint seq, nseq = nm->seq;
		for (uint j = 0; m = nm, seq = nseq, j < count - 1; j++) {
			nm = t[j + 1];
			m->next = nm;
			m->next->prev = m;
			nseq = nm->seq;
			nm->seq = nseq - seq;
		}
		msgs->tail = &m->next;
		*msgs->tail = NULL;

		free( t );
	}
	msgs->cursor_ptr = msgs->head;
	msgs->cursor_seq = msgs->head->seq;
}

void
imap_ensure_absolute( imap_messages_t *msgs )
{
	if (!msgs->cursor_ptr)
		return;
	uint seq = 0;
	for (imap_message_t *msg = msgs->head; msg; msg = msg->next) {
		seq += msg->seq;
		msg->seq = seq;
	}
	msgs->cursor_ptr = NULL;
	msgs->cursor_seq = 0;
}

imap_message_t *
imap_expunge_msg( imap_messages_t *msgs, uint fseq )
{
	dbg( "expunge %u\n", fseq );
	imap_ensure_relative( msgs );
	imap_message_t *ret = NULL, *msg = msgs->cursor_ptr;
	if (msg) {
		uint seq = msgs->cursor_seq;
		for (;;) {
			dbg( "  now on message %u (uid %u), %sdead\n", seq, msg->uid, (msg->status & M_DEAD) ? "" : "not " );
			if (seq == fseq && !(msg->status & M_DEAD)) {
				dbg( "  => expunging\n" );
				msg->status = M_DEAD;
				ret = msg;
				break;
			}
			if (seq < fseq) {
				dbg( "    is below\n" );
				if (!msg->next) {
					dbg( "    no next\n" );
					goto done;
				}
				msg = msg->next;
				seq += msg->seq;
			} else {
				dbg( "    is not below\n" );
				if (!msg->prev) {
					dbg( "    no prev\n" );
					break;
				}
				uint pseq = seq - msg->seq;
				if (pseq < fseq) {
					dbg( "    prev too low\n" );
					break;
				}
				seq = pseq;
				msg = msg->prev;
			}
		}
		dbg( "  => lowering\n" );
		assert( msg->seq );
		msg->seq--;
		seq--;
	  done:
		dbg( "  saving cursor on %u (uid %u)\n", seq, msg->uid );
		msgs->cursor_ptr = msg;
		msgs->cursor_seq = seq;
	} else {
		dbg( "  => no messages\n" );
	}
	return ret;
}
