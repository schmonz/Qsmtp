/** \file getfile.c
 \brief functions to get information from filterconf files
 */

#include <qsmtpd/userfilters.h>

#include <control.h>
#include <diropen.h>
#include <qsmtpd/userconf.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/**
 * @brief open a file in the given directory
 * @param dirname the path of the directory
 * @param dirlen strlen(dirname)
 * @param fn the name of the file
 * @return the file descriptor of the opened file
 * @retval -1 no file descriptor could be opened (errno is set)
 *
 * dirname has to end in a '/'.
 */
static int
open_in_dir(const char *dirname, const char *fn)
{
	int dfd, fd;

	/* FIXME: change this to -1 to enforce absolute path */
	dfd = get_dirfd(AT_FDCWD, dirname);

	if (dfd < 0)
		return dfd;

	fd = openat(dfd, fn, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fd = errno;
		close(dfd);
		errno = fd;
		return -1;
	} else {
		close(dfd);
		return fd;
	}
}

/**
 * check in user and domain directory if a file with given filename exists
 *
 * @param ds strings of user and domain directory
 * @param fn filename to search
 * @param type if user, domain or global directory matched, undefined if result != 1
 * @param useglobal if a global configuration lookup should be performed
 * @return file descriptor of opened file
 * @retval -1 on error (errno is set)
 */
int
getfile(const struct userconf *ds, const char *fn, enum config_domain *type, int useglobal)
{
	int fd;

	if (ds->userdirfd >= 0) {
		*type = CONFIG_USER;

		fd = openat(ds->userdirfd, fn, O_RDONLY | O_CLOEXEC);

		if ((fd >= 0) || (errno != ENOENT))
			return fd;
	}

	if (ds->domainpath.len > 0) {
		*type = CONFIG_DOMAIN;

		fd = open_in_dir(ds->domainpath.s, fn);

		if (!useglobal || (fd != -1) || (errno != ENOENT))
			return fd;
	} else if (!useglobal) {
		errno = ENOENT;
		return -1;
	}

	/* neither user nor domain specified how to handle this feature
	 * now look up the global setting */
	*type = CONFIG_GLOBAL;
	return openat(controldir_fd, fn, O_RDONLY | O_CLOEXEC);
}

/**
 * search a value in a given list of config values
 *
 * @param config list of settings, last entry has to be NULL, list may be NULL
 * @param flag the value to find
 * @param l strlen(flag)
 * @return the value assotiated with flag
 * @retval 0 no match
 * @retval -1 syntax error
 */
static long
checkconfig(const char * const *config, const char *flag, const size_t l)
{
	int i = 0;

	errno = 0;
	if (!config || !*config)
		return 0;
	while (config[i]) {
		if (!strncmp(config[i], flag, l)) {
			if (!config[i][l]) {
				/* only the name of the value is given: implicitely set to 1 */
				return 1;
			} else {
				if (config[i][l] == '=') {
					char *s;
					long r;

					r = strtol(config[i] + l + 1, &s, 10);
					if (*s) {
						errno = EINVAL;
						return -1;
					}
					return r;
				}
			}
		}
		i++;
	}
	return 0;
}

static long
getsetting_internal(const struct userconf *ds, const char *flag, enum config_domain *type, const int useglobal)
{
	size_t l = strlen(flag);
	long r;

	*type = CONFIG_USER;
	r = checkconfig((const char **)ds->userconf, flag, l);
	if (r > 0) {
		return r;
	} else if (r < 0) {
		/* if user sets this to a value <0 this means "0 and don't override with domain setting" */
		if (errno)
			return r;
		return 0;
	}
	*type = CONFIG_DOMAIN;
	r = checkconfig((const char **)ds->domainconf, flag, l);
	if (r > 0) {
		return r;
	} else if (r < 0) {
		/* forced 0 from domain config */
		if (errno)
			return r;
		return 0;
	}
	if (!useglobal)
		return 0;

	*type = CONFIG_GLOBAL;
	r = checkconfig(globalconf, flag, l);
	if ((r < 0) && !errno)
		return 0;
	return r;
}

/**
 * get setting from user or domain filterconf file
 *
 * @param ds struct with the user/domain config info
 * @param flag name of the setting to find (case sensitive)
 * @param type if user or domain directory matched, undefined if result != 1)
 * @return value of setting
 * @retval 1 boolean setting or no number given
 * @retval 0 setting not found
 * @retval -1 on syntax error
 */
long
getsetting(const struct userconf *ds, const char *flag, enum config_domain *type)
{
	return getsetting_internal(ds, flag, type, 0);
}

/**
 * use getsetting and fall back to /var/qmail/control if this finds nothing
 *
 * @param ds struct with the user/domain config info
 * @param flag name of the setting to find (case sensitive)
 * @param type if user, domain or global file matched, undefined if result != 1
 * @return value of setting
 * @retval 1 boolean setting or no number given
 * @retval 0 setting not found
 * @retval -1 on syntax error
 */
long
getsettingglobal(const struct userconf *ds, const char *flag, enum config_domain *type)
{
	return getsetting_internal(ds, flag, type, 1);
}
