// SPDX-FileCopyrightText: 2018-2021 Georgy Kibardin <georgy@kibardin.name>
// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#include "imap_p.h"

#ifdef DEBUG_IMAP_UTF7
# define dbg(...) print(__VA_ARGS__)
#else
# define dbg(...) do { } while (0)
#endif

struct bit_fifo {
	unsigned long long value;
	uint bits;
};

static void
add_bits( struct bit_fifo *fifo, uint bits, uint size )
{
	fifo->value = (fifo->value << size) | bits;
	fifo->bits += size;
	assert( fifo->bits <= sizeof(fifo->value) * 8 );
}

static uint
eat_bits( struct bit_fifo *fifo, uint size )
{
	fifo->bits -= size;
	return (fifo->value >> fifo->bits) & ((1LL << size) - 1);
}

static uint
peek_bits( struct bit_fifo *fifo, uint size )
{
	return (fifo->value >> (fifo->bits - size)) & ((1LL << size) - 1);
}

static void
add_char( char **p, uint chr )
{
	*((*p)++) = (char)chr;
}

static uchar
eat_char( const char **p )
{
	return (uchar)*((*p)++);
}

static uint
read_as_utf8( const char **utf8_buf_p )
{
	uchar chr = eat_char( utf8_buf_p );
	if (chr < 0x80)
		return chr;
	if ((chr & 0xf8) == 0xf0) {
		uchar chr2 = eat_char( utf8_buf_p );
		if ((chr2 & 0xc0) != 0x80)
			return ~0;
		uchar chr3 = eat_char( utf8_buf_p );
		if ((chr3 & 0xc0) != 0x80)
			return ~0;
		uchar chr4 = eat_char( utf8_buf_p );
		if ((chr4 & 0xc0) != 0x80)
			return ~0;
		return ((chr & 0x7) << 18) |
		       ((chr2 & 0x3f) << 12) |
		       ((chr3 & 0x3f) << 6) |
		       (chr4 & 0x3f);
	}
	if ((chr & 0xf0) == 0xe0) {
		uchar chr2 = eat_char( utf8_buf_p );
		if ((chr2 & 0xc0) != 0x80)
			return ~0;
		uchar chr3 = eat_char( utf8_buf_p );
		if ((chr3 & 0xc0) != 0x80)
			return ~0;
		return ((chr & 0xf) << 12) |
		       ((chr2 & 0x3f) << 6) |
		       (chr3 & 0x3f);
	}
	if ((chr & 0xe0) == 0xc0) {
		uchar chr2 = eat_char( utf8_buf_p );
		if ((chr2 & 0xc0) != 0x80)
			return ~0;
		return (chr & 0x1f) << 6 |
		       (chr2 & 0x3f);
	}
	return ~0;
}

static int
needs_encoding( uint chr )
{
	return chr && (chr <= 0x1f || chr >= 0x7f);
}

static uint
utf16_encode( uint chr )
{
	chr -= 0x10000;
	return (((chr >> 10) + 0xd800) << 16) | ((chr & 0x3ff) + 0xdc00);
}

static uchar
b64_encode( uint chr )
{
	assert( chr <= 0x3f );
	return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,"[chr];
}

char *
imap_utf8_to_utf7( const char *buf )
{
	// Size requirements:
	// - pass-through: l, 1 => 1
	// - all "&": l * 2, 1 => 2
	// - 7-bit: (l * 2 * 4 + 2) / 3 + 2, ~ l * 2.7, 1 => 5
	// - 3-octet: (l / 3 * 2 * 4 + 2) / 3 + 2, ~ l * 0.9, 3 => 5
	// - 4-octet: (l / 4 * 2 * 2 * 4 + 2) / 3 + 2, ~ l * 1.3, 4 => 8
	// => worst case: "&" and 7-bit alternating: l * 3.5, 2 => 7
	int outsz = strlen( buf ) * 7 / 2 + 3;
	char *result = nfmalloc( outsz );
	char *outp = result;
	struct bit_fifo fifo = { 0, 0 };
	int encoding = 0;
	uint chr;
	do {
		chr = read_as_utf8( &buf );
		if (chr == ~0U) {
			dbg( "Error: invalid UTF-8 string\n" );
			free( result );
			return NULL;
		}
		if (needs_encoding( chr )) {
			if (!encoding) {
				add_char( &outp, '&' );
				encoding = 1;
			}
			if (chr <= 0xffff)
				add_bits( &fifo, chr, 16 );
			else
				add_bits( &fifo, utf16_encode( chr ), 32 );
			while (fifo.bits >= 6)
				add_char( &outp, b64_encode( eat_bits( &fifo, 6 ) ) );
		} else {
			if (encoding) {
				if (fifo.bits) {
					uint trailing_bits = 6 - fifo.bits;
					uchar trail = b64_encode( eat_bits( &fifo, fifo.bits ) << trailing_bits );
					add_char( &outp, trail );
				}
				add_char( &outp, '-' );
				encoding = 0;
			}
			add_char( &outp, chr );
			if (chr == '&')
				add_char( &outp, '-' );
		}
	} while (chr);
	assert( (int)(outp - result) <= outsz );
	return result;
}

static void
write_as_utf8( char **outp, uint chr )
{
	if (chr <= 0x7f) {
		add_char( outp, chr );
	} else if (chr <= 0x7ff) {
		add_char( outp, (chr >> 6) | 0xc0 );
		add_char( outp, (chr & 0x3f) | 0x80 );
	} else if (chr <= 0xffff) {
		add_char( outp, (chr >> 12) | 0xe0 );
		add_char( outp, ((chr >> 6) & 0x3f) | 0x80 );
		add_char( outp, (chr & 0x3f) | 0x80 );
	} else {
		assert( chr <= 0xfffff );
		add_char( outp, (chr >> 18) | 0xf0 );
		add_char( outp, ((chr >> 12) & 0x3f) | 0x80 );
		add_char( outp, ((chr >> 6) & 0x3f) | 0x80 );
		add_char( outp, (chr & 0x3f) | 0x80 );
	}
}

static int
need_another_16bit( uint bits )
{
	return (bits & 0xfc00) == 0xd800;
}

static uint
utf16_decode( uint subject )
{
	return 0x10000 + (((subject >> 16) - 0xd800) << 10) + ((subject & 0xffff) - 0xdc00);
}

static uint
b64_decode( uchar chr )
{
	static uint lu[128] = {
		~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0,
		~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0,
		~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, ~0, 62, 63, ~0, ~0, ~0,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, ~0, ~0, ~0, ~0, ~0, ~0,
		~0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, ~0, ~0, ~0, ~0, ~0,
		~0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, ~0, ~0, ~0, ~0, ~0,
	};
	return lu[chr];
}

int
imap_utf7_to_utf8( const char *buf, int bufl, char *outbuf )
{
	// Size requirements:
	// - pass-through: l (shortest worst case)
	// - all "&": l / 2, 2 => 1, * .5
	// - 7-bit: ((l - 2) * 3 + 1) / 4 / 2, ~ l * .38, 5 => 1, * .2
	// - 3-octet: ((l - 2) * 3 + 1) / 4 / 2 * 3, ~ l * 1.13, 5 => 3, * .6 (generic worst case)
	// - 4-octet: ((l - 2) * 3 + 1) / 4 / 2 / 2 * 4, ~ l * .75, 8 => 4, * .5
	// => reserve bufl * 9 / 8
	char *outp = outbuf;
	struct bit_fifo fifo = { 0, 0 };
	const char *bufe = buf + bufl;
	while (buf != bufe) {
		uchar chr = *buf++;
		if (chr != '&') {
			if (chr & 0x80) {
				dbg( "Error: 8-bit char %x\n", chr );
				return -1;
			}
			add_char( &outp, chr );
			continue;
		}
		if (buf == bufe) {
			dbg( "Error: unterminated shift sequence\n" );
			return -1;
		}
		chr = *buf++;
		if (chr == '-') {
			add_char( &outp, '&' );
			continue;
		}
		fifo.bits = 0;
		do {
			if (chr & 0x80) {
				dbg( "Error: 8-bit char %x\n", chr );
				return -1;
			}
			uint bits = b64_decode( chr );
			if (bits == ~0U) {
				dbg( "Error: char %x outside alphabet\n", chr );
				return -1;
			}
			add_bits( &fifo, bits, 6 );
			if (fifo.bits >= 16) {
				if (need_another_16bit( peek_bits( &fifo, 16 ) )) {
					if (fifo.bits >= 32) {
						uint utf16 = eat_bits( &fifo, 32 );
						if ((utf16 & 0xfc00) != 0xdc00) {
							dbg( "Error: unpaired UTF-16 surrogate\n" );
							return -1;
						}
						write_as_utf8( &outp, utf16_decode( utf16 ) );
					}
				} else {
					write_as_utf8( &outp, eat_bits( &fifo, 16 ) );
				}
			}
			if (buf == bufe) {
				dbg( "Error: unterminated shift sequence\n" );
				return -1;
			}
			chr = *buf++;
		} while (chr != '-');
		if (fifo.bits > 6) {
			dbg( "Error: incomplete code point\n" );
			return -1;
		}
	}
	return (int)(outp - outbuf);
}
