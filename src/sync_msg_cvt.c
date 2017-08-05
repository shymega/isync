// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#include "sync_p.h"

static void
copy_msg_bytes( char **out_ptr, const char *in_buf, uint *in_idx, uint in_len, int in_cr, int out_cr, uint max_line_len )
{
	char *out = *out_ptr;
	uint idx = *in_idx;
	if (out_cr != in_cr) {
		/* message needs to be converted */
		assert( !max_line_len );  // not supported yet
		if (out_cr) {
			/* adding CR */
			for (char c, pc = 0; idx < in_len; idx++) {
				if (((c = in_buf[idx]) == '\n') && (pc != '\r'))
					*out++ = '\r';
				*out++ = c;
				pc = c;
			}
		} else {
			/* removing CR */
			for (char c, pc = 0; idx < in_len; idx++) {
				if (((c = in_buf[idx]) == '\n') && (pc == '\r'))
					out--;
				*out++ = c;
				pc = c;
			}
		}
	} else {
		/* no CRLF change */
		if (max_line_len > 0) {
			/* there are too long lines in the message */
			for (;;) {
				const char *curLine = in_buf + idx;
				uint leftLen = in_len - idx;
				char *nextLine = memchr( curLine, '\n', leftLen );
				uint curLineLen = nextLine ? (uint)(nextLine - curLine) + 1 : leftLen;
				uint line_idx = 0;
				for (;;) {
					uint cutLen = curLineLen - line_idx;
					if (cutLen > max_line_len)
						cutLen = max_line_len;
					memcpy( out, curLine + line_idx, cutLen );
					out += cutLen;
					line_idx += cutLen;
					if (line_idx == curLineLen)
						break;
					/* add (CR)LF except for the last line */
					if (out_cr)
						*out++ = '\r';
					*out++ = '\n';
				}
				idx += curLineLen;
				if (!nextLine)
					break;
			}
			//debug("End index %d (message size %d), message size should be %d\n", idx, in_len, *in_idx + out - *out_ptr);
		} else {
			/* simple copy */
			memcpy( out, in_buf + idx, in_len - idx );
			out += in_len - idx;
			idx = in_len;
		}
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
	uint lines = 0, hdr_crs = 0, bdy_crs = 0, app_cr = 0, extra = 0, extra_bytes = 0;
	uint add_subj = 0, fix_tuid = 0, fix_subj = 0, fix_hdr = 0, end_hdr = 0;

	if (vars->srec) {
		if (global_conf.max_line_len) {
			char *curLine = in_buf;
			uint leftLen = in_len;
			for (;;) {
				char *nextLine = memchr( curLine, '\n', leftLen );
				uint curLineLen = nextLine ? (uint)(nextLine - curLine) + 1 : leftLen;
				if (curLineLen > global_conf.max_line_len) {
					if (!global_conf.cut_lines) {
						/* stop here with too long line error */
						free( in_buf );
						return "contains too long line(s)";
					}
					/* compute the addded lines as we are going to cut them */
					uint extra_lines = (curLineLen - 1) / global_conf.max_line_len;
					if (out_cr)
						extra_bytes += extra_lines;   // CR
					extra_bytes += extra_lines;   // LF
				}
				if (!nextLine)
					break;
				curLine = nextLine + 1;
				leftLen -= curLineLen;
			}
		}
		for (;;) {
			uint start = idx;
			uint line_cr = 0;
			uint got_line = 0;
			char pc = 0;
			while (idx < in_len) {
				char c = in_buf[idx++];
				if (c == '\n') {
					if (pc == '\r')
						line_cr = 1;
					got_line = 1;
					break;
				}
				pc = c;
			}
			if (!ebreak && starts_with_upper( in_buf + start, (int)(in_len - start), "X-TUID: ", 8 )) {
				extra = (sbreak = start) - (ebreak = idx);
				if (!vars->minimal)
					break;
				continue;
			}
			if (break2 == UINT_MAX && vars->minimal &&
			    starts_with_upper( in_buf + start, (int)(in_len - start), "SUBJECT:", 8 )) {
				break2 = start + 8;
				if (break2 < in_len && in_buf[break2] == ' ')
					break2++;
			}
			hdr_crs += line_cr;
			if (got_line) {
				lines++;
				if (idx - line_cr - 1 != start)
					continue;
				// Empty line => end of headers
			} else {
				// The line is incomplete.
				if (pc == '\r')
					idx--;  // For simplicity, move back before trailing CR
				if (idx != start) {
					// The line is non-empty, so schedule completing it
					fix_hdr = 1;
					// ... and put our headers after it. (It would seem easier
					// to prepend them, as then we could avoid the fixing - but
					// the line might be a continuation. We could also prepend
					// it to _all_ pre-exiting headers, but then we would risk
					// masking an (incorrectly present) leading 'From ' header.)
					start = idx;
				}
				end_hdr = 1;
			}
			if (!ebreak) {
				sbreak = ebreak = start;
				fix_tuid = fix_hdr;
				fix_hdr = 0;
			}
			if (vars->minimal) {
				in_len = idx;
				if (break2 == UINT_MAX) {
					break2 = start;
					add_subj = 1;
					fix_subj = fix_hdr;
					fix_hdr = 0;
				}
			} else {
				fix_hdr = 0;
				end_hdr = 0;
			}
			break;
		}
		app_cr = out_cr && (!in_cr || hdr_crs || !lines);
		if (fix_tuid || fix_subj || fix_hdr)
			extra += app_cr + 1;
		if (end_hdr)
			extra += app_cr + 1;
		extra += 8 + TUIDL + app_cr + 1;
	}
	if (out_cr != in_cr) {
		for (char pc = 0; idx < in_len; idx++) {
			char c = in_buf[idx];
			if (c == '\n') {
				lines++;
				if (pc == '\r')
					bdy_crs++;
			}
			pc = c;
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

	vars->data.len = in_len + extra + extra_bytes;
	if (vars->data.len > INT_MAX) {
		free( in_buf );
		return "is too big after conversion";
	}
	char *out_buf = vars->data.data = nfmalloc( vars->data.len );
	idx = 0;
	if (vars->srec) {
		if (break2 < sbreak) {
			copy_msg_bytes( &out_buf, in_buf, &idx, break2, in_cr, out_cr, 0 );
			memcpy( out_buf, dummy_pfx, strlen(dummy_pfx) );
			out_buf += strlen(dummy_pfx);
		}
		//debug ("Calling copy_msg_bytes for the header (0 to %d) with %d extra bytes\n", sbreak, extra);
		copy_msg_bytes( &out_buf, in_buf, &idx, sbreak, in_cr, out_cr, 0 );

		if (fix_tuid)
			ADD_NL();
		memcpy( out_buf, "X-TUID: ", 8 );
		out_buf += 8;
		memcpy( out_buf, vars->srec->tuid, TUIDL );
		out_buf += TUIDL;
		ADD_NL();
		idx = ebreak;

		if (break2 != UINT_MAX && break2 >= sbreak) {
			copy_msg_bytes( &out_buf, in_buf, &idx, break2, in_cr, out_cr, 0 );
			if (!add_subj) {
				memcpy( out_buf, dummy_pfx, strlen(dummy_pfx) );
				out_buf += strlen(dummy_pfx);
			} else {
				if (fix_subj)
					ADD_NL();
				memcpy( out_buf, dummy_subj, strlen(dummy_subj) );
				out_buf += strlen(dummy_subj);
				ADD_NL();
			}
		}
	}
	//debug ("Calling copy_msg_bytes for the body (at %d) with %d extra byte(s), limit is %d \n", ebreak, extra_bytes, extra_bytes > 0 ? global_conf.max_line_len : 0);
	copy_msg_bytes( &out_buf, in_buf, &idx, in_len, in_cr, out_cr, extra_bytes > 0 ? global_conf.max_line_len : 0 );
	//debug("Message after %s\n", vars->data.data);
	//debug("Good message size should be %d + %d\n",vars->data.len-extra, extra);

	if (vars->minimal) {
		if (end_hdr) {
			if (fix_hdr)
				ADD_NL();
			ADD_NL();
		}
		memcpy( out_buf, dummy_msg_buf, dummy_msg_len );
	}

	free( in_buf );
	return NULL;
}
