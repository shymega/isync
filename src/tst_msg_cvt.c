// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later
//
// isync test suite
//

#include "sync_p.h"

#define TUID "one two tuid"

static_assert( sizeof(TUID) - 1 == TUIDL, "TUID size mismatch" );

static size_t
strip_cr( char *buf )
{
	size_t i, j = 0;
	char c, pc = 0;
	for (i = 0; (c = buf[i]); i++) {
		if (c == '\n' && pc == '\r')
			j--;
		buf[j++] = c;
		pc = c;
	}
	buf[j] = 0;
	return j;
}

#define NL_UNIX 0
#define NL_ANY 1

#define AS_IS 0
#define ADD_TUID 1

#define FULL 0
#define MINIMAL 1

#define REGULAR 0
#define FLAGGED 1

#define BIG_SIZE 2345687
#define BIG_SIZE_STR "2.2MiB"

#define SEP "============="

static void
test( const char *name, const char *in, int scr, int rscr, const char *out, int tcr, int rtcr, int add_tuid, int minimal, int flagged )
{
	assert( !rscr || scr );
	assert( !rtcr || tcr );
	assert( !minimal || add_tuid );
	assert( !flagged || minimal );

	printf( "Testing %s, %s (%s) => %s (%s)%s%s%s ...\n", name,
	        rscr ? "CRLF" : "LF", scr ? "Any" : "Unix", rtcr ? "CRLF" : "LF", tcr ? "Any" : "Unix",
	        add_tuid ? ", add TUID" : "", minimal ? ", minimal" : "", flagged ? ", flagged" : "" );

	sync_rec_t srec;
	message_t msg;
	copy_vars_t vars;
	vars.minimal = minimal;
	if (add_tuid) {
		vars.srec = &srec;
		memcpy( vars.srec->tuid, TUID, TUIDL );
		if (minimal) {
			vars.msg = &msg;
			vars.msg->size = BIG_SIZE;
			vars.data.flags = flagged ? F_FLAGGED : 0;
		}
	} else {
		vars.srec = 0;
	}
	vars.data.data = strdup( in );
	vars.data.len = rscr ? strlen( in ) : strip_cr( vars.data.data );
	char *orig = strdup( vars.data.data );
	const char *err = copy_msg_convert( scr, tcr, &vars );
	if (err) {
		printf( "FAIL: %s!\n", err );
		exit( 1 );
	}
	if (!rtcr) {
		char *tout = strdup( out );
		size_t toutl = strip_cr( tout );
		if (toutl != vars.data.len || memcmp( vars.data.data, tout, toutl )) {
			xprintf( "FAIL!\n"
			         SEP " Input " SEP "\n%!&s\n"
			         SEP " Expected output " SEP "\n%!&s\n"
			         SEP " Output " SEP "\n%.*!&s\n" SEP "\n",
			         orig, tout, vars.data.len, vars.data.data );
			exit( 1 );
		}
		free( tout );
	} else {
		size_t outl = strlen( out );
		if (outl != vars.data.len || memcmp( vars.data.data, out, outl )) {
			xprintf( "FAIL!\n"
			         SEP " Input " SEP "\n%!&s\n"
			         SEP " Expected output (%u bytes) " SEP "\n%!&s\n"
			         SEP " Actual output (%u bytes) " SEP "\n%.*!&s\n" SEP "\n",
			         orig, outl, out, vars.data.len, vars.data.len, vars.data.data );
			exit( 1 );
		}
	}
	free( orig );
	free( vars.data.data );
}

static void
tests( const char *name, const char *in, const char *out, int add_tuid, int minimal, int flagged )
{
	test( name, in, NL_UNIX, NL_UNIX, out, NL_ANY, NL_ANY, add_tuid, minimal, flagged );
	test( name, in, NL_ANY, NL_UNIX, out, NL_UNIX, NL_UNIX, add_tuid, minimal, flagged );
	test( name, in, NL_ANY, NL_ANY, out, NL_UNIX, NL_UNIX, add_tuid, minimal, flagged );
	// Skip if (scr == tcr && !srec), like copy_msg() does.
	if (add_tuid) {
		test( name, in, NL_UNIX, NL_UNIX, out, NL_UNIX, NL_UNIX, ADD_TUID, minimal, flagged );
		test( name, in, NL_ANY, NL_UNIX, out, NL_ANY, NL_UNIX, ADD_TUID, minimal, flagged );
		test( name, in, NL_ANY, NL_ANY, out, NL_ANY, NL_ANY, ADD_TUID, minimal, flagged );
	}
}

static void
fulltests( const char *name, const char *in, const char *out, int add_tuid )
{
	tests( name, in, out, add_tuid, FULL, REGULAR );
}

static void
mintests( const char *name, const char *in, const char *out, int flagged )
{
	tests( name, in, out, ADD_TUID, MINIMAL, flagged );
}

#define FROM "From: de\rvil\r\n"
#define R_TO "To: me"
#define TO R_TO "\r\n"
#define R_IN_TUID "X-TUID: garbage"
#define IN_TUID R_IN_TUID "\r\n"
#define OUT_TUID "X-TUID: " TUID "\r\n"
#define R_SUBJECT "Subject: hell"
#define SUBJECT R_SUBJECT "\r\n"
#define PH_SUBJECT "Subject: [placeholder] hell\r\n"
#define NO_SUBJECT "Subject: [placeholder] (No Subject)\r\n"
#define BODY "\r\nHi,\r\n\r\n...\r\n"
#define PH_BODY "\r\nHaving a size of 2.2MiB, this message is over the MaxSize limit.\r\n" \
	"Flag it and sync again (Sync mode Upgrade) to fetch its real contents.\r\n"
#define FLAGGED_PH_BODY PH_BODY "\r\nThe original message is flagged as important.\r\n"

#define scc static const char

int
main( void )
{
	scc in_from_to[] = FROM TO BODY;
	fulltests( "from / to", in_from_to, in_from_to, AS_IS );
	scc out_from_to[] = FROM TO OUT_TUID BODY;
	fulltests( "from / to", in_from_to, out_from_to, ADD_TUID );
	scc in_from_tuid_to[] = FROM IN_TUID TO BODY;
	scc out_from_tuid_to[] = FROM OUT_TUID TO BODY;
	fulltests( "from / tuid / to", in_from_tuid_to, out_from_tuid_to, ADD_TUID );

	scc out_from_to_ph[] = FROM TO OUT_TUID NO_SUBJECT PH_BODY;
	mintests( "from / to", in_from_to, out_from_to_ph, REGULAR );
	scc out_from_to_flagged_ph[] = FROM TO OUT_TUID NO_SUBJECT FLAGGED_PH_BODY;
	mintests( "from / to", in_from_to, out_from_to_flagged_ph, FLAGGED );
	scc out_from_tuid_to_ph[] = FROM OUT_TUID TO NO_SUBJECT PH_BODY;
	mintests( "from / tuid / to", in_from_tuid_to, out_from_tuid_to_ph, REGULAR );
	scc in_from_subj_to[] = FROM SUBJECT TO BODY;
	scc out_from_subj_to[] = FROM PH_SUBJECT TO OUT_TUID PH_BODY;
	mintests( "from / subject / to", in_from_subj_to, out_from_subj_to, REGULAR );
	scc in_from_subj_tuid_to[] = FROM SUBJECT IN_TUID TO BODY;
	scc out_from_subj_tuid_to[] = FROM PH_SUBJECT OUT_TUID TO PH_BODY;
	mintests( "from / subject / tuid / to", in_from_subj_tuid_to, out_from_subj_tuid_to, REGULAR );
	scc in_subj_from_tuid_to[] = SUBJECT FROM IN_TUID TO BODY;
	scc out_subj_from_tuid_to[] = PH_SUBJECT FROM OUT_TUID TO PH_BODY;
	mintests( "subject / from / tuid / to", in_subj_from_tuid_to, out_subj_from_tuid_to, REGULAR );
	scc in_from_tuid_subj_to[] = FROM IN_TUID SUBJECT TO BODY;
	scc out_from_tuid_subj_to[] = FROM OUT_TUID PH_SUBJECT TO PH_BODY;
	mintests( "from / tuid / subject / to", in_from_tuid_subj_to, out_from_tuid_subj_to, REGULAR );
	scc in_tuid_from_subj_to[] = IN_TUID FROM SUBJECT TO BODY;
	scc out_tuid_from_subj_to[] = OUT_TUID FROM PH_SUBJECT TO PH_BODY;
	mintests( "tuid / from / subject / to", in_tuid_from_subj_to, out_tuid_from_subj_to, REGULAR );


	scc in_from_to_b1[] = FROM TO;
	fulltests( "from / to w/o end", in_from_to_b1, in_from_to_b1, AS_IS );
	scc out_from_to_b1[] = FROM TO OUT_TUID;
	fulltests( "from / to w/o end", in_from_to_b1, out_from_to_b1, ADD_TUID );
	scc in_from_tuid_to_b1[] = FROM IN_TUID TO;
	scc out_from_tuid_to_b1[] = FROM OUT_TUID TO;
	fulltests( "from / tuid / to w/o end", in_from_tuid_to_b1, out_from_tuid_to_b1, ADD_TUID );
	scc in_from_to_tuid_b1[] = FROM TO IN_TUID;
	scc out_from_to_tuid_b1[] = FROM TO OUT_TUID;
	fulltests( "from / to / tuid w/o end", in_from_to_tuid_b1, out_from_to_tuid_b1, ADD_TUID );

	mintests( "from / to w/o end", in_from_to_b1, out_from_to_ph, REGULAR );
	mintests( "from / tuid / to w/o end", in_from_tuid_to_b1, out_from_tuid_to_ph, REGULAR );
	scc in_from_subj_to_b1[] = FROM SUBJECT TO;
	mintests( "from / subject / to w/o end", in_from_subj_to_b1, out_from_subj_to, REGULAR );
	scc in_from_subj_tuid_to_b1[] = FROM SUBJECT IN_TUID TO;
	mintests( "from / subject / tuid / to w/o end", in_from_subj_tuid_to_b1, out_from_subj_tuid_to, REGULAR );
	scc in_from_subj_to_tuid_b1[] = FROM SUBJECT TO IN_TUID;
	scc out_from_subj_to_tuid_b1[] = FROM PH_SUBJECT TO OUT_TUID PH_BODY;
	mintests( "from / subject / to / tuid w/o end", in_from_subj_to_tuid_b1, out_from_subj_to_tuid_b1, REGULAR );
	scc in_from_tuid_subj_to_b1[] = FROM IN_TUID SUBJECT TO;
	mintests( "from / tuid / subject / to w/o end", in_from_tuid_subj_to_b1, out_from_tuid_subj_to, REGULAR );
	scc in_from_tuid_to_subj_b1[] = FROM IN_TUID TO SUBJECT;
	scc out_from_tuid_to_subj_b1[] = FROM OUT_TUID TO PH_SUBJECT PH_BODY;
	mintests( "from / tuid / to / subject w/o end", in_from_tuid_to_subj_b1, out_from_tuid_to_subj_b1, REGULAR );


	scc in_from_to_b2[] = FROM R_TO "\r";
	fulltests( "from / to w/o lf", in_from_to_b2, in_from_to_b2, AS_IS );
	scc out_from_to_b2[] = FROM TO OUT_TUID "\r";
	fulltests( "from / to w/o lf", in_from_to_b2, out_from_to_b2, ADD_TUID );
	scc in_from_tuid_to_b2[] = FROM IN_TUID R_TO "\r";
	scc out_from_tuid_to_b2[] = FROM OUT_TUID R_TO "\r";
	fulltests( "from / tuid / to w/o lf", in_from_tuid_to_b2, out_from_tuid_to_b2, ADD_TUID );
	scc in_from_to_tuid_b2[] = FROM TO R_IN_TUID "\r";
	fulltests( "from / to / tuid w/o lf", in_from_to_tuid_b2, out_from_to_tuid_b1, ADD_TUID );

	mintests( "from / to w/o lf", in_from_to_b2, out_from_to_ph, REGULAR );
	mintests( "from / tuid / to w/o lf", in_from_tuid_to_b2, out_from_tuid_to_ph, REGULAR );
	scc in_from_subj_to_b2[] = FROM SUBJECT R_TO "\r";
	mintests( "from / subject / to w/o lf", in_from_subj_to_b2, out_from_subj_to, REGULAR );
	scc in_from_subj_tuid_to_b2[] = FROM SUBJECT IN_TUID R_TO "\r";
	mintests( "from / subject / tuid / to w/o lf", in_from_subj_tuid_to_b2, out_from_subj_tuid_to, REGULAR );
	scc in_from_subj_to_tuid_b2[] = FROM SUBJECT TO R_IN_TUID "\r";
	mintests( "from / subject / to / tuid w/o lf", in_from_subj_to_tuid_b2, out_from_subj_to_tuid_b1, REGULAR );
	scc in_from_tuid_subj_to_b2[] = FROM IN_TUID SUBJECT R_TO "\r";
	mintests( "from / tuid / subject / to w/o lf", in_from_tuid_subj_to_b2, out_from_tuid_subj_to, REGULAR );
	scc in_from_tuid_to_subj_b2[] = FROM IN_TUID TO R_SUBJECT "\r";
	mintests( "from / tuid / to / subject w/o lf", in_from_tuid_to_subj_b2, out_from_tuid_to_subj_b1, REGULAR );


	scc in_from_to_b3[] = FROM R_TO;
	fulltests( "from / to w/o crlf", in_from_to_b3, in_from_to_b3, AS_IS );
	fulltests( "from / to w/o crlf", in_from_to_b3, out_from_to_b1, ADD_TUID );
	scc in_from_tuid_to_b3[] = FROM IN_TUID R_TO;
	scc out_from_tuid_to_b3[] = FROM OUT_TUID R_TO;
	fulltests( "from / tuid / to w/o crlf", in_from_tuid_to_b3, out_from_tuid_to_b3, ADD_TUID );
	scc in_from_to_tuid_b3[] = FROM TO R_IN_TUID;
	fulltests( "from / to / tuid w/o crlf", in_from_to_tuid_b3, out_from_to_tuid_b1, ADD_TUID );

	mintests( "from / to w/o crlf", in_from_to_b3, out_from_to_ph, REGULAR );
	mintests( "from / tuid / to w/o crlf", in_from_tuid_to_b3, out_from_tuid_to_ph, REGULAR );
	scc in_from_subj_to_b3[] = FROM SUBJECT R_TO;
	mintests( "from / subject / to w/o crlf", in_from_subj_to_b3, out_from_subj_to, REGULAR );
	scc in_from_subj_tuid_to_b3[] = FROM SUBJECT IN_TUID R_TO;
	mintests( "from / subject / tuid / to w/o crlf", in_from_subj_tuid_to_b3, out_from_subj_tuid_to, REGULAR );
	scc in_from_subj_to_tuid_b3[] = FROM SUBJECT TO R_IN_TUID;
	mintests( "from / subject / to / tuid w/o crlf", in_from_subj_to_tuid_b3, out_from_subj_to_tuid_b1, REGULAR );
	scc in_from_tuid_subj_to_b3[] = FROM IN_TUID SUBJECT R_TO;
	mintests( "from / tuid / subject / to w/o crlf", in_from_tuid_subj_to_b3, out_from_tuid_subj_to, REGULAR );
	scc in_from_tuid_to_subj_b3[] = FROM IN_TUID TO R_SUBJECT;
	mintests( "from / tuid / to / subject w/o crlf", in_from_tuid_to_subj_b3, out_from_tuid_to_subj_b1, REGULAR );

	return 0;
}
