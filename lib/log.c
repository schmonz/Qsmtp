#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include "log.h"

void
log_writen(int priority, const char **s)
{
	unsigned int j;
	size_t i = 0;
	char *buf;

	for (j = 0; s[j]; j++)
		i += strlen(s[j]);
	buf = malloc(i + 2);
	if (!buf) {
#ifdef USESYSLOG
		syslog(LOG_ERR, "out of memory\n");
#endif
#ifndef NOSTDERR
		write(2, "not enough memory for log message\n", 34);
#endif
		return;
	} else {
		i = 0;
		for (j = 0; s[j]; j++) {
			strcpy(buf + i, s[j]);
			i += strlen(s[j]);
		}
		buf[i++] = '\n';
		buf[i] = '\0';
	}
#ifdef USESYSLOG
	syslog(priority, "%s", buf);
#endif
#ifndef NOSTDERR
	write(2, buf, i);
#endif
	free(buf);
}

inline void
log_write(int priority, const char *s)
{
	const char *t[] = {s, NULL};
	log_writen(priority, t);
}

const char *diemsg;

void __attribute__ ((noreturn))
dieerror(int error)
{
	if (diemsg)
		write(1, diemsg, strlen(diemsg) + 1);

	switch (error) {
		case ETIMEDOUT:	log_write(LOG_WARNING, "connection timed out"); break;
		case ECONNRESET:log_write(LOG_WARNING, "connection died"); break;
	}
	_exit(error);
}
