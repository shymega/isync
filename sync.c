/* $Id$
 *
 * isync - IMAP4 to maildir mailbox synchronizer
 * Copyright (C) 2000 Michael R. Elkins <me@mutt.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include "isync.h"

static unsigned int MaildirCount = 0;

static message_t *
find_msg (message_t * list, unsigned int uid)
{
    for (; list; list = list->next)
	if (list->uid == uid)
	    return list;
    return 0;
}

int
sync_mailbox (mailbox_t * mbox, imap_t * imap, int flags)
{
    message_t *cur;
    message_t *tmp;
    char path[_POSIX_PATH_MAX];
    char newpath[_POSIX_PATH_MAX];
    char *p;
    int fd;
    int ret;

    for (cur = mbox->msgs; cur; cur = cur->next)
    {
	tmp = find_msg (imap->msgs, cur->uid);
	if (!tmp)
	{
	    printf ("warning, uid %d doesn't exist on server\n", cur->uid);
	    if (flags & SYNC_DELETE)
	    {
		cur->flags |= D_DELETED;
		cur->dead = 1;
		mbox->deleted++;
	    }
	    continue;
	}
	tmp->processed = 1;

	if (!(flags & SYNC_FAST))
	{
	    /* check if local flags are different from server flags.
	     * ignore \Recent and \Draft
	     */
	    if (cur->flags != (tmp->flags & ~(D_RECENT | D_DRAFT)))
	    {
		/* set local flags that don't exist on the server */
		if (!(tmp->flags & D_DELETED) && (cur->flags & D_DELETED))
		    imap->deleted++;
		imap_set_flags (imap, cur->uid, cur->flags & ~tmp->flags);

		/* update local flags */
		if((cur->flags & D_DELETED) == 0 && (tmp->flags & D_DELETED))
		    mbox->deleted++;
		cur->flags |= (tmp->flags & ~(D_RECENT | D_DRAFT));
		cur->changed = 1;
		mbox->changed = 1;
	    }
	}
    }

    fputs ("Fetching new messages", stdout);
    fflush (stdout);
    for (cur = imap->msgs; cur; cur = cur->next)
    {
	if (!cur->processed)
	{
	    /* new message on server */

	    if ((flags & SYNC_EXPUNGE) && (cur->flags & D_DELETED))
	    {
		/* this message has been marked for deletion and
		 * we are currently expunging a mailbox.  don't
		 * bother downloading this message
		 */
		continue;
	    }

	    /* create new file */
	    snprintf (path, sizeof (path), "%s/tmp/%s.%ld_%d.%d.UID%d",
		    mbox->path, Hostname, time (0), MaildirCount++,
		    getpid (), cur->uid);

	    if (cur->flags)
	    {
		/* append flags */
		snprintf (path + strlen (path), sizeof (path) - strlen (path),
			  ":2,%s%s%s%s",
			  (cur->flags & D_FLAGGED) ? "F" : "",
			  (cur->flags & D_ANSWERED) ? "R" : "",
			  (cur->flags & D_SEEN) ? "S" : "",
			  (cur->flags & D_DELETED) ? "T" : "");

	    }

	    /* give some visual feedback that something is happening */
	    fputs (".", stdout);
	    fflush (stdout);

//          printf("creating %s\n", path);
	    fd = open (path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	    if (fd < 0)
	    {
		perror ("open");
		continue;
	    }

	    ret = imap_fetch_message (imap, cur->uid, fd);

	    close (fd);

	    if (!ret)
	    {
		p = strrchr (path, '/');

		snprintf (newpath, sizeof (newpath), "%s/%s%s", mbox->path,
			(cur->flags & D_SEEN) ? "cur" : "new", p);

		//          printf ("moving %s to %s\n", path, newpath);

		if (rename (path, newpath))
		    perror ("rename");
	    }
	    else
		unlink(path);
	}
    }
    puts ("");

    return 0;
}
