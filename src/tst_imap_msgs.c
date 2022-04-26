// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later
//
// isync test suite
//

#include "imap_p.h"

static imap_messages_t smsgs;

// from driver.c
void
free_generic_messages( message_t *msgs )
{
	message_t *tmsg;

	for (; msgs; msgs = tmsg) {
		tmsg = msgs->next;
		// free( msgs->msgid );
		free( msgs );
	}
}

static void
dump_messages( void )
{
	print( "=>" );
	uint seq = 0;
	for (imap_message_t *msg = smsgs.head; msg; msg = msg->next) {
		seq += msg->seq;
		if (msg->status & M_DEAD)
			print( " (%u:%u)", seq, msg->uid );
		else
			print( " %u:%u", seq, msg->uid );
	}
	print( "\n" );
}

static void
init( uint *in )
{
	reset_imap_messages( &smsgs );
	for (; *in; in++) {
		imap_message_t *msg = imap_new_msg( &smsgs );
		msg->seq = *in;
		// We (ab)use the initial sequence number as the UID. That's not
		// exactly realistic, but it's valid, and saves us redundant data.
		msg->uid = *in;
	}
}

static void
modify( uint *in )
{
	for (; *in; in++) {
		imap_expunge_msg( &smsgs, *in );
#ifdef DEBUG_IMAP_MSGS
		dump_messages();
#endif
	}
}

static void
verify( uint *in, const char *name )
{
	int fails = 0;
	imap_message_t *msg = smsgs.head;
	for (;;) {
		if (msg && *in && msg->uid == *in) {
			if (msg->status & M_DEAD) {
				printf( "*** %s: message %u is dead\n", name, msg->uid );
				fails++;
			} else {
				assert( msg->seq );
			}
			msg = msg->next;
			in++;
		} else if (*in && (!msg || msg->uid > *in)) {
			printf( "*** %s: message %u is missing\n", name, *in );
			fails++;
			in++;
		} else if (msg) {
			if (!(msg->status & M_DEAD)) {
				printf( "*** %s: excess message %u\n", name, msg->uid );
				fails++;
			}
			msg = msg->next;
		} else {
			assert( !*in );
			break;
		}
	}
	if (fails)
		dump_messages();
}

static void
test( uint *ex, uint *out, const char *name )
{
	printf( "test %s ...\n", name );
	modify( ex );
	verify( out, name );
}

int
main( void )
{
	static uint arr_0[] = { 0 };
	static uint arr_1[] = { 1, 0 };

	static uint full_in[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0 };
	init( full_in );
#if 0
	static uint nop[] = { 0 };
	static uint nop_out[] = { /* 1, */ 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, /* 17, */ 18 /*!*/, 0 };
	test( nop, nop_out, "self-test" );
#endif
	static uint full_ex_fw1[] = { 18, 13, 13, 13, 1, 1, 1, 0 };
	static uint full_out_fw1[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 17, 0 };
	test( full_ex_fw1, full_out_fw1, "full, forward 1" );
	static uint full_ex_fw2[] = { 10, 10, 0 };
	static uint full_out_fw2[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 0 };
	test( full_ex_fw2, full_out_fw2, "full, forward 2" );

	init( full_in );
	static uint full_ex_bw1[] = { 18, 17, 16, 15, 14, 13, 5, 4, 3, 0 };
	static uint full_out_bw1[] = { 1, 2, 6, 7, 8, 9, 10, 11, 12, 0 };
	test( full_ex_bw1, full_out_bw1, "full, backward 1" );
	static uint full_ex_bw2[] = { 2, 1, 0 };
	static uint full_out_bw2[] = { 6, 7, 8, 9, 10, 11, 12, 0 };
	test( full_ex_bw2, full_out_bw2, "full, backward 2" );

	static uint hole_wo1_in[] = { 10, 11, 12, 20, 21, 31, 32, 33, 34, 35, 36, 37, 0 };
	init( hole_wo1_in );
	static uint hole_wo1_ex_1[] = { 31, 30, 29, 28, 22, 21, 11, 2, 1, 0 };
	static uint hole_wo1_out_1[] = { 10, 12, 20, 32, 33, 34, 35, 36, 37, 0 };
	test( hole_wo1_ex_1, hole_wo1_out_1, "hole w/o 1, backward" );

	init( hole_wo1_in );
	static uint hole_wo1_ex_2[] = { 1, 1, 9, 18, 18, 23, 23, 23, 23, 0 };
	test( hole_wo1_ex_2, hole_wo1_out_1, "hole w/o 1, forward" );
	test( arr_1, hole_wo1_out_1, "hole w/o 1, forward 2" );
	static uint hole_wo1_ex_4[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
	static uint hole_wo1_out_4[] = { 37, 0 };
	test( hole_wo1_ex_4, hole_wo1_out_4, "hole w/o 1, forward 3" );
	test( arr_1, arr_0, "hole w/o 1, forward 4" );
	test( arr_1, arr_0, "hole w/o 1, forward 5" );

	static uint hole_w1_in[] = { 1, 10, 11, 12, 0 };
	init( hole_w1_in );
	static uint hole_w1_ex_1[] = { 11, 10, 2, 1, 0 };
	static uint hole_w1_out_1[] = { 12, 0 };
	test( hole_w1_ex_1, hole_w1_out_1, "hole w/ 1, backward" );
	test( arr_1, hole_w1_out_1, "hole w/ 1, backward 2" );

	init( hole_w1_in );
	static uint hole_w1_ex_2[] = { 1, 1, 8, 8, 0 };
	test( hole_w1_ex_2, hole_w1_out_1, "hole w/ 1, forward" );
	static uint hole_w1_ex_4[] = { 1, 1, 1, 1, 1, 1, 1, 0 };
	static uint hole_w1_out_4[] = { 12, 0 };
	test( hole_w1_ex_4, hole_w1_out_4, "hole w/ 1, forward 2" );
	test( arr_1, arr_0, "hole w/ 1, forward 3" );
	test( arr_1, arr_0, "hole w/ 1, forward 4" );

	return 0;
}
