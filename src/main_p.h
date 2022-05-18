// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#ifndef MAIN_P_H
#define MAIN_P_H

#define DEBUG_FLAG DEBUG_MAIN

#include "sync.h"

typedef struct {
	int ret;
	int all;
	int list;
	int list_stores;
	int ops[2];
} core_vars_t;

void sync_chans( core_vars_t *cvars, char **argv );
void list_stores( core_vars_t *cvars, char **argv );

#endif
