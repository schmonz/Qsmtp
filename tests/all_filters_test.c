#include <qsmtpd/userfilters.h>

#include <control.h>
#include <diropen.h>
#include <libowfatconn.h>
#include <qsmtpd/addrparse.h>
#include <qsmtpd/antispam.h>
#include <qsmtpd/qsmtpd.h>
#include <qsmtpd/userconf.h>
#include "test_io/testcase_io.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

struct xmitstat xmitstat;
unsigned int goodrcpt;
struct recip *thisrecip;
const char **globalconf;
struct rcpt_list head;

static unsigned int testindex;
static int err;

int
check_host(const char *domain __attribute__ ((unused)))
{
	return SPF_NONE;
}

int
test_ask_dnsa(const char *a, struct in6_addr **b)
{
	if (strcmp(a, "9.8.168.192.dnsblerror.example.net") == 0) {
		errno = EAGAIN;
		return -1;
	}

	if (strcmp(a, "9.8.168.192.dnsblmatch.example.net") == 0) {
		assert(b == NULL);
		return 1;
	}

	return 0;
}

void
dieerror(int a __attribute__ ((unused)))
{
	abort();
}

static struct {
	const char *testname;		/**< visual name of the test */
	const char *mailfrom;		/**< the from address to set */
	const char *failmsg;		/**< the expected failure message to log */
	const char *goodmailfrom;	/**< the goodmailfrom configuration */
	const char *badmailfrom;	/**< the badmailfrom configuration */
	const char *namebl;		/**< the namebl configuration */
	const char *dnsbl;		/**< the dnsbl configuration */
	const char *dnswl;		/**< the dnsbl whitelist configuration */
	const char *dnstxt_reply;	/**< result of dnstxt lookup */
	const char *userconf;		/**< the contents of the filterconf file */
	const char *netmsg;		/**< the expected message written to the network */
	const char *logmsg;		/**< the expected log message */
	const enum config_domain conf;	/**< which configuration type should be returned for the config entries */
	const int esmtp;		/**< if transmission should be sent in ESMTP mode */
} testdata[] = {
	{
		.mailfrom = NULL,
		.conf = CONFIG_USER
	},
	{
		.mailfrom = "foo@example.com",
		.failmsg = "bad mail from",
		.badmailfrom = "foo@example.com\0\0",
		.conf = CONFIG_USER
	},
	{
		.mailfrom = "foo@example.com",
		.failmsg = "bad mail from",
		.badmailfrom = "@example.com\0\0",
		.conf = CONFIG_USER
	},
	{
		.mailfrom = "foo@example.com",
		.badmailfrom = "@example.co\0\0",
		.conf = CONFIG_USER
	},
	{
		.mailfrom = "foo@example.com",
		.failmsg = "bad mail from",
		.badmailfrom = ".com\0\0",
		.goodmailfrom = "@test.example.com\0\0",
		.conf = CONFIG_USER
	},
	{
		.mailfrom = NULL,
		.badmailfrom = ".com\0\0",
		.goodmailfrom = "@example.com\0\0",
		.conf = CONFIG_USER
	},
	{
		.mailfrom = "foo@example.com",
		.namebl = "foo.example.net\0bar.example.net\0\0",
		.conf = CONFIG_USER
	},
	{
		.testname = "X-Mas tree: (nearly) everything on, but should still pass",
		.mailfrom = "foo@example.com",
		.conf = CONFIG_USER,
		.userconf = "whitelistauth\0forcestarttls=0\0nobounce\0noapos\0check_strict_rfc2822\0"
				"fromdomain=7\0reject_ipv6only\0helovalid\0smtp_space_bug=0\0block_SoberG\0"
				"spfpolicy=1\0fail_hard_on_temp\0usersize=100000\0block_wildcardns\0\0"
	},
	{
		.testname = "catched by nobounce filter",
		.userconf = "nobounce\0\0",
		.netmsg = "550 5.7.1 address does not send mail, there can't be any bounces\r\n",
		.logmsg = "rejected message to <postmaster> from IP [::ffff:192.168.8.9] {no bounces allowed}",
		.conf = CONFIG_USER,
	},
	{
		.testname = "mail too big",
		.userconf = "usersize=1024\0\0",
		.failmsg = "message too big",
		.netmsg = "552 5.2.3 Requested mail action aborted: exceeded storage allocation\r\n",
		.conf = CONFIG_USER,
		.esmtp = 1
	},
	{
		.testname = "reject because of SMTP space bug",
		.userconf = "smtp_space_bug=1\0\0",
		.netmsg = "500 5.5.2 command syntax error\r\n",
		.logmsg = "rejected message to <postmaster> from <> from IP [::ffff:192.168.8.9] {SMTP space bug}",
		.conf = CONFIG_USER
	},
	{
		.testname = "passed because of SMTP space bug in ESMTP mode",
		.userconf = "smtp_space_bug=1\0\0",
		.conf = CONFIG_USER,
		.esmtp = 1
	},
	{
		.testname = "rejected because of SMTP space bug in ESMTP mode, but authentication is required",
		.mailfrom = "ba'al@example.org",
		.userconf = "smtp_space_bug=2\0\0",
		.netmsg = "500 5.5.2 command syntax error\r\n",
		.logmsg = "rejected message to <postmaster> from <ba'al@example.org> from IP [::ffff:192.168.8.9] {SMTP space bug}",
		.conf = CONFIG_USER,
		.esmtp = 1
	},
	{
		.testname = "rejected because no STARTTLS mode is used",
		.userconf = "forcestarttls\0\0",
		.failmsg = "TLS required",
		.netmsg = "501 5.7.1 recipient requires encrypted message transmission\r\n",
		.conf = CONFIG_USER,
		.esmtp = 1
	},
	{
		.testname = "apostroph rejected",
		.mailfrom = "ba'al@example.org",
		.failmsg = "apostroph in from",
		.userconf = "noapos\0\0",
		.conf = CONFIG_USER,
		.esmtp = 1
	},
	{
		.testname = "dnsbl present, but too long",
		.mailfrom = "foo@example.com",
		.dnsbl = "this.dnsbl.is.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.example.net\0\0",
		.logmsg = "!name of rbl too long: \"this.dnsbl.is.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.far.too.long.example.net\"",
		.conf = CONFIG_USER
	},
	{
		.testname = "dnsbl match",
		.mailfrom = "foo@example.com",
		.dnsbl = "dnsblmatch.example.net\0\0",
		.logmsg = "rejected message to <postmaster> from <foo@example.com> from IP [::ffff:192.168.8.9] {listed in dnsblmatch.example.net from domain dnsbl}",
		.netmsg = "501 5.7.1 message rejected, you are listed in dnsblmatch.example.net\r\n",
		.conf = CONFIG_DOMAIN
	},
	{
		.testname = "dnsbl match with message",
		.mailfrom = "foo@example.com",
		.dnsbl = "dnsblmatch.example.net\0\0",
		.dnstxt_reply = "TEST !REJECT!",
		.logmsg = "rejected message to <postmaster> from <foo@example.com> from IP [::ffff:192.168.8.9] {listed in dnsblmatch.example.net from domain dnsbl}",
		.netmsg = "501 5.7.1 message rejected, you are listed in dnsblmatch.example.net, message: TEST !REJECT!\r\n",
		.conf = CONFIG_DOMAIN
	},
	{
		.testname = "dnsbl+whitelist match",
		.mailfrom = "foo@example.com",
		.dnsbl = "dnsblmatch.example.net\0\0",
		.dnswl = "dnsblmatch.example.net\0\0",
		.logmsg = "not rejected message to <postmaster> from <foo@example.com> from IP [::ffff:192.168.8.9] {listed in dnsblmatch.example.net from domain dnsbl, but whitelisted by dnsblmatch.example.net from domain whitelist}",
		.conf = CONFIG_DOMAIN
	},
	{
		.testname = "dnsbl match + whitelist no match",
		.mailfrom = "foo@example.com",
		.dnsbl = "dnsblmatch.example.net\0\0",
		.dnswl = "foo.example.net\0\0",
		.logmsg = "rejected message to <postmaster> from <foo@example.com> from IP [::ffff:192.168.8.9] {listed in dnsblmatch.example.net from domain dnsbl}",
		.netmsg = "501 5.7.1 message rejected, you are listed in dnsblmatch.example.net\r\n",
		.conf = CONFIG_DOMAIN
	},
	{
		.testname = "dnsbl DNS error",
		.mailfrom = "foo@example.com",
		.dnsbl = "dnsblerror.example.net\0\0",
		.failmsg = "temporary DNS error on RBL lookup",
		.netmsg = "401 5.7.1 message rejected, you are listed in dnsblmatch.example.net\r\n",
		.conf = CONFIG_DOMAIN
	},
	{
		.testname = "dnsbl match + dnswl DNS error",
		.mailfrom = "foo@example.com",
		.dnsbl = "dnsblmatch.example.net\0\0",
		.dnswl = "dnsblerror.example.net\0\0",
		.failmsg = "temporary DNS error on RBL lookup",
		.netmsg = "401 5.7.1 message rejected, you are listed in dnsblmatch.example.net\r\n",
		.conf = CONFIG_DOMAIN
	},
};

int
dnstxt(char **a, const char *b)
{
	if (testdata[testindex].dnstxt_reply == NULL) {
		errno = ENOENT;
		return -1;
	}

	assert(strcmp(b, "9.8.168.192.dnsblmatch.example.net") == 0);

	*a = strdup(testdata[testindex].dnstxt_reply);
	if (*a == NULL)
		exit(ENOMEM);

	return 0;
}

static char **
map_from_list(const char *values)
{
	unsigned int i;
	const char *c = values;
	char **res;

	for (i = 0; *c != '\0'; i++)
		c += strlen(c) + 1;

	res = calloc(i + 1, sizeof(*res));
	if (res == NULL)
		exit(ENOMEM);

	c = values;
	for (i = 0; *c != '\0'; i++) {
		res[i] = (char *)c;
		c += strlen(c) + 1;
	}

	return res;
}

int
userconf_get_buffer(const struct userconf *uc __attribute__ ((unused)), const char *key,
		char ***values, checkfunc cf, const unsigned int flags)
{
	const char *res = NULL;
	checkfunc expected_cf;
	unsigned int expected_flags = userconf_global;

	if (strcmp(key, "goodmailfrom") == 0) {
		res = testdata[testindex].goodmailfrom;
		expected_cf = checkaddr;
	} else if (strcmp(key, "badmailfrom") == 0) {
		res = testdata[testindex].badmailfrom;
		expected_cf = NULL;
		expected_flags = userconf_global | userconf_inherit;
	} else if (strcmp(key, "namebl") == 0) {
		res = testdata[testindex].namebl;
		expected_cf = domainvalid_or_inherit;
		expected_flags = userconf_global | userconf_inherit;
	} else if (strcmp(key, "dnsbl") == 0) {
		res = testdata[testindex].dnsbl;
		expected_cf = domainvalid_or_inherit;
		expected_flags = userconf_global | userconf_inherit;
	} else if (strcmp(key, "whitednsbl") == 0) {
		res = testdata[testindex].dnswl;
		expected_cf = domainvalid;
		expected_flags = userconf_none;
	} else {
		*values = NULL;
		return CONFIG_NONE;
	}

	if (flags != expected_flags) {
		fprintf(stderr, "%s() was called with flags %i instead of %i for key %s\n",
				__func__, flags, expected_flags, key);
		exit(1);
	}

	if (cf != expected_cf) {
		fprintf(stderr, "%s() was called with cf %p instead of %p\n",
				__func__, cf, expected_cf);
		exit(1);
	}

	if (res == NULL) {
		*values = NULL;
		return CONFIG_NONE;
	}

	*values = map_from_list(res);

	assert((testdata[testindex].conf >= CONFIG_USER) && (testdata[testindex].conf <= CONFIG_GLOBAL));
	return testdata[testindex].conf;
}

int
userconf_find_domain(const struct userconf *ds __attribute__ ((unused)), const char *key __attribute__ ((unused)),
		const char *domain __attribute__ ((unused)), const unsigned int flags __attribute__ ((unused)))
{
	return 0;
}

static struct in6_addr frommxip;
static struct ips frommx = {
	.addr = &frommxip,
	.priority = 42,
	.count = 1
};

static void
default_session_config(void)
{
	xmitstat.ipv4conn = 1; /* yes */
	xmitstat.check2822 = 2; /* no decision yet */
	xmitstat.helostatus = 1; /* HELO is my name */
	xmitstat.spf = SPF_NONE;
	xmitstat.fromdomain = DNS_ERROR_PERM; /* permanent error */
	xmitstat.spacebug = 1; /* yes */
	xmitstat.mailfrom.s = "user@invalid";
	xmitstat.mailfrom.len = strlen(xmitstat.mailfrom.s);
	xmitstat.helostr.s = "my.host.example.org";
	xmitstat.helostr.len = strlen(xmitstat.helostr.s);
	xmitstat.thisbytes = 5000;
	strncpy(xmitstat.remoteip, "::ffff:192.168.8.9", sizeof(xmitstat.remoteip) - 1);
	// a.root-servers.net, I'm sure it will stay
	int r = inet_pton(AF_INET6, "::ffff:198.41.0.4", frommx.addr);
	assert(r == 1);
	xmitstat.frommx = &frommx;

	TAILQ_INIT(&head);
}

int
main(void)
{
	struct recip dummyrecip;
	struct recip firstrecip;
	char confpath[PATH_MAX];

	struct userconf uc = {
		.domainpath = STREMPTY_INIT,
		.userdirfd = -1,
		.domaindirfd = -1
	};
	globalconf = NULL;
	memset(&xmitstat, 0, sizeof(xmitstat));

	controldir_fd = AT_FDCWD;

	TAILQ_INIT(&head);

	thisrecip = &dummyrecip;
	dummyrecip.to.s = "postmaster";
	dummyrecip.to.len = strlen(dummyrecip.to.s);
	dummyrecip.ok = 0;
	TAILQ_INSERT_TAIL(&head, &dummyrecip, entries);

	xmitstat.spf = SPF_IGNORE;

	for (int i = 0; rcpt_cbs[i] != NULL; i++) {
		const char *errmsg;
		enum config_domain bt = CONFIG_NONE;
		int r = rcpt_cbs[i](&uc, &errmsg, &bt);

		if (r != 0) {
			fprintf(stderr, "filter %i returned %i\n", i, r);
			err++;
		}
	}

	/* Now change some global state to get better coverage. But the
	 * result may not change, the mail may still not be blocked. */
	default_session_config();
	xmitstat.esmtp = 1; /* yes */

	thisrecip = &dummyrecip;
	firstrecip.to.s = "baz@example.com";
	firstrecip.to.len = strlen(firstrecip.to.s);
	firstrecip.ok = 0;
	TAILQ_INSERT_TAIL(&head, &firstrecip, entries);
	TAILQ_INSERT_TAIL(&head, &dummyrecip, entries);

	for (int i = 0; rcpt_cbs[i] != NULL; i++) {
		const char *errmsg;
		enum config_domain bt = CONFIG_NONE;
		int r = rcpt_cbs[i](&uc, &errmsg, &bt);

		if (r != 0) {
			fprintf(stderr, "filter %i returned %i\n", i, r);
			err++;
		}
	}

	strncpy(confpath, "0/", sizeof(confpath));

	testcase_setup_log_writen(testcase_log_writen_combine);
	testcase_setup_log_write(testcase_log_write_compare);
	testcase_setup_netnwrite(testcase_netnwrite_compare);
	testcase_setup_net_writen(testcase_net_writen_combine);
	testcase_setup_ask_dnsa(test_ask_dnsa);

	while (testindex < sizeof(testdata) / sizeof(testdata[0])) {
		char userpath[PATH_MAX];
		int j;
		int r = 0;			/* filter result */
		const char *fmsg = NULL;	/* returned failure message */
		int expected_r = 0;		/* expected filter result */

		/* set default configuration */
		default_session_config();

		thisrecip = &dummyrecip;
		firstrecip.to.s = "baz@example.com";
		firstrecip.to.len = strlen(firstrecip.to.s);
		firstrecip.ok = 0;
		TAILQ_INSERT_TAIL(&head, &firstrecip, entries);
		TAILQ_INSERT_TAIL(&head, &dummyrecip, entries);

		xmitstat.mailfrom.s = (char *)testdata[testindex].mailfrom;
		xmitstat.mailfrom.len = (xmitstat.mailfrom.s == NULL) ? 0 : strlen(xmitstat.mailfrom.s);
		xmitstat.esmtp = testdata[testindex].esmtp;
		const char *failmsg = testdata[testindex].failmsg;	/* expected failure message */
		netnwrite_msg = testdata[testindex].netmsg;
		log_write_msg = testdata[testindex].logmsg;
		if (log_write_msg && (*log_write_msg == '!')) {
			log_write_priority = LOG_ERR;
			log_write_msg = log_write_msg + 1;
		} else {
			log_write_priority = LOG_INFO;
		}

		if (testdata[testindex].netmsg != NULL) {
			if (*testdata[testindex].netmsg == '5')
				expected_r = 1;
			else if (*testdata[testindex].netmsg == '4')
				expected_r = 4;
			else
				fprintf(stderr, "unexpected net message, does not start with 4 or 5: %s\n",
						testdata[testindex].netmsg);
		} else if (testdata[testindex].failmsg != NULL) {
			expected_r = 2;
		}

		if (inet_pton(AF_INET6, xmitstat.remoteip, &xmitstat.sremoteip) <= 0) {
			fprintf(stderr, "configuration %u: bad ip address given: %s\n",
					testindex, xmitstat.remoteip);
			return 1;
		}
		xmitstat.ipv4conn = IN6_IS_ADDR_V4MAPPED(&xmitstat.sremoteip) ? 1 : 0;

		snprintf(userpath, sizeof(userpath), "%u/user/", testindex);
		uc.userdirfd = get_dirfd(AT_FDCWD, userpath);

		if (testdata[testindex].userconf == NULL)
			uc.userconf = NULL;
		else
			uc.userconf = map_from_list(testdata[testindex].userconf);

		snprintf(confpath, sizeof(confpath), "%u/domain/", testindex);
		uc.domaindirfd = get_dirfd(AT_FDCWD, confpath);
		if (uc.domaindirfd < 0) {
			uc.domainpath.s = NULL;
			uc.domainpath.len = 0;
		} else {
			uc.domainpath.s = confpath;
			uc.domainpath.len = strlen(uc.domainpath.s);
		}

		if (testdata[testindex].testname)
			printf("%2u: %s\n", testindex, testdata[testindex].testname);
		else
			printf("%2u: testing %s configuration\n", testindex,
					blocktype[testdata[testindex].conf]);

		for (j = 0; (rcpt_cbs[j] != NULL) && (r == 0); j++) {
			enum config_domain bt = CONFIG_NONE;
			fmsg = NULL;
			r = rcpt_cbs[j](&uc, &fmsg, &bt);
		}

		if (r != expected_r) {
			fprintf(stderr, "configuration %u: filter %i returned %i instead of %i, message %s (should be %s)\n",
					testindex, j, r, expected_r, fmsg, failmsg);
			err++;
		} else if (failmsg != NULL) {
			if (fmsg == NULL) {
				fprintf(stderr, "configuration %u: filter %i matched with code %i, but the expected message '%s' was not set\n",
						testindex, j, r, failmsg);
				err++;
			} else if (strcmp(fmsg, failmsg) != 0) {
				fprintf(stderr, "configuration %u: filter %i matched with code %i, but the expected message '%s' was not set, but '%s'\n",
						testindex, j, r, failmsg, fmsg);
				err++;
			}
		} else if (fmsg != NULL) {
			fprintf(stderr, "configuration %u: filter %i matched with code %i, but unexpected message '%s' was set\n",
					testindex, j, r, fmsg);
			err++;
		}

		if (log_write_msg != NULL) {
			fprintf(stderr, "configuration %u: expected a log messages, got none\n",
					testindex);
			err++;
		}

		testindex++;
		snprintf(confpath, sizeof(confpath), "%u/", testindex);
		free(uc.userconf);
		if (uc.userdirfd >= 0)
			close(uc.userdirfd);
		if (uc.domaindirfd >= 0)
			close(uc.domaindirfd);
	}

	return err;
}
