// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#include "sync_p.h"

static void
copy_msg_bytes( char **out_ptr, const char *in_buf, uint *in_idx, uint in_len, int in_cr, int out_cr )
{
	char *out = *out_ptr;
	uint idx = *in_idx;
	if (out_cr != in_cr) {
		char c;
		if (out_cr) {
			for (; idx < in_len; idx++) {
				if ((c = in_buf[idx]) != '\r') {
					if (c == '\n')
						*out++ = '\r';
					*out++ = c;
				}
			}
		} else {
			for (; idx < in_len; idx++) {
				if ((c = in_buf[idx]) != '\r')
					*out++ = c;
			}
		}
	} else {
		memcpy( out, in_buf + idx, in_len - idx );
		out += in_len - idx;
		idx = in_len;
	}
	*out_ptr = out;
	*in_idx = idx;
}

char *
copy_msg_convert( int in_cr, int out_cr, copy_vars_t *vars )
{
	char *in_buf = vars->data.data;
	uint in_len = vars->data.len;
	uint idx = 0, sbreak = 0, ebreak = 0, break2 = UINT_MAX;
	uint lines = 0, hdr_crs = 0, bdy_crs = 0, app_cr = 0, extra = 0;
	uint add_subj = 0;

	if (vars->srec) {
	  nloop: ;
		uint start = idx;
		uint line_crs = 0;
		while (idx < in_len) {
			char c = in_buf[idx++];
			if (c == '\r') {
				line_crs++;
			} else if (c == '\n') {
				if (!ebreak && starts_with_upper( in_buf + start, (int)(in_len - start), "X-TUID: ", 8 )) {
					extra = (sbreak = start) - (ebreak = idx);
					if (!vars->minimal)
						goto oke;
				} else {
					if (break2 == UINT_MAX && vars->minimal &&
					    starts_with_upper( in_buf + start, (int)(in_len - start), "SUBJECT:", 8 )) {
						break2 = start + 8;
						if (break2 < in_len && in_buf[break2] == ' ')
							break2++;
					}
					lines++;
					hdr_crs += line_crs;
				}
				if (idx - line_crs - 1 == start) {
					if (!ebreak)
						sbreak = ebreak = start;
					if (vars->minimal) {
						in_len = idx;
						if (break2 == UINT_MAX) {
							break2 = start;
							add_subj = 1;
						}
					}
					goto oke;
				}
				goto nloop;
			}
		}
		free( in_buf );
		return "has incomplete header";
	  oke:
		app_cr = out_cr && (!in_cr || hdr_crs);
		extra += 8 + TUIDL + app_cr + 1;
	}
	if (out_cr != in_cr) {
		for (; idx < in_len; idx++) {
			char c = in_buf[idx];
			if (c == '\r')
				bdy_crs++;
			else if (c == '\n')
				lines++;
		}
		extra -= hdr_crs + bdy_crs;
		if (out_cr)
			extra += lines;
	}

	uint dummy_msg_len = 0;
	char dummy_msg_buf[256];
	static const char dummy_pfx[] = "[placeholder] ";
	static const char dummy_subj[] = "Subject: [placeholder] (No Subject)";
	static const char dummy_msg[] =
		"Having a size of %s, this message is over the MaxSize limit.%s"
		"Flag it and sync again (Sync mode Upgrade) to fetch its real contents.%s";
	static const char dummy_flag[] =
		"%s"
		"The original message is flagged as important.%s";

	if (vars->minimal) {
		char sz[32];

		if (vars->msg->size < 1024000)
			sprintf( sz, "%dKiB", (int)(vars->msg->size >> 10) );
		else
			sprintf( sz, "%.1fMiB", vars->msg->size / 1048576. );
		const char *nl = app_cr ? "\r\n" : "\n";
		dummy_msg_len = (uint)sprintf( dummy_msg_buf, dummy_msg, sz, nl, nl );
		if (vars->data.flags & F_FLAGGED) {
			vars->data.flags &= ~F_FLAGGED;
			dummy_msg_len += (uint)sprintf( dummy_msg_buf + dummy_msg_len, dummy_flag, nl, nl );
		}
		extra += dummy_msg_len;
		extra += add_subj ? strlen(dummy_subj) + app_cr + 1 : strlen(dummy_pfx);
	}

#define ADD_NL() \
		do { \
			if (app_cr) \
				*out_buf++ = '\r'; \
			*out_buf++ = '\n'; \
		} while (0)

	vars->data.len = in_len + extra;
	if (vars->data.len > INT_MAX) {
		free( in_buf );
		return "is too big after conversion";
	}
	char *out_buf = vars->data.data = nfmalloc( vars->data.len );
	idx = 0;
	if (vars->srec) {
		if (break2 < sbreak) {
			copy_msg_bytes( &out_buf, in_buf, &idx, break2, in_cr, out_cr );
			memcpy( out_buf, dummy_pfx, strlen(dummy_pfx) );
			out_buf += strlen(dummy_pfx);
		}
		copy_msg_bytes( &out_buf, in_buf, &idx, sbreak, in_cr, out_cr );

		memcpy( out_buf, "X-TUID: ", 8 );
		out_buf += 8;
		memcpy( out_buf, vars->srec->tuid, TUIDL );
		out_buf += TUIDL;
		ADD_NL();
		idx = ebreak;

		if (break2 != UINT_MAX && break2 >= sbreak) {
			copy_msg_bytes( &out_buf, in_buf, &idx, break2, in_cr, out_cr );
			if (!add_subj) {
				memcpy( out_buf, dummy_pfx, strlen(dummy_pfx) );
				out_buf += strlen(dummy_pfx);
			} else {
				memcpy( out_buf, dummy_subj, strlen(dummy_subj) );
				out_buf += strlen(dummy_subj);
				ADD_NL();
			}
		}
	}
	copy_msg_bytes( &out_buf, in_buf, &idx, in_len, in_cr, out_cr );

	if (vars->minimal)
		memcpy( out_buf, dummy_msg_buf, dummy_msg_len );

	free( in_buf );
	return NULL;
}
