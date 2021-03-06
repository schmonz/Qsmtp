#include <qsmtpd/userfilters.h>

#include <qsmtpd/addrparse.h>
#include <qsmtpd/qsmtpd.h>
#include <qsmtpd/userconf.h>
#include "test_io/testcase_io.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

struct xmitstat xmitstat;
unsigned int goodrcpt;
struct recip *thisrecip;
const char **globalconf;
struct rcpt_list head;

extern int cb_badcc(const struct userconf *ds, const char **logmsg, enum config_domain *t);

static struct recip recips[] = {
	{
		.to = {
			.s = "foo@example.com"
		}
	},
	{
		.to = {
			.s = "bar@example.com"
		}
	},
	{
		.to = {
			.s = "foo@example.net"
		}
	},
	{
		.to = {
			.s = "bar@example.net"
		}
	},
	{
		.to = {
			.s = "baz@sub.example.net"
		}
	}
};

static const char badcc_foo[] = "nonexistent@invalid.example.net\0@example.com\0sub.example.net\0\0";
static const char badcc_domain[] = "foo@example.com\0\0";

#define RCPT_PATTERNS 5
#define VALID_USERDIRFD (420 + 42)
#define VALID_DOMAINDIRFD (VALID_USERDIRFD + 1)

static struct userconf ds;

static void
setup_userconf()
{
	if (strstr(thisrecip->to.s, "@example.net") != NULL) {
		ds.domainpath.s = "example.net/";
		ds.domaindirfd = VALID_DOMAINDIRFD;
		if (strcmp(thisrecip->to.s, "foo@example.net") == 0)
			ds.userdirfd = VALID_USERDIRFD;
		else
			ds.userdirfd = -1;
	} else {
		ds.domainpath.s = NULL;
		ds.domaindirfd = -1;
		ds.userdirfd = -1;
	}

	if (ds.domainpath.s == NULL)
		ds.domainpath.len = 0;
	else
		ds.domainpath.len = strlen(ds.domainpath.s);
}

static void
setup_recip_order(unsigned int valid, int r0, int r1, int r2, int r3, int r4)
{
	/* it's easier to pass them as single values from outside
	 * since this is only a testcase I go this way */
	int rflags[RCPT_PATTERNS] = { r0, r1, r2, r3, r4 };

	assert(r0 >= 0);
	for (int i = 0; i < RCPT_PATTERNS; i++) {
		assert(rflags[i] < RCPT_PATTERNS);
	}
	assert(valid < (1 << RCPT_PATTERNS));

	goodrcpt = 0;
	TAILQ_INIT(&head);

	for (int i = 0; (i < RCPT_PATTERNS) && (rflags[i] >= 0); i++) {
		thisrecip = &recips[rflags[i]];
		thisrecip->ok = (valid & (1 << rflags[i]));
		memset(&thisrecip->entries, 0, sizeof(thisrecip->entries));
		TAILQ_INSERT_TAIL(&head, thisrecip, entries);
		if (thisrecip->ok)
			goodrcpt++;
	}

	setup_userconf();
}

int
userconf_get_buffer(const struct userconf *uc, const char *key, char ***values, checkfunc cf, const unsigned int flags)
{
	int type;
	const char *res = NULL;
	unsigned int i;

	if (flags != userconf_global) {
		fprintf(stderr, "%s() was called with flags %i instead of %i\n", __func__, flags, userconf_global);
		exit(1);
	}

	if (strcmp(key, "badcc") != 0) {
		fprintf(stderr, "%s() was called with key %s set\n", __func__, key);
		exit(1);
	}

	if (cf != checkaddr) {
		fprintf(stderr, "%s() was called with cf %p instead of checkaddr\n", __func__, cf);
		exit(1);
	}

	if (uc->userdirfd == VALID_USERDIRFD) {
		res = badcc_foo;
		type = CONFIG_USER;
	} else if ((uc->domainpath.s != NULL) && (strcmp(uc->domainpath.s, "example.net/") == 0)) {
		res = badcc_domain;
		type = CONFIG_DOMAIN;
	} else {
		*values = NULL;
		return CONFIG_NONE;
	}

	const char *c = res;
	for (i = 0; *c != '\0'; i++)
		c += strlen(c) + 1;

	*values = calloc(i + 1, sizeof(**values));
	if (*values == NULL)
		return -ENOMEM;

	c = res;
	for (i = 0; *c != '\0'; i++) {
		(*values)[i] = (char *)c;
		c += strlen(c) + 1;
	}

	assert((type >= CONFIG_USER) && (type <= CONFIG_GLOBAL));
	return type;
}

int
main(void)
{
	const char *logmsg;
	enum config_domain t;
	int err = 0;

	testcase_ignore_log_writen();

	memset(&ds, 0, sizeof(ds));
	globalconf = NULL;
	for (int i = 0; i < RCPT_PATTERNS; i++)
		recips[i].to.len = strlen(recips[i].to.s);

	setup_recip_order(1, 0, -1, -1, -1, -1);

	int r = cb_badcc(&ds, &logmsg, &t);
	if (r != 0) {
		fprintf(stderr, "with only a single recipient no errors should happen,"
				" but result is %i\n", r);
		err++;
	}

	setup_recip_order(3, 0, 1, -1, -1, -1);
	r = cb_badcc(&ds, &logmsg, &t);
	if (r != 0) {
		fprintf(stderr, "for recipients without config no errors should happen,"
				" but result is %i\n", r);
		err++;
	}

	setup_recip_order(5, 0, 2, -1, -1, -1);
	r = cb_badcc(&ds, &logmsg, &t);
	if (r != 2) {
		fprintf(stderr, "foo@example.net should reject bad CC, but result is %i\n", r);
		err++;
	}

	setup_recip_order(4, 2, 0, -1, -1, -1);
	thisrecip = &recips[2];
	setup_userconf();
	t = -1;
	r = cb_badcc(&ds, &logmsg, &t);
	if (r != 2) {
		fprintf(stderr, "foo@example.net as first recipient with example.net following"
				" should reject, but result is %i, t = %i\n", r, t);
		err++;
	} else if (t != CONFIG_USER) {
		fprintf(stderr, "foo@example.net should reject with user policy,"
				" but t is %i\n", t);
		err++;
	}

	setup_recip_order(8, 0, 3, -1, -1, -1);
	t = -1;
	r = cb_badcc(&ds, &logmsg, &t);
	if (r != 2) {
		fprintf(stderr, "bar@example.net should reject foo@example.com,"
				" but result is %i, t = %i\n", r, t);
		err++;
	} else if (t != CONFIG_DOMAIN) {
		fprintf(stderr, "bar@example.net should reject with domain policy,"
				" but t is %i\n", t);
		err++;
	}

	setup_recip_order(4, 4, 2, -1, -1, -1);
	t = -1;
	r = cb_badcc(&ds, &logmsg, &t);
	if (r != 2) {
		fprintf(stderr, "foo@example.net should reject baz@sub.example.net,"
				" but result is %i, t = %i\n", r, t);
		err++;
	} else if (t != CONFIG_USER) {
		fprintf(stderr, "bar@example.net should reject with user policy,"
				" but t is %i\n", t);
		err++;
	}

	return err;
}
