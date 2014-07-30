/** \file userfilters.h
 \brief common function definitions for all user filters
 */
#ifndef USERFILTERS_H
#define USERFILTERS_H

#include "qsmtpd.h"
#include <sstring.h>

#include <sys/queue.h>
#include <sys/types.h>

struct userconf;

/** @enum config_domain
 * @brief describe where the domain a read config value is originating from
 */
enum config_domain {
	CONFIG_NONE = 0,		/**< no entry was returned */
	CONFIG_USER = 1,		/**< the config entry was found in the user specific configuration */
	CONFIG_DOMAIN = 2,		/**< the config entry was found in the domain specific configuration */
	CONFIG_GLOBAL = 4		/**< the config entry was found in the global configuration */
};

extern const char **globalconf;

extern int getfile(const struct userconf *, const char *, enum config_domain *, int);
extern long getsetting(const struct userconf *, const char *, enum config_domain  *);
extern long getsettingglobal(const struct userconf *, const char *, enum config_domain *);

/** @enum filter_result
 * @brief describes the result of a policy filter
 */
enum filter_result {
	FILTER_ERROR = -1,		/**< error during processing, errno is set */
	FILTER_PASSED = 0,		/**< filter passed */
	FILTER_DENIED_WITH_MESSAGE = 1,	/**< filter denied, network message has already been sent */
	FILTER_DENIED_UNSPECIFIC = 2,	/**< filter denied, caller should announce general policy error */
	FILTER_DENIED_NOUSER = 3,	/**< filter denied, caller should announce user does not exist */
	FILTER_DENIED_TEMPORARY = 4,	/**< filter denied temporarily, a transient error message should be given by the caller */
	FILTER_WHITELISTED = 5		/**< filter passed, the mail is whitelisted and should not be rejected by any other filter */
};

/**
 * @brief check if the given filter code was a denied code
 * @param r the code to check
 */
static inline int
filter_denied(const enum filter_result r)
{
	return ((r > FILTER_PASSED) && (r != FILTER_WHITELISTED));
}

/** \var rcpt_cb
 * \brief this is a function for a user filter
 *
 * \param ds:     the struct with the paths of domain- and userpath
 * \param logmsg: store here a reference to the message to write into logfile or NULL if you logged yourself
 * \param type:   which policy matched (user, domain, global)
 * @return filter result codes as in enum filter_result
 */
typedef enum filter_result (*rcpt_cb)(const struct userconf *ds, const char **logmsg, enum config_domain *t);

extern rcpt_cb rcpt_cbs[];
extern rcpt_cb late_rcpt_cbs[];

extern const char *blocktype[];

extern void logwhitelisted(const char *, const int, const int);

#define THISRCPT (thisrecip->to.s)

#if 0
TAILQ_HEAD(pftailhead, pfixpol) pfixhead;

/*! \struct pfixpol
 Describes the settings for one Postfix policy daemon
 */
struct pfixpol {
	TAILQ_ENTRY(pfixpol) entries;	/**< List pointers of policy daemons. */
	char	*name;			/**< name for this filter (to be used in log and userconf) */
	pid_t	pid;			/**< pid of daemon or 0 if not running */
	int	fd;			/**< pipe for communication */
};

#define PFIXPOLDIR	"/var/qmail/control/postfixpol"
#define PFIXSPOOLDIR	"/var/spool/Qsmtp"
#endif

#endif
