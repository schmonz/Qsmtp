OWFATPATH=../libowfat-0.20-eike
CDBPATH=../vpopmail-5.4.0/cdb
SHELL=/bin/sh
CC=gcc
CFLAGS=-O2 -c -Wall -W -I$(shell pwd)/include -DIPV4ONLY -g
LD=gcc
LDFLAGS=-lssl -lcrypto #-lefence

export SHELL CC CFLAGS LD LDFLAGS

SUBDIRS = lib callbacks qsmtpd

TARGETS = targets/Qsmtpd targets/addipbl

.phony: all clean subdirs install

default: all

all: subdirs $(TARGETS)

subdirs:
	for dir in $(SUBDIRS); do\
		$(MAKE) -C $$dir; \
	done

vpath %.h ./include

clean:
	rm -f *.o *~ \#* $(TARGETS) targets/addipbl.o
	for dir in $(SUBDIRS); do\
		$(MAKE) -C $$dir clean; \
	done

targets/Qsmtpd: qsmtpd/qsmtpd.o qsmtpd/antispam.o qsmtpd/auth.o qsmtpd/starttls.o qsmtpd/spf.o \
		qsmtpd/vpopmail.o lib/log.o lib/netio.o lib/dns.o lib/control.o lib/addrsyntax.o \
		lib/getfile.o lib/ssl_timeoutio.o lib/tls.o lib/base64.o \
		callbacks/badmailfrom.o callbacks/dnsbl.o callbacks/badcc.o callbacks/usersize.o \
		callbacks/rcpt_cbs.o callbacks/boolean.o callbacks/fromdomain.o \
		callbacks/check2822.o callbacks/ipbl.o callbacks/spf.o callbacks/soberg.o \
		callbacks/helo.o callbacks/forceesmtp.o \
		$(OWFATPATH)/libowfat.a $(CDBPATH)/cdb.a
	$(LD) $(LDFLAGS) -o $@ $^
	#chown qmaild:qmail $@

targets/addipbl: targets/addipbl.o
	$(LD) $(LDFLAGS) -o $@ $^

targets/addipbl.o: targets/addipbl.c

#FIXME: the destination directory must be fixed in case someone modifies qsmtpd.c::auto_qmail
install:
	install -s -g qmail -o qmaild targets/Qsmtpd /var/qmail/bin
