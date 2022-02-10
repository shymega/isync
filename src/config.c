// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#define DEBUG_FLAG DEBUG_MAIN

#include "config.h"

#include "sync.h"

#include <pwd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__) || defined(__CYGWIN__)
char FieldDelimiter = ';';
#else
char FieldDelimiter = ':';
#endif

DEF_BIT_FORMATTER_FUNCTION(ops, OP)

char *
expand_strdup( const char *s, const conffile_t *cfile )
{
	struct passwd *pw;
	const char *p, *q;
	char *r;

	if (*s == '~') {
		s++;
		if (!*s) {
			p = NULL;
			q = Home;
		} else if (*s == '/') {
			p = s;
			q = Home;
		} else {
			if ((p = strchr( s, '/' ))) {
				r = nfstrndup( s, (size_t)(p - s) );
				pw = getpwnam( r );
				free( r );
			} else {
				pw = getpwnam( s );
			}
			if (!pw)
				return NULL;
			q = pw->pw_dir;
		}
		nfasprintf( &r, "%s%s", q, p ? p : "" );
		return r;
	} else if (*s != '/') {
		nfasprintf( &r, "%.*s%s", cfile->path_len, cfile->file, s );
		return r;
	} else {
		return nfstrdup( s );
	}
}

char *
get_arg( conffile_t *cfile, int required, int *comment )
{
	char *ret, *p, *t;
	int escaped, quoted;
	char c;

	p = cfile->rest;
	assert( p );
	while ((c = *p) && isspace( (uchar)c ))
		p++;
	if (!c || c == '#') {
		if (comment)
			*comment = (c == '#');
		if (required) {
			error( "%s:%d: parameter missing\n", cfile->file, cfile->line );
			cfile->err = 1;
		}
		ret = NULL;
	} else {
		for (escaped = 0, quoted = 0, ret = t = p; c; c = *p) {
			p++;
			if (escaped && c >= 32) {
				escaped = 0;
				*t++ = c;
			} else if (c == '\\') {
				escaped = 1;
			} else if (c == '"') {
				quoted ^= 1;
			} else if (!quoted && isspace( (uchar)c )) {
				break;
			} else {
				*t++ = c;
			}
		}
		*t = 0;
		if (escaped) {
			error( "%s:%d: unterminated escape sequence\n", cfile->file, cfile->line );
			cfile->err = 1;
			ret = NULL;
		}
		if (quoted) {
			error( "%s:%d: missing closing quote\n", cfile->file, cfile->line );
			cfile->err = 1;
			ret = NULL;
		}
	}
	cfile->rest = p;
	return ret;
}

char
parse_bool( conffile_t *cfile )
{
	if (!strcasecmp( cfile->val, "yes" ) ||
	    !strcasecmp( cfile->val, "true" ) ||
	    !strcasecmp( cfile->val, "on" ) ||
	    !strcmp( cfile->val, "1" ))
		return 1;
	if (strcasecmp( cfile->val, "no" ) &&
	    strcasecmp( cfile->val, "false" ) &&
	    strcasecmp( cfile->val, "off" ) &&
	    strcmp( cfile->val, "0" )) {
		error( "%s:%d: invalid boolean value '%s'\n",
		       cfile->file, cfile->line, cfile->val );
		cfile->err = 1;
	}
	return 0;
}

int
parse_int( conffile_t *cfile )
{
	char *p;
	int ret;

	ret = strtol( cfile->val, &p, 10 );
	if (*p) {
		error( "%s:%d: invalid integer value '%s'\n",
		       cfile->file, cfile->line, cfile->val );
		cfile->err = 1;
		return 0;
	}
	return ret;
}

uint
parse_size( conffile_t *cfile )
{
	char *p;
	uint ret;

	ret = strtoul( cfile->val, &p, 10 );
	if (*p == 'k' || *p == 'K')
		ret *= 1024, p++;
	else if (*p == 'm' || *p == 'M')
		ret *= 1024 * 1024, p++;
	if (*p == 'b' || *p == 'B')
		p++;
	if (*p) {
		fprintf (stderr, "%s:%d: invalid size '%s'\n",
		         cfile->file, cfile->line, cfile->val);
		cfile->err = 1;
		return 0;
	}
	return ret;
}

static const struct {
	int op;
	const char *name;
} boxOps[] = {
	{ OP_EXPUNGE, "Expunge" },
	{ OP_CREATE, "Create" },
	{ OP_REMOVE, "Remove" },
};

static int
getopt_helper( conffile_t *cfile, int *cops, channel_conf_t *conf )
{
	char *arg;
	uint i;

	if (!strcasecmp( "Sync", cfile->cmd )) {
		arg = cfile->val;
		do {
			if (!strcasecmp( "Push", arg )) {
				*cops |= XOP_PUSH;
			} else if (!strcasecmp( "Pull", arg )) {
				*cops |= XOP_PULL;
			} else if (!strcasecmp( "ReNew", arg )) {
				*cops |= OP_RENEW;
			} else if (!strcasecmp( "New", arg )) {
				*cops |= OP_NEW;
			} else if (!strcasecmp( "Delete", arg )) {
				*cops |= OP_DELETE;
			} else if (!strcasecmp( "Flags", arg )) {
				*cops |= OP_FLAGS;
			} else if (!strcasecmp( "PullReNew", arg )) {
				conf->ops[N] |= OP_RENEW;
			} else if (!strcasecmp( "PullNew", arg )) {
				conf->ops[N] |= OP_NEW;
			} else if (!strcasecmp( "PullDelete", arg )) {
				conf->ops[N] |= OP_DELETE;
			} else if (!strcasecmp( "PullFlags", arg )) {
				conf->ops[N] |= OP_FLAGS;
			} else if (!strcasecmp( "PushReNew", arg )) {
				conf->ops[F] |= OP_RENEW;
			} else if (!strcasecmp( "PushNew", arg )) {
				conf->ops[F] |= OP_NEW;
			} else if (!strcasecmp( "PushDelete", arg )) {
				conf->ops[F] |= OP_DELETE;
			} else if (!strcasecmp( "PushFlags", arg )) {
				conf->ops[F] |= OP_FLAGS;
			} else if (!strcasecmp( "All", arg ) || !strcasecmp( "Full", arg )) {
				*cops |= XOP_PULL|XOP_PUSH;
			} else if (!strcasecmp( "None", arg ) || !strcasecmp( "Noop", arg )) {
				conf->ops[F] |= XOP_TYPE_NOOP;
			} else {
				error( "%s:%d: invalid Sync arg '%s'\n",
				       cfile->file, cfile->line, arg );
				cfile->err = 1;
			}
		} while ((arg = get_arg( cfile, ARG_OPTIONAL, NULL )));
		conf->ops[F] |= XOP_HAVE_TYPE;
	} else if (!strcasecmp( "SyncState", cfile->cmd )) {
		conf->sync_state = !strcmp( cfile->val, "*" ) ? "*" : expand_strdup( cfile->val, cfile );
	} else if (!strcasecmp( "CopyArrivalDate", cfile->cmd )) {
		conf->use_internal_date = parse_bool( cfile );
	} else if (!strcasecmp( "MaxMessages", cfile->cmd )) {
		conf->max_messages = parse_int( cfile );
	} else if (!strcasecmp( "ExpireUnread", cfile->cmd )) {
		conf->expire_unread = parse_bool( cfile );
	} else {
		for (i = 0; i < as(boxOps); i++) {
			if (!strcasecmp( boxOps[i].name, cfile->cmd )) {
				int op = boxOps[i].op;
				arg = cfile->val;
				do {
					if (!strcasecmp( "Both", arg )) {
						*cops |= op;
					} else if (!strcasecmp( "Far", arg )) {
						conf->ops[F] |= op;
					} else if (!strcasecmp( "Master", arg )) {  // Pre-1.4 legacy
						conf->ops[F] |= op;
						cfile->ms_warn = 1;
					} else if (!strcasecmp( "Near", arg )) {
						conf->ops[N] |= op;
					} else if (!strcasecmp( "Slave", arg )) {  // Pre-1.4 legacy
						conf->ops[N] |= op;
						cfile->ms_warn = 1;
					} else if (!strcasecmp( "None", arg )) {
						conf->ops[F] |= op * (XOP_EXPUNGE_NOOP / OP_EXPUNGE);
					} else {
						error( "%s:%d: invalid %s arg '%s'\n",
						       cfile->file, cfile->line, boxOps[i].name, arg );
						cfile->err = 1;
					}
				} while ((arg = get_arg( cfile, ARG_OPTIONAL, NULL )));
				conf->ops[F] |= op * (XOP_HAVE_EXPUNGE / OP_EXPUNGE);
				return 1;
			}
		}
		return 0;
	}
	return 1;
}

int
getcline( conffile_t *cfile )
{
	char *arg;
	int comment;

	if (cfile->rest && (arg = get_arg( cfile, ARG_OPTIONAL, NULL ))) {
		error( "%s:%d: excess token '%s'\n", cfile->file, cfile->line, arg );
		cfile->err = 1;
	}
	while (fgets( cfile->buf, cfile->bufl, cfile->fp )) {
		cfile->line++;
		cfile->rest = cfile->buf;
		if (!(cfile->cmd = get_arg( cfile, ARG_OPTIONAL, &comment ))) {
			if (comment)
				continue;
			return 1;
		}
		if (!(cfile->val = get_arg( cfile, ARG_REQUIRED, NULL )))
			continue;
		return 1;
	}
	return 0;
}

static const char *
channel_str( const char *chan_name )
{
	if (!chan_name)
		return "on the command line";

	if (!*chan_name)
		return "in global config section";

	static char buf[100];
	nfsnprintf( buf, sizeof(buf), "in Channel '%s'", chan_name );
	return buf;
}

int
merge_ops( int cops, int ops[], const char *chan_name )
{
	int aops, op;
	uint i;

	if (!cops && !ops[F] && !ops[N])  // Only to denoise the debug output
		return 0;
	debug( "merge ops (%s):\n  common: %s\n  far: %s\n  near: %s\n",
	       channel_str( chan_name ), fmt_ops( cops ).str, fmt_ops( ops[F] ).str, fmt_ops( ops[N] ).str );
	aops = ops[F] | ops[N];
	if (ops[F] & XOP_HAVE_TYPE) {
		if (aops & OP_MASK_TYPE) {  // PullNew, etc.
			if (ops[F] & XOP_TYPE_NOOP) {
			  cfl:
				error( "Conflicting Sync options specified %s.\n", channel_str( chan_name ) );
				return 1;
			}
			if (aops & cops & OP_MASK_TYPE) {  // Overlapping New, etc.
			  ovl:
				error( "Redundant Sync options specified %s.\n", channel_str( chan_name ) );
				return 1;
			}
			// Mix in non-overlapping Push/Pull or New, etc.
			if (cops & XOP_PULL) {
				if (cops & (XOP_PUSH | OP_MASK_TYPE)) {
					// Mixing instant effect flags with row/column flags would be confusing,
					// so instead everything is instant effect. This implies that mixing
					// direction with type would cause overlaps, so PullNew Push Delete, etc.
					// is invalid.
					// Pull Push covers everything, so makes no sense to combine.
				  ivl:
					error( "Invalid combination of simple and compound Sync options %s.\n",
					       channel_str( chan_name ) );
					return 1;
				}
				if (ops[N] & OP_MASK_TYPE)
					goto ovl;
				ops[N] |= OP_MASK_TYPE;
			} else if (cops & XOP_PUSH) {
				if (cops & OP_MASK_TYPE)
					goto ivl;
				if (ops[F] & OP_MASK_TYPE)
					goto ovl;
				ops[F] |= OP_MASK_TYPE;
			} else {
				ops[F] |= cops & OP_MASK_TYPE;
				ops[N] |= cops & OP_MASK_TYPE;
			}
		} else if (cops & (OP_MASK_TYPE | XOP_MASK_DIR)) {  // Pull New, etc.
			if (ops[F] & XOP_TYPE_NOOP)
				goto cfl;
			if (!(cops & OP_MASK_TYPE))
				cops |= OP_MASK_TYPE;
			else if (!(cops & XOP_MASK_DIR))
				cops |= XOP_PULL|XOP_PUSH;
			if (cops & XOP_PULL)
				ops[N] |= cops & OP_MASK_TYPE;
			if (cops & XOP_PUSH)
				ops[F] |= cops & OP_MASK_TYPE;
		}
	}
	for (i = 0; i < as(boxOps); i++) {
		op = boxOps[i].op;
		if (ops[F] & (op * (XOP_HAVE_EXPUNGE / OP_EXPUNGE))) {
			if (((aops | cops) & op) && (ops[F] & (op * (XOP_EXPUNGE_NOOP / OP_EXPUNGE)))) {
				error( "Conflicting %s options specified %s.\n", boxOps[i].name, channel_str( chan_name ) );
				return 1;
			}
			if (aops & cops & op) {
				error( "Redundant %s options specified %s.\n", boxOps[i].name, channel_str( chan_name ) );
				return 1;
			}
			ops[F] |= cops & op;
			ops[N] |= cops & op;
		}
	}
	debug( "  => far: %s\n  => near: %s\n", fmt_ops( ops[F] ).str, fmt_ops( ops[N] ).str );
	return 0;
}

int
load_config( const char *where )
{
	conffile_t cfile;
	store_conf_t *store, **storeapp = &stores;
	channel_conf_t *channel, **channelapp = &channels;
	group_conf_t *group, **groupapp = &groups;
	string_list_t *chanlist, **chanlistapp;
	char *arg, *p;
	uint len, max_size;
	int cops, gcops, glob_ok, fn, i;
	char path[_POSIX_PATH_MAX], path2[_POSIX_PATH_MAX];
	char buf[1024];

	if (!where) {
		int path_len, path_len2;
		const char *config_home = getenv( "XDG_CONFIG_HOME" );
		if (config_home)
			nfsnprintf( path, sizeof(path), "%s/%nisyncrc", config_home, &path_len );
		else
			nfsnprintf( path, sizeof(path), "%s/.config/%nisyncrc", Home, &path_len );
		nfsnprintf( path2, sizeof(path2), "%s/%n.mbsyncrc", Home, &path_len2 );
		struct stat st;
		int ex = !lstat( path, &st );
		int ex2 = !lstat( path2, &st );
		if (ex2 && !ex) {
			cfile.file = path2;
			cfile.path_len = path_len2;
		} else {
			if (ex && ex2)
				warn( "Both %s and %s exist; using the former.\n", path, path2 );
			cfile.file = path;
			cfile.path_len = path_len;
		}
	} else {
		const char *sl = strrchr( where, '/' );
		if (!sl) {
			nfsnprintf( path, sizeof(path), "./%n%s", &cfile.path_len, where );
			cfile.file = path;
		} else {
			cfile.path_len = sl - where + 1;
			cfile.file = where;
		}
	}

	info( "Reading configuration file %s\n", cfile.file );

	if (!(cfile.fp = fopen( cfile.file, "r" ))) {
		sys_error( "Cannot open config file '%s'", cfile.file );
		return 1;
	}
	buf[sizeof(buf) - 1] = 0;
	cfile.buf = buf;
	cfile.bufl = sizeof(buf) - 1;
	cfile.line = 0;
	cfile.err = 0;
	cfile.ms_warn = 0;
	cfile.rest = NULL;

	gcops = 0;
	glob_ok = 1;
	global_conf.expire_unread = -1;
  reloop:
	while (getcline( &cfile )) {
		if (!cfile.cmd)
			continue;
		for (i = 0; i < N_DRIVERS; i++) {
			if (drivers[i]->parse_store( &cfile, &store )) {
				if (store) {
					if (!store->max_size)
						store->max_size = UINT_MAX;
					if (!store->flat_delim)
						store->flat_delim = "";
					*storeapp = store;
					storeapp = &store->next;
					*storeapp = NULL;
				}
				glob_ok = 0;
				goto reloop;
			}
		}
		if (!strcasecmp( "Channel", cfile.cmd )) {
			channel = nfzalloc( sizeof(*channel) );
			channel->name = nfstrdup( cfile.val );
			channel->max_messages = global_conf.max_messages;
			channel->expire_unread = global_conf.expire_unread;
			channel->use_internal_date = global_conf.use_internal_date;
			cops = 0;
			max_size = UINT_MAX;
			while (getcline( &cfile ) && cfile.cmd) {
				if (!strcasecmp( "MaxSize", cfile.cmd )) {
					max_size = parse_size( &cfile );
				} else if (!strcasecmp( "Pattern", cfile.cmd ) ||
				           !strcasecmp( "Patterns", cfile.cmd )) {
					arg = cfile.val;
					do {
						add_string_list( &channel->patterns, arg );
					} while ((arg = get_arg( &cfile, ARG_OPTIONAL, NULL )));
				} else if (!strcasecmp( "Far", cfile.cmd )) {
					fn = F;
					goto linkst;
				} else if (!strcasecmp( "Master", cfile.cmd )) {  // Pre-1.4 legacy
					fn = F;
					goto olinkst;
				} else if (!strcasecmp( "Near", cfile.cmd )) {
					fn = N;
					goto linkst;
				} else if (!strcasecmp( "Slave", cfile.cmd )) {  // Pre-1.4 legacy
					fn = N;
				  olinkst:
					cfile.ms_warn = 1;
				  linkst:
					if (*cfile.val != ':' || !(p = strchr( cfile.val + 1, ':' ))) {
						error( "%s:%d: malformed mailbox spec\n",
						       cfile.file, cfile.line );
						cfile.err = 1;
						continue;
					}
					*p = 0;
					for (store = stores; store; store = store->next) {
						if (!strcmp( store->name, cfile.val + 1 )) {
							channel->stores[fn] = store;
							goto stpcom;
						}
					}
					channel->stores[fn] = (void *)~0;
					error( "%s:%d: unknown store '%s'\n",
					       cfile.file, cfile.line, cfile.val + 1 );
					cfile.err = 1;
					continue;
				  stpcom:
					if (*++p)
						channel->boxes[fn] = nfstrdup( p );
				} else if (!getopt_helper( &cfile, &cops, channel )) {
					error( "%s:%d: keyword '%s' is not recognized in Channel sections\n",
					       cfile.file, cfile.line, cfile.cmd );
					cfile.rest = NULL;
					cfile.err = 1;
				}
			}
			if (!channel->stores[F]) {
				error( "channel '%s' refers to no far side store\n", channel->name );
				cfile.err = 1;
			}
			if (!channel->stores[N]) {
				error( "channel '%s' refers to no near side store\n", channel->name );
				cfile.err = 1;
			}
			if (merge_ops( cops, channel->ops, channel->name ))
				cfile.err = 1;
			if (max_size != UINT_MAX && !cfile.err) {
				if (!max_size)
					max_size = UINT_MAX;
				channel->stores[F]->max_size = channel->stores[N]->max_size = max_size;
			}
			*channelapp = channel;
			channelapp = &channel->next;
			glob_ok = 0;
			goto reloop;
		} else if (!strcasecmp( "Group", cfile.cmd )) {
			group = nfmalloc( sizeof(*group) );
			group->name = nfstrdup( cfile.val );
			*groupapp = group;
			groupapp = &group->next;
			*groupapp = NULL;
			chanlistapp = &group->channels;
			*chanlistapp = NULL;
			while ((arg = get_arg( &cfile, ARG_OPTIONAL, NULL ))) {
			  addone:
				len = strlen( arg );
				chanlist = nfmalloc( sizeof(*chanlist) + len );
				memcpy( chanlist->string, arg, len + 1 );
				*chanlistapp = chanlist;
				chanlistapp = &chanlist->next;
				*chanlistapp = NULL;
			}
			while (getcline( &cfile ) && cfile.cmd) {
				if (!strcasecmp( "Channel", cfile.cmd ) ||
				    !strcasecmp( "Channels", cfile.cmd )) {
					arg = cfile.val;
					goto addone;
				} else {
					error( "%s:%d: keyword '%s' is not recognized in Group sections\n",
					       cfile.file, cfile.line, cfile.cmd );
					cfile.rest = NULL;
					cfile.err = 1;
				}
			}
			glob_ok = 0;
			goto reloop;
		} else if (!strcasecmp( "FSync", cfile.cmd )) {
			UseFSync = parse_bool( &cfile );
		} else if (!strcasecmp( "FieldDelimiter", cfile.cmd )) {
			if (strlen( cfile.val ) != 1) {
				error( "%s:%d: Field delimiter must be exactly one character long\n", cfile.file, cfile.line );
				cfile.err = 1;
			} else {
				FieldDelimiter = cfile.val[0];
				if (!ispunct( FieldDelimiter )) {
					error( "%s:%d: Field delimiter must be a punctuation character\n", cfile.file, cfile.line );
					cfile.err = 1;
				}
			}
		} else if (!strcasecmp( "BufferLimit", cfile.cmd )) {
			BufferLimit = parse_size( &cfile );
			if (!BufferLimit) {
				error( "%s:%d: BufferLimit cannot be zero\n", cfile.file, cfile.line );
				cfile.err = 1;
			}
		} else if (!getopt_helper( &cfile, &gcops, &global_conf )) {
			error( "%s:%d: '%s' is not a recognized section-starting or global keyword\n",
			       cfile.file, cfile.line, cfile.cmd );
			cfile.err = 1;
			cfile.rest = NULL;
			while (getcline( &cfile ))
				if (!cfile.cmd)
					goto reloop;
			break;
		}
		if (!glob_ok) {
			error( "%s:%d: global options may not follow sections\n",
			       cfile.file, cfile.line );
			cfile.err = 1;
		}
	}
	fclose (cfile.fp);
	if (cfile.ms_warn)
		warn( "Notice: Master/Slave are deprecated; use Far/Near instead.\n" );
	cfile.err |= merge_ops( gcops, global_conf.ops, "" );
	if (!global_conf.sync_state) {
		const char *state_home = getenv( "XDG_STATE_HOME" );
		if (state_home)
			nfsnprintf( path, sizeof(path), "%s/isync/", state_home );
		else
			nfsnprintf( path, sizeof(path), "%s/.local/state/isync/", Home );
		nfsnprintf( path2, sizeof(path2), "%s/.mbsync/", Home );
		struct stat st;
		int ex = !lstat( path, &st );
		int ex2 = !lstat( path2, &st );
		if (ex2 && !ex) {
			global_conf.sync_state = nfstrdup( path2 );
		} else {
			if (ex && ex2) {
				error( "Error: both %s and %s exist; delete one or set SyncState globally.\n", path, path2 );
				cfile.err = 1;
			}
			global_conf.sync_state = nfstrdup( path );
		}
	}
	return cfile.err;
}
