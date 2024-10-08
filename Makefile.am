# SPDX-FileCopyrightText: 2000-2002 Michael R. Elkins <me@mutt.org>
# SPDX-FileCopyrightText: 2002-2022 Oswald Buddenhagen <ossi@users.sf.net>
# SPDX-License-Identifier: GPL-2.0-or-later

SUBDIRS = src
bin_SCRIPTS = mbsync-get-cert
EXTRA_DIST = LICENSES VERSION debian isync.spec $(bin_SCRIPTS)

LOG_PL = \
    use POSIX qw(strftime); \
    use Date::Parse; \
    use Text::Wrap; \
    $$Text::Wrap::columns = 72; \
    while (defined($$_ = <>)) { \
        /^commit / or die "commit missing: $$_"; \
        <> =~ /^log size (\d+)$$/ or die "wrong size"; \
        $$len = $$1; \
        read(STDIN, $$log, $$len) == $$len or die "unexpected EOF"; \
        $$log =~ s/^Author: ([^>]+>)\nDate:   (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d [-+]\d{4})\n(.*)$$/$$3/s or die "unexpected log format"; \
        $$author = $$1; $$date = str2time($$2); \
        scalar(<>); \
        @files = (); \
        $$pfx = ""; \
        while (defined($$l = <>) and $$l ne "\n") { \
            chomp $$l; \
            next if ($$l =~ m,^(ChangeLog$$|NEWS$$|TODO$$|debian/),); \
            if (!@files) { \
                $$pfx = $$l; \
                $$pfx =~ s,/?[^/]+$$,,; \
            } else { \
                while (length($$pfx)) { \
                    $$l =~ m,^\Q$$pfx/\E, and last; \
                    $$pfx =~ s,/?[^/]+$$,,; \
                } \
            } \
            push @files, $$l; \
        } \
        next if (!@files); \
        print strftime("%F %H:%M", gmtime($$date))."  ".$$author."\n\n"; \
        if (@files > 1 and ($$len = length($$pfx))) { \
            @efiles = (); \
            for $$f (@files) { push @efiles, substr($$f, $$len + 1); } \
            $$fstr = $$pfx."/: "; \
        } else { \
            @efiles = @files; \
            $$fstr = ""; \
        } \
        print wrap("\t* ", "\t  ", $$fstr.join(", ", @efiles).":")."\n"; \
        $$log =~ s, +$$,,gm; \
        $$log =~ s,^    ,\t,gm; \
        print $$log."\n"; \
    }

$(srcdir)/.git/index:
$(srcdir)/ChangeLog: $(srcdir)/.git/index
	$(MAKE) log

log:
	@test -z "$(srcdir)" || cd $(srcdir) && \
	 ( ! test -d .git || \
	   git log --pretty=medium --date=iso --log-size --name-only --no-merges | \
	     perl -e '$(LOG_PL)' > ChangeLog )

cov-scan: clean
	/opt/cov-analysis-*/bin/cov-build --dir cov-int $(MAKE)
	tar cavf isync-cov.tar.xz cov-int

deb:
	CFLAGS= INSTALL= dpkg-buildpackage -b --no-sign

dist-hook:
	find $(distdir)/debian \( -name .#\*# -o -type l \) -print0 | xargs -0r rm -rf
	-cd $(distdir)/debian && test -f .gitignore && rm -rf `cut -c2- .gitignore` .gitignore

dist-sign: dist
	gpg -b -a $(PACKAGE)-$(VERSION).tar.gz

rpm: dist
	CFLAGS="-O2 -mtune=core2" rpmbuild --clean -ta $(PACKAGE)-$(VERSION).tar.gz

rpm-ia32: dist
	CFLAGS="-O2 -m32 -march=i686" rpmbuild --target i686-unknown-linux --clean -ta $(PACKAGE)-$(VERSION).tar.gz

doc_DATA = README TODO NEWS ChangeLog AUTHORS
