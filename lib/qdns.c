/** \file qdns.c
 \brief DNS query functions
 */

#include <qdns.h>

#include <libowfatconn.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * \brief get info out of the DNS
 *
 * \param name the name to look up
 * \param result first element of a list of results will be placed
 * \retval  0 on success
 * \retval  1 if host is not existent
 * \retval  2 if temporary DNS error
 * \retval  3 if permanent DNS error
 * \retval -1 on error
 */
int
ask_dnsmx(const char *name, struct ips **result)
{
	int i;
	char *r;
	size_t l;
	char *s;
	struct ips **q = result;
	int errtype = 0;

	i = dnsmx(&r, &l, name);

	if ((i != 0) && (errno != ENOENT)) {
		switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:
			return DNS_ERROR_TEMP;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:
			errno = ENOMEM;
			/* fallthrough */
		case ENOMEM:
			return DNS_ERROR_LOCAL;
		case ENOENT:
			return 1;
		default:
			return DNS_ERROR_PERM;
		}
	}

	s = r;

	/* there is no MX record, so we look for an AAAA record */
	if (!l) {
		struct in6_addr *a;
		/* the DNS priority is 2 bytes long so 65536 can
		 * never be returned from a real DNS_MX lookup */
		int rc = ask_dnsaaaa(name, &a);

		if (rc < 0)
			return rc;
		else if (rc == 0)
			return 1;

		*result = in6_to_ips(a, rc, 65536);
		if (*result == NULL)
			return DNS_ERROR_LOCAL;

		return 0;
	}

	while (r + l > s) {
		struct in6_addr *a;
		int rc;

		rc = ask_dnsaaaa(s + 2, &a);
		if (rc == DNS_ERROR_LOCAL) {
			freeips(*result);
			free(r);
			if (errno == ENOMEM)
				return DNS_ERROR_LOCAL;
			return DNS_ERROR_TEMP;
		} else if (rc > 0) {
			struct ips *u = in6_to_ips(a, rc, ntohs(*((uint16_t *) s)));
			struct ips *p;

			/* add the new results to the list */
			if (u == NULL) {
				freeips(*result);
				free(r);
				return DNS_ERROR_LOCAL;
			}

			for (p = u; p != NULL; p = p->next) {
				p->name = strdup(name);
				if (p->name == NULL) {
					freeips(*result);
					freeips(u);
					free(r);
					return DNS_ERROR_LOCAL;
				}
			}

			p = *q;
			while ((p != NULL) && (p->next != NULL))
				p = p->next;

			if (p == NULL)
				*q = u;
			else
				p->next = u;
		} else if (rc != 0) {
			errtype = (1 << -rc);
		}
		s += 3 + strlen(s + 2);
	}

	free(r);

	if (*result)
		return 0;
	else if (errtype & 4)
		return DNS_ERROR_TEMP;
	else if (errtype & 2)
		return 1;
	else
		return DNS_ERROR_PERM;
}

/**
 * \brief get AAAA record from of the DNS
 *
 * \param name the name to look up
 * \param result first element of a list of results will be placed
 * \retval  0 no entries found
 * \retval >0 how many entries were returned in result
 * \retval -2 if temporary DNS error
 * \retval -3 if permanent DNS error
 * \retval -1 on error
 */
int
ask_dnsaaaa(const char *name, struct in6_addr **result)
{
	int i;
	char *r;
	size_t l;
	char *s;
	unsigned int cnt = 0;

	*result = NULL;

	i = dnsip6(&r, &l, name);
	if (i < 0) {
		free(r);
		switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:
			return DNS_ERROR_TEMP;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:
			errno = ENOMEM;
			/* fallthrough */
		case ENOMEM:
			return DNS_ERROR_LOCAL;
		case ENOENT:
			return 0;
		default:
			return DNS_ERROR_PERM;
		}
	}

	s = r;

	if (!l)
		return 0;

	*result = malloc(((l + 15) / 16) * sizeof(**result));
	if (*result == NULL) {
		free(r);
		return DNS_ERROR_LOCAL;
	}

	for (cnt = 0; r + l > s; s += sizeof(**result))
		memcpy(*result + cnt++, s, 16);

	free(r);
	return cnt;
}

/**
 * @brief get A record from of the DNS
 * @param name the name to look up
 * @param result first element of a list of results will be placed, or NULL if only return code is of interest
 * @return if records have been found
 * \retval  0 no entries found
 * \retval >0 how many entries were returned in result
 * @retval -1 on error (errno is set)
 * @retval -2 temporary DNS error
 * @retval -3 permanent DNS error
 */
int
ask_dnsa(const char *name, struct in6_addr **result)
{
	int i;
	char *r;
	size_t l;
	unsigned int idx = 0;

	i = dnsip4(&r, &l, name);
	if (i < 0) {
		switch (errno) {
		case ETIMEDOUT:
		case EAGAIN:
			return DNS_ERROR_TEMP;
		case ENFILE:
		case EMFILE:
		case ENOBUFS:
			errno = ENOMEM;
			/* fallthrough */
		case ENOMEM:
			return DNS_ERROR_LOCAL;
		case ENOENT:
			return 0;
		default:
			return DNS_ERROR_PERM;
		}
	}

	if (l == 0)
		return 0;

	if (result) {
		char *s = r;
		*result = malloc(((l + 3) / 4) * sizeof(**result));
		if (*result == NULL) {
			free(r);
			return DNS_ERROR_LOCAL;
		}
		memset(*result, 0, ((l + 3) / 4) * sizeof(**result));

		while (r + l > s) {
			(*result)[idx].s6_addr32[2] = htonl(0xffff);
			memcpy(&((*result)[idx].s6_addr32[3]), s, 4);

			s += 4;
			idx++;
		}
	}

	free(r);
	return idx;
}

/**
 * \brief get host name for IP address
 *
 * @param ip the IP to look up
 * @param result name will be stored here
 * @return how many names were found, negative on error
 * @retval 0 host not found
 * @retval -1 local error (e.g. ENOMEM)
 * @retval -2 temporary DNS error
 * @retval -3 permanent DNS error
 */
int
ask_dnsname(const struct in6_addr *ip, char **result)
{
	int r;

	r = dnsname(result, ip);

	if (!r)
		return *result ? 1 : 0;

	switch (errno) {
	case ETIMEDOUT:
	case EAGAIN:
		return DNS_ERROR_TEMP;
	case ENFILE:
	case EMFILE:
	case ENOBUFS:
		errno = ENOMEM;
		/* fallthrough */
	case ENOMEM:
		return DNS_ERROR_LOCAL;
	case ENOENT:
		return 0;
	default:
		return DNS_ERROR_PERM;
	}
}
