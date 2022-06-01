// SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
// SPDX-License-Identifier: GPL-2.0-or-later WITH LicenseRef-isync-GPL-exception
//
// mbsync - mailbox synchronizer
//

#include "main_p.h"

typedef struct store_ent {
	struct store_ent *next;
	store_conf_t *conf;
} store_ent_t;

typedef struct {
	core_vars_t *cvars;
	store_conf_t *store;
	driver_t *drv;
	store_t *ctx;
	store_ent_t *storeptr;
	int cben, done;
} list_vars_t;

static store_ent_t *
add_store( store_ent_t ***storeapp, store_conf_t *store )
{
	store_ent_t *se = nfzalloc( sizeof(*se) );
	se->conf = store;
	**storeapp = se;
	*storeapp = &se->next;
	return se;
}

static void do_list_stores( list_vars_t *lvars );
static void list_next_store( list_vars_t *lvars );

void
list_stores( core_vars_t *cvars, char **argv )
{
	list_vars_t lvars[1];
	store_ent_t *strs = NULL, **strapp = &strs;
	store_conf_t *store;

	memset( lvars, 0, sizeof(*lvars) );
	lvars->cvars = cvars;

	if (!stores) {
		fputs( "No stores defined.\n", stderr );
		cvars->ret = 1;
		return;
	}

	if (!*argv) {  // Implicit --all
		for (store = stores; store; store = store->next)
			add_store( &strapp, store );
	} else {
		for (; *argv; argv++) {
			for (store = stores; store; store = store->next) {
				if (!strcmp( store->name, *argv )) {
					add_store( &strapp, store );
					goto gotstr;
				}
			}
			error( "No store named '%s' defined.\n", *argv );
			cvars->ret = 1;
		  gotstr: ;
		}
	}
	if (cvars->ret)
		return;
	lvars->storeptr = strs;

	do_list_stores( lvars );
	main_loop();
}

static void
list_store_bad( void *aux )
{
	list_vars_t *lvars = (list_vars_t *)aux;

	lvars->drv->cancel_store( lvars->ctx );
	lvars->cvars->ret = 1;
	list_next_store( lvars );
}

static void
advance_store( list_vars_t *lvars )
{
	store_ent_t *nstr = lvars->storeptr->next;
	free( lvars->storeptr );
	lvars->storeptr = nstr;
}

static void list_store_connected( int sts, void *aux );

static void
do_list_stores( list_vars_t *lvars )
{
	while (lvars->storeptr) {
		lvars->store = lvars->storeptr->conf;
		lvars->drv = lvars->store->driver;
		int st = lvars->drv->get_fail_state( lvars->store );
		if (st != FAIL_TEMP) {
			info( "Skipping %sfailed store %s.\n",
				  (st == FAIL_WAIT) ? "temporarily " : "", lvars->store->name );
			lvars->cvars->ret = 1;
			goto next;
		}

		uint dcaps = lvars->drv->get_caps( NULL );
		store_t *ctx = lvars->drv->alloc_store( lvars->store, "" );
		if ((DFlags & DEBUG_DRV) || ((DFlags & FORCEASYNC(F)) && !(dcaps & DRV_ASYNC))) {
			lvars->drv = &proxy_driver;
			ctx = proxy_alloc_store( ctx, "", DFlags & FORCEASYNC(F) );
		}
		lvars->ctx = ctx;
		lvars->drv->set_bad_callback( ctx, list_store_bad, lvars );
		info( "Opening store %s...\n", lvars->store->name );
		lvars->cben = lvars->done = 0;
		lvars->drv->connect_store( lvars->ctx, list_store_connected, lvars );
		if (!lvars->done) {
			lvars->cben = 1;
			return;
		}

	  next:
		advance_store( lvars );
	}
	cleanup_drivers();
}

static void
list_next_store( list_vars_t *lvars )
{
	if (lvars->cben) {
		advance_store( lvars );
		do_list_stores( lvars );
	}
}

static void
list_done_store( list_vars_t *lvars )
{
	lvars->done = 1;
	lvars->drv->free_store( lvars->ctx );
	list_next_store( lvars );
}

static void list_store_listed( int sts, string_list_t *boxes, void *aux );

static void
list_store_connected( int sts, void *aux )
{
	list_vars_t *lvars = (list_vars_t *)aux;

	switch (sts) {
	case DRV_CANCELED:
		return;
	case DRV_OK:
		lvars->drv->list_store( lvars->ctx, LIST_INBOX | LIST_PATH_MAYBE, list_store_listed, lvars );
		break;
	default:
		lvars->cvars->ret = 1;
		list_done_store( lvars );
		break;
	}
}

static void
list_store_listed( int sts, string_list_t *boxes, void *aux )
{
	list_vars_t *lvars = (list_vars_t *)aux;
	string_list_t *box;

	switch (sts) {
	case DRV_CANCELED:
		return;
	case DRV_OK:
		printf( "===== %s:\n", lvars->ctx->conf->name );
		for (box = boxes; box; box = box->next)
			puts( box->string );
		break;
	default:
		lvars->cvars->ret = 1;
		break;
	}
	list_done_store( lvars );
}
