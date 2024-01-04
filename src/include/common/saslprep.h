/*-------------------------------------------------------------------------
 *
 * saslprep.h
 *	  SASLprep normalization, for SCRAM authentication
 *
 * These definitions are used by both frontend and backend code.
 *
 * Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * src/include/common/saslprep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SASLPREP_H
#define SASLPREP_H

/*
 * Return codes for pg_saslprep() function.
 */
typedef enum
{
	SASLPREP_SUCCESS = 0,
	SASLPREP_OOM = -1,			/* out of memory (only in frontend) */
	SASLPREP_INVALID_UTF8 = -2, /* input is not a valid UTF-8 string */
	SASLPREP_PROHIBITED = -3,	/* output would contain prohibited characters */
} pg_saslprep_rc;

extern pg_saslprep_rc pg_saslprep(const char *input, char **output);

#endif							/* SASLPREP_H */
