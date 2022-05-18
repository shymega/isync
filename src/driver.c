// SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
// SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
/*
 * mbsync - mailbox synchronizer
 */

#include "driver.h"

store_conf_t *stores;

driver_t *drivers[N_DRIVERS] = { &maildir_driver, &imap_driver };

void
cleanup_drivers( void )
{
	for (int t = 0; t < N_DRIVERS; t++)
		drivers[t]->cleanup();
}

// Keep the MESSAGE_FLAGS in sync (grep that)!
const char MsgFlags[] = { 'D', 'F', 'P', 'R', 'S', 'T' };

void
make_flags( uchar flags, char *buf )
{
	uint i, d;

	for (i = d = 0; i < as(MsgFlags); i++)
		if (flags & (1 << i))
			buf[d++] = MsgFlags[i];
	buf[d] = 0;
}

uint
count_generic_messages( message_t *msgs )
{
	uint count = 0;
	for (; msgs; msgs = msgs->next)
		count++;
	return count;
}

void
free_generic_messages( message_t *msgs )
{
	while (msgs) {
		message_t *tmsg = msgs->next;
		free( msgs->msgid );
		free( msgs );
		msgs = tmsg;
	}
}

void
parse_generic_store( store_conf_t *store, conffile_t *cfg, const char *type )
{
	if (!strcasecmp( "Trash", cfg->cmd )) {
		store->trash = nfstrdup( cfg->val );
	} else if (!strcasecmp( "TrashRemoteNew", cfg->cmd )) {
		store->trash_remote_new = parse_bool( cfg );
	} else if (!strcasecmp( "TrashNewOnly", cfg->cmd )) {
		store->trash_only_new = parse_bool( cfg );
	} else if (!strcasecmp( "MaxSize", cfg->cmd )) {
		store->max_size = parse_size( cfg );
	} else if (!strcasecmp( "MapInbox", cfg->cmd )) {
		store->map_inbox = nfstrdup( cfg->val );
	} else if (!strcasecmp( "Flatten", cfg->cmd )) {
		const char *p;
		for (p = cfg->val; *p; p++) {
			if (*p == '/') {
				error( "%s:%d: flattened hierarchy delimiter cannot contain the canonical delimiter '/'\n", cfg->file, cfg->line );
				cfg->err = 1;
				return;
			}
		}
		store->flat_delim = nfstrdup( cfg->val );
	} else {
		error( "%s:%d: keyword '%s' is not recognized in %s sections\n", cfg->file, cfg->line, cfg->cmd, type );
		cfg->rest = NULL;
		cfg->err = 1;
	}
}
