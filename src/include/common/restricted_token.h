/*
 *	restricted_token.h
 *		helper routine to ensure restricted token on Windows
 *
 *	Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/include/common/restricted_token.h
 */
#ifndef COMMON_RESTRICTED_TOKEN_H
#define COMMON_RESTRICTED_TOKEN_H

/*
 * On Windows make sure that we are running with a restricted token,
 * On other platforms do nothing.
 */
void		get_restricted_token(void);

#ifdef WIN32
/* Create a restricted token and execute the specified process with it. */
HANDLE		CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo);
#endif

#endif							/* COMMON_RESTRICTED_TOKEN_H */
