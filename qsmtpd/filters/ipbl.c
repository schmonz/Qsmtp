#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include "antispam.h"
#include "userfilters.h"
#include "log.h"
#include "qsmtpd.h"

int
cb_ipbl(const struct userconf *ds, char **logmsg, int *t)
{
	int i;			/* counter of the array position */
	int rc;			/* return code */
	int fd;			/* file descriptor of the policy file */
	const char *fnb;	/* filename of the blacklist file */
	const char *fnw;	/* filename of the whitelist file */

	if (xmitstat.ipv4conn) {
		fnb = "ipbl";
		fnw = "ipwl";
	} else {
		fnb = "ipblv6";
		fnw = "ipwlv6";
	}

	if ( (fd = getfileglobal(ds, fnb, t)) < 0)
		return (errno == ENOENT) ? 0 : fd;

	i = lookupipbl(fd);
	if (errno == ENOLCK) {
		return 0;
	}
	if (i > 0) {
		int u;

		if ( (fd = getfileglobal(ds, fnw, &u)) < 0) {
			if (errno != ENOENT)
				return fd;
			i = 0;
		} else {
			i = lookupipbl(fd);
		}
		if (i > 0) {
			logwhitelisted("ipbl", *t, u);
			rc = 0;
		} else if (!i) {
			*logmsg = "ipbl";
			rc = 2;
		} else {
			const char *logmess[] = {"bad input data in ", blocktype[u],
						"ipwl file for address <", THISRCPT, ">", NULL};
			log_writen(LOG_ERR, logmess);
			rc = 0;
		}
	} else {
		if (i) {
			const char *logmess[] = {"bad input data in ", blocktype[*t],
						"ipbl file for address <", THISRCPT, ">", NULL};

			log_writen(LOG_ERR, logmess);
		}
		rc = 0;
	}
	return rc;
}
