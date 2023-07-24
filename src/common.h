// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#ifndef COMMON_H
#define COMMON_H

#include <autodefs.h>

#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;

#define as(ar) (sizeof(ar)/sizeof(ar[0]))

#define stringify__(x) #x
#define stringify(x) stringify__(x)

// From https://stackoverflow.com/a/62984543/3685191
#define deparen(x) esc_(ish_ x)
#define esc_(...) esc__(__VA_ARGS__)
#define esc__(...) van_ ## __VA_ARGS__
#define ish_(...) ish_ __VA_ARGS__
#define van_ish_

#define shifted_bit(in, from, to) \
	((int)(((uint)(in) / (from > to ? from / to : 1) * (to > from ? to / from : 1)) & to))

#define BIT_ENUM_VAL(name) \
	name, \
	name##_dummy = 2 * name - 1,

#define DEFINE_BIT_ENUM(list) \
	enum list { \
		list##_dummy, \
		list(BIT_ENUM_VAL) \
	};

#define PFX_BIT_ENUM_VAL(pfx, name) \
	pfx##_##name, \
	pfx##_##name##_dummy = 2 * pfx##_##name - 1,

#define PFX_BIT_ENUM_NUM(pfx, name) \
	pfx##_##name##_bit,

#define DEFINE_PFX_BIT_ENUM(list) \
	enum list { \
		list##_dummy, \
		list(PFX_BIT_ENUM_VAL) \
	}; \
	enum { \
		list(PFX_BIT_ENUM_NUM) \
		list##_num_bits \
	};

#define BIT_ENUM_STR(pfx, name) \
	stringify(name) "\0"

#define BIT_ENUM_STR_OFF(pfx, name) \
	name##_str_off, \
	name##_str_off_dummy = name##_str_off + sizeof(stringify(name)) - 1,

#define GET_BIT_ENUM_STR_OFF(pfx, name) \
	name##_str_off,

#define PFX_BIT_ENUM_STR(pfx, name) \
	stringify(pfx) "_" stringify(name) "\0"

#define PFX_BIT_ENUM_STR_OFF(pfx, name) \
	pfx##_##name##_str_off, \
	pfx##_##name##_str_end = pfx##_##name##_str_off + sizeof(stringify(pfx) "_" stringify(name)) - 1,

#define GET_PFX_BIT_ENUM_STR_OFF(pfx, name) \
	pfx##_##name##_str_off,

#define static_assert_bits(list, type, field) \
	static_assert( list##_num_bits <= sizeof(((type){ 0 }).field) * 8, \
	               stringify(type) "::" stringify(field) " is too small" )

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
# define ATTR_UNUSED __attribute__((unused))
# define ATTR_NORETURN __attribute__((noreturn))
# define ATTR_PRINTFLIKE(fmt,var) __attribute__((format(printf,fmt,var)))
# define ATTR_OPTIMIZE __attribute__((optimize("2")))
#else
# define ATTR_UNUSED
# define ATTR_NORETURN
# define ATTR_PRINTFLIKE(fmt,var)
# define ATTR_OPTIMIZE
#endif

#if defined(__clang__)
# define DO_PRAGMA__(text) _Pragma(#text)
# define DIAG_PUSH DO_PRAGMA__(clang diagnostic push)
# define DIAG_POP DO_PRAGMA__(clang diagnostic pop)
# define DIAG_DISABLE(text) DO_PRAGMA__(clang diagnostic ignored text)
#elif __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 5)
# define DO_PRAGMA__(text) _Pragma(#text)
# define DIAG_PUSH DO_PRAGMA__(GCC diagnostic push)
# define DIAG_POP DO_PRAGMA__(GCC diagnostic pop)
# define DIAG_DISABLE(text) DO_PRAGMA__(GCC diagnostic ignored text)
#else
# define DIAG_PUSH
# define DIAG_POP
# define DIAG_DISABLE(text)
#endif

#if __GNUC__ >= 7 || defined(__clang__)
# define FALLTHROUGH __attribute__((fallthrough));
#else
# define FALLTHROUGH
#endif

#ifdef __GNUC__
# define INLINE __inline__
#else
# define INLINE
#endif

#define EXE "mbsync"

/* main.c */

enum {
	VERYQUIET,
	QUIET,
	TERSE,
	VERBOSE,
};

#define options_enum(fn) \
	fn(DEBUG_MAILDIR) \
	fn(DEBUG_NET) \
	fn(DEBUG_NET_ALL) \
	fn(DEBUG_SYNC) \
	fn(DEBUG_MAIN) \
	fn(DEBUG_DRV) \
	fn(DEBUG_DRV_ALL) \
	\
	fn(DEBUG_CRASH) \
	\
	fn(PROGRESS) \
	\
	fn(DRYRUN) \
	\
	fn(EXT_EXIT) \
	\
	fn(ZERODELAY) \
	fn(KEEPJOURNAL) \
	fn(FORCEJOURNAL) \
	fn(FORCEASYNC_F) \
	fn(FORCEASYNC_N) \
	fn(FAKEEXPUNGE) \
	fn(FAKEDUMBSTORE)
DEFINE_BIT_ENUM(options_enum)
#define FORCEASYNC(b) (FORCEASYNC_F << (b))

#define DEBUG_ANY (DEBUG_MAILDIR | DEBUG_NET | DEBUG_SYNC | DEBUG_MAIN | DEBUG_DRV)
#define DEBUG_ALL (DEBUG_ANY | DEBUG_CRASH)

// Global options
extern int Verbosity;
extern int DFlags;
extern int JLimit, JCount;
extern int UseFSync;

// Global constants (inited by main())
extern int Pid;
extern char Hostname[256];
extern const char *Home;

void countStep( void );
void stats( void );

/* util.c */

#ifdef DEBUG_FLAG
#  define debug(...) \
	do { \
		if (DFlags & DEBUG_FLAG) \
			print( __VA_ARGS__ ); \
	} while (0)
#  define debugn(...) \
	do { \
		if (DFlags & DEBUG_FLAG) \
			printn( __VA_ARGS__ ); \
	} while (0)
#endif

void ATTR_PRINTFLIKE(1, 2) print( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) printn( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) info( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) infon( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) progress( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) notice( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) warn( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) error( const char *, ... );
void ATTR_PRINTFLIKE(1, 0) vsys_error( const char *, va_list va );
void ATTR_PRINTFLIKE(1, 2) sys_error( const char *, ... );
void flushn( void );

char *xvasprintf( const char *fmt, va_list ap );
void xprintf( const char *fmt, ... );

#if !defined(_POSIX_SYNCHRONIZED_IO) || _POSIX_SYNCHRONIZED_IO <= 0
# define fdatasync fsync
#endif

void ATTR_PRINTFLIKE(2, 0) vFprintf( FILE *f, const char *msg, va_list va );
void ATTR_PRINTFLIKE(2, 3) Fprintf( FILE *f, const char *msg, ... );
void Fclose( FILE *f, int safe );

typedef struct string_list {
	struct string_list *next;
	char string[1];
} string_list_t;

void add_string_list_n( string_list_t **list, const char *str, uint len );
void add_string_list( string_list_t **list, const char *str );
void free_string_list( string_list_t *list );

#ifndef HAVE_MEMRCHR
void *memrchr( const void *s, int c, size_t n );
#endif
#ifndef HAVE_STRNLEN
size_t strnlen( const char *str, size_t maxlen );
#endif

void to_upper( char *str, uint len );

int starts_with( const char *str, int strl, const char *cmp, uint cmpl );
int starts_with_upper( const char *str, int strl, const char *cmp, uint cmpl );
int equals( const char *str, int strl, const char *cmp, uint cmpl );
int equals_upper( const char *str, int strl, const char *cmp, uint cmpl );

#ifndef HAVE_TIMEGM
time_t timegm( struct tm *tm );
#endif

void fmt_bits( uint bits, uint num_bits, const char *bit_str, const int *bit_off, char *buf );

#define BIT_FORMATTER_RET(name, list, fn) \
	enum { list(fn##_OFF) name##_str_len }; \
	struct name##_str { char str[name##_str_len]; };

#define BIT_FORMATTER_PROTO(name, storage) \
	storage struct name##_str ATTR_OPTIMIZE  /* force RVO */ \
	fmt_##name( uint bits )

#define BIT_FORMATTER_IMPL(name, list, fn, storage) \
	BIT_FORMATTER_PROTO(name, storage) \
	{ \
		static const char strings[] = list(fn); \
		static const int offsets[] = { list(GET_##fn##_OFF) }; \
		\
		struct name##_str buf; \
		fmt_bits( bits, as(offsets), strings, offsets, buf.str ); \
		return buf; \
	}

// Note that this one uses enum labels without prefix ...
#define BIT_FORMATTER_FUNCTION(name, list) \
	BIT_FORMATTER_RET(name, list, BIT_ENUM_STR) \
	BIT_FORMATTER_IMPL(name, list, BIT_ENUM_STR, static)

// ... while these ones use enum labels with prefix - this
// is not fundamental, but simply because of the use cases.
#define DECL_BIT_FORMATTER_FUNCTION(name, list) \
	BIT_FORMATTER_RET(name, list, PFX_BIT_ENUM_STR) \
	BIT_FORMATTER_PROTO(name, );

#define DEF_BIT_FORMATTER_FUNCTION(name, list) \
	BIT_FORMATTER_IMPL(name, list, PFX_BIT_ENUM_STR, )

void *nfmalloc( size_t sz );
void *nfzalloc( size_t sz );
void *nfrealloc( void *mem, size_t sz );
char *nfstrndup( const char *str, size_t nchars );
char *nfstrdup( const char *str );
int ATTR_PRINTFLIKE(2, 0) nfvasprintf( char **str, const char *fmt, va_list va );
int ATTR_PRINTFLIKE(2, 3) nfasprintf( char **str, const char *fmt, ... );
int ATTR_PRINTFLIKE(3, 4) nfsnprintf( char *buf, int blen, const char *fmt, ... );
void ATTR_NORETURN oob( void );
void ATTR_NORETURN oom( void );

int map_name( const char *arg, int argl, char **result, uint reserve, const char *in, const char *out );

int mkdir_p( char *path, int len );

#define DEFINE_ARRAY_TYPE(T) \
	typedef struct { \
		T *data; \
		uint size; \
	} T##_array_t; \
	typedef union { \
		T##_array_t array; \
		struct { \
			T *data; \
			uint size; \
			uint alloc; \
		}; \
	} T##_array_alloc_t; \
	static INLINE T *T##_array_append( T##_array_alloc_t *arr ) \
	{ \
		if (arr->size == arr->alloc) { \
			arr->alloc = arr->alloc * 2 + 100; \
			arr->data = nfrealloc( arr->data, arr->alloc * sizeof(T) ); \
		} \
		return &arr->data[arr->size++]; \
	}

#define ARRAY_INIT(arr) \
	do { (arr)->data = NULL; (arr)->size = (arr)->alloc = 0; } while (0)

#define ARRAY_SQUEEZE(arr) \
	do { \
		(arr)->data = nfrealloc( (arr)->data, (arr)->size * sizeof((arr)->data[0]) ); \
	} while (0)

DEFINE_ARRAY_TYPE(uint)
void sort_uint_array( uint_array_t array );
int find_uint_array( const uint_array_t array, uint value );

void arc4_init( void );
uchar arc4_getbyte( void );

uint bucketsForSize( uint size );

typedef struct list_head {
	struct list_head *next, *prev;
} list_head_t;

typedef struct notifier {
	struct notifier *next;
	void (*cb)( int what, void *aux );
	void *aux;
#ifdef HAVE_POLL_H
	uint index;
#else
	int fd;
	short events;
#endif
} notifier_t;

#ifdef HAVE_POLL_H
# include <poll.h>
#else
# define POLLIN 1
# define POLLOUT 4
# define POLLERR 8
#endif

void init_notifier( notifier_t *sn, int fd, void (*cb)( int, void * ), void *aux );
void conf_notifier( notifier_t *sn, short and_events, short or_events );
short notifier_config( notifier_t *sn );
void wipe_notifier( notifier_t *sn );

typedef struct {
	list_head_t links;
	void (*cb)( void *aux );
	void *aux;
	int64_t timeout;
} wakeup_t;

void init_timers( void );
int64_t get_now( void );
void init_wakeup( wakeup_t *tmr, void (*cb)( void * ), void *aux );
void conf_wakeup( wakeup_t *tmr, int timeout );
void wipe_wakeup( wakeup_t *tmr );
static INLINE int ATTR_UNUSED pending_wakeup( wakeup_t *tmr ) { return tmr->links.next != NULL; }

void main_loop( void );

#endif
