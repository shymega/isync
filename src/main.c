// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#include "main_p.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#ifdef __linux__
# include <sys/prctl.h>
#endif

static void ATTR_NORETURN
version( void )
{
	puts( PACKAGE " " VERSION );
	exit( 0 );
}

static void ATTR_NORETURN
usage( int code )
{
	fputs(
PACKAGE " " VERSION " - mailbox synchronizer\n"
"Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>\n"
"Copyright (C) 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>\n"
"Copyright (C) 2004 Theodore Ts'o <tytso@mit.edu>\n"
"usage:\n"
" " EXE " [flags] {{channel[:box,...]|group} ...|-a}\n"
"  -a, --all		operate on all defined channels\n"
"  -l, --list		list mailboxes instead of syncing them\n"
"  -ls, --list-stores	raw listing of stores' mailboxes\n"
"  -n, --new		propagate new messages\n"
"  -d, --delete		propagate message deletions\n"
"  -f, --flags		propagate message flag changes\n"
"  -u, --upgrade		upgrade placeholders to full messages\n"
"  -L, --pull		propagate from far to near side\n"
"  -H, --push		propagate from near to far side\n"
"  -C, --create		propagate creations of mailboxes\n"
"  -R, --remove		propagate deletions of mailboxes\n"
"  -X, --expunge		expunge	deleted messages\n"
"  -c, --config CONFIG	read an alternate config file (default: ~/." EXE "rc)\n"
"  -D, --debug		debugging modes (see manual)\n"
"  -V, --verbose		display what is happening\n"
"  -q, --quiet		don't display progress counters\n"
"  -v, --version		display version\n"
"  -h, --help		display this help message\n"
"\nIf neither --pull nor --push are specified, both are active.\n"
"If neither --new, --delete, --flags, nor --upgrade are specified, all are\n"
"active. Direction and operation can be concatenated like --pull-new, etc.\n"
"--create, --remove, and --expunge can be suffixed with -far/-near.\n"
"See the man page for details.\n"
"\nSupported mailbox formats are: IMAP4rev1, Maildir\n"
"\nCompile time options:\n"
#ifdef HAVE_LIBSSL
"  +HAVE_LIBSSL"
#else
"  -HAVE_LIBSSL"
#endif
#ifdef HAVE_LIBSASL
" +HAVE_LIBSASL"
#else
" -HAVE_LIBSASL"
#endif
#ifdef HAVE_LIBZ
" +HAVE_LIBZ"
#else
" -HAVE_LIBZ"
#endif
#ifdef USE_DB
" +USE_DB"
#else
" -USE_DB"
#endif
#ifdef HAVE_IPV6
" +HAVE_IPV6\n"
#else
" -HAVE_IPV6\n"
#endif
	, code ? stderr : stdout );
	exit( code );
}

#ifdef __linux__
static void ATTR_NORETURN
crashHandler( int n )
{
	int dpid;
	char pbuf[10], pabuf[20];

	close( 0 );
	open( "/dev/tty", O_RDWR );
	dup2( 0, 1 );
	dup2( 0, 2 );
	error( "*** " EXE " caught signal %d. Starting debugger ...\n", n );
#ifdef PR_SET_PTRACER
	int pip[2];
	if (pipe( pip ) < 0) {
		perror( "pipe()" );
		exit( 3 );
	}
#endif
	switch ((dpid = fork())) {
	case -1:
		perror( "fork()" );
		break;
	case 0:
#ifdef PR_SET_PTRACER
		close( pip[1] );
		read( pip[0], pbuf, 1 );
		close( pip[0] );
#endif
		sprintf( pbuf, "%d", Pid );
		sprintf( pabuf, "/proc/%d/exe", Pid );
		execlp( "gdb", "gdb", pabuf, pbuf, (char *)0 );
		perror( "execlp()" );
		_exit( 1 );
	default:
#ifdef PR_SET_PTRACER
		prctl( PR_SET_PTRACER, (ulong)dpid );
		close( pip[1] );
		close( pip[0] );
#endif
		waitpid( dpid, NULL, 0 );
		break;
	}
	exit( 3 );
}
#endif

void
countStep( void )
{
	if (!--JLimit)
		exit( 100 );
}

int
main( int argc, char **argv )
{
	core_vars_t mvars[1];
	char *config = NULL, *opt, *ochar;
	int oind, cops = 0, op, ms_warn = 0, renew_warn = 0;

	tzset();
	gethostname( Hostname, sizeof(Hostname) );
	if ((ochar = strchr( Hostname, '.' )))
		*ochar = 0;
	Pid = getpid();
	if (!(Home = getenv("HOME"))) {
		fputs( "Fatal: $HOME not set\n", stderr );
		return 1;
	}
	arc4_init();

	memset( mvars, 0, sizeof(*mvars) );

	for (oind = 1, ochar = NULL; ; ) {
		if (!ochar || !*ochar) {
			if (oind >= argc)
				break;
			if (argv[oind][0] != '-')
				break;
			if (argv[oind][1] == '-') {
				opt = argv[oind++] + 2;
				if (!*opt)
					break;
				if (!strcmp( opt, "config" )) {
					if (oind >= argc) {
						error( "--config requires an argument.\n" );
						return 1;
					}
					config = argv[oind++];
				} else if (starts_with( opt, -1, "config=", 7 )) {
					config = opt + 7;
				} else if (!strcmp( opt, "all" )) {
					mvars->all = 1;
				} else if (!strcmp( opt, "list" )) {
					mvars->list = 1;
				} else if (!strcmp( opt, "list-stores" )) {
					mvars->list_stores = 1;
				} else if (!strcmp( opt, "help" )) {
					usage( 0 );
				} else if (!strcmp( opt, "version" )) {
					version();
				} else if (!strcmp( opt, "quiet" )) {
					if (Verbosity > VERYQUIET)
						Verbosity--;
				} else if (!strcmp( opt, "verbose" )) {
					Verbosity = VERBOSE;
				} else if (starts_with( opt, -1, "debug", 5 )) {
					opt += 5;
					if (!*opt)
						op = DEBUG_ALL;
					else if (!strcmp( opt, "-crash" ))
						op = DEBUG_CRASH;
					else if (!strcmp( opt, "-driver" ))
						op = DEBUG_DRV;
					else if (!strcmp( opt, "-driver-all" ))
						op = DEBUG_DRV | DEBUG_DRV_ALL;
					else if (!strcmp( opt, "-maildir" ))
						op = DEBUG_MAILDIR;
					else if (!strcmp( opt, "-main" ))
						op = DEBUG_MAIN;
					else if (!strcmp( opt, "-net" ))
						op = DEBUG_NET;
					else if (!strcmp( opt, "-net-all" ))
						op = DEBUG_NET | DEBUG_NET_ALL;
					else if (!strcmp( opt, "-sync" ))
						op = DEBUG_SYNC;
					else
						goto badopt;
					DFlags |= op;
				} else if (!strcmp( opt, "pull" )) {
					cops |= XOP_PULL, mvars->ops[F] |= XOP_HAVE_TYPE;
				} else if (!strcmp( opt, "push" )) {
					cops |= XOP_PUSH, mvars->ops[F] |= XOP_HAVE_TYPE;
				} else if (starts_with( opt, -1, "create", 6 )) {
					opt += 6;
					op = OP_CREATE|XOP_HAVE_CREATE;
				  lcop:
					if (!*opt)
						cops |= op;
					else if (!strcmp( opt, "-far" ))
						mvars->ops[F] |= op;
					else if (!strcmp( opt, "-master" ))  // Pre-1.4 legacy
						mvars->ops[F] |= op, ms_warn = 1;
					else if (!strcmp( opt, "-near" ))
						mvars->ops[N] |= op;
					else if (!strcmp( opt, "-slave" ))  // Pre-1.4 legacy
						mvars->ops[N] |= op, ms_warn = 1;
					else
						goto badopt;
					mvars->ops[F] |= op & (XOP_HAVE_CREATE | XOP_HAVE_REMOVE | XOP_HAVE_EXPUNGE);
				} else if (starts_with( opt, -1, "remove", 6 )) {
					opt += 6;
					op = OP_REMOVE|XOP_HAVE_REMOVE;
					goto lcop;
				} else if (starts_with( opt, -1, "expunge", 7 )) {
					opt += 7;
					op = OP_EXPUNGE|XOP_HAVE_EXPUNGE;
					goto lcop;
				} else if (!strcmp( opt, "no-expunge" )) {
					mvars->ops[F] |= XOP_EXPUNGE_NOOP | XOP_HAVE_EXPUNGE;
				} else if (!strcmp( opt, "no-create" )) {
					mvars->ops[F] |= XOP_CREATE_NOOP | XOP_HAVE_CREATE;
				} else if (!strcmp( opt, "no-remove" )) {
					mvars->ops[F] |= XOP_REMOVE_NOOP | XOP_HAVE_REMOVE;
				} else if (!strcmp( opt, "full" )) {
					mvars->ops[F] |= XOP_HAVE_TYPE | XOP_PULL | XOP_PUSH;
				} else if (!strcmp( opt, "noop" )) {
					mvars->ops[F] |= XOP_TYPE_NOOP | XOP_HAVE_TYPE;
				} else if (starts_with( opt, -1, "pull", 4 )) {
					op = XOP_PULL;
				  lcac:
					opt += 4;
					if (!*opt) {
						cops |= op;
					} else if (*opt == '-') {
						opt++;
						goto rlcac;
					} else {
						goto badopt;
					}
				} else if (starts_with( opt, -1, "push", 4 )) {
					op = XOP_PUSH;
					goto lcac;
				} else {
					op = 0;
				  rlcac:
					if (!strcmp( opt, "new" )) {
						op |= OP_NEW;
					} else if (!strcmp( opt, "upgrade" )) {
						op |= OP_UPGRADE;
					} else if (!strcmp( opt, "renew" )) {
						renew_warn = 1;
						op |= OP_UPGRADE;
					} else if (!strcmp( opt, "delete" )) {
						op |= OP_DELETE;
					} else if (!strcmp( opt, "flags" )) {
						op |= OP_FLAGS;
					} else {
					  badopt:
						error( "Unknown option '%s'\n", argv[oind - 1] );
						return 1;
					}
					switch (op & XOP_MASK_DIR) {
					case XOP_PULL: mvars->ops[N] |= op & OP_MASK_TYPE; break;
					case XOP_PUSH: mvars->ops[F] |= op & OP_MASK_TYPE; break;
					default: cops |= op; break;
					}
					mvars->ops[F] |= XOP_HAVE_TYPE;
				}
				continue;
			}
			ochar = argv[oind++] + 1;
			if (!*ochar) {
				error( "Invalid option '-'\n" );
				return 1;
			}
		}
		switch (*ochar++) {
		case 'a':
			mvars->all = 1;
			break;
		case 'l':
			if (*ochar == 's')
				mvars->list_stores = 1, ochar++;
			else
				mvars->list = 1;
			break;
		case 'c':
			if (oind >= argc) {
				error( "-c requires an argument.\n" );
				return 1;
			}
			config = argv[oind++];
			break;
		case 'C':
			op = OP_CREATE|XOP_HAVE_CREATE;
		  cop:
			if (*ochar == 'f')
				mvars->ops[F] |= op, ochar++;
			else if (*ochar == 'm')  // Pre-1.4 legacy
				mvars->ops[F] |= op, ms_warn = 1, ochar++;
			else if (*ochar == 'n')
				mvars->ops[N] |= op, ochar++;
			else if (*ochar == 's')  // Pre-1.4 legacy
				mvars->ops[N] |= op, ms_warn = 1, ochar++;
			else if (*ochar == '-')
				ochar++;
			else
				cops |= op;
			mvars->ops[F] |= op & (XOP_HAVE_CREATE | XOP_HAVE_REMOVE | XOP_HAVE_EXPUNGE);
			break;
		case 'R':
			op = OP_REMOVE|XOP_HAVE_REMOVE;
			goto cop;
		case 'X':
			op = OP_EXPUNGE|XOP_HAVE_EXPUNGE;
			goto cop;
		case 'F':
			cops |= XOP_PULL|XOP_PUSH;
			mvars->ops[F] |= XOP_HAVE_TYPE;
			break;
		case '0':
			mvars->ops[F] |= XOP_TYPE_NOOP | XOP_HAVE_TYPE;
			break;
		case 'n':
		case 'd':
		case 'f':
		case 'N':
		case 'u':
			--ochar;
			op = 0;
		  cac:
			for (;; ochar++) {
				if (*ochar == 'n')
					op |= OP_NEW;
				else if (*ochar == 'd')
					op |= OP_DELETE;
				else if (*ochar == 'f')
					op |= OP_FLAGS;
				else if (*ochar == 'u')
					op |= OP_UPGRADE;
				else if (*ochar == 'N')
					op |= OP_UPGRADE, renew_warn = 1;
				else
					break;
			}
			if (op & OP_MASK_TYPE) {
				switch (op & XOP_MASK_DIR) {
				case XOP_PULL: mvars->ops[N] |= op & OP_MASK_TYPE; break;
				case XOP_PUSH: mvars->ops[F] |= op & OP_MASK_TYPE; break;
				default: cops |= op; break;
				}
			} else {
				cops |= op;
			}
			mvars->ops[F] |= XOP_HAVE_TYPE;
			break;
		case 'L':
			op = XOP_PULL;
			goto cac;
		case 'H':
			op = XOP_PUSH;
			goto cac;
		case 'q':
			if (Verbosity > VERYQUIET)
				Verbosity--;
			break;
		case 'V':
			Verbosity = VERBOSE;
			break;
		case 'D':
			for (op = 0; *ochar; ochar++) {
				switch (*ochar) {
				case 'C':
					op |= DEBUG_CRASH;
					break;
				case 'd':
					op |= DEBUG_DRV;
					break;
				case 'D':
					op |= DEBUG_DRV | DEBUG_DRV_ALL;
					break;
				case 'm':
					op |= DEBUG_MAILDIR;
					break;
				case 'M':
					op |= DEBUG_MAIN;
					break;
				case 'n':
					op |= DEBUG_NET;
					break;
				case 'N':
					op |= DEBUG_NET | DEBUG_NET_ALL;
					break;
				case 's':
					op |= DEBUG_SYNC;
					break;
				default:
					error( "Unknown -D flag '%c'\n", *ochar );
					return 1;
				}
			}
			if (!op)
				op = DEBUG_ALL;
			DFlags |= op;
			break;
		case 'T':
			for (; *ochar; ) {
				switch (*ochar++) {
				case 'a':
					DFlags |= FORCEASYNC(F);
					break;
				case 'A':
					DFlags |= FORCEASYNC(F) | FORCEASYNC(N);
					break;
				case 'j':
					DFlags |= KEEPJOURNAL;
					break;
				case 'J':
					DFlags |= FORCEJOURNAL;
					break;
				case 's':
					JLimit = strtol( ochar, &ochar, 10 );
					break;
				case 'x':
					DFlags |= FAKEEXPUNGE;
					break;
				case 'z':
					DFlags |= ZERODELAY;
					break;
				default:
					error( "Unknown -T flag '%c'\n", *(ochar - 1) );
					return 1;
				}
			}
			break;
		case 'v':
			version();
		case 'h':
			usage( 0 );
		default:
			error( "Unknown option '-%c'\n", *(ochar - 1) );
			return 1;
		}
	}
	if (ms_warn)
		warn( "Notice: -master/-slave/m/s suffixes are deprecated; use -far/-near/f/n instead.\n" );
	if (renew_warn)
		warn( "Notice: --renew/-N are deprecated; use --upgrade/-u instead.\n" );

	if (DFlags & DEBUG_ANY) {
		Verbosity = VERBOSE;

		fputs( PACKAGE " " VERSION " called with:", stdout );
		for (op = 1; op < argc; op++)
			printf( " '%s'", argv[op] );
		puts( "" );
	} else if (Verbosity >= TERSE && isatty( 1 )) {
		DFlags |= PROGRESS;
	}

#ifdef __linux__
	if (DFlags & DEBUG_CRASH) {
		signal( SIGSEGV, crashHandler );
		signal( SIGBUS, crashHandler );
		signal( SIGILL, crashHandler );
	}
#endif

	if (merge_ops( cops, mvars->ops, NULL ))
		return 1;

	if (load_config( config ))
		return 1;

	if (mvars->list_stores)
		list_stores( mvars, argv + oind );
	else
		sync_chans( mvars, argv + oind );
	return mvars->ret;
}
