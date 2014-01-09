/** \file sstring.h
 \brief definition of string record and headers of corresponding helper functions
 */
#ifndef SSTRING_H
#define SSTRING_H

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>

/** \struct string
 \brief record of a string
 */
typedef struct string {
	char *s;	/**< string buffer */
	size_t len;	/**< length of string */
} string;

/** \struct cstring
 \brief record of a constant string
 */
typedef struct cstring {
	const char *s;	/**< string buffer */
	size_t len;	/**< length of string */
} cstring;

#define STREMPTY(x) { (x).s = NULL; (x).len = 0; }

static inline int newstr(string *, const size_t) __attribute__ ((nonnull (1)));

/**
 * @brief allocate a new string buffer of the given length
 * @param s the string container
 * @param len the desired length of the embedded buffer
 * @return if the allocation was successful
 * @retval 0 buffer was allocated
 * @retval -1 out of memory
 *
 * If len is 0 s->s is set to NULL and 0 is returned.
 */
static inline int
newstr(string *s, const size_t len)
{
	if (len == 0) {
		STREMPTY(*s);
		return 0;
	}

	s->len = len;
	s->s = malloc(len);
	return (len && !s->s) ? -1 : 0;
}

#endif
