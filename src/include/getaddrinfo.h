/*-------------------------------------------------------------------------
 *
 * getaddrinfo.h
 *	  Support getaddrinfo() on platforms that don't have it.
 *
 * Note: we use our own routines on platforms that don't HAVE_STRUCT_ADDRINFO,
 * whether or not the library routine getaddrinfo() can be found.  This
 * policy is needed because on some platforms a manually installed libbind.a
 * may provide getaddrinfo(), yet the system headers may not provide the
 * struct definitions needed to call it.  To avoid conflict with the libbind
 * definition in such cases, we rename our routines to pg_xxx() via macros.
 *
 * This code will also work on platforms where struct addrinfo is defined
 * in the system headers but no getaddrinfo() can be located.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * $Id: getaddrinfo.h,v 1.11 2003/08/14 18:32:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GETADDRINFO_H
#define GETADDRINFO_H

#if !defined(_MSC_VER) && !defined(__BORLANDC__)
#include <sys/socket.h>
#include <netdb.h>
#endif


/* Various macros that ought to be in <netdb.h>, but might not be */

#ifndef EAI_FAIL
#define EAI_BADFLAGS	-1
#define EAI_NONAME		-2
#define EAI_AGAIN		-3
#define EAI_FAIL		-4
#define EAI_FAMILY		-6
#define EAI_SOCKTYPE	-7
#define EAI_SERVICE		-8
#define EAI_MEMORY		-10
#define EAI_SYSTEM		-11
#endif

#ifndef AI_PASSIVE
#define AI_PASSIVE		0x0001
#endif

#ifndef AI_NUMERICHOST
/*
 * some platforms don't support AI_NUMERICHOST; define as zero if using
 * the system version of getaddrinfo...
 */
#if defined(HAVE_STRUCT_ADDRINFO) && defined(HAVE_GETADDRINFO)
#define AI_NUMERICHOST	0
#else
#define AI_NUMERICHOST	0x0004
#endif
#endif

#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST	1
#endif
#ifndef NI_NUMERICSERV
#define NI_NUMERICSERV	2
#endif

#ifndef NI_MAXHOST
#define NI_MAXHOST	1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV	32
#endif


#ifndef HAVE_STRUCT_ADDRINFO

struct addrinfo
{
	int			ai_flags;
	int			ai_family;
	int			ai_socktype;
	int			ai_protocol;
	size_t		ai_addrlen;
	struct sockaddr *ai_addr;
	char	   *ai_canonname;
	struct addrinfo *ai_next;
};
#endif   /* HAVE_STRUCT_ADDRINFO */


#ifndef HAVE_GETADDRINFO

/* Rename private copies per comments above */
#ifdef getaddrinfo
#undef getaddrinfo
#endif
#define getaddrinfo pg_getaddrinfo

#ifdef freeaddrinfo
#undef freeaddrinfo
#endif
#define freeaddrinfo pg_freeaddrinfo

#ifdef gai_strerror
#undef gai_strerror
#endif
#define gai_strerror pg_gai_strerror

#ifdef getnameinfo
#undef getnameinfo
#endif
#define getnameinfo pg_getnameinfo

extern int getaddrinfo(const char *node, const char *service,
			const struct addrinfo * hints, struct addrinfo ** res);
extern void freeaddrinfo(struct addrinfo * res);
extern const char *gai_strerror(int errcode);
extern int getnameinfo(const struct sockaddr * sa, int salen,
			char *node, int nodelen,
			char *service, int servicelen, int flags);
#endif   /* HAVE_GETADDRINFO */

#endif   /* GETADDRINFO_H */
