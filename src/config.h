// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

typedef struct {
	const char *file;
	FILE *fp;
	char *buf;
	int bufl;
	int line;
	int err;
	int ms_warn;
	char *cmd, *val, *rest;
} conffile_t;

extern char FieldDelimiter;

#define ARG_OPTIONAL 0
#define ARG_REQUIRED 1

char *expand_strdup( const char *s );

char *get_arg( conffile_t *cfile, int required, int *comment );

char parse_bool( conffile_t *cfile );
int parse_int( conffile_t *cfile );
uint parse_size( conffile_t *cfile );
int getcline( conffile_t *cfile );
int merge_ops( int cops, int ops[] );
int load_config( const char *filename );

#endif
