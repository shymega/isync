// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-FileCopyrightText: 2004 Theodore Y. Ts'o <tytso@mit.edu>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#include "imap_p.h"

#include "socket.h"

#include <ctype.h>
#include <sys/wait.h>

#ifdef HAVE_LIBSASL
# include <sasl/sasl.h>
# include <sasl/saslutil.h>
#endif

#ifdef HAVE_MACOS_KEYCHAIN
# include <Security/Security.h>
#endif

#ifdef HAVE_LIBSSL
enum { SSL_None, SSL_STARTTLS, SSL_IMAPS };
#endif

typedef struct imap_server_conf {
	struct imap_server_conf *next;
	char *name;
	server_conf_t sconf;
	char *user;
	char *user_cmd;
	char *pass;
	char *pass_cmd;
	int max_in_progress;
	uint cap_mask;
	string_list_t *auth_mechs;
#ifdef HAVE_LIBSSL
	char ssl_type;
#endif
#ifdef HAVE_MACOS_KEYCHAIN
	char use_keychain;
#endif
	char failed;
} imap_server_conf_t;

typedef union imap_store_conf {
	store_conf_t gen;
	struct {
		STORE_CONF
		imap_server_conf_t *server;
		char *path;  // Note: this may be modified after the delimiter is determined.
		char delimiter;
		char use_namespace;
		char use_lsub;
	};
} imap_store_conf_t;

typedef union imap_store imap_store_t;

enum { AtomNil, AtomString, AtomLiteral, AtomChunkedLiteral };

typedef struct {
	int (*enter)( imap_store_t *ctx );
	int (*leave)( imap_store_t *ctx );
	int (*atom)( imap_store_t *ctx, char *val, uint len, int type );
	int (*done)( imap_store_t *ctx, int resp );
} parse_list_cb_t;

typedef struct {
	parse_list_cb_t *callback;
	uint need_bytes;
	int level, chunked, big_literal;
	enum { NotLiteral, AtomicLiteral, ChunkedLiteral } in_literal;
	const char *err;
} parse_list_state_t;

typedef enum {
	NsInit,
	Ns1st,
	Ns1stNs,
	Ns1stDelim,
	NsDone
} ParseNsState;

typedef enum {
	FetchInit,
	FetchAttrib,
	FetchUid,
	FetchFlags,
	FetchFlagVal,
	FetchDate,
	FetchSize,
	FetchBody,
	FetchBodyChunk,
	FetchHeaders,
	FetchHeaderFields,
	FetchHeaderBracket,
	FetchHeaderCont
} ParseFetchState;

typedef enum {
	ListInit,
	ListAttrib,
	ListSkip
} ParseListState;

typedef enum {
	PermflagsInit,
	PermflagsFlags,
	PermflagsHasFwd
} ParsePermflagsState;

typedef struct imap_cmd imap_cmd_t;

union imap_store {
	store_t gen;
	struct {
		STORE(union imap_store)
		const char *label;  // foreign
		const char *name;
		char *prefix;
		uint ref_count;
		uint opts;
		enum { SST_BAD, SST_HALF, SST_GOOD } state;
		// The trash folder's existence is not confirmed yet
		enum { TrashUnknown, TrashChecking, TrashKnown } trashnc;
		// What kind of BODY-less FETCH response we're expecting
		enum { FetchNone, FetchMsgs, FetchUidNext } fetch_sts;
		uint got_namespace:1;
		uint has_forwarded:1;
		uint capability_hack:1;
		char delimiter[2];  // Hierarchy delimiter
		char *ns_prefix, ns_delimiter;  // NAMESPACE info
		string_list_t *boxes;  // _list results
		char listed;  // was _list already run with these flags?
		// note that the message counts do _not_ reflect stats from msgs,
		// but mailbox totals.
		int total_msgs, recent_msgs;
		uint uidvalidity, uidnext;
		imap_messages_t msgs;
		struct {  // pending FETCH result
			char *body, *msgid;
			time_t date;
			uint seq, uid, size, body_len;
			uchar flags, status;
			char tuid[TUIDL];
		} fetch;
		uint caps;  // CAPABILITY results
		string_list_t *auth_mechs;
		parse_list_state_t parse_list_sts;
		const char *parse_list_what;
		char *parse_list_cmd;
		union {
			ParseNsState ns_state;
			ParseFetchState fetch_state;
			ParseListState list_state;
			ParsePermflagsState permflags_state;
			int any_state;  // We always init this one to zero
		};
		// Command queue
		imap_cmd_t *pending, **pending_append;
		imap_cmd_t *in_progress, **in_progress_append;
		imap_cmd_t *wait_check, **wait_check_append;
		int nexttag, num_in_progress, num_wait_check;
		uint buffer_mem;  // Memory currently occupied by buffers in the queue

		// Used during sequential operations like connect
		enum { GreetingPending = 0, GreetingBad, GreetingOk, GreetingPreauth } greeting;
		int expectBYE;  // LOGOUT is in progress
		int expectEOF;  // received LOGOUT's OK or unsolicited BYE
		int canceling;  // imap_cancel_cmds() is in progress
		union {
			void (*imap_open)( int sts, void *aux );
			void (*imap_cancel)( void *aux );
		} callbacks;
		void *callback_aux;
#ifdef HAVE_LIBSASL
		sasl_conn_t *sasl;
		int sasl_cont;
#endif

		void (*expunge_callback)( message_t *msg, void *aux );
		void (*bad_callback)( void *aux );
		void *drv_callback_aux;

		conn_t conn;  // This is BIG, so put it last
	};
};

#define IMAP_CMD \
	struct imap_cmd *next; \
	char *cmd; \
	int tag; \
	\
	struct { \
		/* Will be called on each continuation request until it resets this pointer. \
		 * Needs to invoke bad_callback and return -1 on error, otherwise return 0. */ \
		int (*cont)( imap_store_t *ctx, imap_cmd_t *cmd, const char *prompt ); \
		void (*done)( imap_store_t *ctx, imap_cmd_t *cmd, int response ); \
		char *data; \
		uint data_len; \
		uint uid;  /* to identify fetch responses */ \
		char high_prio;  /* if command is queued, put it at the front of the queue. */ \
		char wait_check;  /* Don't report success until subsequent CHECK success. */ \
		char to_trash;  /* we are storing to trash, not current. */ \
		char create;  /* create the mailbox if we get an error which suggests so. */ \
		char failok;  /* Don't complain about NO (@1) / BAD (@2) response. */ \
	} param;

struct imap_cmd {
	IMAP_CMD
};

#define IMAP_CMD_SIMPLE \
	IMAP_CMD \
	void (*callback)( int sts, void *aux ); \
	void *callback_aux;

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD_SIMPLE
	};
} imap_cmd_simple_t;

typedef union {
	imap_cmd_simple_t gen;
	struct {
		IMAP_CMD_SIMPLE
		msg_data_t *msg_data;
	};
} imap_cmd_fetch_msg_t;

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD
		void (*callback)( int sts, uint uid, void *aux );
		void *callback_aux;
	};
} imap_cmd_out_uid_t;

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD
		void (*callback)( int sts, message_t *msgs, void *aux );
		void *callback_aux;
		imap_message_t **out_msgs;
		uint uid;
	};
} imap_cmd_find_new_t;

#define IMAP_CMD_REFCOUNTED_STATE \
	uint ref_count; \
	int ret_val;

typedef struct {
	IMAP_CMD_REFCOUNTED_STATE
} imap_cmd_refcounted_state_t;

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD
		imap_cmd_refcounted_state_t *state;
	};
} imap_cmd_refcounted_t;

#define CAP(cap) (ctx->caps & (1 << (cap)))

enum CAPABILITY {
	IMAP4REV1,
	NOLOGIN,
#ifdef HAVE_LIBSASL
	SASLIR,
#endif
#ifdef HAVE_LIBSSL
	STARTTLS,
#endif
	UIDPLUS,
	LITERALPLUS,
	LITERALMINUS,
	MOVE,
	NAMESPACE,
	COMPRESS_DEFLATE
};

static const struct {
	const char *str;
	uint len;
} cap_list[] = {
	{ "IMAP4REV1", 9 },
	{ "LOGINDISABLED", 13 },
#ifdef HAVE_LIBSASL
	{ "SASL-IR", 7 },
#endif
#ifdef HAVE_LIBSSL
	{ "STARTTLS", 8 },
#endif
	{ "UIDPLUS", 7 },
	{ "LITERAL+", 8 },
	{ "LITERAL-", 8 },
	{ "MOVE", 4 },
	{ "NAMESPACE", 9 },
	{ "COMPRESS=DEFLATE", 16 },
};

#define RESP_OK       0
#define RESP_NO       1
#define RESP_CANCEL   2

static INLINE void imap_ref( imap_store_t *ctx ) { ++ctx->ref_count; }
static int imap_deref( imap_store_t *ctx );

static void imap_invoke_bad_callback( imap_store_t *ctx );

// Keep the MESSAGE_FLAGS in sync (grep that)!
static const struct {
	const char *str;
	const char *ustr;
	uint len;
} Flags[] = {
	{ "\\Draft", "\\DRAFT", 6 },         // 'D'
	{ "\\Flagged", "\\FLAGGED", 8 },     // 'F'
	{ "$Forwarded", "$FORWARDED", 10 },  // 'P'
	{ "\\Answered", "\\ANSWERED", 9 },   // 'R'
	{ "\\Seen", "\\SEEN", 5 },           // 'S'
	{ "\\Deleted", "\\DELETED", 8 },     // 'T'
};

static imap_cmd_t *
new_imap_cmd( uint size )
{
	imap_cmd_t *cmd = nfmalloc( size );
	memset( &cmd->param, 0, sizeof(cmd->param) );
	return cmd;
}

#define INIT_IMAP_CMD(type, cmdp, cb, aux) \
	type *cmdp = (type *)new_imap_cmd( sizeof(*cmdp) ); \
	cmdp->callback = cb; \
	cmdp->callback_aux = aux;

static void
done_imap_cmd( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	if (cmd->param.wait_check)
		ctx->num_wait_check--;
	if (cmd->param.data) {
		free( cmd->param.data );
		cmd->param.data = NULL;
		// This needs to happen before calling back.
		ctx->buffer_mem -= cmd->param.data_len;
	}
	cmd->param.done( ctx, cmd, response );
	free( cmd->cmd );
	free( cmd );
}

static void
send_imap_cmd( imap_store_t *ctx, imap_cmd_t *cmd )
{
	int litplus = 0, iovcnt = 3;
	conn_iovec_t iov[5];
	char tagbuf[16];

	cmd->tag = ++ctx->nexttag;
	uint tbufl = nfsnprintf( tagbuf, sizeof(tagbuf), "%d ", cmd->tag );
	uint cmdl = strlen( cmd->cmd );
	if (cmd->param.data) {
		assert( cmdl > 2 && !memcmp( cmd->cmd + cmdl - 2, "+}", 2 ) );
		if ((!cmd->param.to_trash || ctx->trashnc != TrashUnknown) &&
		    ((CAP(LITERALPLUS) && cmd->param.data_len <= 100*1024) ||
		     (CAP(LITERALMINUS) && cmd->param.data_len <= 4*1024))) {
			litplus = 1;
		} else {
			cmd->cmd[cmdl - 2] = '}';
			cmd->cmd[cmdl - 1] = 0;
			cmdl--;
		}
	}
	if (DFlags & DEBUG_NET) {
		if (ctx->num_in_progress)
			printf( "(%d in progress) ", ctx->num_in_progress );
		if (starts_with( cmd->cmd, -1, "LOGIN", 5 ))
			printf( "%s>>> %sLOGIN <user> <pass>\r\n", ctx->label, tagbuf );
		else if (starts_with( cmd->cmd, -1, "AUTHENTICATE PLAIN", 18 ))
			printf( "%s>>> %sAUTHENTICATE PLAIN <authdata>\r\n", ctx->label, tagbuf );
		else
			printf( "%s>>> %s%s\r\n", ctx->label, tagbuf, cmd->cmd );
		fflush( stdout );
	}
	iov[0].buf = tagbuf;
	iov[0].len = tbufl;
	iov[0].takeOwn = KeepOwn;
	iov[1].buf = cmd->cmd;
	iov[1].len = cmdl;
	iov[1].takeOwn = KeepOwn;
	iov[2].buf = "\r\n";
	iov[2].len = 2;
	iov[2].takeOwn = KeepOwn;
	if (litplus) {
		if (DFlags & DEBUG_NET) {
			if (DFlags & DEBUG_NET_ALL) {
				printf( "%s>>>>>>>>>\n", ctx->label );
				fwrite( cmd->param.data, cmd->param.data_len, 1, stdout );
				printf( "%s>>>>>>>>>\n", ctx->label );
			} else {
				printf( "%s>>>>> (%u bytes omitted)\n", ctx->label, cmd->param.data_len );
			}
			fflush( stdout );
		}
		iov[3].buf = cmd->param.data;
		iov[3].len = cmd->param.data_len;
		iov[3].takeOwn = GiveOwn;
		cmd->param.data = NULL;
		ctx->buffer_mem -= cmd->param.data_len;
		iov[4].buf = "\r\n";
		iov[4].len = 2;
		iov[4].takeOwn = KeepOwn;
		iovcnt = 5;
	}
	socket_write( &ctx->conn, iov, iovcnt );
	if (cmd->param.to_trash && ctx->trashnc == TrashUnknown)
		ctx->trashnc = TrashChecking;
	cmd->next = NULL;
	*ctx->in_progress_append = cmd;
	ctx->in_progress_append = &cmd->next;
	ctx->num_in_progress++;
	socket_expect_activity( &ctx->conn, 1 );
}

static int
cmd_sendable( imap_store_t *ctx, imap_cmd_t *cmd )
{
	if (ctx->conn.write_buf) {
		/* Don't build up a long queue in the socket, so we can
		 * control when the commands are actually sent.
		 * This allows reliable cancelation of pending commands,
		 * injecting commands in front of other pending commands,
		 * and keeping num_in_progress accurate. */
		return 0;
	}
	if (ctx->in_progress) {
		/* If the last command in flight ... */
		imap_cmd_t *cmdp = (imap_cmd_t *)((char *)ctx->in_progress_append -
		                                  offsetof(imap_cmd_t, next));
		if (cmdp->param.cont || cmdp->param.data) {
			/* ... is expected to trigger a continuation request, we need to
			 * wait for that round-trip before sending the next command. */
			return 0;
		}
	}
	if (cmd->param.to_trash && ctx->trashnc == TrashChecking) {
		/* Don't build a queue of MOVE/COPY/APPEND commands that may all fail. */
		return 0;
	}
	if (ctx->num_in_progress >= ctx->conf->server->max_in_progress) {
		/* Too many commands in flight. */
		return 0;
	}
	return 1;
}

static void
flush_imap_cmds( imap_store_t *ctx )
{
	imap_cmd_t *cmd;

	if ((cmd = ctx->pending) && cmd_sendable( ctx, cmd )) {
		if (!(ctx->pending = cmd->next))
			ctx->pending_append = &ctx->pending;
		send_imap_cmd( ctx, cmd );
	}
}

static void
finalize_checked_imap_cmds( imap_store_t *ctx, int resp )
{
	imap_cmd_t *cmd;

	while ((cmd = ctx->wait_check)) {
		if (!(ctx->wait_check = cmd->next))
			ctx->wait_check_append = &ctx->wait_check;
		done_imap_cmd( ctx, cmd, resp );
	}
}

static void
cancel_pending_imap_cmds( imap_store_t *ctx )
{
	imap_cmd_t *cmd;

	while ((cmd = ctx->pending)) {
		if (!(ctx->pending = cmd->next))
			ctx->pending_append = &ctx->pending;
		done_imap_cmd( ctx, cmd, RESP_CANCEL );
	}
}

static void
cancel_sent_imap_cmds( imap_store_t *ctx )
{
	imap_cmd_t *cmd;

	socket_expect_activity( &ctx->conn, 0 );
	while ((cmd = ctx->in_progress)) {
		ctx->in_progress = cmd->next;
		/* don't update num_in_progress and in_progress_append - store is dead */
		done_imap_cmd( ctx, cmd, RESP_CANCEL );
	}
}

static void
submit_imap_cmd( imap_store_t *ctx, imap_cmd_t *cmd )
{
	assert( ctx );
	assert( ctx->bad_callback );
	assert( cmd );
	assert( cmd->param.done );

	if (cmd->param.wait_check)
		ctx->num_wait_check++;
	if ((ctx->pending && !cmd->param.high_prio) || !cmd_sendable( ctx, cmd )) {
		if (ctx->pending && cmd->param.high_prio) {
			cmd->next = ctx->pending;
			ctx->pending = cmd;
		} else {
			cmd->next = NULL;
			*ctx->pending_append = cmd;
			ctx->pending_append = &cmd->next;
		}
	} else {
		send_imap_cmd( ctx, cmd );
	}
}

static void
imap_exec( imap_store_t *ctx, imap_cmd_t *cmdp,
           void (*done)( imap_store_t *ctx, imap_cmd_t *cmd, int response ),
           const char *fmt, ... )
{
	va_list ap;

	if (!cmdp)
		cmdp = new_imap_cmd( sizeof(*cmdp) );
	cmdp->param.done = done;
	va_start( ap, fmt );
	cmdp->cmd = xvasprintf( fmt, ap );
	va_end( ap );
	submit_imap_cmd( ctx, cmdp );
}

static void
transform_box_response( int *response )
{
	switch (*response) {
	case RESP_CANCEL: *response = DRV_CANCELED; break;
	case RESP_NO: *response = DRV_BOX_BAD; break;
	default: *response = DRV_OK; break;
	}
}

static void
imap_done_simple_box( imap_store_t *ctx ATTR_UNUSED,
                      imap_cmd_t *cmd, int response )
{
	imap_cmd_simple_t *cmdp = (imap_cmd_simple_t *)cmd;

	transform_box_response( &response );
	cmdp->callback( response, cmdp->callback_aux );
}

static void
transform_msg_response( int *response )
{
	switch (*response) {
	case RESP_CANCEL: *response = DRV_CANCELED; break;
	case RESP_NO: *response = DRV_MSG_BAD; break;
	default: *response = DRV_OK; break;
	}
}

static void
imap_done_simple_msg( imap_store_t *ctx ATTR_UNUSED,
                      imap_cmd_t *cmd, int response )
{
	imap_cmd_simple_t *cmdp = (imap_cmd_simple_t *)cmd;

	transform_msg_response( &response );
	cmdp->callback( response, cmdp->callback_aux );
}

static imap_cmd_refcounted_state_t *
imap_refcounted_new_state( uint sz )
{
	imap_cmd_refcounted_state_t *sts = nfmalloc( sz );
	sts->ref_count = 1; /* so forced sync does not cause an early exit */
	sts->ret_val = DRV_OK;
	return sts;
}

#define INIT_REFCOUNTED_STATE(type, sts, cb, aux) \
	type *sts = (type *)imap_refcounted_new_state( sizeof(type) ); \
	sts->callback = cb; \
	sts->callback_aux = aux;

static imap_cmd_t *
imap_refcounted_new_cmd( imap_cmd_refcounted_state_t *sts )
{
	imap_cmd_refcounted_t *cmd = (imap_cmd_refcounted_t *)new_imap_cmd( sizeof(*cmd) );
	cmd->state = sts;
	sts->ref_count++;
	return &cmd->gen;
}

#define DONE_REFCOUNTED_STATE_FINALIZE(sts, finalize) \
	if (!--sts->ref_count) { \
		finalize \
		sts->callback( sts->ret_val, sts->callback_aux ); \
		free( sts ); \
	}

#define DONE_REFCOUNTED_STATE(sts) \
	DONE_REFCOUNTED_STATE_FINALIZE(sts, )

#define DONE_REFCOUNTED_STATE_ARGS(sts, finalize, ...) \
	if (!--sts->ref_count) { \
		finalize \
		sts->callback( sts->ret_val, __VA_ARGS__, sts->callback_aux ); \
		free( sts ); \
	}

static void
transform_refcounted_box_response( imap_cmd_refcounted_state_t *sts, int response )
{
	switch (response) {
	case RESP_CANCEL:
		sts->ret_val = DRV_CANCELED;
		break;
	case RESP_NO:
		if (sts->ret_val == DRV_OK) /* Don't override cancelation. */
			sts->ret_val = DRV_BOX_BAD;
		break;
	}
}

static void
transform_refcounted_msg_response( imap_cmd_refcounted_state_t *sts, int response )
{
	switch (response) {
	case RESP_CANCEL:
		sts->ret_val = DRV_CANCELED;
		break;
	case RESP_NO:
		if (sts->ret_val == DRV_OK) /* Don't override cancelation. */
			sts->ret_val = DRV_MSG_BAD;
		break;
	}
}

static const char *
imap_strchr( const char *s, char tc )
{
	for (;; s++) {
		char c = *s;
		if (c == '\\')
			c = *++s;
		if (!c)
			return NULL;
		if (c == tc)
			return s;
	}
}

static char *
next_arg( char **ps, uint *len )
{
	char *ret, *s, *d;
	char c;

	assert( ps );
	s = *ps;
	if (!s)
		return NULL;
	while (isspace( (uchar)*s ))
		s++;
	if (!*s) {
		*ps = NULL;
		return NULL;
	}
	if (*s == '"') {
		s++;
		ret = d = s;
		while ((c = *s++) != '"') {
			if (c == '\\')
				c = *s++;
			if (!c) {
				*ps = NULL;
				return NULL;
			}
			*d++ = c;
		}
		*d = 0;
		*len = (uint)(d - ret);
		if (!*s)
			s = NULL;
	} else {
		ret = s;
		while ((c = *s)) {
			if (isspace( (uchar)c )) {
				*len = (uint)(s - ret);
				*s++ = 0;
				if (!*s)
					s = NULL;
				goto out;
			}
			s++;
		}
		*len = (uint)(s - ret);
		s = NULL;
	}

  out:
	*ps = s;
	return ret;
}

enum {
	LIST_OK,
	LIST_PARTIAL,
	LIST_BAD
};

static int
parse_imap_list( imap_store_t *ctx, char **sp, parse_list_state_t *sts )
{
	char *s = *sp, *d, *p;
	uint bytes;
	uint n;
	char c;

	assert( ctx );
	assert( sts );
	if (sts->in_literal != NotLiteral) {
		bytes = sts->need_bytes;
		if (!bytes)
			goto getline;
		if (sts->in_literal == ChunkedLiteral)
			goto get_chunked;
		goto get_atomic;
	}

	if (!s) {
		sts->err = "missing value";
		return LIST_BAD;
	}
	for (;;) {
		while (isspace( (uchar)*s ))
			s++;
		if (sts->level && *s == ')') {
			s++;
			if (sts->callback->leave && sts->callback->leave( ctx ) != LIST_OK)
				goto bail;
			sts->level--;
			goto next;
		}
		if (*s == '(') {
			/* sublist */
			s++;
			sts->level++;
			if (sts->callback->enter && sts->callback->enter( ctx ) != LIST_OK)
				goto bail;
			goto next2;
		} else if (*s == '{') {
			/* literal */
			bytes = strtoul( s + 1, &s, 10 );
			if (*s != '}' || *++s) {
				sts->err = "malformed literal";
				goto bail;
			}
			if (bytes >= INT_MAX) {
				sts->err = "excessively large literal - THIS MIGHT BE AN ATTEMPT TO HACK YOU!";
				goto bail;
			}

			if (sts->chunked) {
				sts->chunked = 0;
				assert( sts->callback->atom );
				if (sts->callback->atom( ctx, NULL, bytes, AtomChunkedLiteral ) != LIST_OK)
					goto bail;
				sts->in_literal = ChunkedLiteral;
				sts->big_literal = 1;
			  get_chunked:
				n = 1;
			} else {
				if (bytes > sizeof(ctx->conn.buf)) {
					sts->err = "unexpectedly large literal";
					goto bail;
				}
				sts->in_literal = AtomicLiteral;
				sts->big_literal = bytes > 64;
			  get_atomic:
				n = bytes;
			}
			if (!(p = socket_read( &ctx->conn, n, bytes, &n )))
				goto postpone;
			if (p == (void *)~0) {
			  badeof:
				sts->err = "unexpected EOF";
				goto bail;
			}
			if (DFlags & DEBUG_NET) {
				if (!sts->big_literal) {
					xprintf( "%s%.*!s\n", ctx->label, (int)n, p );
				} else if (DFlags & DEBUG_NET_ALL) {
					printf( "%s=========\n", ctx->label );
					fwrite( p, n, 1, stdout );
					if (p[n - 1] != '\n')
						fputs( "\n(no-nl) ", stdout );
					printf( "%s=========\n", ctx->label );
				} else {
					printf( "%s=== (%u bytes omitted)\n", ctx->label, n );
				}
				fflush( stdout );
			}
			if (sts->callback->atom && sts->callback->atom( ctx, p, n, AtomLiteral ) != LIST_OK)
				goto bail;
			bytes -= n;
			if (bytes > 0)
				goto postpone;
			if (sts->in_literal == ChunkedLiteral && sts->callback->atom( ctx, NULL, 0, AtomLiteral ) != LIST_OK)
				goto bail;
		  getline:
			if (!(s = socket_read_line( &ctx->conn )))
				goto postpone;
			if (s == (void *)~0)
				goto badeof;
			if (DFlags & DEBUG_NET) {
				printf( "%s%s\n", ctx->label, s );
				fflush( stdout );
			}
			sts->in_literal = NotLiteral;
		} else if (*s == '"') {
			/* quoted string */
			s++;
			p = d = s;
			while ((c = *s++) != '"') {
				if (c == '\\')
					c = *s++;
				if (!c) {
					sts->err = "unterminated quoted string";
					goto bail;
				}
				*d++ = c;
			}
			if (sts->callback->atom && sts->callback->atom( ctx, p, (uint)(d - p), AtomString ) != LIST_OK)
				goto bail;
		} else {
			/* atom */
			p = s;
			for (; *s && !isspace( (uchar)*s ); s++)
				if (sts->level && *s == ')')
					break;
			if (sts->callback->atom) {
				int t = AtomString;
				n = (uint)(s - p);
				if (equals_upper( p, (int)n, "NIL", 3 ))
					p = NULL, n = 0, t = AtomNil;
				if (sts->callback->atom( ctx, p, n, t ) != LIST_OK)
					goto bail;
			}
		}

	  next:
		if (!sts->level)
			break;
	  next2:
		if (!*s) {
			sts->err = "unterminated list";
			goto bail;
		}
	}
	*sp = s;
	return LIST_OK;

  postpone:
	sts->need_bytes = bytes;
	return LIST_PARTIAL;
  bail:
	sts->level = 0;
	return LIST_BAD;
}

static void
parse_list_init( imap_store_t *ctx, parse_list_cb_t *cb )
{
	parse_list_state_t *sts = &ctx->parse_list_sts;
	sts->in_literal = NotLiteral;
	sts->level = 0;
	sts->chunked = 0;
	sts->callback = cb;
	sts->err = NULL;
	ctx->any_state = 0;
}

static int
parse_list_continue( imap_store_t *ctx )
{
	int resp;
	if ((resp = parse_imap_list( ctx, &ctx->parse_list_cmd, &ctx->parse_list_sts )) != LIST_PARTIAL) {
		if (ctx->parse_list_sts.callback->done)
			resp = ctx->parse_list_sts.callback->done( ctx, resp );
		if (resp == LIST_BAD) {
			if (!ctx->parse_list_sts.err)
				ctx->parse_list_sts.err = "unexpected value";
			error( "IMAP error: malformed %s response from %s: %s\n",
			       ctx->parse_list_what, ctx->conn.name, ctx->parse_list_sts.err );
		}
	}
	return resp;
}

static int
parse_next_list( imap_store_t *ctx, parse_list_cb_t *cb )
{
	parse_list_init( ctx, cb );
	return parse_list_continue( ctx );
}

static int
parse_list( imap_store_t *ctx, char *s, parse_list_cb_t *cb, const char *what )
{
	ctx->parse_list_cmd = s;
	ctx->parse_list_what = what;
	return parse_next_list( ctx, cb );
}

static parse_list_cb_t ignore_one_list_cb = {
    .done = NULL
};

static int
ignore_two_done( imap_store_t *ctx, int sts )
{
	if (sts != LIST_OK)
		return sts;
	return parse_next_list( ctx, &ignore_one_list_cb );
}

static parse_list_cb_t ignore_two_lists_cb = {
    .done = ignore_two_done,
};

static int
namespace_rsp_enter( imap_store_t *ctx )
{
	// We use only the 1st personal namespace. Making this configurable
	// would not add value over just specifying Path.
	switch (ctx->ns_state) {
	case NsInit:
		ctx->ns_state = Ns1st;
		return LIST_OK;
	case Ns1st:
		ctx->ns_state = Ns1stNs;
		return LIST_OK;
	case NsDone:
		return LIST_OK;
	default:
		return LIST_BAD;
	}
}

static int
namespace_rsp_leave( imap_store_t *ctx )
{
	if (ctx->ns_state != NsDone)
		return LIST_BAD;
	return LIST_OK;
}

static int
namespace_rsp_atom( imap_store_t *ctx, char *val, uint len, int type ATTR_UNUSED )
{
	if (ctx->ns_state == Ns1stNs) {
		if (!val)
			return LIST_BAD;
		ctx->ns_prefix = nfstrndup( val, len );
		ctx->ns_state = Ns1stDelim;
	} else if (ctx->ns_state == Ns1stDelim) {
		if (val) {
			if (!len)
				return LIST_BAD;
			ctx->ns_delimiter = val[0];
		}
		// Namespace response extensions may follow here; we don't care.
		ctx->ns_state = NsDone;
	}
	return LIST_OK;
}

static int
namespace_rsp_done( imap_store_t *ctx, int sts )
{
	if (sts != LIST_OK)
		return sts;
	return parse_next_list( ctx, &ignore_two_lists_cb );
}

static parse_list_cb_t namespace_rsp_cb = {
    .enter = namespace_rsp_enter,
    .leave = namespace_rsp_leave,
    .atom = namespace_rsp_atom,
    .done = namespace_rsp_done,
};

static time_t
parse_date( const char *str )
{
	char *end;
	time_t date;
	int hours, mins;
	struct tm datetime;

	memset( &datetime, 0, sizeof(datetime) );
	if (!(end = strptime( str, "%e-%b-%Y %H:%M:%S ", &datetime )))
		return -1;
	if ((date = timegm( &datetime )) == -1)
		return -1;
	if (sscanf( end, "%3d%2d", &hours, &mins ) != 2)
		return -1;
	return date - (hours * 60 + mins) * 60;
}

static void
parse_fetched_flag( char *val, uint len, uchar *flags, uchar *status )
{
	if (val[0] != '\\' && val[0] != '$')
		return;
	to_upper( val, len );
	if (equals( val, len, "\\RECENT", 7 )) {
		*status |= M_RECENT;
		return;
	}
	for (uint i = 0; i < as(Flags); i++) {
		if (equals( val, len, Flags[i].ustr, Flags[i].len )) {
			*flags |= 1 << i;
			return;
		}
	}
	if (val[0] == '$')
		return; // Ignore unknown user-defined flags (keywords)
	if (len > 2 && val[1] == 'X' && val[2] == '-')
		return; // Ignore system flag extensions
	warn( "IMAP warning: unknown system flag %.*s\n", (int)len, val );
}

static void
parse_fetched_header( char *val, uint vlen, uint uid, char *tuid, char **msgid )
{
	char *end;
	int off, in_msgid = 0;
	for (; (end = memchr( val, '\n', vlen )); val = end + 1) {
		int len = (int)(end - val);
		vlen -= len + 1;
		if (len && end[-1] == '\r')
			len--;
		if (!len)
			break;
		if (starts_with_upper( val, len, "X-TUID: ", 8 )) {
			if (len != 8 + TUIDL) {
				warn( "IMAP warning: malformed X-TUID header (UID %u)\n", uid );
				continue;
			}
			memcpy( tuid, val + 8, TUIDL );
			in_msgid = 0;
			continue;
		}
		if (starts_with_upper( val, len, "MESSAGE-ID:", 11 )) {
			off = 11;
		} else if (in_msgid) {
			if (!isspace( val[0] )) {
				in_msgid = 0;
				continue;
			}
			off = 1;
		} else {
			continue;
		}
		while (off < len && isspace( val[off] ))
			off++;
		if (off == len) {
			in_msgid = 1;
			continue;
		}
		*msgid = nfstrndup( val + off, (uint)(len - off) );
		in_msgid = 0;
	}
}

static int
str_to_num( const char *val, uint len, uint *outp )
{
	uint i = 0;
	uint out = 0;

	if (!len)
		return 0;
	for (;;) {
		uchar c = val[i] - '0';
		if (c > 9)  // Covers underflow as well
			return 0;
		out += c;
		if (++i == len)
			break;
		if (out >= UINT_MAX / 10)
			return 0;
		out *= 10;
	}
	*outp = out;
	return 1;
}

static int
fetch_rsp_enter( imap_store_t *ctx )
{
	switch (ctx->fetch_state) {
	case FetchInit:
		ctx->fetch_state = FetchAttrib;
		return LIST_OK;
	case FetchFlags:
		ctx->fetch_state = FetchFlagVal;
		return LIST_OK;
	case FetchHeaders:
		ctx->fetch_state = FetchHeaderFields;
		return LIST_OK;
	default:
		return LIST_BAD;
	}
}

static int
fetch_rsp_leave( imap_store_t *ctx )
{
	switch (ctx->fetch_state) {
	case FetchAttrib:
		return LIST_OK;
	case FetchFlagVal:
		ctx->fetch_state = FetchAttrib;
		return LIST_OK;
	case FetchHeaderFields:
		ctx->fetch_state = FetchHeaderBracket;
		return LIST_OK;
	default:
		return LIST_BAD;
	}
}

static int
fetch_rsp_atom( imap_store_t *ctx, char *val, uint len, int type )
{
	switch (ctx->fetch_state) {
	case FetchInit:
	case FetchFlags:
	case FetchHeaders:
		return LIST_BAD;
	case FetchAttrib:
		if (!len) {
			ctx->parse_list_sts.err = "bogus attribute name";
			return LIST_BAD;
		}
		uchar field;
		to_upper( val, len );
		if (equals( val, len, "UID", 3 )) {
			ctx->fetch_state = FetchUid;
			return LIST_OK;
		} else if (equals( val, len, "FLAGS", 5 )) {
			ctx->fetch_state = FetchFlags;
			field = M_FLAGS;
		} else if (equals( val, len, "INTERNALDATE", 12 )) {
			ctx->fetch_state = FetchDate;
			field = M_DATE;
		} else if (equals( val, len, "RFC822.SIZE", 11 )) {
			ctx->fetch_state = FetchSize;
			field = M_SIZE;
		} else if (equals( val, len, "BODY[]", 6 ) || equals( val, len, "BODY[HEADER]", 12 )) {
			ctx->parse_list_sts.chunked = 1;
			ctx->fetch_state = FetchBody;
			field = M_BODY;
		} else if (equals( val, len, "BODY[HEADER.FIELDS", 18 )) {
			ctx->fetch_state = FetchHeaders;
			field = M_HEADER;
		} else {
			ctx->parse_list_sts.err = "unexpected attribute";
			return LIST_BAD;
		}
		if (ctx->fetch.status & field) {
			ctx->parse_list_sts.err = "duplicated attribute";
			return LIST_BAD;
		}
		ctx->fetch.status |= field;
		return LIST_OK;
	case FetchUid:
		if (!str_to_num( val, len, &ctx->fetch.uid )) {
			ctx->parse_list_sts.err = "unable to parse UID";
			return LIST_BAD;
		}
		break;
	case FetchFlagVal:
		if (!len) {
			ctx->parse_list_sts.err = "unable to parse FLAGS";
			return LIST_BAD;
		}
		parse_fetched_flag( val, len, &ctx->fetch.flags, &ctx->fetch.status );
		return LIST_OK;
	case FetchDate:
		if (type != AtomString) {
		  dfail:
			ctx->parse_list_sts.err = "unable to parse INTERNALDATE";
			return LIST_BAD;
		}
		val[len] = 0;
		if ((ctx->fetch.date = parse_date( val )) == -1)
			goto dfail;
		break;
	case FetchSize:
		if (!str_to_num( val, len, &ctx->fetch.size )) {
			ctx->parse_list_sts.err = "unable to parse RFC822.SIZE";
			return LIST_BAD;
		}
		break;
	case FetchBody:
		if (type != AtomChunkedLiteral) {
			ctx->parse_list_sts.err = "BODY is no literal";
			return LIST_BAD;
		}
		ctx->fetch.body = nfmalloc( len );
		ctx->fetch.body_len = 0;
		ctx->fetch_state = FetchBodyChunk;
		return LIST_OK;
	case FetchBodyChunk:
		if (!len)
			break;
		memcpy( ctx->fetch.body + ctx->fetch.body_len, val, len );
		ctx->fetch.body_len += len;
		return LIST_OK;
	case FetchHeaderFields:
		// This looks like BODY[HEADER.FIELDS (X-TUID Message-Id)] {content}.
		// To avoid parsing the bracketed list, we just treat these as separate tokens.
		return LIST_OK;
	case FetchHeaderBracket:
		if (!equals( val, len, "]", 1 )) {
		  bfail:
			ctx->parse_list_sts.err = "unable to parse BODY[HEADER.FIELDS ...]";
			return LIST_BAD;
		}
		ctx->fetch_state = FetchHeaderCont;
		return LIST_OK;
	case FetchHeaderCont:
		if (!val)
			goto bfail;
		parse_fetched_header( val, len, ctx->fetch.uid, ctx->fetch.tuid, &ctx->fetch.msgid );
		break;
	}
	ctx->fetch_state = FetchAttrib;
	return LIST_OK;
}

static int
fetch_rsp_done( imap_store_t *ctx, int sts )
{
	imap_message_t *cur;
	msg_data_t *msgdata;
	imap_cmd_t *cmdp;

	if (sts != LIST_OK)
		goto bail;

	uchar status = ctx->fetch.status;
	if (!ctx->fetch.uid) {
		// Ignore async flag updates for now.
		status &= ~(M_FLAGS | M_RECENT);
	} else if (status & M_BODY) {
		for (cmdp = ctx->in_progress; cmdp; cmdp = cmdp->next)
			if (cmdp->param.uid == ctx->fetch.uid)
				goto gotuid;
		ctx->parse_list_sts.err = "unexpected BODY";
		sts = LIST_BAD;
		goto bail;
	  gotuid:
		msgdata = ((imap_cmd_fetch_msg_t *)cmdp)->msg_data;
		msgdata->data = ctx->fetch.body;
		ctx->fetch.body = NULL;
		msgdata->len = ctx->fetch.body_len;
		msgdata->date = ctx->fetch.date;
		if (status & M_FLAGS)
			msgdata->flags = ctx->fetch.flags;
		status &= ~(M_FLAGS | M_RECENT | M_BODY | M_DATE);
	} else if (ctx->fetch_sts == FetchUidNext) {
		// Workaround for server not sending UIDNEXT and/or APPENDUID.
		ctx->uidnext = ctx->fetch.uid + 1;
	} else if (ctx->fetch_sts == FetchMsgs) {
		imap_ensure_absolute( &ctx->msgs );  // In case of interleaved EXPUNGE
		cur = imap_new_msg( & ctx->msgs );
		cur->seq = ctx->fetch.seq;
		cur->uid = ctx->fetch.uid;
		cur->flags = ctx->fetch.flags;
		cur->status = status;
		cur->size = ctx->fetch.size;
		cur->msgid = ctx->fetch.msgid;
		ctx->fetch.msgid = NULL;
		if (ctx->fetch.tuid[0])
			memcpy( cur->tuid, ctx->fetch.tuid, TUIDL );
		status &= ~(M_FLAGS | M_RECENT | M_SIZE | M_HEADER);
	} else {
		// These may come in as a result of STORE FLAGS despite .SILENT.
		status &= ~(M_FLAGS | M_RECENT);
	}

	if (status) {
		ctx->parse_list_sts.err = "extraneous data";
		sts = LIST_BAD;
	}

  bail:
	free( ctx->fetch.body );
	ctx->fetch.body = NULL;
	free( ctx->fetch.msgid );
	ctx->fetch.msgid = NULL;
	return sts;
}

static parse_list_cb_t fetch_rsp_cb = {
    .enter = fetch_rsp_enter,
    .leave = fetch_rsp_leave,
    .atom = fetch_rsp_atom,
    .done = fetch_rsp_done,
};

static void
parse_capability( imap_store_t *ctx, char *cmd )
{
	char *arg;
	uint i, argl;

	free_string_list( ctx->auth_mechs );
	ctx->auth_mechs = NULL;
	ctx->caps = 0x80000000;
	while ((arg = next_arg( &cmd, &argl ))) {
		to_upper( arg, argl );
		if (starts_with( arg, argl, "AUTH=", 5 )) {
			add_string_list_n( &ctx->auth_mechs, arg + 5, argl - 5 );
		} else {
			for (i = 0; i < as(cap_list); i++)
				if (equals( arg, argl, cap_list[i].str, cap_list[i].len ))
					ctx->caps |= 1 << i;
		}
	}
	ctx->caps &= ~ctx->conf->server->cap_mask;
	if (!CAP(NOLOGIN))
		add_string_list_n( &ctx->auth_mechs, "LOGIN", 5 );
}

static int
permflags_enter( imap_store_t *ctx )
{
	if (ctx->permflags_state != PermflagsInit)
		return LIST_BAD;
	ctx->permflags_state = PermflagsFlags;
	return LIST_OK;
}

static int
permflags_atom( imap_store_t *ctx, char *val, uint len, int type ATTR_UNUSED )
{
	if (ctx->permflags_state == PermflagsHasFwd)
		return LIST_OK;
	if (ctx->permflags_state != PermflagsFlags || !val)
		return LIST_BAD;
	if (equals( val, len, "\\*", 2 ) || equals_upper( val, len, "$FORWARDED", 10 ))
		ctx->permflags_state = PermflagsHasFwd;
	return LIST_OK;
}

static parse_list_cb_t permflags_cb = {
    .enter = permflags_enter,
    .atom = permflags_atom,
};

static int
parse_response_code( imap_store_t *ctx, imap_cmd_t *cmd, char *s )
{
	char *arg, *earg, *p;
	uint argl;

	if (!s || *s != '[')
		return RESP_OK;		/* no response code */
	s++;
	if (!(arg = next_arg( &s, &argl ))) {
		error( "IMAP error: malformed response code\n" );
		return RESP_CANCEL;
	}
	to_upper( arg, argl );
	if (equals( arg, argl, "UIDVALIDITY", 11 )) {
		if (!(arg = next_arg( &s, &argl )) ||
		    (ctx->uidvalidity = strtoul( arg, &earg, 10 ), *earg != ']'))
		{
			error( "IMAP error: malformed UIDVALIDITY status\n" );
			return RESP_CANCEL;
		}
	} else if (equals( arg, argl, "UIDNEXT", 7 )) {
		if (!(arg = next_arg( &s, &argl )) ||
		    (ctx->uidnext = strtoul( arg, &earg, 10 ), *earg != ']'))
		{
			error( "IMAP error: malformed UIDNEXT status\n" );
			return RESP_CANCEL;
		}
	} else if (equals( arg, argl, "CAPABILITY", 10 )) {
		if (!s || !(p = strchr( s, ']' ))) {
			error( "IMAP error: malformed CAPABILITY status\n" );
			return RESP_CANCEL;
		}
		*p = 0;
		parse_capability( ctx, s );
		if (strstr( p + 1, "mac.com IMAP4 service (Oracle Communications Messaging Server" ))
			ctx->capability_hack = 1;
	} else if (equals( arg, argl, "ALERT]", 6 )) {
		/* RFC2060 says that these messages MUST be displayed
		 * to the user
		 */
		if (!s) {
			error( "IMAP error: malformed ALERT status\n" );
			return RESP_CANCEL;
		}
		for (; isspace( (uchar)*s ); s++);
		error( "*** IMAP ALERT *** %s\n", s );
	} else if (equals( arg, argl, "APPENDUID", 9 )) {
		// The checks ensure that:
		// - cmd => this is the final tagged response of a command, at which
		//   point cmd was already removed from ctx->in_progress, so param.uid
		//   is available for reuse.
		// - !param.uid => the command isn't actually a FETCH. This doesn't
		//   really matter, as the field is safe to overwrite given the
		//   previous condition; it just has no effect for non-APPENDs.
		if (!cmd || cmd->param.uid) {
			error( "IMAP error: unexpected APPENDUID status\n" );
			return RESP_CANCEL;
		}
		if (!(arg = next_arg( &s, &argl )) ||
		    (ctx->uidvalidity = strtoul( arg, &earg, 10 ), *earg) ||
		    !(arg = next_arg( &s, &argl )) ||
		    (cmd->param.uid = strtoul( arg, &earg, 10 ), *earg != ']'))
		{
			error( "IMAP error: malformed APPENDUID status\n" );
			return RESP_CANCEL;
		}
	} else if (equals( arg, argl, "PERMANENTFLAGS", 14 )) {
		parse_list_init( ctx, &permflags_cb );
		// Note: we croak on LIST_PARTIAL, as literals are not expected anyway.
		if (parse_imap_list( ctx, &s, &ctx->parse_list_sts ) != LIST_OK || *s != ']') {
			error( "IMAP error: malformed PERMANENTFLAGS status\n" );
			return RESP_CANCEL;
		}
		ctx->has_forwarded = (ctx->permflags_state == PermflagsHasFwd);
	}
	return RESP_OK;
}

static int
list_rsp_enter( imap_store_t *ctx )
{
	if (ctx->list_state != ListInit)
		return LIST_BAD;
	ctx->list_state = ListAttrib;
	return LIST_OK;
}

static int
list_rsp_atom( imap_store_t *ctx, char *val, uint len, int type ATTR_UNUSED )
{
	if (ctx->list_state == ListSkip)
		return LIST_OK;
	if (ctx->list_state != ListAttrib || !val)
		return LIST_BAD;
	if (equals_upper( val, len, "\\NOSELECT", 9 ))
		ctx->list_state = ListSkip;
	return LIST_OK;
}

static parse_list_cb_t list2_rsp_cb;
static parse_list_cb_t list3_rsp_cb;

static int
list_rsp_done( imap_store_t *ctx, int sts )
{
	if (sts != LIST_OK)
		return sts;
	if (ctx->list_state == ListSkip)
		return parse_next_list( ctx, &ignore_two_lists_cb );
	return parse_next_list( ctx, &list2_rsp_cb );
}

static parse_list_cb_t list_rsp_cb = {
    .enter = list_rsp_enter,
    .atom = list_rsp_atom,
    .done = list_rsp_done,
};

static int
list2_rsp_enter( imap_store_t *ctx ATTR_UNUSED )
{
	return LIST_BAD;
}

static int
list2_rsp_atom( imap_store_t *ctx, char *val, uint len, int type ATTR_UNUSED )
{
	if (!ctx->delimiter[0] && val)
		ctx->delimiter[0] = len > 0 ? val[0] : 0;
	return LIST_OK;
}

static int
list2_rsp_done( imap_store_t *ctx, int sts )
{
	if (sts != LIST_OK)
		return sts;
	return parse_next_list( ctx, &list3_rsp_cb );
}

static parse_list_cb_t list2_rsp_cb = {
    .enter = list2_rsp_enter,
    .atom = list2_rsp_atom,
    .done = list2_rsp_done,
};

// Use this to check whether a full path refers to the actual IMAP INBOX.
static int
is_inbox( imap_store_t *ctx, const char *arg, int argl )
{
	if (!starts_with_upper( arg, argl, "INBOX", 5 ))
		return 0;
	if (arg[5] && arg[5] != ctx->delimiter[0])
		return 0;
	return 1;
}

// Use this to check whether a path fragment collides with the canonical INBOX.
static int
is_INBOX( imap_store_t *ctx, const char *arg, int argl )
{
	if (!starts_with( arg, argl, "INBOX", 5 ))
		return 0;
	if (argl > 5 && arg[5] != ctx->delimiter[0])
		return 0;
	return 1;
}

static void
normalize_INBOX( imap_store_t *ctx, char *arg, int argl )
{
	if (is_inbox( ctx, arg, argl ))
		memcpy( arg, "INBOX", 5 );
}

static int
list3_rsp_atom( imap_store_t *ctx, char *arg, uint len, int type ATTR_UNUSED )
{
	string_list_t *narg;
	int argl = (int)len;
	uint l;

	if (!arg)
		return LIST_BAD;
	if (argl > 1000) {
		warn( "IMAP warning: ignoring unreasonably long mailbox name '%.100s[...]'\n", arg );
		return LIST_OK;
	}
	// The server might be weird and have a non-uppercase INBOX. It
	// may legitimately do so, but we need the canonical spelling.
	normalize_INBOX( ctx, arg, argl );
	if ((l = strlen( ctx->prefix ))) {
		if (!starts_with( arg, argl, ctx->prefix, l )) {
			if (!is_INBOX( ctx, arg, argl ))
				return LIST_OK;
			// INBOX and its subfolders bypass the namespace.
		} else {
			arg += l;
			argl -= l;
			// A folder named "INBOX" would be indistinguishable from the
			// actual INBOX after prefix stripping, so drop it. This applies
			// only to the fully uppercased spelling, as our canonical box
			// names are case-sensitive (unlike IMAP's INBOX).
			if (is_INBOX( ctx, arg, argl )) {
				if (argl > 5)  // No need to complain about subfolders as well.
					warn( "IMAP warning: ignoring INBOX in %s\n", ctx->prefix );
				return LIST_OK;
			}
		}
	}
	if (argl >= 5 && !memcmp( arg + argl - 5, ".lock", 5 )) /* workaround broken servers */
		return LIST_OK;
	if (map_name( arg, argl, (char **)&narg, offsetof(string_list_t, string), ctx->delimiter, "/") < 0) {
		warn( "IMAP warning: ignoring mailbox %.*s (reserved character '/' in name)\n", argl, arg );
		return LIST_OK;
	}
	// Validate the normalized name. Technically speaking, we could tolerate
	// '//' and '/./', and '/../' being forbidden is a limitation of the Maildir
	// driver, but there isn't really a legitimate reason for these being present.
	for (const char *p = narg->string, *sp = p;;) {
		char c;
		if (!(c = *p) || c == '/') {
			uint pcl = (uint)(p - sp);
			if (!pcl) {
				error( "IMAP warning: ignoring mailbox '%s' due to empty name component\n", narg->string );
				free( narg );
				return LIST_OK;
			}
			if (pcl == 1 && sp[0] == '.') {
				error( "IMAP warning: ignoring mailbox '%s' due to '.' component\n", narg->string );
				free( narg );
				return LIST_OK;
			}
			if (pcl == 2 && sp[0] == '.' && sp[1] == '.') {
				error( "IMAP error: LIST'd mailbox name '%s' contains '..' component - THIS MIGHT BE AN ATTEMPT TO HACK YOU!\n", narg->string );
				free( narg );
				return LIST_BAD;
			}
			if (!c)
				break;
			sp = ++p;
		} else {
			++p;
		}
	}
	narg->next = ctx->boxes;
	ctx->boxes = narg;
	return LIST_OK;
}

static parse_list_cb_t list3_rsp_cb = {
    .enter = list2_rsp_enter,  // (sic!)
    .atom = list3_rsp_atom,
};

static int
prepare_name( char **buf, const imap_store_t *ctx, const char *prefix, const char *name )
{
	uint pl = strlen( prefix );

	switch (map_name( name, -1, buf, pl, "/", ctx->delimiter )) {
	case -1:
		error( "IMAP error: mailbox name %s contains server's hierarchy delimiter\n", name );
		return -1;
	case -2:
		error( "IMAP error: server's hierarchy delimiter not known\n" );
		return -1;
	default:
		memcpy( *buf, prefix, pl );
		return 0;
	}
}

static int
prepare_box( char **buf, const imap_store_t *ctx )
{
	const char *name = ctx->name;
	const char *pfx = ctx->prefix;

	if (starts_with_upper( name, -1, "INBOX", 5 ) && (!name[5] || name[5] == '/')) {
		if (!memcmp( name, "INBOX", 5 )) {
			pfx = "";
		} else if (!*pfx) {
			error( "IMAP error: cannot use unqualified '%s'. Did you mean INBOX?", name );
			return -1;
		}
	}
	return prepare_name( buf, ctx, pfx, name );
}

static int
prepare_trash( char **buf, const imap_store_t *ctx )
{
	return prepare_name( buf, ctx, ctx->prefix, ctx->conf->trash );
}

static void
record_expunge( imap_store_t *ctx, uint seq )
{
	imap_message_t *eptr = imap_expunge_msg( &ctx->msgs, seq );
	if (eptr)
		ctx->expunge_callback( &eptr->gen, ctx->drv_callback_aux );
}

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD
		imap_cmd_t *orig_cmd;
	};
} imap_cmd_trycreate_t;

static void imap_open_store_greeted( imap_store_t * );
static void get_cmd_result_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_socket_read( void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;
	imap_cmd_t *cmdp, **pcmdp;
	char *cmd, *arg, *arg1, *p;
	int resp, resp2, tag;
	uint seq, argl, argl1;
	conn_iovec_t iov[2];

	for (;;) {
		if (ctx->parse_list_sts.level) {
			resp = parse_list_continue( ctx );
		  listret:
			if (resp == LIST_PARTIAL) {
				socket_expect_bytes( &ctx->conn, ctx->parse_list_sts.need_bytes );
				return;
			}
			if (resp == LIST_BAD)
				break;
			continue;
		}
		if (!(cmd = socket_read_line( &ctx->conn ))) {
			socket_expect_bytes( &ctx->conn, 0 );
			return;
		}
		if (cmd == (void *)~0) {
			if (!ctx->expectEOF)
				error( "IMAP error: unexpected EOF from %s\n", ctx->conn.name );
			/* A clean shutdown sequence ends with bad_callback as well (see imap_cleanup()). */
			break;
		}
		if (DFlags & DEBUG_NET) {
			printf( "%s%s\n", ctx->label, cmd );
			fflush( stdout );
		}

		if (!(arg = next_arg( &cmd, &argl ))) {
			error( "IMAP error: empty response\n" );
			break;
		}
		if (*arg == '*') {
			if (!(arg = next_arg( &cmd, &argl ))) {
				error( "IMAP error: malformed untagged response\n" );
				break;
			}

			to_upper( arg, argl );
			if (ctx->greeting == GreetingPending && equals( arg, argl, "PREAUTH", 7 )) {
				parse_response_code( ctx, NULL, cmd );
				ctx->greeting = GreetingPreauth;
			  dogreet:
				imap_ref( ctx );
				imap_open_store_greeted( ctx );
				if (imap_deref( ctx ))
					return;
			} else if (equals( arg, argl, "OK", 2 )) {
				parse_response_code( ctx, NULL, cmd );
				if (ctx->greeting == GreetingPending) {
					ctx->greeting = GreetingOk;
					goto dogreet;
				}
			} else if (equals( arg, argl, "BYE", 3 )) {
				if (!ctx->expectBYE) {
					ctx->greeting = GreetingBad;
					error( "IMAP error: unexpected BYE response: %s\n", cmd );
					/* We just wait for the server to close the connection now. */
					ctx->expectEOF = 1;
					socket_expect_eof( &ctx->conn );
				} else {
					/* We still need to wait for the LOGOUT's tagged OK. */
				}
			} else if (ctx->greeting == GreetingPending) {
				error( "IMAP error: bogus greeting response %s\n", arg );
				break;
			} else if (equals( arg, argl, "NO", 2 )) {
				warn( "Warning from IMAP server: %s\n", cmd );
			} else if (equals( arg, argl, "BAD", 3 )) {
				error( "Error from IMAP server: %s\n", cmd );
			} else if (equals( arg, argl, "CAPABILITY", 10 )) {
				parse_capability( ctx, cmd );
			} else if (equals( arg, argl, "LIST", 4 ) || equals( arg, argl, "LSUB", 4 )) {
				resp = parse_list( ctx, cmd, &list_rsp_cb, "LIST" );
				goto listret;
			} else if (equals( arg, argl, "NAMESPACE", 9 )) {
				resp = parse_list( ctx, cmd, &namespace_rsp_cb, "NAMESPACE" );
				goto listret;
			} else if ((arg1 = next_arg( &cmd, &argl1 ))) {
				to_upper( arg1, argl1 );
				if (equals( arg1, argl1, "EXISTS", 6 )) {
					ctx->total_msgs = atoi( arg );
				} else if (equals( arg1, argl1, "EXPUNGE", 7 )) {
					if (!(seq = strtoul( arg, &arg1, 10 )) || *arg1) {
					  badseq:
						error( "IMAP error: malformed sequence number '%s'\n", arg );
						break;
					}
					record_expunge( ctx, seq );
					ctx->total_msgs--;
				} else if (equals( arg1, argl1, "RECENT", 6 )) {
					ctx->recent_msgs = atoi( arg );
				} else if (equals( arg1, argl1, "FETCH", 5 )) {
					memset( &ctx->fetch, 0, sizeof(ctx->fetch) );
					if (!(seq = strtoul( arg, &arg1, 10 )) || *arg1)
						goto badseq;
					ctx->fetch.seq = seq;
					resp = parse_list( ctx, cmd, &fetch_rsp_cb, "FETCH" );
					goto listret;
				}
			} else {
				error( "IMAP error: unrecognized untagged response '%s'\n", arg );
				break; /* this may mean anything, so prefer not to spam the log */
			}
			continue;
		} else if (!ctx->in_progress) {
			error( "IMAP error: unexpected reply: %s %s\n", arg, cmd ? cmd : "" );
			break; /* this may mean anything, so prefer not to spam the log */
		} else if (*arg == '+') {
			socket_expect_activity( &ctx->conn, 0 );
			/* There can be any number of commands in flight, but only the last
			 * one can require a continuation, as it enforces a round-trip. */
			cmdp = (imap_cmd_t *)((char *)ctx->in_progress_append -
			                      offsetof(imap_cmd_t, next));
			if (cmdp->param.data) {
				if (cmdp->param.to_trash)
					ctx->trashnc = TrashKnown; /* Can't get NO [TRYCREATE] any more. */
				if (DFlags & DEBUG_NET) {
					if (DFlags & DEBUG_NET_ALL) {
						printf( "%s>>>>>>>>>\n", ctx->label );
						fwrite( cmdp->param.data, cmdp->param.data_len, 1, stdout );
						printf( "%s>>>>>>>>>\n", ctx->label );
					} else {
						printf( "%s>>>>> (%u bytes omitted)\n", ctx->label, cmdp->param.data_len );
					}
					fflush( stdout );
				}
				iov[0].buf = cmdp->param.data;
				iov[0].len = cmdp->param.data_len;
				iov[0].takeOwn = GiveOwn;
				cmdp->param.data = NULL;
				ctx->buffer_mem -= cmdp->param.data_len;
				iov[1].buf = "\r\n";
				iov[1].len = 2;
				iov[1].takeOwn = KeepOwn;
				socket_write( &ctx->conn, iov, 2 );
			} else if (cmdp->param.cont) {
				if (cmdp->param.cont( ctx, cmdp, cmd ))
					return;
			} else {
				error( "IMAP error: unexpected command continuation request\n" );
				break;
			}
			socket_expect_activity( &ctx->conn, 1 );
		} else {
			tag = atoi( arg );
			for (pcmdp = &ctx->in_progress; (cmdp = *pcmdp); pcmdp = &cmdp->next)
				if (cmdp->tag == tag)
					goto gottag;
			error( "IMAP error: unexpected tag %s\n", arg );
			break;
		  gottag:
			if (!(*pcmdp = cmdp->next))
				ctx->in_progress_append = pcmdp;
			if (!--ctx->num_in_progress)
				socket_expect_activity( &ctx->conn, 0 );
			if (!(arg = next_arg( &cmd, &argl ))) {
				error( "IMAP error: malformed tagged response\n" );
				break;
			}
			to_upper( arg, argl );
			if (equals( arg, argl, "OK", 2 )) {
				if (cmdp->param.to_trash)
					ctx->trashnc = TrashKnown; /* Can't get NO [TRYCREATE] any more. */
				resp = RESP_OK;
			} else {
				if (equals( arg, argl, "NO", 2 )) {
					if (cmdp->param.create && cmd && starts_with_upper( cmd, -1, "[TRYCREATE]", 11 )) { /* APPEND or UID COPY */
						imap_cmd_trycreate_t *cmd2 =
							(imap_cmd_trycreate_t *)new_imap_cmd( sizeof(*cmd2) );
						cmd2->orig_cmd = cmdp;
						cmd2->param.high_prio = 1;
						p = strchr( cmdp->cmd, '"' );
						imap_exec( ctx, &cmd2->gen, get_cmd_result_p2,
						           "CREATE %.*s", imap_strchr( p + 1, '"' ) - p + 1, p );
						continue;
					}
					resp = RESP_NO;
					if (cmdp->param.failok)  // SELECT
						goto doresp;
				} else /*if (equals( arg, argl, "BAD", 3 ))*/ {
					if (cmdp->param.failok == 2 && cmd && starts_with_upper( cmd, -1, "[TOOBIG]", 8 )) {  // APPEND
						resp = RESP_NO;
						// Fall through - we still complain
					} else {
						resp = RESP_CANCEL;
					}
				}
				error( "IMAP command '%s' returned an error: %s\n",
				       starts_with( cmdp->cmd, -1, "LOGIN", 5 ) ?
				           "LOGIN <user> <pass>" :
				           starts_with( cmdp->cmd, -1, "AUTHENTICATE PLAIN", 18 ) ?
				               "AUTHENTICATE PLAIN <authdata>" :
				                cmdp->cmd,
				       cmd ? cmd : "<unspecified>" );
			}
		  doresp:
			if ((resp2 = parse_response_code( ctx, cmdp, cmd )) > resp)
				resp = resp2;
			imap_ref( ctx );
			if (resp == RESP_CANCEL)
				imap_invoke_bad_callback( ctx );
			if (resp == RESP_OK && cmdp->param.wait_check) {
				cmdp->next = NULL;
				*ctx->wait_check_append = cmdp;
				ctx->wait_check_append = &cmdp->next;
			} else {
				done_imap_cmd( ctx, cmdp, resp );
			}
			if (imap_deref( ctx ))
				return;
			if (ctx->canceling && !ctx->in_progress) {
				ctx->canceling = 0;
				ctx->callbacks.imap_cancel( ctx->callback_aux );
				return;
			}
		}
		flush_imap_cmds( ctx );
	}
	imap_invoke_bad_callback( ctx );
}

static void
get_cmd_result_p2( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	imap_cmd_trycreate_t *cmdp = (imap_cmd_trycreate_t *)cmd;
	imap_cmd_t *ocmd = cmdp->orig_cmd;

	if (response != RESP_OK) {
		done_imap_cmd( ctx, ocmd, response );
	} else {
		assert( !ocmd->param.wait_check );
		ctx->uidnext = 1;
		if (ocmd->param.to_trash)
			ctx->trashnc = TrashKnown;
		ocmd->param.create = 0;
		ocmd->param.high_prio = 1;
		submit_imap_cmd( ctx, ocmd );
	}
}

/******************* imap_cancel_store *******************/

static void
imap_cancel_store( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

#ifdef HAVE_LIBSASL
	sasl_dispose( &ctx->sasl );
#endif
	socket_close( &ctx->conn );
	finalize_checked_imap_cmds( ctx, RESP_CANCEL );
	cancel_sent_imap_cmds( ctx );
	cancel_pending_imap_cmds( ctx );
	free( ctx->ns_prefix );
	free_string_list( ctx->auth_mechs );
	free_generic_messages( &ctx->msgs.head->gen );
	free_string_list( ctx->boxes );
	imap_deref( ctx );
}

static int
imap_deref( imap_store_t *ctx )
{
	if (!--ctx->ref_count) {
		free( ctx );
		return -1;
	}
	return 0;
}

static void
imap_set_callbacks( store_t *gctx, void (*exp_cb)( message_t *, void * ),
                    void (*bad_cb)( void * ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	ctx->expunge_callback = exp_cb;
	ctx->bad_callback = bad_cb;
	ctx->drv_callback_aux = aux;
}

static void
imap_invoke_bad_callback( imap_store_t *ctx )
{
	ctx->bad_callback( ctx->drv_callback_aux );
}

/******************* imap_free_store *******************/

static imap_store_t *unowned;

static void
imap_cancel_unowned( void *gctx )
{
	imap_store_t *store, **storep;

	for (storep = &unowned; (store = *storep); storep = &store->next) {
		if (store == gctx) {
			*storep = store->next;
			break;
		}
	}
	imap_cancel_store( gctx );
}

static void
imap_free_store( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	assert( !ctx->pending && !ctx->in_progress && !ctx->wait_check );

	if (ctx->state == SST_BAD) {
		imap_cancel_store( gctx );
		return;
	}

	reset_imap_messages( &ctx->msgs );
	imap_set_callbacks( gctx, NULL, imap_cancel_unowned, gctx );
	ctx->next = unowned;
	unowned = ctx;
}

/******************* imap_cleanup *******************/

static void imap_cleanup_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_cleanup( void )
{
	imap_store_t *ctx, *nctx;

	for (ctx = unowned; ctx; ctx = nctx) {
		nctx = ctx->next;
		imap_set_callbacks( &ctx->gen, NULL, (void (*)( void * ))imap_cancel_store, ctx );
		ctx->expectBYE = 1;
		imap_exec( ctx, NULL, imap_cleanup_p2, "LOGOUT" );
	}
}

static void
imap_cleanup_p2( imap_store_t *ctx,
                 imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO) {
		imap_cancel_store( &ctx->gen );
	} else if (response == RESP_OK) {
		ctx->expectEOF = 1;
		socket_expect_eof( &ctx->conn );
	}
}

/******************* imap_open_store *******************/

static void imap_open_store_connected( int, void * );
#ifdef HAVE_LIBSSL
static void imap_open_store_tlsstarted1( int, void * );
#endif
static void imap_open_store_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_authenticate( imap_store_t * );
#ifdef HAVE_LIBSSL
static void imap_open_store_authenticate_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_tlsstarted2( int, void * );
static void imap_open_store_authenticate_p3( imap_store_t *, imap_cmd_t *, int );
#endif
static void imap_open_store_authenticate2( imap_store_t * );
static void imap_open_store_authenticate2_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_authenticate2_p3( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_compress( imap_store_t * );
#ifdef HAVE_LIBZ
static void imap_open_store_compress_p2( imap_store_t *, imap_cmd_t *, int );
#endif
static void imap_open_store_namespace( imap_store_t * );
static void imap_open_store_namespace_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_store_namespace2( imap_store_t * );
static void imap_open_store_finalize( imap_store_t * );
#ifdef HAVE_LIBSSL
static void imap_open_store_ssl_bail( imap_store_t * );
#endif
static void imap_open_store_bail( imap_store_t *, int );

static store_t *
imap_alloc_store( store_conf_t *conf, const char *label )
{
	imap_store_conf_t *cfg = (imap_store_conf_t *)conf;
	imap_server_conf_t *srvc = cfg->server;
	imap_store_t *ctx, **ctxp;

	/* First try to recycle a whole store. */
	for (ctxp = &unowned; (ctx = *ctxp); ctxp = &ctx->next) {
		if (ctx->state == SST_GOOD && ctx->conf == cfg) {
			*ctxp = ctx->next;
			goto gotstore;
		}
	}

	/* Then try to recycle a server connection. */
	for (ctxp = &unowned; (ctx = *ctxp); ctxp = &ctx->next) {
		if (ctx->conf->server == srvc) {
			*ctxp = ctx->next;
			free_string_list( ctx->boxes );
			ctx->boxes = NULL;
			ctx->listed = 0;
			/* One could ping the server here, but given that the idle timeout
			 * is at least 30 minutes, this sounds pretty pointless. */
			ctx->state = SST_HALF;
			goto gotsrv;
		}
	}

	/* Finally, schedule opening a new server connection. */
	ctx = nfzalloc( sizeof(*ctx) );
	ctx->driver = &imap_driver;
	ctx->ref_count = 1;
	socket_init( &ctx->conn, &srvc->sconf,
	             (void (*)( void * ))imap_invoke_bad_callback,
	             imap_socket_read, (void (*)(void *))flush_imap_cmds, ctx );
	ctx->in_progress_append = &ctx->in_progress;
	ctx->pending_append = &ctx->pending;
	ctx->wait_check_append = &ctx->wait_check;

  gotsrv:
	ctx->conf = cfg;
  gotstore:
	ctx->label = label;
	return &ctx->gen;
}

static void
imap_connect_store( store_t *gctx,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	if (ctx->state == SST_GOOD) {
		cb( DRV_OK, aux );
	} else {
		ctx->callbacks.imap_open = cb;
		ctx->callback_aux = aux;
		if (ctx->state == SST_HALF)
			imap_open_store_namespace( ctx );
		else
			socket_connect( &ctx->conn, imap_open_store_connected );
	}
}

static void
imap_open_store_connected( int ok, void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;

	if (!ok)
		imap_open_store_bail( ctx, FAIL_WAIT );
#ifdef HAVE_LIBSSL
	else if (ctx->conf->server->ssl_type == SSL_IMAPS)
		socket_start_tls( &ctx->conn, imap_open_store_tlsstarted1 );
#endif
	else
		socket_expect_activity( &ctx->conn, 1 );
}

#ifdef HAVE_LIBSSL
static void
imap_open_store_tlsstarted1( int ok, void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;

	if (!ok)
		imap_open_store_ssl_bail( ctx );
	else
		socket_expect_activity( &ctx->conn, 1 );
}
#endif

static void
imap_open_store_greeted( imap_store_t *ctx )
{
	socket_expect_activity( &ctx->conn, 0 );
	if (!ctx->caps)
		imap_exec( ctx, NULL, imap_open_store_p2, "CAPABILITY" );
	else
		imap_open_store_authenticate( ctx );
}

static void
imap_open_store_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		imap_open_store_authenticate( ctx );
}

static void
imap_open_store_authenticate( imap_store_t *ctx )
{
#ifdef HAVE_LIBSSL
	imap_server_conf_t *srvc = ctx->conf->server;
#endif

	if (!CAP(IMAP4REV1)) {
		error( "IMAP error: Server does not support IMAP4rev1\n" );
		imap_open_store_bail( ctx, FAIL_FINAL );
		return;
	}

	if (ctx->greeting != GreetingPreauth) {
#ifdef HAVE_LIBSSL
		if (srvc->ssl_type == SSL_STARTTLS) {
			if (CAP(STARTTLS)) {
				imap_exec( ctx, NULL, imap_open_store_authenticate_p2, "STARTTLS" );
				return;
			} else {
				error( "IMAP error: SSL support not available\n" );
				imap_open_store_bail( ctx, FAIL_FINAL );
				return;
			}
		}
#endif
		imap_open_store_authenticate2( ctx );
	} else {
#ifdef HAVE_LIBSSL
		if (srvc->ssl_type == SSL_STARTTLS) {
			error( "IMAP error: SSL support not available\n" );
			imap_open_store_bail( ctx, FAIL_FINAL );
			return;
		}
#endif
		imap_open_store_compress( ctx );
	}
}

#ifdef HAVE_LIBSSL
static void
imap_open_store_authenticate_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		socket_start_tls( &ctx->conn, imap_open_store_tlsstarted2 );
}

static void
imap_open_store_tlsstarted2( int ok, void *aux )
{
	imap_store_t *ctx = (imap_store_t *)aux;

	if (!ok)
		imap_open_store_ssl_bail( ctx );
	else
		imap_exec( ctx, NULL, imap_open_store_authenticate_p3, "CAPABILITY" );
}

static void
imap_open_store_authenticate_p3( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		imap_open_store_authenticate2( ctx );
}
#endif

static char *
cred_from_cmd( const char *cred, const char *cmd, const char *srv_name )
{
	FILE *fp;
	int ret;
	char buffer[8192];  // Hopefully more than enough room for XOAUTH2, etc. tokens

	if (*cmd == '+') {
		flushn();
		cmd++;
	}
	if (!(fp = popen( cmd, "r" ))) {
	  pipeerr:
		sys_error( "Skipping account %s, %s failed", srv_name, cred );
		return NULL;
	}
	if (!fgets( buffer, sizeof(buffer), fp ))
		buffer[0] = 0;
	if ((ret = pclose( fp )) < 0)
		goto pipeerr;
	if (ret) {
		if (WIFSIGNALED( ret ))
			error( "Skipping account %s, %s crashed\n", srv_name, cred );
		else
			error( "Skipping account %s, %s exited with status %d\n", srv_name, cred, WEXITSTATUS( ret ) );
		return NULL;
	}
	if (!buffer[0]) {
		error( "Skipping account %s, %s produced no output\n", srv_name, cred );
		return NULL;
	}
	buffer[strcspn( buffer, "\n" )] = 0; /* Strip trailing newline */
	return nfstrdup( buffer );
}

static const char *
ensure_user( imap_server_conf_t *srvc )
{
	if (!srvc->user) {
		if (srvc->user_cmd) {
			srvc->user = cred_from_cmd( "UserCmd", srvc->user_cmd, srvc->name );
		} else {
			error( "Skipping account %s, no user\n", srvc->name );
		}
	}
	return srvc->user;
}

static const char *
ensure_password( imap_server_conf_t *srvc )
{
	if (!srvc->pass) {
		if (srvc->pass_cmd) {
			srvc->pass = cred_from_cmd( "PassCmd", srvc->pass_cmd, srvc->name );
#ifdef HAVE_MACOS_KEYCHAIN
		} else if (srvc->use_keychain) {
			void *password_data;
			UInt32 password_length;
			OSStatus ret = SecKeychainFindInternetPassword(
					NULL,  // keychainOrArray
					strlen( srvc->sconf.host ), srvc->sconf.host,
					0, NULL,  // securityDomain
					strlen( srvc->user ), srvc->user,
					0, NULL,  // path
					0,  // port - we could use it, but it seems pointless
					kSecProtocolTypeIMAP,
					kSecAuthenticationTypeDefault,
					&password_length, &password_data,
					NULL );  // itemRef
			if (ret != errSecSuccess) {
				CFStringRef errmsg = SecCopyErrorMessageString( ret, NULL );
				error( "Looking up Keychain failed: %s\n",
				       CFStringGetCStringPtr( errmsg, kCFStringEncodingUTF8 ) );
				CFRelease( errmsg );
				return NULL;
			}
			srvc->pass = nfstrndup( password_data, password_length );
			SecKeychainItemFreeContent( NULL, password_data );
#endif /* HAVE_MACOS_KEYCHAIN */
		} else {
			flushn();
			char prompt[80];
			sprintf( prompt, "Password (%s): ", srvc->name );
			char *pass = getpass( prompt );
			if (!pass) {
				perror( "getpass" );
				exit( 1 );
			}
			if (!*pass) {
				error( "Skipping account %s, no password\n", srvc->name );
				return NULL;
			}
			/* getpass() returns a pointer to a static buffer. Make a copy for long term storage. */
			srvc->pass = nfstrdup( pass );
		}
	}
	return srvc->pass;
}

#ifdef HAVE_LIBSASL

static sasl_callback_t sasl_callbacks[] = {
	{ SASL_CB_USER,     NULL, NULL },
	{ SASL_CB_AUTHNAME, NULL, NULL },
	{ SASL_CB_PASS,     NULL, NULL },
	{ SASL_CB_LIST_END, NULL, NULL }
};

static int
process_sasl_interact( sasl_interact_t *interact, imap_server_conf_t *srvc )
{
	const char *val;

	for (;; ++interact) {
		switch (interact->id) {
		case SASL_CB_LIST_END:
			return 0;
		case SASL_CB_USER:  // aka authorization id - who to act as
		case SASL_CB_AUTHNAME:  // who is really logging in
			val = ensure_user( srvc );
			break;
		case SASL_CB_PASS:
			val = ensure_password( srvc );
			break;
		default:
			error( "Error: Unknown SASL interaction ID\n" );
			return -1;
		}
		if (!val)
			return -1;
		interact->result = val;
		interact->len = strlen( val );
	}
}

static int
process_sasl_step( imap_store_t *ctx, int rc, const char *in, uint in_len,
                   sasl_interact_t *interact, const char **out, uint *out_len )
{
	imap_server_conf_t *srvc = ctx->conf->server;

	while (rc == SASL_INTERACT) {
		if (process_sasl_interact( interact, srvc ) < 0)
			return -1;
		rc = sasl_client_step( ctx->sasl, in, in_len, &interact, out, out_len );
	}
	if (rc == SASL_CONTINUE) {
		ctx->sasl_cont = 1;
	} else if (rc == SASL_OK) {
		ctx->sasl_cont = 0;
	} else {
		error( "Error performing SASL authentication step: %s\n", sasl_errdetail( ctx->sasl ) );
		return -1;
	}
	return 0;
}

static int
decode_sasl_data( const char *prompt, char **in, uint *in_len )
{
	if (prompt) {
		int rc;
		uint prompt_len = strlen( prompt );
		/* We're decoding, the output will be shorter than prompt_len. */
		*in = nfmalloc( prompt_len );
		rc = sasl_decode64( prompt, prompt_len, *in, prompt_len, in_len );
		if (rc != SASL_OK) {
			free( *in );
			error( "Error decoding SASL prompt: %s\n", sasl_errstring( rc, NULL, NULL ) );
			return -1;
		}
	} else {
		*in = NULL;
		*in_len = 0;
	}
	return 0;
}

static int
encode_sasl_data( const char *out, uint out_len, char **enc, uint *enc_len )
{
	int rc;
	uint enc_len_max = ((out_len + 2) / 3) * 4 + 1;
	*enc = nfmalloc( enc_len_max );
	rc = sasl_encode64( out, out_len, *enc, enc_len_max, enc_len );
	if (rc != SASL_OK) {
		free( *enc );
		error( "Error encoding SASL response: %s\n", sasl_errstring( rc, NULL, NULL ) );
		return -1;
	}
	return 0;
}

static int
do_sasl_auth( imap_store_t *ctx, imap_cmd_t *cmdp ATTR_UNUSED, const char *prompt )
{
	int rc, ret, iovcnt = 0;
	uint in_len, out_len, enc_len;
	const char *out;
	char *in, *enc;
	sasl_interact_t *interact = NULL;
	conn_iovec_t iov[2];

	if (!ctx->sasl_cont) {
		error( "Error: IMAP wants more steps despite successful SASL authentication.\n" );
		goto bail;
	}
	if (decode_sasl_data( prompt, &in, &in_len ) < 0)
		goto bail;
	rc = sasl_client_step( ctx->sasl, in, in_len, &interact, &out, &out_len );
	ret = process_sasl_step( ctx, rc, in, in_len, interact, &out, &out_len );
	free( in );
	if (ret < 0)
		goto bail;

	if (out) {
		if (encode_sasl_data( out, out_len, &enc, &enc_len ) < 0)
			goto bail;

		iov[0].buf = enc;
		iov[0].len = enc_len;
		iov[0].takeOwn = GiveOwn;
		iovcnt = 1;

		if (DFlags & DEBUG_NET) {
			printf( "%s>+> %s\n", ctx->label, enc );
			fflush( stdout );
		}
	} else {
		if (DFlags & DEBUG_NET) {
			printf( "%s>+>\n", ctx->label );
			fflush( stdout );
		}
	}
	iov[iovcnt].buf = "\r\n";
	iov[iovcnt].len = 2;
	iov[iovcnt].takeOwn = KeepOwn;
	iovcnt++;
	socket_write( &ctx->conn, iov, iovcnt );
	return 0;

  bail:
	imap_open_store_bail( ctx, FAIL_FINAL );
	return -1;
}

static void
done_sasl_auth( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_OK && ctx->sasl_cont) {
		sasl_interact_t *interact = NULL;
		const char *out;
		uint out_len;
		int rc = sasl_client_step( ctx->sasl, NULL, 0, &interact, &out, &out_len );
		if (process_sasl_step( ctx, rc, NULL, 0, interact, &out, &out_len ) < 0)
			warn( "Warning: SASL reported failure despite successful IMAP authentication. Ignoring...\n" );
		else if (out_len > 0)
			warn( "Warning: SASL wants more steps despite successful IMAP authentication. Ignoring...\n" );
	}

	imap_open_store_authenticate2_p2( ctx, NULL, response );
}

#endif

static void
imap_open_store_authenticate2( imap_store_t *ctx )
{
	imap_server_conf_t *srvc = ctx->conf->server;
	string_list_t *mech, *cmech;
	int auth_login = 0;
	int skipped_login = 0;
#ifdef HAVE_LIBSASL
	const char *saslavail;
	char saslmechs[1024], *saslend = saslmechs;
	int want_external = 0;
#endif

	// Ensure that there are no leftovers from previous runs. This is needed in case
	// the credentials have a timing dependency or otherwise lose validity after use.
	if (srvc->user_cmd) {
		free( srvc->user );
		srvc->user = NULL;
	}
	if (srvc->pass_cmd) {
		free( srvc->pass );
		srvc->pass = NULL;
	}

	info( "Logging in...\n" );
	for (mech = srvc->auth_mechs; mech; mech = mech->next) {
		int any = equals( mech->string, -1, "*", 1 );
		for (cmech = ctx->auth_mechs; cmech; cmech = cmech->next) {
			if (any || !strcasecmp( mech->string, cmech->string )) {
				if (!strcasecmp( cmech->string, "LOGIN" )) {
#ifdef HAVE_LIBSSL
					if (ctx->conn.ssl || ctx->conn.conf->tunnel || !any)
#else
					if (ctx->conn.conf->tunnel || !any)
#endif
						auth_login = 1;
					else
						skipped_login = 1;
#ifdef HAVE_LIBSASL
				} else {
					uint len = strlen( cmech->string );
					if (saslend + len + 2 > saslmechs + sizeof(saslmechs))
						oob();
					*saslend++ = ' ';
					memcpy( saslend, cmech->string, len + 1 );
					saslend += len;

					if (!strcasecmp( cmech->string, "EXTERNAL" ))
						want_external = 1;
#endif
				}
			}
		}
	}
#ifdef HAVE_LIBSASL
	if (saslend != saslmechs) {
		int rc;
		uint out_len = 0;
		char *enc = NULL;
		const char *gotmech = NULL, *out = NULL;
		sasl_interact_t *interact = NULL;
		imap_cmd_t *cmd;
		static int sasl_inited;

		if (!sasl_inited) {
			rc = sasl_client_init( sasl_callbacks );
			if (rc != SASL_OK) {
			  saslbail:
				error( "Error initializing SASL client: %s\n", sasl_errstring( rc, NULL, NULL ) );
				goto bail;
			}
			sasl_inited = 1;
		}

		rc = sasl_client_new( "imap", srvc->sconf.host, NULL, NULL, NULL, 0, &ctx->sasl );
		if (rc != SASL_OK) {
			if (rc == SASL_NOMECH)
				goto notsasl;
			if (!ctx->sasl)
				goto saslbail;
			error( "Error initializing SASL context: %s\n", sasl_errdetail( ctx->sasl ) );
			goto bail;
		}

		// The built-in EXTERNAL mechanism wants the authentication id to be set
		// even before instantiation; consequently it won't prompt for it, either.
		// While this clearly makes sense on the server side, it arguably does not
		// on the client side. Ah, well ...
		if (want_external && ensure_user( srvc )) {
			rc = sasl_setprop( ctx->sasl, SASL_AUTH_EXTERNAL, srvc->user );
			if (rc != SASL_OK ) {
				error( "Error setting SASL authentication id: %s\n", sasl_errdetail( ctx->sasl ) );
				goto bail;
			}
		}

		rc = sasl_client_start( ctx->sasl, saslmechs + 1, &interact, CAP(SASLIR) ? &out : NULL, &out_len, &gotmech );
		if (rc == SASL_NOMECH)
			goto notsasl;
		if (gotmech)
			info( "Authenticating with SASL mechanism %s...\n", gotmech );
		/* Technically, we are supposed to loop over sasl_client_start(),
		 * but it just calls sasl_client_step() anyway. */
		if (process_sasl_step( ctx, rc, NULL, 0, interact, CAP(SASLIR) ? &out : NULL, &out_len ) < 0)
			goto bail;
		if (out) {
			if (!out_len)
				enc = nfstrdup( "=" ); /* A zero-length initial response is encoded as padding. */
			else if (encode_sasl_data( out, out_len, &enc, NULL ) < 0)
				goto bail;
		}

		cmd = new_imap_cmd( sizeof(*cmd) );
		cmd->param.cont = do_sasl_auth;
		ctx->caps = 0;
		imap_exec( ctx, cmd, done_sasl_auth, enc ? "AUTHENTICATE %s %s" : "AUTHENTICATE %s", gotmech, enc );
		free( enc );
		return;
	  notsasl:
		if (!ctx->sasl || sasl_listmech( ctx->sasl, NULL, "", " ", "", &saslavail, NULL, NULL ) != SASL_OK)
			saslavail = "(none)";
		if (!auth_login) {
			error( "IMAP error: selected SASL mechanism(s) not available;\n"
			       "   selected:%s\n   available: %s\n", saslmechs, saslavail );
			goto skipnote;
		}
		info( "NOT using available SASL mechanism(s): %s\n", saslavail );
		sasl_dispose( &ctx->sasl );
	}
#endif
	if (auth_login) {
		if (!ensure_user( srvc ) || !ensure_password( srvc ))
			goto bail;
#ifdef HAVE_LIBSSL
		if (!ctx->conn.ssl && !ctx->conn.conf->tunnel)
#endif
			warn( "*** IMAP Warning *** Password is being sent in the clear\n" );
		ctx->caps = 0;
		imap_exec( ctx, NULL, imap_open_store_authenticate2_p2,
		           "LOGIN \"%\\s\" \"%\\s\"", srvc->user, srvc->pass );
		return;
	}
	error( "IMAP error: server supports no acceptable authentication mechanism\n" );
#ifdef HAVE_LIBSASL
  skipnote:
#endif
	if (skipped_login)
		error( "Note: not using LOGIN because connection is not encrypted;\n"
		       "      use 'AuthMechs LOGIN' explicitly to force it.\n" );

  bail:
	imap_open_store_bail( ctx, FAIL_FINAL );
}

static void
imap_open_store_authenticate2_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO) {
		imap_open_store_bail( ctx, FAIL_FINAL );
	} else if (response == RESP_OK) {
		// iCloud (imap.mail.me.com) apparently runs the real server behind a
		// proxy that injects XAPPLEPUSHSERVICE into (and deletes STARTTLS from)
		// the server's outgoing data stream following occurrences of CAPABILITY.
		// This process is rather indiscriminate, so it will mess up IMAP
		// literals if it is not deactivated in time by issuing a (redundant)
		// CAPABILITY command after logging in.
		if (!ctx->caps || ctx->capability_hack)
			imap_exec( ctx, NULL, imap_open_store_authenticate2_p3, "CAPABILITY" );
		else
			imap_open_store_compress( ctx );
	}
}

static void
imap_open_store_authenticate2_p3( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO)
		imap_open_store_bail( ctx, FAIL_FINAL );
	else if (response == RESP_OK)
		imap_open_store_compress( ctx );
}

static void
imap_open_store_compress( imap_store_t *ctx )
{
#ifdef HAVE_LIBZ
	if (CAP(COMPRESS_DEFLATE)) {
		imap_exec( ctx, NULL, imap_open_store_compress_p2, "COMPRESS DEFLATE" );
		return;
	}
#endif
	imap_open_store_namespace( ctx );
}

#ifdef HAVE_LIBZ
static void
imap_open_store_compress_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO) {
		/* We already reported an error, but it's not fatal to us. */
		imap_open_store_namespace( ctx );
	} else if (response == RESP_OK) {
		socket_start_deflate( &ctx->conn );
		imap_open_store_namespace( ctx );
	}
}
#endif

static void
imap_open_store_namespace( imap_store_t *ctx )
{
	imap_store_conf_t *cfg = ctx->conf;

	ctx->state = SST_HALF;
	ctx->prefix = cfg->path;
	ctx->delimiter[0] = cfg->delimiter;
	if (((!ctx->prefix && cfg->use_namespace) || !cfg->delimiter) && CAP(NAMESPACE)) {
		/* get NAMESPACE info */
		if (!ctx->got_namespace)
			imap_exec( ctx, NULL, imap_open_store_namespace_p2, "NAMESPACE" );
		else
			imap_open_store_namespace2( ctx );
		return;
	}
	imap_open_store_finalize( ctx );
}

static void
imap_open_store_namespace_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	if (response == RESP_NO) {
		imap_open_store_bail( ctx, FAIL_FINAL );
	} else if (response == RESP_OK) {
		ctx->got_namespace = 1;
		imap_open_store_namespace2( ctx );
	}
}

static void
imap_open_store_namespace2( imap_store_t *ctx )
{
	if (!ctx->prefix && ctx->conf->use_namespace)
		ctx->prefix = ctx->ns_prefix;
	if (!ctx->delimiter[0])
		ctx->delimiter[0] = ctx->ns_delimiter;
	imap_open_store_finalize( ctx );
}

static void
imap_open_store_finalize( imap_store_t *ctx )
{
	ctx->state = SST_GOOD;
	if (!ctx->prefix)
		ctx->prefix = "";
	else
		normalize_INBOX( ctx, ctx->prefix, -1 );
	ctx->trashnc = TrashUnknown;
	ctx->callbacks.imap_open( DRV_OK, ctx->callback_aux );
}

#ifdef HAVE_LIBSSL
static void
imap_open_store_ssl_bail( imap_store_t *ctx )
{
	/* This avoids that we try to send LOGOUT to an unusable socket. */
	socket_close( &ctx->conn );
	imap_open_store_bail( ctx, FAIL_FINAL );
}
#endif

static void
imap_open_store_bail( imap_store_t *ctx, int failed )
{
	ctx->conf->server->failed = (char)failed;
	ctx->callbacks.imap_open( DRV_STORE_BAD, ctx->callback_aux );
}

/******************* imap_open_box *******************/

static int
imap_select_box( store_t *gctx, const char *name )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	assert( !ctx->pending && !ctx->in_progress && !ctx->wait_check );

	reset_imap_messages( &ctx->msgs );

	ctx->name = name;
	return DRV_OK;
}

static const char *
imap_get_box_path( store_t *gctx ATTR_UNUSED )
{
	return NULL;
}

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD
		void (*callback)( int sts, uint uidvalidity, void *aux );
		void *callback_aux;
	};
} imap_cmd_open_box_t;

static void imap_open_box_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_open_box_p3( imap_store_t *, imap_cmd_t *, int );
static void imap_open_box_p4( imap_store_t *, imap_cmd_open_box_t *, int );

static void
imap_open_box( store_t *gctx,
               void (*cb)( int sts, uint uidvalidity, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	char *buf;

	if (prepare_box( &buf, ctx ) < 0) {
		cb( DRV_BOX_BAD, UIDVAL_BAD, aux );
		return;
	}

	ctx->uidvalidity = UIDVAL_BAD;
	ctx->uidnext = 0;

	INIT_IMAP_CMD(imap_cmd_open_box_t, cmd, cb, aux)
	cmd->param.failok = 1;
	imap_exec( ctx, &cmd->gen, imap_open_box_p2,
	           "SELECT \"%\\s\"", buf );
	free( buf );
}

static void
imap_open_box_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_open_box_t *cmdp = (imap_cmd_open_box_t *)gcmd;

	if (response != RESP_OK || ctx->uidnext) {
		imap_open_box_p4( ctx, cmdp, response );
		return;
	}

	assert( ctx->fetch_sts == FetchNone );
	ctx->fetch_sts = FetchUidNext;
	INIT_IMAP_CMD(imap_cmd_open_box_t, cmd, cmdp->callback, cmdp->callback_aux)
	imap_exec( ctx, &cmd->gen, imap_open_box_p3,
	           "UID FETCH * (UID)" );
}

static void
imap_open_box_p3( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_open_box_t *cmdp = (imap_cmd_open_box_t *)gcmd;

	ctx->fetch_sts = FetchNone;
	if (!ctx->uidnext) {
		if (ctx->total_msgs) {
			error( "IMAP error: querying server for highest UID failed\n" );
			imap_open_box_p4( ctx, cmdp, RESP_NO );
			return;
		}
		// This is ok, the box is simply empty.
		ctx->uidnext = 1;
	}

	imap_open_box_p4( ctx, cmdp, response );
}

static void
imap_open_box_p4( imap_store_t *ctx, imap_cmd_open_box_t *cmdp, int response )
{
	transform_box_response( &response );
	cmdp->callback( response, ctx->uidvalidity, cmdp->callback_aux );
}

static uint
imap_get_uidnext( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->uidnext;
}

static xint
imap_get_supported_flags( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->has_forwarded ? 255 : (255 & ~F_FORWARDED);
}

/******************* imap_create_box *******************/

static void
imap_create_box( store_t *gctx,
                 void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	char *buf;

	if (prepare_box( &buf, ctx ) < 0) {
		cb( DRV_BOX_BAD, aux );
		return;
	}

	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
	imap_exec( ctx, &cmd->gen, imap_done_simple_box,
	           "CREATE \"%\\s\"", buf );
	free( buf );
}

/******************* imap_delete_box *******************/

static int
imap_confirm_box_empty( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->total_msgs ? DRV_BOX_BAD : DRV_OK;
}

static void imap_delete_box_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_delete_box( store_t *gctx,
                 void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
	imap_exec( ctx, &cmd->gen, imap_delete_box_p2, "CLOSE" );
}

static void
imap_delete_box_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_simple_t *cmdp = (imap_cmd_simple_t *)gcmd;
	char *buf;

	if (response != RESP_OK) {
		imap_done_simple_box( ctx, &cmdp->gen, response );
		return;
	}

	if (prepare_box( &buf, ctx ) < 0) {
		imap_done_simple_box( ctx, &cmdp->gen, RESP_NO );
		return;
	}
	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cmdp->callback, cmdp->callback_aux)
	imap_exec( ctx, &cmd->gen, imap_done_simple_box,
	           "DELETE \"%\\s\"", buf );
	free( buf );
}

static int
imap_finish_delete_box( store_t *gctx ATTR_UNUSED )
{
	return DRV_OK;
}

/******************* imap_load_box *******************/

static uint
imap_prepare_load_box( store_t *gctx, uint opts )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	if (!CAP(UIDPLUS))
		opts &= ~OPEN_UID_EXPUNGE;
	ctx->opts = opts;
	return opts;
}

enum { WantSize = 1, WantTuids = 2, WantMsgids = 4 };
typedef struct {
	uint first, last;
	int flags;
} imap_range_t;

#define MAX_RANGES 4

static void
imap_set_range( imap_range_t *ranges, uint *nranges, int low_flags, int high_flags, uint maxlow )
{
	if (low_flags != high_flags) {
		for (uint r = 0; r < *nranges; r++) {
			if (ranges[r].first > maxlow)
				break; /* Range starts above split point; so do all subsequent ranges. */
			if (ranges[r].last < maxlow)
				continue; /* Range ends below split point; try next one. */
			if (ranges[r].last != maxlow) {
				/* Range does not end exactly at split point; need to split. */
				if (*nranges == MAX_RANGES)
					oob();
				memmove( &ranges[r + 1], &ranges[r], ((*nranges)++ - r) * sizeof(*ranges) );
				ranges[r].last = maxlow;
				ranges[r + 1].first = maxlow + 1;
			}
			break;
		}
	}
	for (uint r = 0; r < *nranges; r++)
		ranges[r].flags |= (ranges[r].last <= maxlow) ? low_flags : high_flags;
}

typedef union {
	imap_cmd_refcounted_state_t gen;
	struct {
		IMAP_CMD_REFCOUNTED_STATE
		void (*callback)( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux );
		void *callback_aux;
	};
} imap_load_box_state_t;

static void imap_submit_load( imap_store_t *, const char *, int, imap_load_box_state_t * );
static void imap_submit_load_p3( imap_store_t *ctx, imap_load_box_state_t * );

static void
imap_load_box( store_t *gctx, uint minuid, uint maxuid, uint finduid, uint pairuid, uint newuid, uint_array_t excs,
               void (*cb)( int sts, message_t *msgs, int total_msgs, int recent_msgs, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	char buf[1000];

	if (!ctx->total_msgs) {
		free( excs.data );
		cb( DRV_OK, NULL, 0, 0, aux );
	} else {
		assert( ctx->fetch_sts == FetchNone );
		ctx->fetch_sts = FetchMsgs;

		INIT_REFCOUNTED_STATE(imap_load_box_state_t, sts, cb, aux)
		for (uint i = 0; i < excs.size; ) {
			for (int bl = 0; i < excs.size && bl < 960; i++) {
				if (bl)
					buf[bl++] = ',';
				bl += sprintf( buf + bl, "%u", excs.data[i] );
				uint j = i;
				for (; i + 1 < excs.size && excs.data[i + 1] == excs.data[i] + 1; i++) {}
				if (i != j)
					bl += sprintf( buf + bl, ":%u", excs.data[i] );
			}
			imap_submit_load( ctx, buf, shifted_bit( ctx->opts, OPEN_PAIRED_IDS, WantMsgids ), sts );
		}
		if (maxuid == UINT_MAX)
			maxuid = ctx->uidnext - 1;
		if (maxuid >= minuid) {
			imap_range_t ranges[MAX_RANGES];
			ranges[0].first = minuid;
			ranges[0].last = maxuid;
			ranges[0].flags = 0;
			uint nranges = 1;
			if (ctx->opts & OPEN_OLD_SIZE) {
				if (ctx->opts & OPEN_NEW_SIZE)
					ranges[0].flags = WantSize;
				else
					imap_set_range( ranges, &nranges, WantSize, 0, newuid );
			} else if (ctx->opts & OPEN_NEW_SIZE) {
				imap_set_range( ranges, &nranges, 0, WantSize, newuid );
			}
			if (ctx->opts & OPEN_FIND)
				imap_set_range( ranges, &nranges, 0, WantTuids, finduid - 1 );
			if (ctx->opts & OPEN_PAIRED_IDS)
				imap_set_range( ranges, &nranges, WantMsgids, 0, pairuid );
			for (uint r = 0; r < nranges; r++) {
				sprintf( buf, "%u:%u", ranges[r].first, ranges[r].last );
				imap_submit_load( ctx, buf, ranges[r].flags, sts );
			}
		}
		free( excs.data );
		imap_submit_load_p3( ctx, sts );
	}
}

static void imap_submit_load_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_submit_load( imap_store_t *ctx, const char *buf, int flags, imap_load_box_state_t *sts )
{
	imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_submit_load_p2,
	           "UID FETCH %s (UID%s%s%s%s%s%s%s)", buf,
	           (ctx->opts & OPEN_FLAGS) ? " FLAGS" : "",
	           (flags & WantSize) ? " RFC822.SIZE" : "",
	           (flags & (WantTuids | WantMsgids)) ? " BODY.PEEK[HEADER.FIELDS (" : "",
	           (flags & WantTuids) ? "X-TUID" : "",
	           !(~flags & (WantTuids | WantMsgids)) ? " " : "",
	           (flags & WantMsgids) ? "MESSAGE-ID" : "",
	           (flags & (WantTuids | WantMsgids)) ? ")]" : "");
}

static void
imap_submit_load_p2( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	imap_load_box_state_t *sts = (imap_load_box_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_box_response( &sts->gen, response );
	imap_submit_load_p3( ctx, sts );
}

static void
imap_submit_load_p3( imap_store_t *ctx, imap_load_box_state_t *sts )
{
	DONE_REFCOUNTED_STATE_ARGS(sts, {
		ctx->fetch_sts = FetchNone;
		if (sts->ret_val == DRV_OK)
			imap_ensure_relative( &ctx->msgs );
	}, &ctx->msgs.head->gen, ctx->total_msgs, ctx->recent_msgs)
}

/******************* imap_fetch_msg *******************/

static void imap_fetch_msg_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_fetch_msg( store_t *ctx, message_t *msg, msg_data_t *data, int minimal,
                void (*cb)( int sts, void *aux ), void *aux )
{
	INIT_IMAP_CMD(imap_cmd_fetch_msg_t, cmd, cb, aux)
	cmd->param.uid = msg->uid;
	cmd->msg_data = data;
	data->data = NULL;
	imap_exec( (imap_store_t *)ctx, &cmd->gen.gen, imap_fetch_msg_p2,
	           "UID FETCH %u (%s%sBODY.PEEK[%s])", msg->uid,
	           !(msg->status & M_FLAGS) ? "FLAGS " : "",
	           (data->date== -1) ? "INTERNALDATE " : "",
	           minimal ? "HEADER" : "" );
}

static void
imap_fetch_msg_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_fetch_msg_t *cmd = (imap_cmd_fetch_msg_t *)gcmd;

	if (response == RESP_OK && !cmd->msg_data->data) {
		// The UID FETCH succeeded, but there is no message with this UID.
		// The corresponding EXPUNGE response has been received by this time,
		// so the message is already marked as dead.
		response = RESP_NO;
	}
	imap_done_simple_msg( ctx, gcmd, response );
}

/******************* imap_set_msg_flags *******************/

static uint
imap_make_flags( int flags, char *buf )
{
	const char *s;
	uint i, d;

	for (i = d = 0; i < as(Flags); i++) {
		if (flags & (1 << i)) {
			buf[d++] = ' ';
			for (s = Flags[i].str; *s; s++)
				buf[d++] = *s;
		}
	}
	buf[0] = '(';
	buf[d++] = ')';
	return d;
}

typedef union {
	imap_cmd_refcounted_state_t gen;
	struct {
		IMAP_CMD_REFCOUNTED_STATE
		void (*callback)( int sts, void *aux );
		void *callback_aux;
		message_t *msg;
		int add, del;
	};
} imap_set_msg_flags_state_t;

static void imap_set_flags_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_set_flags_p3( imap_set_msg_flags_state_t * );

static void
imap_flags_helper( imap_store_t *ctx, uint uid, char what, int flags,
                   imap_set_msg_flags_state_t *sts )
{
	char buf[256];

	buf[imap_make_flags( flags, buf )] = 0;
	imap_cmd_t *cmd = imap_refcounted_new_cmd( &sts->gen );
	cmd->param.wait_check = 1;
	imap_exec( ctx, cmd, imap_set_flags_p2,
	           "UID STORE %u %cFLAGS.SILENT %s", uid, what, buf );
}

static void
imap_set_msg_flags( store_t *gctx, message_t *msg, uint uid, int add, int del,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	assert( add || del );
	INIT_REFCOUNTED_STATE(imap_set_msg_flags_state_t, sts, cb, aux)
	sts->msg = msg;
	sts->add = add;
	sts->del = del;
	if (add)
		imap_flags_helper( ctx, uid, '+', add, sts );
	if (del)
		imap_flags_helper( ctx, uid, '-', del, sts );
	imap_set_flags_p3( sts );
}

static void
imap_set_flags_p2( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *cmd, int response )
{
	imap_set_msg_flags_state_t *sts = (imap_set_msg_flags_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_msg_response( &sts->gen, response);
	imap_set_flags_p3( sts );
}

static void
imap_set_flags_p3( imap_set_msg_flags_state_t *sts )
{
	DONE_REFCOUNTED_STATE_FINALIZE(sts, {
		if (sts->msg)
			sts->msg->flags = (sts->msg->flags & ~sts->del) | sts->add;
	})
}

/******************* imap_close_box *******************/

#define IMAP_CMD_EXPUNGE \
	void (*callback)( int sts, int reported, void *aux ); \
	void *callback_aux;

typedef union {
	imap_cmd_refcounted_state_t gen;
	struct {
		IMAP_CMD_REFCOUNTED_STATE
		IMAP_CMD_EXPUNGE
	};
} imap_expunge_state_t;

typedef union {
	imap_cmd_t gen;
	struct {
		IMAP_CMD
		IMAP_CMD_EXPUNGE
	};
} imap_cmd_close_t;

static void imap_close_box_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_close_box_p3( imap_expunge_state_t * );
static void imap_close_box_simple_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_close_box( store_t *gctx,
                void (*cb)( int sts, int reported, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	assert( !ctx->num_wait_check );

	if (ctx->opts & OPEN_UID_EXPUNGE) {
		INIT_REFCOUNTED_STATE(imap_expunge_state_t, sts, cb, aux)
		imap_message_t *msg, *fmsg, *nmsg;
		int bl;
		char buf[1000];

		for (msg = ctx->msgs.head; ; ) {
			for (bl = 0; msg && bl < 960; msg = msg->next) {
				if ((msg->status & M_DEAD) || !(msg->status & M_EXPUNGE))
					continue;
				if (bl)
					buf[bl++] = ',';
				bl += sprintf( buf + bl, "%u", msg->uid );
				fmsg = msg;
				for (; (nmsg = msg->next); msg = nmsg) {
					if (nmsg->status & M_DEAD) {
						// Messages that jump a gap interrupt the range, even expunged ones.
						if (nmsg->seq)
							break;
					} else {
						if (nmsg->seq > 1)
							break;
						if (!(nmsg->flags & M_EXPUNGE))
							break;
					}
				}
				if (msg != fmsg)
					bl += sprintf( buf + bl, ":%u", msg->uid );
			}
			if (!bl)
				break;
			imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_close_box_p2,
			           "UID EXPUNGE %s", buf );
		}
		imap_close_box_p3( sts );
	} else {
		/* This is inherently racy: it may cause messages which other clients
		 * marked as deleted to be expunged without being trashed. */
		// Note that, to save bandwidth, we don't use EXPUNGE. Also, in many
		// cases, we wouldn't be able to map the EXPUNGE responses' seq numbers
		// anyway, due to not having fetched the messages.
		INIT_IMAP_CMD(imap_cmd_close_t, cmd, cb, aux)
		imap_exec( ctx, &cmd->gen, imap_close_box_simple_p2, "CLOSE" );
	}
}

static void
imap_close_box_p2( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *cmd, int response )
{
	imap_expunge_state_t *sts = (imap_expunge_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_box_response( &sts->gen, response );
	imap_close_box_p3( sts );
}

static void
imap_close_box_p3( imap_expunge_state_t *sts )
{
	DONE_REFCOUNTED_STATE_ARGS(sts, , 1)
}

static void
imap_close_box_simple_p2( imap_store_t *ctx ATTR_UNUSED,
                          imap_cmd_t *cmd, int response )
{
	imap_cmd_close_t *cmdp = (imap_cmd_close_t *)cmd;

	transform_box_response( &response );
	cmdp->callback( response, 0, cmdp->callback_aux );
}

/******************* imap_trash_msg *******************/

static void
imap_trash_msg( store_t *gctx, message_t *msg,
                void (*cb)( int sts, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	char *buf;

	INIT_IMAP_CMD(imap_cmd_simple_t, cmd, cb, aux)
	cmd->param.create = 1;
	cmd->param.to_trash = 1;
	if (prepare_trash( &buf, ctx ) < 0) {
		cb( DRV_BOX_BAD, aux );
		free( cmd );
		return;
	}
	imap_exec( ctx, &cmd->gen, imap_done_simple_msg,
	           CAP(MOVE) ? "UID MOVE %u \"%\\s\"" : "UID COPY %u \"%\\s\"", msg->uid, buf );
	free( buf );
}

/******************* imap_store_msg *******************/

static void imap_store_msg_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_store_msg( store_t *gctx, msg_data_t *data, int to_trash,
                void (*cb)( int sts, uint uid, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	char *buf;
	uint d;
	char flagstr[128], datestr[64];

	d = 0;
	if (data->flags) {
		d = imap_make_flags( data->flags, flagstr );
		flagstr[d++] = ' ';
	}
	flagstr[d] = 0;
	if (data->date) {
DIAG_PUSH
DIAG_DISABLE("-Wformat")  // configure ensures that %z actually works.
		strftime( datestr, sizeof(datestr), "\"%d-%b-%Y %H:%M:%S %z\" ", localtime( &data->date ) );
DIAG_POP
	} else {
		datestr[0] = 0;
	}

	INIT_IMAP_CMD(imap_cmd_out_uid_t, cmd, cb, aux)
	ctx->buffer_mem += data->len;
	cmd->param.data_len = data->len;
	cmd->param.data = data->data;
	cmd->param.failok = 2;

	if (to_trash) {
		cmd->param.create = 1;
		cmd->param.to_trash = 1;
		if (prepare_trash( &buf, ctx ) < 0) {
			cb( DRV_BOX_BAD, 0, aux );
			free( cmd );
			return;
		}
	} else {
		if (prepare_box( &buf, ctx ) < 0) {
			cb( DRV_BOX_BAD, 0, aux );
			free( cmd );
			return;
		}
	}
	imap_exec( ctx, &cmd->gen, imap_store_msg_p2,
	           "APPEND \"%\\s\" %s%s{%u+}", buf, flagstr, datestr, data->len );
	free( buf );
}

static void
imap_store_msg_p2( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *cmd, int response )
{
	imap_cmd_out_uid_t *cmdp = (imap_cmd_out_uid_t *)cmd;

	transform_msg_response( &response );
	cmdp->callback( response, cmdp->param.uid, cmdp->callback_aux );
}

/******************* imap_find_new_msgs *******************/

static void imap_find_new_msgs_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_find_new_msgs_p3( imap_store_t *, imap_cmd_t *, int );
static void imap_find_new_msgs_p4( imap_store_t *, imap_cmd_t *, int );

static void
imap_find_new_msgs( store_t *gctx, uint newuid,
                    void (*cb)( int sts, message_t *msgs, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	INIT_IMAP_CMD(imap_cmd_find_new_t, cmd, cb, aux)
	cmd->out_msgs = ctx->msgs.tail;
	cmd->uid = newuid;
	// Some servers fail to enumerate recently APPENDed messages without syncing first.
	imap_exec( ctx, &cmd->gen, imap_find_new_msgs_p2, "CHECK" );
}

static void
imap_find_new_msgs_p2( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_find_new_t *cmdp = (imap_cmd_find_new_t *)gcmd;

	if (response != RESP_OK) {
		imap_done_simple_box( ctx, gcmd, response );
		return;
	}

	// We appended messages, so we need to re-query UIDNEXT.
	ctx->uidnext = 0;
	assert( ctx->fetch_sts == FetchNone );
	ctx->fetch_sts = FetchUidNext;

	INIT_IMAP_CMD(imap_cmd_find_new_t, cmd, cmdp->callback, cmdp->callback_aux)
	cmd->out_msgs = cmdp->out_msgs;
	cmd->uid = cmdp->uid;
	imap_exec( ctx, &cmd->gen, imap_find_new_msgs_p3,
	           "UID FETCH * (UID)" );
}

static void
imap_find_new_msgs_p3( imap_store_t *ctx, imap_cmd_t *gcmd, int response )
{
	imap_cmd_find_new_t *cmdp = (imap_cmd_find_new_t *)gcmd;

	if (response != RESP_OK) {
		imap_find_new_msgs_p4( ctx, gcmd, response );
		return;
	}
	if (ctx->uidnext <= cmdp->uid) {
		if (!ctx->uidnext && ctx->total_msgs) {
			error( "IMAP error: re-querying server for highest UID failed\n" );
			imap_find_new_msgs_p4( ctx, gcmd, RESP_NO );
		} else {
			// The messages evaporated, or the server just didn't register them -
			// we'll catch that later (via lost TUIDs).
			// This case is why we do the extra roundtrip instead of simply passing
			// '*' as the end of the range below - IMAP ranges are unordered, so we
			// would potentially re-fetch an already loaded message.
			imap_find_new_msgs_p4( ctx, gcmd, RESP_OK );
		}
		return;
	}
	ctx->fetch_sts = FetchMsgs;
	INIT_IMAP_CMD(imap_cmd_find_new_t, cmd, cmdp->callback, cmdp->callback_aux)
	cmd->out_msgs = cmdp->out_msgs;
	imap_exec( ctx, &cmd->gen, imap_find_new_msgs_p4,
	           "UID FETCH %u:%u (UID BODY.PEEK[HEADER.FIELDS (X-TUID)])", cmdp->uid, ctx->uidnext - 1 );
}

static void
imap_find_new_msgs_p4( imap_store_t *ctx ATTR_UNUSED, imap_cmd_t *gcmd, int response )
{
	imap_cmd_find_new_t *cmdp = (imap_cmd_find_new_t *)gcmd;

	ctx->fetch_sts = FetchNone;
	transform_box_response( &response );
	// Note: unlike in load_box(), we don't call imap_ensure_relative() here,
	// as it's unnecessary. It being called due to unsolicited responses
	// causes no harm.
	cmdp->callback( response, &(*cmdp->out_msgs)->gen, cmdp->callback_aux );
}

/******************* imap_list_store *******************/

typedef union {
	imap_cmd_refcounted_state_t gen;
	struct {
		IMAP_CMD_REFCOUNTED_STATE
		void (*callback)( int sts, string_list_t *, void *aux );
		void *callback_aux;
	};
} imap_list_store_state_t;

static void imap_list_store_p2( imap_store_t *, imap_cmd_t *, int );
static void imap_list_store_p3( imap_store_t *, imap_list_store_state_t * );

static void
imap_list_store( store_t *gctx, int flags,
                 void (*cb)( int sts, string_list_t *boxes, void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;
	imap_store_conf_t *cfg = ctx->conf;
	INIT_REFCOUNTED_STATE(imap_list_store_state_t, sts, cb, aux)

	// ctx->prefix may be empty, "INBOX.", or something else.
	// 'flags' may be LIST_INBOX, LIST_PATH (or LIST_PATH_MAYBE), or both. 'listed'
	// already containing a particular value effectively removes it from 'flags'.
	// This matrix determines what to query, and what comes out as a side effect.
	// The lowercase letters indicate unnecessary results; the queries are done
	// this way to have non-overlapping result sets, so subsequent calls create
	// no duplicates:
	//
	// qry \ pfx | empty | inbox | other
	// ----------+-------+-------+-------
	// inbox     | p [I] | I [p] | I
	// both      | P [I] | I [P] | I + P
	// path      | P [i] | i [P] | P
	//
	int pfx_is_empty = !*ctx->prefix;
	int pfx_is_inbox = !pfx_is_empty && is_inbox( ctx, ctx->prefix, -1 );
	if (((flags & (LIST_PATH | LIST_PATH_MAYBE)) || pfx_is_empty) && !pfx_is_inbox && !(ctx->listed & LIST_PATH)) {
		ctx->listed |= LIST_PATH;
		if (pfx_is_empty)
			ctx->listed |= LIST_INBOX;
		imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_list_store_p2,
		           "%s \"\" \"%\\s*\"", cfg->use_lsub ? "LSUB" : "LIST", ctx->prefix );
	}
	if (((flags & LIST_INBOX) || pfx_is_inbox) && !pfx_is_empty && !(ctx->listed & LIST_INBOX)) {
		ctx->listed |= LIST_INBOX;
		if (pfx_is_inbox)
			ctx->listed |= LIST_PATH;
		imap_exec( ctx, imap_refcounted_new_cmd( &sts->gen ), imap_list_store_p2,
		           "%s \"\" INBOX*", cfg->use_lsub ? "LSUB" : "LIST" );
	}
	imap_list_store_p3( ctx, sts );
}

static void
imap_list_store_p2( imap_store_t *ctx, imap_cmd_t *cmd, int response )
{
	imap_list_store_state_t *sts = (imap_list_store_state_t *)((imap_cmd_refcounted_t *)cmd)->state;

	transform_refcounted_box_response( &sts->gen, response );
	imap_list_store_p3( ctx, sts );
}

static void
imap_list_store_p3( imap_store_t *ctx, imap_list_store_state_t *sts )
{
	DONE_REFCOUNTED_STATE_ARGS(sts, , ctx->boxes)
}

/******************* imap_cancel_cmds *******************/

static void
imap_cancel_cmds( store_t *gctx,
                  void (*cb)( void *aux ), void *aux )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	finalize_checked_imap_cmds( ctx, RESP_CANCEL );
	cancel_pending_imap_cmds( ctx );
	if (ctx->in_progress) {
		ctx->canceling = 1;
		ctx->callbacks.imap_cancel = cb;
		ctx->callback_aux = aux;
	} else {
		cb( aux );
	}
}

/******************* imap_commit_cmds *******************/

static void imap_commit_cmds_p2( imap_store_t *, imap_cmd_t *, int );

static void
imap_commit_cmds( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	if (ctx->num_wait_check)
		imap_exec( ctx, NULL, imap_commit_cmds_p2, "CHECK" );
}

static void
imap_commit_cmds_p2( imap_store_t *ctx, imap_cmd_t *cmd ATTR_UNUSED, int response )
{
	finalize_checked_imap_cmds( ctx, response );
}

/******************* imap_get_memory_usage *******************/

static uint
imap_get_memory_usage( store_t *gctx )
{
	imap_store_t *ctx = (imap_store_t *)gctx;

	return ctx->buffer_mem + ctx->conn.buffer_mem;
}

/******************* imap_get_fail_state *******************/

static int
imap_get_fail_state( store_conf_t *gconf )
{
	return ((imap_store_conf_t *)gconf)->server->failed;
}

/******************* imap_parse_store *******************/

static imap_server_conf_t *servers, **serverapp = &servers;

static int
imap_parse_store( conffile_t *cfg, store_conf_t **storep )
{
	imap_store_conf_t *store;
	imap_server_conf_t *server, *srv, sserver;
	const char *type, *name, *arg;
	unsigned u;
	int acc_opt = 0;
	/* Legacy SASL option */
	int require_cram = -1;

	if (!strcasecmp( "IMAPAccount", cfg->cmd )) {
		server = nfzalloc( sizeof(*server) );
		name = server->name = nfstrdup( cfg->val );
		*serverapp = server;
		serverapp = &server->next;
		store = NULL;
		*storep = NULL;
		type = "IMAP account";
	} else if (!strcasecmp( "IMAPStore", cfg->cmd )) {
		store = nfzalloc( sizeof(*store) );
		store->driver = &imap_driver;
		name = store->name = nfstrdup( cfg->val );
		store->use_namespace = 1;
		*storep = &store->gen;
		memset( &sserver, 0, sizeof(sserver) );
		server = &sserver;
		type = "IMAP store";
	} else {
		return 0;
	}

	server->sconf.timeout = 20000;
#ifdef HAVE_LIBSSL
	server->ssl_type = -1;
	server->sconf.ssl_versions = TLSv1_2 | TLSv1_3;
	server->sconf.system_certs = 1;
#endif
	server->max_in_progress = INT_MAX;

	while (getcline( cfg ) && cfg->cmd) {
		if (!strcasecmp( "Host", cfg->cmd )) {
			server->sconf.host = nfstrdup( cfg->val );
		} else if (!strcasecmp( "User", cfg->cmd )) {
			server->user = nfstrdup( cfg->val );
		} else if (!strcasecmp( "UserCmd", cfg->cmd )) {
			server->user_cmd = nfstrdup( cfg->val );
		} else if (!strcasecmp( "Pass", cfg->cmd )) {
			server->pass = nfstrdup( cfg->val );
		} else if (!strcasecmp( "PassCmd", cfg->cmd )) {
			server->pass_cmd = nfstrdup( cfg->val );
#ifdef HAVE_MACOS_KEYCHAIN
		} else if (!strcasecmp( "UseKeychain", cfg->cmd )) {
			server->use_keychain = parse_bool( cfg );
#endif
		} else if (!strcasecmp( "Port", cfg->cmd )) {
			int port = parse_int( cfg );
			if ((unsigned)port > 0xffff) {
				error( "%s:%d: Invalid port number\n", cfg->file, cfg->line );
				cfg->err = 1;
			} else {
				server->sconf.port = (ushort)port;
			}
		} else if (!strcasecmp( "Timeout", cfg->cmd )) {
			server->sconf.timeout = parse_int( cfg ) * 1000;
		} else if (!strcasecmp( "PipelineDepth", cfg->cmd )) {
			if ((server->max_in_progress = parse_int( cfg )) < 1) {
				error( "%s:%d: PipelineDepth must be at least 1\n", cfg->file, cfg->line );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "DisableExtension", cfg->cmd ) ||
		           !strcasecmp( "DisableExtensions", cfg->cmd )) {
			arg = cfg->val;
			do {
				for (u = 0; u < as(cap_list); u++) {
					if (equals_upper( arg, -1, cap_list[u].str, cap_list[u].len )) {
						server->cap_mask |= 1 << u;
						goto gotcap;
					}
				}
				error( "%s:%d: Unrecognized IMAP extension '%s'\n", cfg->file, cfg->line, arg );
				cfg->err = 1;
			  gotcap: ;
			} while ((arg = get_arg( cfg, ARG_OPTIONAL, NULL )));
#ifdef HAVE_LIBSSL
		} else if (!strcasecmp( "CertificateFile", cfg->cmd )) {
			server->sconf.cert_file = expand_strdup( cfg->val, cfg );
			if (access( server->sconf.cert_file, R_OK )) {
				sys_error( "%s:%d: CertificateFile '%s'",
				           cfg->file, cfg->line, server->sconf.cert_file );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "SystemCertificates", cfg->cmd )) {
			server->sconf.system_certs = parse_bool( cfg );
		} else if (!strcasecmp( "ClientCertificate", cfg->cmd )) {
			server->sconf.client_certfile = expand_strdup( cfg->val, cfg );
			if (access( server->sconf.client_certfile, R_OK )) {
				sys_error( "%s:%d: ClientCertificate '%s'",
				           cfg->file, cfg->line, server->sconf.client_certfile );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "ClientKey", cfg->cmd )) {
			server->sconf.client_keyfile = expand_strdup( cfg->val, cfg );
			if (access( server->sconf.client_keyfile, R_OK )) {
				sys_error( "%s:%d: ClientKey '%s'",
				           cfg->file, cfg->line, server->sconf.client_keyfile );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "CipherString", cfg->cmd )) {
			server->sconf.cipher_string = nfstrdup( cfg->val );
		} else if (!strcasecmp( "TLSVersions", cfg->cmd )) {
			arg = cfg->val;
			do {
				int or_mask = 0, and_mask = 0, val;
				if (*arg == '+') {
					or_mask = ~0;
				} else if (*arg == '-') {
					and_mask = ~0;
				} else {
					error( "%s:%d: TLSVersions arguments must start with +/-\n", cfg->file, cfg->line );
					cfg->err = 1;
					continue;
				}
				arg++;
				if (!strcmp( "1.0", arg )) {
					val = TLSv1;
				} else if (!strcmp( "1.1", arg )) {
					val = TLSv1_1;
				} else if (!strcmp( "1.2", arg )) {
					val = TLSv1_2;
				} else if (!strcmp( "1.3", arg )) {
					val = TLSv1_3;
				} else {
					error( "%s:%d: Unrecognized TLS version '%s'\n", cfg->file, cfg->line, arg );
					cfg->err = 1;
					continue;
				}
				or_mask &= val;
				and_mask &= val;
				server->sconf.ssl_versions = (server->sconf.ssl_versions & ~and_mask) | or_mask;
			} while ((arg = get_arg( cfg, ARG_OPTIONAL, NULL )));
		} else if (!strcasecmp( "SSLVersion", cfg->cmd ) ||
		           !strcasecmp( "SSLVersions", cfg->cmd )) {
			static int sslv_warned;
			if (!sslv_warned) {
				sslv_warned = 1;
				warn( "Notice: SSLVersions is deprecated. Use TLSVersions instead.\n" );
			}
			server->sconf.ssl_versions = 0;
			arg = cfg->val;
			do {
				if (!strcasecmp( "SSLv2", arg )) {
					warn( "Warning: SSLVersion SSLv2 is no longer supported\n" );
				} else if (!strcasecmp( "SSLv3", arg )) {
					warn( "Warning: SSLVersion SSLv3 is no longer supported\n" );
				} else if (!strcasecmp( "TLSv1", arg )) {
					server->sconf.ssl_versions |= TLSv1;
				} else if (!strcasecmp( "TLSv1.1", arg )) {
					server->sconf.ssl_versions |= TLSv1_1;
				} else if (!strcasecmp( "TLSv1.2", arg )) {
					server->sconf.ssl_versions |= TLSv1_2;
				} else if (!strcasecmp( "TLSv1.3", arg )) {
					server->sconf.ssl_versions |= TLSv1_3;
				} else {
					error( "%s:%d: Unrecognized SSL version\n", cfg->file, cfg->line );
					cfg->err = 1;
				}
			} while ((arg = get_arg( cfg, ARG_OPTIONAL, NULL )));
#else
		} else if (!strcasecmp( "CertificateFile", cfg->cmd ) ||
		           !strcasecmp( "SystemCertificates", cfg->cmd ) ||
		           !strcasecmp( "ClientCertificate", cfg->cmd ) ||
		           !strcasecmp( "ClientKey", cfg->cmd ) ||
		           !strcasecmp( "CipherString", cfg->cmd ) ||
		           !strcasecmp( "SSLVersion", cfg->cmd ) ||
		           !strcasecmp( "SSLVersions", cfg->cmd ) ||
		           !strcasecmp( "TLSVersions", cfg->cmd )) {
			error( "Error: " EXE " built without OpenSSL; %s is not supported.\n", cfg->cmd );
			cfg->err = 1;
#endif
		} else if (!strcasecmp( "TLSType", cfg->cmd )) {
			goto tlstype;
		} else if (!strcasecmp( "SSLType", cfg->cmd )) {
			static int sslt_warned;
			if (!sslt_warned) {
				sslt_warned = 1;
				warn( "Notice: SSLType is deprecated. Use TLSType instead.\n" );
			}
		  tlstype:
			if (!strcasecmp( "None", cfg->val )) {
#ifdef HAVE_LIBSSL
				server->ssl_type = SSL_None;
			} else if (!strcasecmp( "STARTTLS", cfg->val )) {
				server->ssl_type = SSL_STARTTLS;
			} else if (!strcasecmp( "IMAPS", cfg->val )) {
				server->ssl_type = SSL_IMAPS;
#else
			} else if (!strcasecmp( "STARTTLS", cfg->val ) ||
			           !strcasecmp( "IMAPS", cfg->val )) {
				error( "Error: " EXE " built without OpenSSL; only TLSType None is supported.\n" );
				cfg->err = 1;
#endif
			} else {
				error( "%s:%d: Invalid TLS type\n", cfg->file, cfg->line );
				cfg->err = 1;
			}
		} else if (!strcasecmp( "AuthMech", cfg->cmd ) ||
		         !strcasecmp( "AuthMechs", cfg->cmd )) {
			arg = cfg->val;
			do {
				add_string_list( &server->auth_mechs, arg );
			} while ((arg = get_arg( cfg, ARG_OPTIONAL, NULL )));
		} else if (!strcasecmp( "RequireCRAM", cfg->cmd )) {
			require_cram = parse_bool( cfg );
		} else if (!strcasecmp( "Tunnel", cfg->cmd )) {
			server->sconf.tunnel = nfstrdup( cfg->val );
		} else if (store) {
			if (!strcasecmp( "Account", cfg->cmd )) {
				for (srv = servers; srv; srv = srv->next)
					if (srv->name && !strcmp( srv->name, cfg->val ))
						goto gotsrv;
				store->server = (void *)~0;
				error( "%s:%d: unknown IMAP account '%s'\n", cfg->file, cfg->line, cfg->val );
				cfg->err = 1;
				continue;
			  gotsrv:
				store->server = srv;
			} else if (!strcasecmp( "UseNamespace", cfg->cmd )) {
				store->use_namespace = parse_bool( cfg );
			} else if (!strcasecmp( "SubscribedOnly", cfg->cmd )) {
				store->use_lsub = parse_bool( cfg );
			} else if (!strcasecmp( "Path", cfg->cmd )) {
				store->path = nfstrdup( cfg->val );
			} else if (!strcasecmp( "PathDelimiter", cfg->cmd )) {
				if (strlen( cfg->val ) != 1) {
					error( "%s:%d: Path delimiter must be exactly one character long\n", cfg->file, cfg->line );
					cfg->err = 1;
					continue;
				}
				store->delimiter = cfg->val[0];
			} else {
				parse_generic_store( &store->gen, cfg, "IMAPStore" );
			}
			continue;
		} else {
			error( "%s:%d: keyword '%s' is not recognized in IMAPAccount sections\n",
			       cfg->file, cfg->line, cfg->cmd );
			cfg->rest = NULL;
			cfg->err = 1;
			continue;
		}
		acc_opt = 1;
	}
	if (!store || !store->server) {
		if (!server->sconf.tunnel && !server->sconf.host) {
			error( "%s '%s' has neither Tunnel nor Host\n", type, name );
			cfg->err = 1;
			return 1;
		}
		if (server->user && server->user_cmd) {
			error( "%s '%s' has both User and UserCmd\n", type, name );
			cfg->err = 1;
			return 1;
		}
		if (server->pass && server->pass_cmd) {
			error( "%s '%s' has both Pass and PassCmd\n", type, name );
			cfg->err = 1;
			return 1;
		}
#ifdef HAVE_MACOS_KEYCHAIN
		if (server->use_keychain && (server->pass || server->pass_cmd)) {
			error( "%s '%s' has UseKeychain enabled despite specifying Pass/PassCmd\n", type, name );
			cfg->err = 1;
			return 1;
		}
#endif
#ifdef HAVE_LIBSSL
		if (server->ssl_type < 0)
			server->ssl_type = server->sconf.tunnel ? SSL_None : SSL_STARTTLS;
#endif
		if (require_cram >= 0) {
			if (server->auth_mechs) {
				error( "%s '%s': The deprecated RequireCRAM option is mutually exclusive with AuthMech.\n", type, name );
				cfg->err = 1;
				return 1;
			}
			warn( "Notice: %s '%s': RequireCRAM is deprecated. Use AuthMech instead.\n", type, name );
			if (require_cram)
				add_string_list(&server->auth_mechs, "CRAM-MD5");
		}
#ifndef HAVE_LIBSASL
		for (string_list_t *mech = server->auth_mechs; mech; mech = mech->next) {
			if (strcmp( mech->string, "*" ) && strcasecmp( mech->string, "LOGIN" )) {
				error( "Error: " EXE " built without LibSASL; only AuthMech LOGIN is supported.\n" );
				cfg->err = 1;
				break;
			}
		}
#endif
		if (!server->auth_mechs)
			add_string_list( &server->auth_mechs, "*" );
		if (!server->sconf.port) {
			server->sconf.port =
#ifdef HAVE_LIBSSL
				server->ssl_type == SSL_IMAPS ? 993 :
#endif
				143;
		}
	}
	if (store) {
		if (!store->server) {
			store->server = nfmalloc( sizeof(sserver) );
			memcpy( store->server, &sserver, sizeof(sserver) );
			store->server->name = store->name;
		} else if (acc_opt) {
			error( "%s '%s' has both Account and account-specific options\n", type, name );
			cfg->err = 1;
		}
	}
	return 1;
}

static uint
imap_get_caps( store_t *gctx ATTR_UNUSED )
{
	return DRV_CRLF | DRV_VERBOSE | DRV_ASYNC;
}

struct driver imap_driver = {
	imap_get_caps,
	imap_parse_store,
	imap_cleanup,
	imap_alloc_store,
	imap_set_callbacks,
	imap_connect_store,
	imap_free_store,
	imap_cancel_store,
	imap_list_store,
	imap_select_box,
	imap_get_box_path,
	imap_create_box,
	imap_open_box,
	imap_get_uidnext,
	imap_get_supported_flags,
	imap_confirm_box_empty,
	imap_delete_box,
	imap_finish_delete_box,
	imap_prepare_load_box,
	imap_load_box,
	imap_fetch_msg,
	imap_store_msg,
	imap_find_new_msgs,
	imap_set_msg_flags,
	imap_trash_msg,
	imap_close_box,
	imap_cancel_cmds,
	imap_commit_cmds,
	imap_get_memory_usage,
	imap_get_fail_state,
};
