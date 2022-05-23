// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#ifndef IMAP_P_H
#define IMAP_P_H

#include "driver.h"

//#define DEBUG_IMAP_MSGS
//#define DEBUG_IMAP_UTF7

typedef union imap_message {
	message_t gen;
	struct {
		MESSAGE(union imap_message)

		union imap_message *prev;  // Used to optimize lookup by seq.
		// This is made relative once the fetches complete - to avoid that
		// each expunge re-enumerates all subsequent messages. Dead messages
		// "occupy" no sequence number themselves, but may still jump a gap.
		// Note that use of sequence numbers to address messages in commands
		// imposes limitations on permissible pipelining. We don't do that,
		// so this is of no concern; however, we might miss the closing of
		// a gap, which would result in a tiny performance hit.
		uint seq;
	};
} imap_message_t;

typedef struct {
	imap_message_t *head;
	imap_message_t **tail;
	// Bulk changes (which is where performance matters) are assumed to be
	// reported sequentially (be it forward or reverse), so walking the
	// sorted linked list from the previously used message is efficient.
	imap_message_t *cursor_ptr;
	uint cursor_seq;
	uint count;
} imap_messages_t;

imap_message_t *imap_new_msg( imap_messages_t *msgs );
imap_message_t *imap_expunge_msg( imap_messages_t *msgs, uint fseq );
void reset_imap_messages( imap_messages_t *msgs );
void imap_ensure_relative( imap_messages_t *msgs );
void imap_ensure_absolute( imap_messages_t *msgs );

char *imap_utf8_to_utf7( const char *buf );
int imap_utf7_to_utf8( const char *buf, int argl, char *outbuf );

#endif
