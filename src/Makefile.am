# SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
# SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
# SPDX-License-Identifier: GPL-2.0-or-later

mbsync_SOURCES = \
	util.c config.c socket.c \
	driver.c drv_proxy.c \
	drv_imap.c \
	drv_maildir.c \
	sync.c sync_state.c \
	main.c
noinst_HEADERS = \
	common.h config.h socket.h \
	driver.h \
	sync.h sync_p.h
mbsync_LDADD = $(DB_LIBS) $(SSL_LIBS) $(SOCK_LIBS) $(SASL_LIBS) $(Z_LIBS) $(KEYCHAIN_LIBS)

drv_proxy.$(OBJEXT): drv_proxy.inc
drv_proxy.inc: $(srcdir)/driver.h $(srcdir)/drv_proxy.c $(srcdir)/drv_proxy_gen.pl
	perl $(srcdir)/drv_proxy_gen.pl $(srcdir)/driver.h $(srcdir)/drv_proxy.c drv_proxy.inc

ENUM_GEN = $(srcdir)/bit_enum_gen.pl

$(mbsync_OBJECTS): common_enum.h
common_enum.h: common.h $(ENUM_GEN)
	perl $(ENUM_GEN) < $< > $@

$(mbsync_OBJECTS): driver_enum.h
driver_enum.h: driver.h $(ENUM_GEN)
	perl $(ENUM_GEN) < $< > $@

$(mbsync_OBJECTS): sync_enum.h
sync_enum.h: sync.h $(ENUM_GEN)
	perl $(ENUM_GEN) < $< > $@

sync.$(OBJEXT): sync_c_enum.h
sync_c_enum.h: sync.c $(ENUM_GEN)
	perl $(ENUM_GEN) < $< > $@

sync.$(OBJEXT) sync_state.$(OBJEXT): sync_p_enum.h
sync_p_enum.h: sync_p.h $(ENUM_GEN)
	perl $(ENUM_GEN) < $< > $@

mdconvert_SOURCES = mdconvert.c
mdconvert_LDADD = $(DB_LIBS)
if with_mdconvert
mdconvert_prog = mdconvert
mdconvert_man = mdconvert.1
endif

bin_PROGRAMS = mbsync $(mdconvert_prog)
man_MANS = mbsync.1 $(mdconvert_man)

tst_timers_SOURCES = tst_timers.c util.c

EXTRA_PROGRAMS = tst_timers

exampledir = $(docdir)/examples
example_DATA = mbsyncrc.sample

EXTRA_DIST = bit_enum_gen.pl drv_proxy_gen.pl run-tests.pl $(example_DATA) $(man_MANS)

CLEANFILES = *_enum.h drv_proxy.inc
