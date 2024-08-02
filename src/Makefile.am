# SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
# SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
# SPDX-License-Identifier: GPL-2.0-or-later

mbsync_SOURCES = \
	util.c config.c socket.c \
	driver.c drv_proxy.c \
	drv_imap.c imap_msgs.c imap_utf7.c \
	drv_maildir.c \
	sync.c sync_state.c sync_msg_cvt.c \
	main.c main_sync.c main_list.c
noinst_HEADERS = \
	common.h config.h socket.h \
	driver.h imap_p.h \
	sync.h sync_p.h \
	main_p.h
mbsync_LDADD = $(DB_LIBS) $(SSL_LIBS) $(SOCK_LIBS) $(SASL_LIBS) $(Z_LIBS) $(KEYCHAIN_LIBS)

drv_proxy.$(OBJEXT): drv_proxy.inc
drv_proxy.inc: $(srcdir)/driver.h $(srcdir)/drv_proxy.c $(srcdir)/drv_proxy_gen.pl
	perl $(srcdir)/drv_proxy_gen.pl $(srcdir)/driver.h $(srcdir)/drv_proxy.c drv_proxy.inc

mdconvert_SOURCES = mdconvert.c
mdconvert_LDADD = $(DB_LIBS)
if with_mdconvert
mdconvert_prog = mdconvert
mdconvert_man = mdconvert.1
endif

in_man = mbsync.1.in mdconvert.1.in

bin_PROGRAMS = mbsync $(mdconvert_prog)
# don't forget to update AC_CONFIG_FILES in configure.ac!
man_MANS = mbsync.1 $(mdconvert_man)

tst_imap_msgs_SOURCES = tst_imap_msgs.c imap_msgs.c util.c

tst_imap_utf7_SOURCES = tst_imap_utf7.c imap_utf7.c util.c

tst_msg_cvt_SOURCES = tst_msg_cvt.c sync_msg_cvt.c util.c
tst_msg_cvt_CFLAGS = -DQPRINTF_BUFF=10000

check_PROGRAMS = tst_imap_msgs tst_imap_utf7 tst_msg_cvt
TESTS = $(check_PROGRAMS)

tst_timers_SOURCES = tst_timers.c util.c

EXTRA_PROGRAMS = tst_timers

exampledir = $(docdir)/examples
example_DATA = mbsyncrc.sample

EXTRA_DIST = drv_proxy_gen.pl run-tests.pl $(example_DATA) $(in_man)

CLEANFILES = drv_proxy.inc
