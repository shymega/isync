// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later
//
// isync test suite
//

#include "imap_p.h"

static struct {
	const char *utf8, *utf7;
} data[] = {
	{ u8"", "" },
	{ u8"1", "1" },
	{ u8"word", "word" },
	{ u8"&", "&-" },
	{ NULL, "&" },
	{ NULL, "&-&" },
	{ u8"&&", "&-&-" },
	{ u8"1&1", "1&-1" },
	{ u8"&1&", "&-1&-" },
	{ u8"\t", "&AAk-" },
	{ NULL, "&AAk" },
	{ NULL, "&AA-" },
	{ NULL, "&*Ak-" },
	{ NULL, "&&-" },
	{ u8"m\x7f""ll", "m&AH8-ll" },
	{ u8"\t&", "&AAk-&-" },
	{ u8"\t&\t", "&AAk-&-&AAk-" },
	{ u8"&\t", "&-&AAk-" },
	{ u8"&\t&", "&-&AAk-&-" },
	{ u8"ä", "&AOQ-" },
	{ u8"\x83\x84", NULL },
	{ u8"\xc3\xc4", NULL },
	{ u8"\xc3", NULL },
	{ u8"äö", "&AOQA9g-" },
	{ u8"äöü", "&AOQA9gD8-" },
	{ u8"Ḁ", "&HgA-" },
	{ u8"\xe1\xc8\x80", NULL },
	{ u8"\xe1\xb8\xf0", NULL },
	{ u8"\xe1\xb8", NULL },
	{ u8"\xe1", NULL },
	{ u8"Ḁḁ", "&HgAeAQ-" },
	{ u8"😂", "&2D3eAg-" },
	{ u8"\xf8\x9f\x98\x82", NULL },
	{ u8"\xf0\xcf\x98\x82", NULL },
	{ u8"\xf0\x9f\xd8\x82", NULL },
	{ u8"\xf0\x9f\x98\xe2", NULL },
	{ u8"\xf0\x9f\x98", NULL },
	{ u8"\xf0\x9f", NULL },
	{ u8"\xf0", NULL },
	{ NULL, "&2D0-" },
	{ u8"😈😎", "&2D3eCNg93g4-" },
	{ u8"müll", "m&APw-ll" },
	{ u8"mü", "m&APw-" },
	{ u8"über", "&APw-ber" },
};

int
main( void )
{
	int ret = 0;

	for (uint i = 0; i < as(data); i++) {
		if (!data[i].utf8)
			continue;
		xprintf( "To UTF-7 \"%s\" (\"%!s\") ...\n", data[i].utf8, data[i].utf8 );
		char *utf7 = imap_utf8_to_utf7( data[i].utf8 );
		if (utf7) {
			if (!data[i].utf7) {
				xprintf( "Unexpected success: \"%s\" (\"%!s\")\n", utf7, utf7 );
				ret = 1;
			} else if (strcmp( utf7, data[i].utf7 )) {
				xprintf( "Mismatch, got \"%s\" (\"%!s\"), want \"%!s\"\n",
				         utf7, utf7, data[i].utf7 );
				ret = 1;
			}
			free( utf7 );
		} else {
			if (data[i].utf7) {
				xprintf( "Conversion failure.\n" );
				ret = 1;
			}
		}
	}

	for (uint i = 0; i < as(data); i++) {
		if (!data[i].utf7)
			continue;
		xprintf( "From UTF-7 \"%!s\" ...\n", data[i].utf7 );
		int utf7len = strlen( data[i].utf7 );
		char utf8buf[1000];
		int utf8len = imap_utf7_to_utf8( data[i].utf7, utf7len, utf8buf );
		if (utf8len >= 0) {
			if (!data[i].utf8) {
				xprintf( "Unexpected success: \"%.*s\" (\"%.*!s\")\n",
				         utf8len, utf8buf, utf8len, utf8buf );
				ret = 1;
			} else {
				int wantlen = strlen( data[i].utf8 );
				if (utf8len != wantlen || memcmp( utf8buf, data[i].utf8, utf8len )) {
					xprintf( "Mismatch, got \"%.*s\" (\"%.*!s\"), want \"%s\" (\"%!s\")\n",
					         utf8len, utf8buf, utf8len, utf8buf, data[i].utf8, data[i].utf8 );
					ret = 1;
				}
			}
			assert( utf8len < utf7len * 9 / 8 + 1 );
		} else {
			if (data[i].utf8) {
				xprintf( "Conversion failure.\n" );
				ret = 1;
			}
		}
	}

	return ret;
}
