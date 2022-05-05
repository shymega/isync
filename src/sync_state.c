// SPDX-FileCopyrightText: 2004-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#define DEBUG_FLAG DEBUG_SYNC

#include "sync_p.h"

#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#define JOURNAL_VERSION "5"

const char *str_fn[] = { "far side", "near side" }, *str_hl[] = { "push", "pull" };

BIT_FORMATTER_FUNCTION(sts, S)

static char *
clean_strdup( const char *s )
{
	char *cs = nfstrdup( s );
	for (uint i = 0; cs[i]; i++)
		if (cs[i] == '/')
			cs[i] = '!';
	return cs;
}

int
prepare_state( sync_vars_t *svars )
{
	channel_conf_t *chan = svars->chan;
	if (!strcmp( chan->sync_state ? chan->sync_state : global_conf.sync_state, "*" )) {
		const char *path = svars->drv[N]->get_box_path( svars->ctx[N] );
		if (!path) {
			error( "Error: store '%s' does not support in-box sync state\n", chan->stores[N]->name );
			return 0;
		}
		nfasprintf( &svars->dname, "%s/." EXE "state", path );
	} else {
		char *cnname = clean_strdup( svars->box_name[N] );
		if (chan->sync_state) {
			nfasprintf( &svars->dname, "%s%s", chan->sync_state, cnname );
		} else {
			char c = FieldDelimiter;
			char *cfname = clean_strdup( svars->box_name[F] );
			nfasprintf( &svars->dname, "%s%c%s%c%s_%c%s%c%s", global_conf.sync_state,
			            c, chan->stores[F]->name, c, cfname, c, chan->stores[N]->name, c, cnname );
			free( cfname );
		}
		free( cnname );
		char *s;
		if (!(s = strrchr( svars->dname, '/' ))) {
			error( "Error: invalid SyncState location '%s'\n", svars->dname );
			return 0;
		}
		// Note that this may be shorter than the configuration value,
		// as that may contain a filename prefix.
		*s = 0;
		if (mkdir_p( svars->dname, s - svars->dname )) {
			sys_error( "Error: cannot create SyncState directory '%s'", svars->dname );
			return 0;
		}
		*s = '/';
	}
	nfasprintf( &svars->jname, "%s.journal", svars->dname );
	nfasprintf( &svars->nname, "%s.new", svars->dname );
	nfasprintf( &svars->lname, "%s.lock", svars->dname );
	return 1;
}

int
lock_state( sync_vars_t *svars )
{
	struct flock lck;

	if (DFlags & DRYRUN)
		return 1;

	if (svars->lfd >= 0)
		return 1;
	memset( &lck, 0, sizeof(lck) );
#if SEEK_SET != 0
	lck.l_whence = SEEK_SET;
#endif
#if F_WRLCK != 0
	lck.l_type = F_WRLCK;
#endif
	if ((svars->lfd = open( svars->lname, O_WRONLY | O_CREAT, 0666 )) < 0) {
		sys_error( "Error: cannot create lock file %s", svars->lname );
		return 0;
	}
	if (fcntl( svars->lfd, F_SETLK, &lck )) {
		error( "Error: channel :%s:%s-:%s:%s is locked\n",
		       svars->chan->stores[F]->name, svars->orig_name[F], svars->chan->stores[N]->name, svars->orig_name[N] );
		close( svars->lfd );
		svars->lfd = -1;
		return 0;
	}
	return 1;
}

static uchar
parse_flags( const char *buf )
{
	uchar flags = 0;
	for (uint i = 0, d = 0; i < as(MsgFlags); i++) {
		if (buf[d] == MsgFlags[i]) {
			flags |= (1 << i);
			d++;
		}
	}
	return flags;
}

int
load_state( sync_vars_t *svars )
{
	sync_rec_t *srec, *nsrec;
	FILE *jfp;
	uint ll;
	uint maxxnuid = 0;
	char fbuf[16];  // enlarge when support for keywords is added
	char buf[128], buf1[64], buf2[64];

	if ((jfp = fopen( svars->dname, "r" ))) {
		if (!lock_state( svars ))
			goto jbail;
		debug( "reading sync state %s ...\n", svars->dname );
		int line = 0;
		while (fgets( buf, sizeof(buf), jfp )) {
			line++;
			if (!(ll = strlen( buf )) || buf[ll - 1] != '\n') {
				error( "Error: incomplete sync state header entry at %s:%d\n", svars->dname, line );
			  jbail:
				fclose( jfp );
				return 0;
			}
			if (ll == 1)
				goto gothdr;
			if (line == 1 && isdigit( buf[0] )) {  // Pre-1.1 legacy
				if (sscanf( buf, "%63s %63s", buf1, buf2 ) != 2 ||
				    sscanf( buf1, "%u:%u", &svars->uidval[F], &svars->maxuid[F] ) < 2 ||
				    sscanf( buf2, "%u:%u:%u", &svars->uidval[N], &maxxnuid, &svars->maxuid[N] ) < 3) {
					error( "Error: invalid sync state header in %s\n", svars->dname );
					goto jbail;
				}
				goto gothdr;
			}
			uint uid;
			if (sscanf( buf, "%63s %u", buf1, &uid ) != 2) {
				error( "Error: malformed sync state header entry at %s:%d\n", svars->dname, line );
				goto jbail;
			}
			if (!strcmp( buf1, "FarUidValidity" ) || !strcmp( buf1, "MasterUidValidity" ) /* Pre-1.4 legacy */) {
				svars->uidval[F] = uid;
			} else if (!strcmp( buf1, "NearUidValidity" ) || !strcmp( buf1, "SlaveUidValidity" ) /* Pre-1.4 legacy */) {
				svars->uidval[N] = uid;
			} else if (!strcmp( buf1, "MaxPulledUid" )) {
				svars->maxuid[F] = uid;
			} else if (!strcmp( buf1, "MaxPushedUid" )) {
				svars->maxuid[N] = uid;
			} else if (!strcmp( buf1, "MaxExpiredFarUid" ) || !strcmp( buf1, "MaxExpiredMasterUid" ) /* Pre-1.4 legacy */) {
				svars->maxxfuid = uid;
			} else if (!strcmp( buf1, "MaxExpiredSlaveUid" )) {  // Pre-1.3 legacy
				maxxnuid = uid;
			} else {
				error( "Error: unrecognized sync state header entry at %s:%d\n", svars->dname, line );
				goto jbail;
			}
		}
		error( "Error: unterminated sync state header in %s\n", svars->dname );
		goto jbail;
	  gothdr:
		debug( "  uid val %u/%u, max uid %u/%u, max expired %u\n",
		       svars->uidval[F], svars->uidval[N], svars->maxuid[F], svars->maxuid[N], svars->maxxfuid );
		while (fgets( buf, sizeof(buf), jfp )) {
			line++;
			if (!(ll = strlen( buf )) || buf[--ll] != '\n') {
				error( "Error: incomplete sync state entry at %s:%d\n", svars->dname, line );
				goto jbail;
			}
			buf[ll] = 0;
			fbuf[0] = 0;
			uint t1, t2;
			if (sscanf( buf, "%u %u %15s", &t1, &t2, fbuf ) < 2) {
				error( "Error: invalid sync state entry at %s:%d\n", svars->dname, line );
				goto jbail;
			}
			srec = nfzalloc( sizeof(*srec) );
			srec->uid[F] = t1;
			srec->uid[N] = t2;
			char *s = fbuf;
			if (*s == '<') {
				s++;
				srec->status = S_DUMMY(F);
			} else if (*s == '>') {
				s++;
				srec->status = S_DUMMY(N);
			}
			if (*s == '^') {  // Pre-1.4 legacy
				s++;
				srec->status = S_SKIPPED;
			} else if (*s == '~' || *s == 'X' /* Pre-1.3 legacy */) {
				s++;
				srec->status = S_EXPIRE | S_EXPIRED;
			} else if (srec->uid[F] == (uint)-1) {  // Pre-1.3 legacy
				srec->uid[F] = 0;
				srec->status = S_SKIPPED;
			} else if (srec->uid[N] == (uint)-1) {
				srec->uid[N] = 0;
				srec->status = S_SKIPPED;
			}
			srec->flags = parse_flags( s );
			debug( "  entry (%u,%u,%s,%s)\n", srec->uid[F], srec->uid[N],
			       fmt_flags( srec->flags ).str, fmt_sts( srec->status ).str );
			*svars->srecadd = srec;
			svars->srecadd = &srec->next;
			svars->nsrecs++;
		}
		fclose( jfp );
		svars->existing = 1;
	} else {
		if (errno != ENOENT) {
			sys_error( "Error: cannot read sync state %s", svars->dname );
			return 0;
		}
		svars->existing = 0;
	}

	// This is legacy support for pre-1.3 sync states.
	if (maxxnuid) {
		uint minwuid = UINT_MAX;
		for (srec = svars->srecs; srec; srec = srec->next) {
			if ((srec->status & (S_DEAD | S_SKIPPED | S_PENDING)) || !srec->uid[F])
				continue;
			if (srec->status & S_EXPIRED) {
				if (!srec->uid[N]) {
					// The expired message was already gone.
					continue;
				}
				// The expired message was not expunged yet, so re-examine it.
				// This will happen en masse, so just extend the bulk fetch.
			} else {
				if (srec->uid[N] && maxxnuid >= srec->uid[N]) {
					// The non-expired message is in the generally expired range,
					// so don't make it contribute to the bulk fetch.
					continue;
				}
				// Usual non-expired message.
			}
			if (minwuid > srec->uid[F])
				minwuid = srec->uid[F];
		}
		svars->maxxfuid = minwuid - 1;
	}

	svars->newmaxuid[F] = svars->maxuid[F];
	svars->newmaxuid[N] = svars->maxuid[N];
	int line = 0;
	if ((jfp = fopen( svars->jname, "r" ))) {
		if (!lock_state( svars ))
			goto jbail;
		struct stat st;
		if (!stat( svars->nname, &st ) && fgets( buf, sizeof(buf), jfp )) {
			debug( "recovering journal ...\n" );
			if (!(ll = strlen( buf )) || buf[--ll] != '\n') {
				error( "Error: incomplete journal header in %s\n", svars->jname );
				goto jbail;
			}
			buf[ll] = 0;
			if (!equals( buf, (int)ll, JOURNAL_VERSION, strlen(JOURNAL_VERSION) )) {
				error( "Error: incompatible journal version"
				       " (got %s, expected " JOURNAL_VERSION ")\n", buf );
				goto jbail;
			}
			srec = NULL;
			line = 1;
			while (fgets( buf, sizeof(buf), jfp )) {
				line++;
				if (!(ll = strlen( buf )) || buf[--ll] != '\n') {
					error( "Error: incomplete journal entry at %s:%d\n", svars->jname, line );
					goto jbail;
				}
				buf[ll] = 0;
				char c;
				int tn, bad;
				uint t1, t2, t3, t4;
				switch ((c = buf[0])) {
				case '#':
					tn = 0;
					bad = (sscanf( buf + 2, "%u %u %n", &t1, &t2, &tn ) < 2) || !tn || (ll - (uint)tn != TUIDL + 2);
					break;
				case 'N':
				case 'F':
				case 'T':
				case 'P':
				case '+':
				case '&':
				case '-':
				case '_':
				case '|':
					bad = sscanf( buf + 2, "%u %u", &t1, &t2 ) != 2;
					break;
				case '<':
				case '>':
				case '*':
				case '%':
				case '~':
				case '^':
					bad = sscanf( buf + 2, "%u %u %u", &t1, &t2, &t3 ) != 3;
					break;
				case '$':
					bad = sscanf( buf + 2, "%u %u %u %u", &t1, &t2, &t3, &t4 ) != 4;
					break;
				default:
					error( "Error: unrecognized journal entry at %s:%d\n", svars->jname, line );
					goto jbail;
				}
				if (bad) {
					error( "Error: malformed journal entry at %s:%d\n", svars->jname, line );
					goto jbail;
				}
				if (c == 'N') {
					svars->maxuid[t1] = svars->newmaxuid[t1] = t2;
					debug( "  maxuid of %s now %u\n", str_fn[t1], t2 );
				} else if (c == 'F') {
					svars->finduid[t1] = t2;
					debug( "  saved UIDNEXT of %s now %u\n", str_fn[t1], t2 );
				} else if (c == 'T') {
					*uint_array_append( &svars->trashed_msgs[t1] ) = t2;
					debug( "  trashed %u from %s\n", t2, str_fn[t1] );
				} else if (c == '|') {
					svars->uidval[F] = t1;
					svars->uidval[N] = t2;
					debug( "  UIDVALIDITYs now %u/%u\n", t1, t2 );
				} else if (c == '+') {
					srec = nfzalloc( sizeof(*srec) );
					srec->uid[F] = t1;
					srec->uid[N] = t2;
					if (svars->newmaxuid[F] < t1)
						svars->newmaxuid[F] = t1;
					if (svars->newmaxuid[N] < t2)
						svars->newmaxuid[N] = t2;
					debug( "  new entry(%u,%u)\n", t1, t2 );
					srec->status = S_PENDING;
					*svars->srecadd = srec;
					svars->srecadd = &srec->next;
					svars->nsrecs++;
				} else {
					for (nsrec = srec; srec; srec = srec->next)
						if (srec->uid[F] == t1 && srec->uid[N] == t2)
							goto syncfnd;
					for (srec = svars->srecs; srec != nsrec; srec = srec->next)
						if (srec->uid[F] == t1 && srec->uid[N] == t2)
							goto syncfnd;
					error( "Error: journal entry at %s:%d refers to non-existing sync state entry\n", svars->jname, line );
					goto jbail;
				  syncfnd:
					debugn( "  entry(%u,%u) ", srec->uid[F], srec->uid[N] );
					switch (c) {
					case '-':
						debug( "killed\n" );
						srec->status = S_DEAD;
						break;
					case '#':
						memcpy( srec->tuid, buf + tn + 2, TUIDL );
						debug( "TUID now %." stringify(TUIDL) "s\n", srec->tuid );
						break;
					case '&':
						debug( "TUID %." stringify(TUIDL) "s lost\n", srec->tuid );
						srec->tuid[0] = 0;
						break;
					case '<':
						debug( "far side now %u\n", t3 );
						assign_uid( svars, srec, F, t3 );
						break;
					case '>':
						debug( "near side now %u\n", t3 );
						assign_uid( svars, srec, N, t3 );
						break;
					case '*':
						srec->flags = (uchar)t3;
						debug( "flags now %s\n", fmt_lone_flags( t3 ).str );
						break;
					case 'P':
						debug( "deleted dummy\n" );
						srec->aflags[F] = srec->aflags[N] = 0;  // Clear F_DELETED
						srec->status = (srec->status & ~S_PURGE) | S_PURGED;
						break;
					case '%':
						srec->pflags = (uchar)t3;
						debug( "pending flags now %s\n", fmt_lone_flags( t3 ).str );
						break;
					case '~':
						srec->status = (srec->status & ~S_LOGGED) | t3;
						if ((srec->status & S_EXPIRED) && svars->maxxfuid < srec->uid[F])
							svars->maxxfuid = srec->uid[F];
						debug( "status now %s\n", fmt_sts( srec->status ).str );
						break;
					case '_':
						debug( "has placeholder now\n" );
						srec->status = S_PENDING | (!srec->uid[F] ? S_DUMMY(F) : S_DUMMY(N));
						break;
					case '^':
						tn = (srec->status & S_DUMMY(F)) ? F : N;
						srec->pflags = (uchar)t3;
						debug( "upgrading %s placeholder, dummy's flags %s\n",
						       str_fn[tn], fmt_lone_flags( t3 ).str );
						srec = upgrade_srec( svars, srec, tn );
						break;
					case '$':
						tn = !srec->uid[F] ? F : N;
						srec->aflags[tn] = (uchar)t3;
						srec->dflags[tn] = (uchar)t4;
						debug( "flag update for %s now +%s -%s\n",
						       str_fn[tn], fmt_flags( t3 ).str, fmt_flags( t4 ).str );
						break;
					default:
						assert( !"Unhandled journal entry" );
					}
				}
			}
		}
		fclose( jfp );
		sort_uint_array( svars->trashed_msgs[F].array );
		sort_uint_array( svars->trashed_msgs[N].array );
	} else {
		if (errno != ENOENT) {
			sys_error( "Error: cannot read journal %s", svars->jname );
			return 0;
		}
	}
	svars->replayed = line;

	return 1;
}

static void
create_state( sync_vars_t *svars )
{
	if (!(svars->nfp = fopen( svars->nname, "w" ))) {
		sys_error( "Error: cannot create new sync state %s", svars->nname );
		exit( 1 );
	}
}

void
jFprintf( sync_vars_t *svars, const char *msg, ... )
{
	va_list va;

	if (!svars->jfp) {
		if (DFlags & DRYRUN)
			goto dryout;
		create_state( svars );
		if (!(svars->jfp = fopen( svars->jname, svars->replayed ? "a" : "w" ))) {
			sys_error( "Error: cannot create journal %s", svars->jname );
			exit( 1 );
		}
		setlinebuf( svars->jfp );
		if (!svars->replayed)
			Fprintf( svars->jfp, JOURNAL_VERSION "\n" );
	}
	va_start( va, msg );
	vFprintf( svars->jfp, msg, va );
	va_end( va );
  dryout:
	countStep();
	JCount++;
}

void
save_state( sync_vars_t *svars )
{
	// If no change was made, the state is also unmodified.
	if (!svars->jfp && !svars->replayed)
		return;

	// jfp is NULL in this case anyway, but we might have replayed.
	if (DFlags & DRYRUN)
		return;

	if (!svars->nfp)
		create_state( svars );
	Fprintf( svars->nfp,
	         "FarUidValidity %u\nNearUidValidity %u\nMaxPulledUid %u\nMaxPushedUid %u\n",
	         svars->uidval[F], svars->uidval[N], svars->maxuid[F], svars->maxuid[N] );
	if (svars->maxxfuid)
		Fprintf( svars->nfp, "MaxExpiredFarUid %u\n", svars->maxxfuid );
	Fprintf( svars->nfp, "\n" );
	for (sync_rec_t *srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		Fprintf( svars->nfp, "%u %u %s%s%s\n", srec->uid[F], srec->uid[N],
		         (srec->status & S_DUMMY(F)) ? "<" : (srec->status & S_DUMMY(N)) ? ">" : "",
		         (srec->status & S_SKIPPED) ? "^" : (srec->status & S_EXPIRED) ? "~" : "",
		         fmt_flags( srec->flags ).str );
	}

	Fclose( svars->nfp, 1 );
	if (svars->jfp)
		Fclose( svars->jfp, 0 );
	if (!(DFlags & KEEPJOURNAL)) {
		// Order is important!
		if (rename( svars->nname, svars->dname ))
			warn( "Warning: cannot commit sync state %s\n", svars->dname );
		else if (unlink( svars->jname ))
			warn( "Warning: cannot delete journal %s\n", svars->jname );
	}
}

void
delete_state( sync_vars_t *svars )
{
	if (DFlags & DRYRUN)
		return;

	unlink( svars->nname );
	unlink( svars->jname );
	if (unlink( svars->dname ) || unlink( svars->lname )) {
		sys_error( "Error: channel %s: sync state cannot be deleted", svars->chan->name );
		svars->ret = SYNC_FAIL;
	}
}


void
assign_uid( sync_vars_t *svars, sync_rec_t *srec, int t, uint uid )
{
	srec->uid[t] = uid;
	if (uid == svars->newmaxuid[t] + 1)
		svars->newmaxuid[t] = uid;
	if (uid) {
		if (srec->status & S_UPGRADE) {
			srec->flags = (srec->flags | srec->aflags[t]) & ~srec->dflags[t];
			srec->aflags[t] = srec->dflags[t] = 0;  // Cleanup after journal replay
		} else {
			srec->flags = srec->pflags;
		}
	}
	srec->status &= ~(S_PENDING | S_UPGRADE);
	srec->tuid[0] = 0;
}

void
assign_tuid( sync_vars_t *svars, sync_rec_t *srec )
{
	for (uint i = 0; i < TUIDL; i++) {
		uchar c = arc4_getbyte() & 0x3f;
		srec->tuid[i] = (char)(c < 26 ? c + 'A' : c < 52 ? c + 'a' - 26 :
		                       c < 62 ? c + '0' - 52 : c == 62 ? '+' : '/');
	}
	JLOG( "# %u %u %." stringify(TUIDL) "s", (srec->uid[F], srec->uid[N], srec->tuid), "new TUID" );
}

int
match_tuids( sync_vars_t *svars, int t, message_t *msgs )
{
	message_t *tmsg, *ntmsg = NULL;
	const char *diag;
	int num_lost = 0;

	for (sync_rec_t *srec = svars->srecs; srec; srec = srec->next) {
		if (srec->status & S_DEAD)
			continue;
		if (!srec->uid[t] && srec->tuid[0]) {
			debug( "pair(%u,%u) TUID %." stringify(TUIDL) "s\n", srec->uid[F], srec->uid[N], srec->tuid );
			for (tmsg = ntmsg; tmsg; tmsg = tmsg->next) {
				if (tmsg->status & M_DEAD)
					continue;
				if (tmsg->tuid[0] && !memcmp( tmsg->tuid, srec->tuid, TUIDL )) {
					diag = (tmsg == ntmsg) ? "adjacently" : "after gap";
					goto mfound;
				}
			}
			for (tmsg = msgs; tmsg != ntmsg; tmsg = tmsg->next) {
				if (tmsg->status & M_DEAD)
					continue;
				if (tmsg->tuid[0] && !memcmp( tmsg->tuid, srec->tuid, TUIDL )) {
					diag = "after reset";
					goto mfound;
				}
			}
			JLOG( "& %u %u", (srec->uid[F], srec->uid[N]), "TUID lost" );
			// Note: status remains S_PENDING.
			srec->tuid[0] = 0;
			num_lost++;
			continue;
		  mfound:
			tmsg->srec = srec;
			srec->msg[t] = tmsg;
			ntmsg = tmsg->next;
			ASSIGN_UID( srec, t, tmsg->uid, "TUID matched %s", diag );
		}
	}
	return num_lost;
}

sync_rec_t *
upgrade_srec( sync_vars_t *svars, sync_rec_t *srec, int t )
{
	// Create an entry and append it to the current one.
	sync_rec_t *nsrec = nfzalloc( sizeof(*nsrec) );
	nsrec->next = srec->next;
	srec->next = nsrec;
	if (svars->srecadd == &srec->next)
		svars->srecadd = &nsrec->next;
	svars->nsrecs++;
	// Move the placeholder to the new entry.
	nsrec->uid[t] = srec->uid[t];
	srec->uid[t] = 0;
	if (srec->msg[t]) {  // NULL during journal replay; is assigned later.
		nsrec->msg[t] = srec->msg[t];
		nsrec->msg[t]->srec = nsrec;
		srec->msg[t] = NULL;
	}
	// Mark the original entry for upgrade.
	srec->status = (srec->status & ~(S_DUMMY(F) | S_DUMMY(N))) | S_PENDING | S_UPGRADE;
	// Mark the placeholder for nuking.
	nsrec->status = S_PURGE | (srec->status & (S_DEL(F) | S_DEL(N)));
	nsrec->aflags[t] = F_DELETED;
	return nsrec;
}
