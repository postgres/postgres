#ifndef STRINGUTILS_H
#define STRINGUTILS_H

/* The cooler version of strtok() which knows about quotes and doesn't
 * overwrite your input */
extern char *
strtokx(const char *s,
	const char *delim,
	const char *quote,
	char escape,
	char * was_quoted,
	unsigned int * token_pos);

#endif	 /* STRINGUTILS_H */
