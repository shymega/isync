// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#include "common.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/time.h>

int Verbosity = TERSE;
int DFlags;
int JLimit, JCount;
int UseFSync = 1;

int Pid;
char Hostname[256];
const char *Home;

static int need_nl, need_del;

void
flushn( void )
{
	if (need_nl) {
		putchar( '\n' );
		fflush( stdout );
		need_nl = 0;
	} else if (need_del) {
		static const char delstr[] =
		        "                                                            "
		        "                                                            ";
		if (need_del > (int)sizeof(delstr) - 1)
			need_del = (int)sizeof(delstr) - 1;
		// We could use ^[[K instead, but we assume a dumb terminal.
		printf( "\r%.*s\r", need_del, delstr );
		fflush( stdout );
		need_del = 0;
	}
}

static void ATTR_PRINTFLIKE(1, 0)
vprint( const char *msg, va_list va )
{
	vprintf( msg, va );
	fflush( stdout );
	need_nl = 0;
}

void
print( const char *msg, ... )
{
	va_list va;

	va_start( va, msg );
	vprint( msg, va );
	va_end( va );
}

static void ATTR_PRINTFLIKE(1, 0)
vprintn( const char *msg, va_list va )
{
	vprint( msg, va );
	need_nl = 1;
}

void
printn( const char *msg, ... )
{
	va_list va;

	va_start( va, msg );
	vprintn( msg, va );
	va_end( va );
}

void
progress( const char *msg, ... )
{
	va_list va;

	va_start( va, msg );
	need_del = vprintf( msg, va ) - 1;
	va_end( va );
	fflush( stdout );
}

static void ATTR_PRINTFLIKE(1, 0)
nvprint( const char *msg, va_list va )
{
	if (*msg == '\v')
		msg++;
	else
		flushn();
	vprint( msg, va );
}

void
info( const char *msg, ... )
{
	va_list va;

	if (Verbosity >= VERBOSE) {
		va_start( va, msg );
		nvprint( msg, va );
		va_end( va );
	}
}

void
infon( const char *msg, ... )
{
	va_list va;

	if (Verbosity >= VERBOSE) {
		va_start( va, msg );
		nvprint( msg, va );
		va_end( va );
		need_nl = 1;
	}
}

void
notice( const char *msg, ... )
{
	va_list va;

	if (Verbosity >= TERSE) {
		va_start( va, msg );
		nvprint( msg, va );
		va_end( va );
	}
}

void
warn( const char *msg, ... )
{
	va_list va;

	if (Verbosity >= QUIET) {
		flushn();
		va_start( va, msg );
		vfprintf( stderr, msg, va );
		va_end( va );
	}
}

void
error( const char *msg, ... )
{
	va_list va;

	flushn();
	va_start( va, msg );
	vfprintf( stderr, msg, va );
	va_end( va );
}

void
vsys_error( const char *msg, va_list va )
{
	char buf[1024];

	int errno_bak = errno;
	flushn();
	if ((uint)vsnprintf( buf, sizeof(buf), msg, va ) >= sizeof(buf))
		oob();
	errno = errno_bak;
	perror( buf );
}

void
sys_error( const char *msg, ... )
{
	va_list va;

	va_start( va, msg );
	vsys_error( msg, va );
	va_end( va );
}

// Minimal printf() replacement with custom format sequence(s):
// - %\\s
//   Print backslash-escaped string literals. Note that this does not
//   automatically add quotes around the printed string, so it is
//   possible to concatenate multiple segments.
// - %!s
//   Same as %\\s, but non-ASCII characters are (hex-)escaped as well.
// - %!&s
//   Same as %!s, but linefeeds are also printed verbatim for legibility.

// TODO: Trade off segments vs. buffer capacity dynamically.
#define QPRINTF_SEGS 16
#ifndef QPRINTF_BUFF
# define QPRINTF_BUFF 1000
#endif

typedef void (*printf_cb)( const char **segs, uint *segls, int nsegs, uint totlen, void *aux );

static void
xvprintf_core( const char *fmt, va_list ap, printf_cb cb, void *cb_aux )
{
	int nsegs = 0;
	uint totlen = 0;
	const char *segs[QPRINTF_SEGS];
	uint segls[QPRINTF_SEGS];
	char buf[QPRINTF_BUFF];

#define ADD_SEG(p, l) \
		do { \
			if (nsegs == QPRINTF_SEGS) \
				oob(); \
			segs[nsegs] = p; \
			segls[nsegs++] = l; \
			totlen += l; \
		} while (0)

	char *d = buf;
	char *ed = d + sizeof(buf);
	const char *s = fmt;
	for (;;) {
		char c = *fmt;
		if (!c || c == '%') {
			uint l = fmt - s;
			if (l)
				ADD_SEG( s, l );
			if (!c)
				break;
			uint maxlen = UINT_MAX;
			c = *++fmt;
			if (c == '.') {
				c = *++fmt;
				if (c != '*') {
					fputs( "Fatal: unsupported string length specification. Please report a bug.\n", stderr );
					abort();
				}
				maxlen = va_arg( ap, uint );
				c = *++fmt;
			}
			int escaped = 0;
			if (c == '\\') {
				escaped = 1;
				c = *++fmt;
			} else if (c == '!') {
				escaped = 2;
				c = *++fmt;
				if (c == '&') {
					escaped = 3;
					c = *++fmt;
				}
			}
			if (c == 'c') {
				if (d + 1 > ed)
					oob();
				ADD_SEG( d, 1 );
				*d++ = (char)va_arg( ap, int );
			} else if (c == 's') {
				s = va_arg( ap, const char * );
				if (escaped) {
					char *bd = d;
					for (l = 0; l < maxlen && (c = *s); l++, s++) {
						if (c == '\\' || c == '"') {
							if (d >= ed)
								oob();
							*d++ = '\\';
						} else if (escaped >= 2 && (c < 32 || c > 126)) {
							switch (c) {
							case '\r': c = 'r'; break;
							case '\t': c = 't'; break;
							case '\a': c = 'a'; break;
							case '\b': c = 'b'; break;
							case '\v': c = 'v'; break;
							case '\f': c = 'f'; break;
							case '\n':
								if (escaped == 2) {
									c = 'n';
									break;
								}
								if (d + 2 >= ed)
									oob();
								*d++ = '\\';
								*d++ = 'n';
								*d++ = c;  // Keep the actual line break for legibility.
								continue;
							default:
								d += nfsnprintf( d, ed - d, "\\x%02x", (uchar)c );
								continue;
							}
							if (d >= ed)
								oob();
							*d++ = '\\';
						}
						if (d >= ed)
							oob();
						*d++ = c;
					}
					l = d - bd;
					if (l)
						ADD_SEG( bd, l );
				} else {
					l = strnlen( s, maxlen );
					if (l)
						ADD_SEG( s, l );
				}
			} else if (c == 'd') {
				l = nfsnprintf( d, ed - d, "%d", va_arg( ap, int ) );
				ADD_SEG( d, l );
				d += l;
			} else if (c == 'u') {
				l = nfsnprintf( d, ed - d, "%u", va_arg( ap, uint ) );
				ADD_SEG( d, l );
				d += l;
			} else {
				fputs( "Fatal: unsupported format specifier. Please report a bug.\n", stderr );
				abort();
			}
			s = ++fmt;
		} else {
			fmt++;
		}
	}
	cb( segs, segls, nsegs, totlen, cb_aux );
}

static void
xasprintf_cb( const char **segs, uint *segls, int nsegs, uint totlen, void *aux )
{
	char *d = nfmalloc( totlen + 1 );
	*(char **)aux = d;
	for (int i = 0; i < nsegs; i++) {
		memcpy( d, segs[i], segls[i] );
		d += segls[i];
	}
	*d = 0;
}

char *
xvasprintf( const char *fmt, va_list ap )
{
	char *out;
	xvprintf_core( fmt, ap, xasprintf_cb, &out );
	return out;
}

#ifndef HAVE_FWRITE_UNLOCKED
# define flockfile(f)
# define funlockfile(f)
# define fwrite_unlocked(b, l, n, f) fwrite(b, l, n, f)
#endif

static void
xprintf_cb( const char **segs, uint *segls, int nsegs, uint totlen ATTR_UNUSED, void *aux ATTR_UNUSED )
{
	flockfile( stdout );
	for (int i = 0; i < nsegs; i++)
		fwrite_unlocked( segs[i], 1, segls[i], stdout );
	funlockfile( stdout );
}

void
xprintf( const char *fmt, ... )
{
	va_list va;

	va_start( va, fmt );
	xvprintf_core( fmt, va, xprintf_cb, NULL );
	va_end( va );
}

void
vFprintf( FILE *f, const char *msg, va_list va )
{
	int r;

	r = vfprintf( f, msg, va );
	if (r < 0) {
		sys_error( "Error: cannot write file" );
		exit( 1 );
	}
}

void
Fprintf( FILE *f, const char *msg, ... )
{
	va_list va;

	va_start( va, msg );
	vFprintf( f, msg, va );
	va_end( va );
}

void
Fclose( FILE *f, int safe )
{
	if ((safe && (fflush( f ) || (UseFSync && fdatasync( fileno( f ) )))) || fclose( f ) == EOF) {
		sys_error( "Error: cannot close file" );
		exit( 1 );
	}
}

void
add_string_list_n( string_list_t **list, const char *str, uint len )
{
	string_list_t *elem;

	elem = nfmalloc( offsetof(string_list_t, string) + len + 1 );
	elem->next = *list;
	*list = elem;
	memcpy( elem->string, str, len );
	elem->string[len] = 0;
}

void
add_string_list( string_list_t **list, const char *str )
{
	add_string_list_n( list, str, strlen( str ) );
}

void
free_string_list( string_list_t *list )
{
	string_list_t *tlist;

	for (; list; list = tlist) {
		tlist = list->next;
		free( list );
	}
}

#ifndef HAVE_VASPRINTF
static int
vasprintf( char **strp, const char *fmt, va_list ap )
{
	int len;
	char tmp[1024];

	if ((len = vsnprintf( tmp, sizeof(tmp), fmt, ap )) < 0 || !(*strp = malloc( len + 1 )))
		return -1;
	if (len >= (int)sizeof(tmp))
		vsprintf( *strp, fmt, ap );
	else
		memcpy( *strp, tmp, (size_t)len + 1 );
	return len;
}
#endif

#ifndef HAVE_MEMRCHR
void *
memrchr( const void *s, int c, size_t n )
{
	u_char *b = (u_char *)s, *e = b + n;

	while (--e >= b)
		if (*e == c)
			return (void *)e;
	return 0;
}
#endif

#ifndef HAVE_STRNLEN
size_t
strnlen( const char *str, size_t maxlen )
{
	const char *estr = memchr( str, 0, maxlen );
	return estr ? (size_t)(estr - str) : maxlen;
}

#endif

void
to_upper( char *str, uint len )
{
	for (uint i = 0; i < len; i++)
		str[i] = toupper( str[i] );
}

int
starts_with( const char *str, int strl, const char *cmp, uint cmpl )
{
	if (strl < 0)
		strl = strnlen( str, cmpl + 1 );
	return ((uint)strl >= cmpl) && !memcmp( str, cmp, cmpl );
}

static int
equals_upper_impl( const char *str, const char *cmp, uint cmpl )
{
	for (uint i = 0; i < cmpl; i++)
		if (toupper( str[i] ) != cmp[i])
			return 0;
	return 1;
}

int
starts_with_upper( const char *str, int strl, const char *cmp, uint cmpl )
{
	if (strl < 0)
		strl = strnlen( str, cmpl + 1 );
	if ((uint)strl < cmpl)
		return 0;
	return equals_upper_impl( str, cmp, cmpl );
}

int
equals( const char *str, int strl, const char *cmp, uint cmpl )
{
	if (strl < 0)
		strl = strnlen( str, cmpl + 1 );
	return ((uint)strl == cmpl) && !memcmp( str, cmp, cmpl );
}

int
equals_upper( const char *str, int strl, const char *cmp, uint cmpl )
{
	if (strl < 0)
		strl = strnlen( str, cmpl + 1 );
	if ((uint)strl != cmpl)
		return 0;
	return equals_upper_impl( str, cmp, cmpl );
}

#ifndef HAVE_TIMEGM
/*
   Converts struct tm to time_t, assuming the data in tm is UTC rather
   than local timezone.

   mktime is similar but assumes struct tm, also known as the
   "broken-down" form of time, is in local time zone.  timegm
   uses mktime to make the conversion understanding that an offset
   will be introduced by the local time assumption.

   mktime_from_utc then measures the introduced offset by applying
   gmtime to the initial result and applying mktime to the resulting
   "broken-down" form.  The difference between the two mktime results
   is the measured offset which is then subtracted from the initial
   mktime result to yield a calendar time which is the value returned.

   tm_isdst in struct tm is set to 0 to force mktime to introduce a
   consistent offset (the non DST offset) since tm and tm+o might be
   on opposite sides of a DST change.

   Some implementations of mktime return -1 for the nonexistent
   localtime hour at the beginning of DST.  In this event, use
   mktime(tm - 1hr) + 3600.

   Schematically
     mktime(tm)   --> t+o
     gmtime(t+o)  --> tm+o
     mktime(tm+o) --> t+2o
     t+o - (t+2o - t+o) = t

   Contributed by Roger Beeman <beeman@cisco.com>, with the help of
   Mark Baushke <mdb@cisco.com> and the rest of the Gurus at CISCO.
   Further improved by Roger with assistance from Edward J. Sabol
   based on input by Jamie Zawinski.
*/

static time_t
my_mktime( struct tm *t )
{
	time_t tl = mktime( t );
	if (tl == -1) {
		t->tm_hour--;
		tl = mktime( t );
		if (tl != -1)
			tl += 3600;
	}
	return tl;
}

time_t
timegm( struct tm *t )
{
	time_t tl, tb;
	struct tm *tg;

	if ((tl = my_mktime( t )) == -1)
		return tl;
	tg = gmtime( &tl );
	tg->tm_isdst = 0;
	if ((tb = my_mktime( tg )) == -1)
		return tb;
	return tl - (tb - tl);
}
#endif

void
fmt_bits( uint bits, uint num_bits, const char *bit_str, const int *bit_off, char *buf )
{
	uint d = 0;
	for (uint i = 0, val = 1; i < num_bits; i++, val <<= 1) {
		if (bits & val) {
			if (d)
				buf[d++] = ',';
			for (const char *s = bit_str + bit_off[i]; *s; s++)
				buf[d++] = *s;
		}
	}
	buf[d] = 0;
}

void
oob( void )
{
	fputs( "Fatal: buffer too small. Please report a bug.\n", stderr );
	abort();
}

int
nfsnprintf( char *buf, int blen, const char *fmt, ... )
{
	int ret;
	va_list va;

	va_start( va, fmt );
	if (blen <= 0 || (uint)(ret = vsnprintf( buf, (size_t)blen, fmt, va )) >= (uint)blen)
		oob();
	va_end( va );
	return ret;
}

void
oom( void )
{
	fputs( "Fatal: Out of memory\n", stderr );
	abort();
}

void *
nfmalloc( size_t sz )
{
	void *ret;

	if (!(ret = malloc( sz )))
		oom();
	return ret;
}

void *
nfzalloc( size_t sz )
{
	void *ret;

	if (!(ret = calloc( sz, 1 )))
		oom();
	return ret;
}

void *
nfrealloc( void *mem, size_t sz )
{
	char *ret;

	if (!(ret = realloc( mem, sz )) && sz)
		oom();
	return ret;
}

char *
nfstrndup( const char *str, size_t nchars )
{
	char *ret = nfmalloc( nchars + 1 );
	memcpy( ret, str, nchars );
	ret[nchars] = 0;
	return ret;
}

char *
nfstrdup( const char *str )
{
	return nfstrndup( str, strlen( str ) );
}

int
nfvasprintf( char **str, const char *fmt, va_list va )
{
	int ret = vasprintf( str, fmt, va );
	if (ret < 0)
		oom();
	return ret;
}

int
nfasprintf( char **str, const char *fmt, ... )
{
	int ret;
	va_list va;

	va_start( va, fmt );
	ret = nfvasprintf( str, fmt, va );
	va_end( va );
	return ret;
}

/*
static struct passwd *
cur_user( void )
{
	char *p;
	struct passwd *pw;
	uid_t uid;

	uid = getuid();
	if ((!(p = getenv("LOGNAME")) || !(pw = getpwnam( p )) || pw->pw_uid != uid) &&
	    (!(p = getenv("USER")) || !(pw = getpwnam( p )) || pw->pw_uid != uid) &&
	    !(pw = getpwuid( uid )))
	{
		fputs ("Cannot determinate current user\n", stderr);
		return 0;
	}
	return pw;
}
*/

/* Return value: 0 = ok, -1 = out found in arg, -2 = in found in arg but no out specified */
int
map_name( const char *arg, int l, char **result, uint reserve, const char *in, const char *out )
{
	char *p;
	int i, ll, num, inl, outl;

	assert( arg );
	if (l < 0)
		l = strlen( arg );
	assert( in );
	inl = strlen( in );
	if (!inl) {
	  copy:
		*result = nfmalloc( reserve + l + 1 );
		memcpy( *result + reserve, arg, l );
		(*result)[reserve + l] = 0;
		return 0;
	}
	assert( out );
	outl = strlen( out );
	if (equals( in, (int)inl, out, outl ))
		goto copy;
	for (num = 0, i = 0; i < l; ) {
		if (i + inl > l)
			goto fout;
		for (ll = 0; ll < inl; ll++)
			if (arg[i + ll] != in[ll])
				goto fout;
		num++;
		i += inl;
		continue;
	  fout:
		if (outl) {
			if (i + outl > l)
				goto fnexti;
			for (ll = 0; ll < outl; ll++)
				if (arg[i + ll] != out[ll])
					goto fnexti;
			return -1;
		}
	  fnexti:
		i++;
	}
	if (!num)
		goto copy;
	if (!outl)
		return -2;
	*result = nfmalloc( reserve + l + num * (outl - inl) + 1 );
	p = *result + reserve;
	for (i = 0; i < l; ) {
		if (i + inl > l)
			goto rnexti;
		for (ll = 0; ll < inl; ll++)
			if (arg[i + ll] != in[ll])
				goto rnexti;
		memcpy( p, out, outl );
		p += outl;
		i += inl;
		continue;
	  rnexti:
		*p++ = arg[i++];
	}
	*p = 0;
	return 0;
}

int
mkdir_p( char *path, int len )
{
	if (!mkdir( path, 0700 ) || errno == EEXIST)
		return 0;
	char *p = memrchr( path, '/', (size_t)len );
	*p = 0;
	if (mkdir_p( path, (int)(p - path) )) {
		*p = '/';
		return -1;
	}
	*p = '/';
	return mkdir( path, 0700 );
}

static int
compare_uints( const void *l, const void *r )
{
	uint li = *(const uint *)l, ri = *(const uint *)r;
	if (li != ri)  // Can't subtract, the result might not fit into signed int.
		return li > ri ? 1 : -1;
	return 0;
}

void
sort_uint_array( uint_array_t array )
{
	qsort( array.data, array.size, sizeof(uint), compare_uints );
}

int
find_uint_array( uint_array_t array, uint value )
{
	uint bot = 0, top = array.size;
	while (bot < top) {
		uint i = (bot + top) / 2;
		uint elt = array.data[i];
		if (elt == value)
			return 1;
		if (elt < value)
			bot = i + 1;
		else
			top = i;
	}
	return 0;
}


static struct {
	uchar i, j, s[256];
} rs;

void
arc4_init( void )
{
	int i, fd;
	uchar j, si, dat[128];

	if ((fd = open( "/dev/urandom", O_RDONLY )) < 0 && (fd = open( "/dev/random", O_RDONLY )) < 0) {
		error( "Fatal: no random number source available.\n" );
		exit( 3 );
	}
	if (read( fd, dat, 128 ) != 128) {
		error( "Fatal: cannot read random number source.\n" );
		exit( 3 );
	}
	close( fd );

	for (i = 0; i < 256; i++)
		rs.s[i] = (uchar)i;
	for (i = j = 0; i < 256; i++) {
		si = rs.s[i];
		j += si + dat[i & 127];
		rs.s[i] = rs.s[j];
		rs.s[j] = si;
	}
	rs.i = rs.j = 0;

	for (i = 0; i < 256; i++)
		arc4_getbyte();
}

uchar
arc4_getbyte( void )
{
	uchar si, sj;

	rs.i++;
	si = rs.s[rs.i];
	rs.j += si;
	sj = rs.s[rs.j];
	rs.s[rs.i] = sj;
	rs.s[rs.j] = si;
	return rs.s[(si + sj) & 0xff];
}

static const uchar prime_deltas[] = {
    0,  0,  1,  3,  1,  5,  3,  3,  1,  9,  7,  5,  3, 17, 27,  3,
    1, 29,  3, 21,  7, 17, 15,  9, 43, 35, 15,  0,  0,  0,  0,  0
};

uint
bucketsForSize( uint size )
{
	uint base = 4, bits = 2;

	for (;;) {
		uint prime = base + prime_deltas[bits];
		if (prime >= size)
			return prime;
		base <<= 1;
		bits++;
	}
}

static void
list_prepend( list_head_t *head, list_head_t *to )
{
	assert( !head->next );
	assert( to->next );
	assert( to->prev->next == to );
	head->next = to;
	head->prev = to->prev;
	head->prev->next = head;
	to->prev = head;
}

static void
list_unlink( list_head_t *head )
{
	assert( head->next );
	assert( head->next->prev == head);
	assert( head->prev->next == head);
	head->next->prev = head->prev;
	head->prev->next = head->next;
	head->next = head->prev = NULL;
}

static notifier_t *notifiers;
static int changed;  /* Iterator may be invalid now. */
#ifdef HAVE_POLL_H
static struct pollfd *pollfds;
static uint npolls, rpolls;
#else
# ifdef HAVE_SYS_SELECT_H
#  include <sys/select.h>
# endif
#endif

void
init_notifier( notifier_t *sn, int fd, void (*cb)( int, void * ), void *aux )
{
#ifdef HAVE_POLL_H
	uint idx = npolls++;
	if (rpolls < npolls) {
		rpolls = npolls;
		pollfds = nfrealloc( pollfds, npolls * sizeof(*pollfds) );
	}
	pollfds[idx].fd = fd;
	pollfds[idx].events = 0; /* POLLERR & POLLHUP implicit */
	sn->index = idx;
#else
	sn->fd = fd;
	sn->events = 0;
#endif
	sn->cb = cb;
	sn->aux = aux;
	sn->next = notifiers;
	notifiers = sn;
}

void
conf_notifier( notifier_t *sn, short and_events, short or_events )
{
#ifdef HAVE_POLL_H
	uint idx = sn->index;
	pollfds[idx].events = (pollfds[idx].events & and_events) | or_events;
#else
	sn->events = (sn->events & and_events) | or_events;
#endif
}

short
notifier_config( notifier_t *sn )
{
#ifdef HAVE_POLL_H
	return pollfds[sn->index].events;
#else
	return sn->events;
#endif
}

void
wipe_notifier( notifier_t *sn )
{
	notifier_t **snp;
#ifdef HAVE_POLL_H
	uint idx;
#endif

	for (snp = &notifiers; *snp != sn; snp = &(*snp)->next)
		assert( *snp );
	*snp = sn->next;
	sn->next = NULL;
	changed = 1;

#ifdef HAVE_POLL_H
	idx = sn->index;
	memmove( pollfds + idx, pollfds + idx + 1, (--npolls - idx) * sizeof(*pollfds) );
	for (sn = notifiers; sn; sn = sn->next) {
		if (sn->index > idx)
			sn->index--;
	}
#endif
}

#if _POSIX_TIMERS - 0 > 0
static clockid_t clkid;
#endif

void
init_timers( void )
{
#if _POSIX_TIMERS - 0 > 0
	struct timespec ts;
# ifdef CLOCK_BOOTTIME
	if (!clock_gettime( CLOCK_BOOTTIME, &ts )) {
		clkid = CLOCK_BOOTTIME;
	} else
# endif
# ifdef CLOCK_MONOTONIC_COARSE
	if (!clock_gettime( CLOCK_MONOTONIC_COARSE, &ts )) {
		clkid = CLOCK_MONOTONIC_COARSE;
	} else
# endif
	clkid = CLOCK_MONOTONIC;
#endif
}

int64_t
get_now( void )
{
#if _POSIX_TIMERS - 0 > 0
	struct timespec ts;
	clock_gettime( clkid, &ts );
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
#else
	struct timeval tv;
	gettimeofday( &tv, NULL );
	return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
#endif
}

static list_head_t timers = { &timers, &timers };

void
init_wakeup( wakeup_t *tmr, void (*cb)( void * ), void *aux )
{
	tmr->cb = cb;
	tmr->aux = aux;
	tmr->links.next = tmr->links.prev = NULL;
}

void
wipe_wakeup( wakeup_t *tmr )
{
	if (tmr->links.next)
		list_unlink( &tmr->links );
}

void
conf_wakeup( wakeup_t *tmr, int to )
{
	list_head_t *head, *succ;

	if (to < 0) {
		if (tmr->links.next)
			list_unlink( &tmr->links );
	} else {
		int64_t timeout = to;
		if (!to) {
			/* We always prepend null timers, to cluster related events. */
			succ = timers.next;
		} else {
			timeout += get_now();
			/* We start at the end in the expectation that the newest timer is likely to fire last
			 * (which will be true only if all timeouts are equal, but it's an as good guess as any). */
			for (succ = &timers; (head = succ->prev) != &timers; succ = head) {
				if (head != &tmr->links && timeout > ((wakeup_t *)head)->timeout)
					break;
			}
			assert( head != &tmr->links );
		}
		tmr->timeout = timeout;
		if (succ != &tmr->links) {
			if (tmr->links.next)
				list_unlink( &tmr->links );
			list_prepend( &tmr->links, succ );
		}
	}
}

static void
event_wait( void )
{
	list_head_t *head;
	notifier_t *sn;
	int m;

#ifdef HAVE_POLL_H
	int timeout = -1;
	if ((head = timers.next) != &timers) {
		wakeup_t *tmr = (wakeup_t *)head;
		int64_t delta = tmr->timeout;
		if (!delta || (delta -= get_now()) <= 0) {
			list_unlink( head );
			tmr->cb( tmr->aux );
			return;
		}
		timeout = (int)delta;
	}
	switch (poll( pollfds, npolls, timeout )) {
	case 0:
		return;
	case -1:
		perror( "poll() failed in event loop" );
		abort();
	default:
		break;
	}
	for (sn = notifiers; sn; sn = sn->next) {
		uint n = sn->index;
		if ((m = pollfds[n].revents)) {
			assert( !(m & POLLNVAL) );
			sn->cb( m | shifted_bit( m, POLLHUP, POLLIN ), sn->aux );
			if (changed) {
				changed = 0;
				break;
			}
		}
	}
#else
	struct timeval *timeout = 0;
	struct timeval to_tv;
	fd_set rfds, wfds, efds;
	int fd;

	if ((head = timers.next) != &timers) {
		wakeup_t *tmr = (wakeup_t *)head;
		int64_t delta = tmr->timeout;
		if (!delta || (delta -= get_now()) <= 0) {
			list_unlink( head );
			tmr->cb( tmr->aux );
			return;
		}
		to_tv.tv_sec = delta / 1000;
		to_tv.tv_usec = delta * 1000;
		timeout = &to_tv;
	}
	FD_ZERO( &rfds );
	FD_ZERO( &wfds );
	FD_ZERO( &efds );
	m = -1;
	for (sn = notifiers; sn; sn = sn->next) {
		fd = sn->fd;
		if (sn->events & POLLIN)
			FD_SET( fd, &rfds );
		if (sn->events & POLLOUT)
			FD_SET( fd, &wfds );
		FD_SET( fd, &efds );
		if (fd > m)
			m = fd;
	}
	switch (select( m + 1, &rfds, &wfds, &efds, timeout )) {
	case 0:
		return;
	case -1:
		perror( "select() failed in event loop" );
		abort();
	default:
		break;
	}
	for (sn = notifiers; sn; sn = sn->next) {
		fd = sn->fd;
		m = 0;
		if (FD_ISSET( fd, &rfds ))
			m |= POLLIN;
		if (FD_ISSET( fd, &wfds ))
			m |= POLLOUT;
		if (FD_ISSET( fd, &efds ))
			m |= POLLERR;
		if (m) {
			sn->cb( m, sn->aux );
			if (changed) {
				changed = 0;
				break;
			}
		}
	}
#endif
}

void
main_loop( void )
{
	while (notifiers || timers.next != &timers)
		event_wait();
}
