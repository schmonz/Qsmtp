/** \file qsurvey.c
 \brief main functions of Qsurvey

 This file contains the main functions of Qsurvey, a simple SMTP server survey
 to check for remote SMTP server capabilities and software version.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <fcntl.h>
#include "netio.h"
#include "qdns.h"
#include "control.h"
#include "log.h"
#include "match.h"
#include "sstring.h"
#include <qremote/conn.h>
#include <qremote/starttlsr.h>
#include <qremote/qremote.h>
#include "fmt.h"
#include "qmaildir.h"
#include "tls.h"

int socketd;
string heloname;
unsigned int smtpext;
char *rhost;
size_t rhostlen;
char *partner_fqdn;
static int logfd;
static int logdirfd = -1;
static struct ips *mx;

/**
 * @brief write status message to stdout
 * @param str the string to write
 *
 * This will include the trailing 0-byte in the output as qmail-rspawn awaits
 * that as separator between the output fields.
 */
void
write_status(const char *str)
{
	(void) write(1, str, strlen(str) + 1);
}

static void quitmsg(void);

void
err_mem(const int doquit)
{
	if (doquit)
		quitmsg();
/* write text including 0 byte */
	write_status("Z4.3.0 Out of memory.\n");
	_exit(0);
}

void
err_confn(const char **errmsg, void *freebuf)
{
	log_writen(LOG_ERR, errmsg);
	free(freebuf);
	/* write text including 0 byte */
	write_status("Z4.3.0 Configuration error.\n");
	_exit(0);
}

void
err_conf(const char *errmsg)
{
	const char *msg[] = {errmsg, NULL};
	err_confn(msg, NULL);
}

void
net_conn_shutdown(const enum conn_shutdown_type sd_type)
{
	if ((sd_type == shutdown_clean) && (socketd >= 0)) {
		quitmsg();
	} else if (socketd >= 0) {
		close(socketd);
		socketd = -1;
	}

	freeips(mx);
	free(heloname.s);
	if (logdirfd >= 0)
		close(logdirfd);

	ssl_exit(0);
}

/*
 * private version: ignore all configured smtproutes since this tool will not
 * really deliver any mail.
 */
struct ips *
smtproute(const char *remhost __attribute__((unused)), const size_t reml __attribute__((unused)), unsigned int *port __attribute__((unused)))
{
	errno = 0;
	return NULL;
}

static void
setup(void)
{
	int j;
	unsigned long tmp;

#undef USESYSLOG

	if (chdir(AUTOQMAIL)) {
		err_conf("cannot chdir to qmail directory");
	}

	if ( (j = loadoneliner("control/helohost", &heloname.s, 1) ) < 0 ) {
		if ( ( j = loadoneliner("control/me", &heloname.s, 0) ) < 0 ) {
			err_conf("can open neither control/helohost nor control/me");
		}
		if (domainvalid(heloname.s)) {
			err_conf("control/me contains invalid name");
		}
	} else {
		if (domainvalid(heloname.s)) {
			err_conf("control/helohost contains invalid name");
		}
	}
	if ( (j = loadintfd(open("control/timeoutremote", O_RDONLY), &tmp, 320)) < 0) {
		err_conf("parse error in control/timeoutremote");
	}
	timeout = tmp;

	heloname.len = j;

}

static void
quitmsg(void)
{
	netwrite("QUIT\r\n");
	do {
/* don't care about what he replies: we want to quit, if he don't want us to he must pay money *eg* */
		if (net_read()) {
			log_write(LOG_ERR, "network read error while waiting for QUIT reply");
			break;
		}
	} while ((linelen >= 4) && (linein[3] == '-'));
	close(socketd);
	socketd = -1;
}

void
quit(void)
{
	quitmsg();
	_exit(0);
}

/**
 * print remote host information to buffer
 *
 * @param m list of MX entries, entry with priority 65538 is active
 */
static inline void
getrhost(const struct ips *m)
{
	int r;

	free(partner_fqdn);
	free(rhost);

	/* find active mx */
	while (m->priority != 65538)
		m = m->next;

	r = ask_dnsname(&m->addr, &partner_fqdn);
	if (r <= 0) {
		if ((r == 0) || (errno != ENOMEM)) {
			rhost = malloc(INET6_ADDRSTRLEN + 2);
		}
		if (errno == ENOMEM) {
			err_mem(1);
		}
		rhost[0] = '[';
		rhostlen = 1;
		partner_fqdn = NULL;
	} else {
		rhostlen = strlen(partner_fqdn);
		rhost = malloc(rhostlen + INET6_ADDRSTRLEN + 3);

		if (!rhost) {
			err_mem(1);
		}

		memcpy(rhost, partner_fqdn, rhostlen);
		rhost[rhostlen++] = ' ';
		rhost[rhostlen++] = '[';
	}
	/* there can't be any errors here ;) */
	(void) inet_ntop(AF_INET6, &m->addr, rhost + rhostlen, INET6_ADDRSTRLEN);
	rhostlen = strlen(rhost);
	rhost[rhostlen++] = ']';
	rhost[rhostlen] = '\0';
}

/**
 * get one line from the network, handle all error cases
 *
 * @return SMTP return code of the message
 */
int
netget(void)
{
	int q, r;

	if (net_read()) {
		switch (errno) {
		case ENOMEM:	err_mem(1);
		case EINVAL:
		case E2BIG:	goto syntax;
		default:
			{
				char *tmp = strerror(errno);

				write(1, "Z", 1);
				write_status(tmp);
				net_conn_shutdown(shutdown_clean);
			}
		}
	}
	if (linelen < 3)
		goto syntax;
	if ((linelen > 3) && ((linein[3] != ' ') && (linein[3] != '-')))
		goto syntax;
	r = linein[0] - '0';
	if ((r < 2) || (r > 5))
		goto syntax;
	q = linein[1] - '0';
	if ((q < 0) || (q > 9))
		goto syntax;
	r = r * 10 + q;
	q = linein[2] - '0';
	if ((q < 0) || (q > 9))
		goto syntax;

	if (logfd > 0) {
		write(logfd, linein, linelen);
		write(logfd, "\n" ,1);
	}

	return r * 10 + q;
syntax:
	/* if this fails we're already in bad trouble */
	(void) write_status("Zsyntax error in server reply\n");
	net_conn_shutdown(shutdown_clean);
}

/**
 * check the reply of the server
 *
 * @param status status codes to print or NULL if not to
 * @param pre text to write to stdout before server reply if mask matches
 * @param mask bitmask for pre: 1: 2xx, 2: 4xx, 4: 5xx
 * @return the SMTP result code
 *
 * status must be at least 3 bytes long but only the first 3 will have any effect. The first
 * one is the status code writen on success (server response is 2xx), the second on on temporary
 * error (4xx) and the third on permanent error (5xx). If no status code should be written status
 * must be set to NULL. If the first character in status is ' ' no message will be printed for
 * success messages.
 */
int
checkreply(const char *status, const char **pre, const int mask)
{
	int res;
	int ignore = 0;

	res = netget();
	if (status) {
		int m;

		if ((res >= 211) && (res <= 252)) {
			if (status[0] == ' ') {
				ignore = 1;
			} else {
				write(1, status, 1);
			}
			m = 1;
		} else if ((res >= 421) && (res <= 452)) {
			write(1, status + 1, 1);
			m = 2;
		} else {
			write(1, status + 2, 1);
			m = 4;
		}
		if (!ignore) {
			if (pre && (m & mask)) {
				int i = 0;

				while (pre[i]) {
					write(1, pre[i], strlen(pre[i]));
					i++;
				}
			}
			write(1, linein, linelen);
		}
	}
	while (linein[3] == '-') {
		/* ignore the SMTP code sent here, if it's different from the one before the server is broken */
		(void) netget();
		if (status && !ignore) {
			write(1, linein, linelen);
			write(1, "\n", 1);
		}
	}

	if (status && !ignore)
		write(1, "", 1);
	/* this allows us to check for 2xx with (x < 300) later */
	if (res < 200)
		res = 599;
	return res;
}

static unsigned long remotesize;

static int
cb_size(void)
{
	char *s;

	if (!linein[8])
		return 0;

	remotesize = strtoul(linein + 8, &s, 10);
	return *s;
}

/**
 * greet the server, try ehlo and fall back to helo if needed
 *
 * @return 0 if greeting succeeded, 1 on error
 */
static int
greeting(void)
{
	struct smtpexts {
		const char *name;
		unsigned int len;	/* strlen(name) */
		int (*func)(void);	/* used to handle arguments to this extension, NULL if no arguments allowed */
	} extensions[] = {
		{ .name = "SIZE",	.len = 4,	.func = cb_size	}, /* 0x01 */
		{ .name = "PIPELINING",	.len = 10,	.func = NULL	}, /* 0x02 */
		{ .name = "STARTTLS",	.len = 8,	.func = NULL	}, /* 0x04 */
		{ .name = "8BITMIME",	.len = 8,	.func = NULL	}, /* 0x08 */
		{ .name = "CHUNKING",	.len = 8,	.func = NULL	}, /* 0x10 */
		{ .name = NULL }
	};
	const char *cmd[3];
	int s;			/* SMTP status */

	cmd[0] = "EHLO ";
	cmd[1] = heloname.s;
	cmd[2] = NULL;
	net_writen(cmd);
	do {
		s = netget();
		if (s == 250) {
			int j = 0;

			while (extensions[j].name) {
				if (!strncasecmp(linein + 4, extensions[j].name, extensions[j].len)) {
					if (extensions[j].func) {
						int r;

						r = extensions[j].func();
						if (!r) {
							smtpext |= (1 << j);
							break;
/*						} else if (r < 0) {
							return r;
*/						} else {
							const char *logmsg[4] = {"syntax error in EHLO response \"",
									    extensions[j].name,
									    "\"", NULL};

							log_writen(LOG_WARNING, logmsg);
						}
					} else {
						if (!*(linein + 4 + extensions[j].len)) {
							smtpext |= (1 << j);
							break;
						}
					}
				}
				j++;
			}
		}
	} while (linein[3] == '-');

	if (s != 250) {
/* EHLO failed, try HELO */
		cmd[0] = "HELO ";
		net_writen(cmd);
		do {
			s = netget();
		} while (linein[3] == '-');
		if (s == 250) {
			smtpext = 0;
		} else {
			return 1;
		}
	}
	return 0;
}

void
dieerror(int error)
{
	switch (error) {
	case ETIMEDOUT:
		write_status("Zconnection to remote server died\n");
		log_write(LOG_WARNING, "connection timed out");
		break;
	case ECONNRESET:
		write_status("Zconnection to remote timed out\n");
		log_write(LOG_WARNING, "connection died");
		break;
	}
	_exit(0);
}

static void
makelog(const char *ext)
{
	char fn[30];

	if (logfd)
		close(logfd);
	logfd = openat(logdirfd, ext, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (logfd == -1) {
		if (strcmp(ext, "conn")) {
			write(2, "can not create ", 15);
			write(2, fn, strlen(fn));
			write(2, "\n", 1);
			net_conn_shutdown(shutdown_clean);
		} else {
			net_conn_shutdown(shutdown_abort);
		}
	}
}

/**
 * @brief create a directory tree
 * @param pattern the dot-separated pattern to use
 * @return fd of the last directory created
 *
 * Given a pattern of foo.bar it will create the directories foo/ and foo/bar.
 * It is no error if they already exist. If creation fails the process is
 * terminated.
 */
static int
mkdir_pr(const char *pattern)
{
	char fnbuf[PATH_MAX];
	const char *end;
	const char *start;
	int r;
	int dirfd = dup(logdirfd);

	if (dirfd < 0) {
		fprintf(stderr, "cannot open current directory: %s\n",
			strerror(errno));
		exit(1);
	}

	end = pattern + strlen(pattern);
	start = strrchr(pattern, '.');

	if (start == NULL)
		start = pattern;

	while (start != pattern) {
		const size_t len = end - start - 1;
		int nextdir;
		strncpy(fnbuf, start + 1, end - start - 1);
		fnbuf[len] = '\0';
		r = mkdirat(dirfd, fnbuf, 0755);

		if ((r < 0) && (errno != EEXIST)) {
			fprintf(stderr, "cannot create %s: %s\n",
					fnbuf, strerror(errno));
			exit(1);
		}

		nextdir = openat(dirfd, fnbuf, O_RDONLY);
		if (nextdir < 0) {
			fprintf(stderr, "cannot open %s: %s\n",
					fnbuf, strerror(errno));
			close(dirfd);
			exit(1);
		}
		close(dirfd);
		dirfd = nextdir;

		end = start;
		start--;
		while ((start != pattern) && (*start != '.'))
			start--;
	}

	strncpy(fnbuf, pattern, end - pattern);
	fnbuf[end - pattern] = '\0';

	r = mkdirat(dirfd, fnbuf, 0755);
	if ((r < 0) && (errno != EEXIST)) {
		fprintf(stderr, "cannot create %s: %s\n",
				fnbuf, strerror(errno));
		close(dirfd);
		exit(1);
	}

	r = openat(dirfd, fnbuf, O_RDONLY);
	if (r < 0) {
		fprintf(stderr, "cannot open %s: %s\n",
				fnbuf, strerror(errno));
		close(dirfd);
		exit(1);
	}
	close(dirfd);

	return r;
}

int
main(int argc, char *argv[])
{
	int i;
	struct ips *cur;
	const char *logdir = getenv("QSURVEY_LOGDIR");
	int dirfd;
	char ipname[17]; /* enough for "255/255/255/255/" */
	char iplinkname[PATH_MAX];

	if (argc != 2) {
		write(2, "Usage: Qsurvey hostname\n", 24);
		return EINVAL;
	}

	setup();

	getmxlist(argv[1], &mx);
#ifndef IPV4ONLY
	/* IPv6 addresses are currently not supported, so filter them out.
	 * This is not needed if IPV4ONLY is set, then this has already
	 * been done. */
	for (cur = mx; cur != NULL; cur = cur->next)
		if (!IN6_IS_ADDR_V4MAPPED(&(cur->addr)))
			cur->priority = 65537;
#endif
	sortmx(&mx);

	/* if no IPv4 address is available just exit */
	if (mx->priority > 65536) {
		freeips(mx);
		return 0;
	}

	/* only one IPv4 address is available: just do it in this
	 * process, no need to fork. */
	cur = mx;
	if ((mx->next == NULL) || (mx->next->priority > 65536))
		goto work;

	while (cur) {
		while ((cur != NULL) && (cur->priority > 65536))
			cur = cur->next;

		if (cur == NULL) {
			freeips(mx);
			return 0;
		}

		switch (fork()) {
		case -1:
			i = errno;
			write(2, "unable to fork\n", 15);
			freeips(mx);
			return i;
		case 0:
			break;
		default:
			cur = cur->next;
			continue;
		}

		break;
	}

work:
	if (cur == NULL) {
		freeips(mx);
		return 0;
	}

	if (logdir == NULL)
		logdir = "/tmp/Qsurvey";

	logdirfd = open(logdir, O_RDONLY);

	if (logdirfd < 0) {
		fprintf(stderr, "cannot open log directory %s: %s\n",
				logdir, strerror(errno));
		freeips(mx);
		return 1;
	}

	dirfd = mkdir_pr(argv[1]);

	memset(ipname, 0, sizeof(ipname));
	for (i = 12; i <= 15; i++) {
		ultostr(cur->addr.s6_addr[i], ipname + strlen(ipname));
		if ((mkdirat(logdirfd, ipname, S_IRUSR | S_IWUSR | S_IXUSR) < 0) && (errno != EEXIST)) {
			fprintf(stderr, "cannot create directory %s: %s\n", ipname, strerror(errno));
			close(dirfd);
			net_conn_shutdown(shutdown_abort);
		}
		ipname[strlen(ipname)] = '/';
	}
	i = openat(logdirfd, ipname, O_RDONLY);
	if (i < 0) {
		fprintf(stderr, "cannot open IP directory %s: %s\n",
				ipname, strerror(errno));
		close(logdirfd);
		close(dirfd);
		net_conn_shutdown(shutdown_abort);
	}

	close(logdirfd);
	logdirfd = i;

	ipname[strlen(ipname) - 1] = '\0';
	sprintf(iplinkname, "%s/%s", logdir, ipname);

	if (IN6_IS_ADDR_V4MAPPED(&(cur->addr)))
		inet_ntop(AF_INET, cur->addr.s6_addr32 + 3, ipname, sizeof(ipname));
	else
		inet_ntop(AF_INET6, &(cur->addr), ipname, sizeof(ipname));
	symlinkat(iplinkname, dirfd, ipname);

	makelog("conn");

	tryconn(cur, &in6addr_any, &in6addr_any);
	close(0);
	dup2(socketd, 0);
	if (netget() != 220) {
		freeips(mx);
		net_conn_shutdown(shutdown_clean);
	}

	/* AOL and others */
	while (linein[3] == '-')
		netget();

	makelog("ehlo");

	if (greeting()) {
		freeips(mx);
		net_conn_shutdown(shutdown_clean);
	}

	getrhost(cur);
	freeips(mx);

	if (smtpext & 0x04) {
		makelog("tls-init");
		if (tls_init()) {
			makelog("tls-ehlo");
			if (greeting()) {
				write(2, "EHLO failed after STARTTLS\n", 28);
				net_conn_shutdown(shutdown_clean);
			}
		}
	}

	makelog("vrfy");
	netwrite("VRFY postmaster\r\n");
	do {
		netget();
	} while (linein[3] == '-');
	makelog("noop");
	netwrite("NOOP\r\n");
	do {
		netget();
	} while (linein[3] == '-');
	makelog("rset");
	netwrite("RSET\r\n");
	do {
		netget();
	} while (linein[3] == '-');
	makelog("help");
	netwrite("HELP\r\n");
	do {
		netget();
	} while (linein[3] == '-');
	net_conn_shutdown(shutdown_clean);
}
