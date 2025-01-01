/*-------------------------------------------------------------------------
 *
 * ip.c
 *	  IPv6-aware network access.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/ip.c
 *
 * This file and the IPV6 implementation were initially provided by
 * Nigel Kukard <nkukard@lbsd.net>, Linux Based Systems Design
 * http://www.lbsd.net.
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/file.h>

#include "common/ip.h"



static int	getaddrinfo_unix(const char *path,
							 const struct addrinfo *hintsp,
							 struct addrinfo **result);

static int	getnameinfo_unix(const struct sockaddr_un *sa, int salen,
							 char *node, int nodelen,
							 char *service, int servicelen,
							 int flags);


/*
 *	pg_getaddrinfo_all - get address info for Unix, IPv4 and IPv6 sockets
 */
int
pg_getaddrinfo_all(const char *hostname, const char *servname,
				   const struct addrinfo *hintp, struct addrinfo **result)
{
	int			rc;

	/* not all versions of getaddrinfo() zero *result on failure */
	*result = NULL;

	if (hintp->ai_family == AF_UNIX)
		return getaddrinfo_unix(servname, hintp, result);

	/* NULL has special meaning to getaddrinfo(). */
	rc = getaddrinfo((!hostname || hostname[0] == '\0') ? NULL : hostname,
					 servname, hintp, result);

	return rc;
}


/*
 *	pg_freeaddrinfo_all - free addrinfo structures for IPv4, IPv6, or Unix
 *
 * Note: the ai_family field of the original hint structure must be passed
 * so that we can tell whether the addrinfo struct was built by the system's
 * getaddrinfo() routine or our own getaddrinfo_unix() routine.  Some versions
 * of getaddrinfo() might be willing to return AF_UNIX addresses, so it's
 * not safe to look at ai_family in the addrinfo itself.
 */
void
pg_freeaddrinfo_all(int hint_ai_family, struct addrinfo *ai)
{
	if (hint_ai_family == AF_UNIX)
	{
		/* struct was built by getaddrinfo_unix (see pg_getaddrinfo_all) */
		while (ai != NULL)
		{
			struct addrinfo *p = ai;

			ai = ai->ai_next;
			free(p->ai_addr);
			free(p);
		}
	}
	else
	{
		/* struct was built by getaddrinfo() */
		if (ai != NULL)
			freeaddrinfo(ai);
	}
}


/*
 *	pg_getnameinfo_all - get name info for Unix, IPv4 and IPv6 sockets
 *
 * The API of this routine differs from the standard getnameinfo() definition
 * in two ways: first, the addr parameter is declared as sockaddr_storage
 * rather than struct sockaddr, and second, the node and service fields are
 * guaranteed to be filled with something even on failure return.
 */
int
pg_getnameinfo_all(const struct sockaddr_storage *addr, int salen,
				   char *node, int nodelen,
				   char *service, int servicelen,
				   int flags)
{
	int			rc;

	if (addr && addr->ss_family == AF_UNIX)
		rc = getnameinfo_unix((const struct sockaddr_un *) addr, salen,
							  node, nodelen,
							  service, servicelen,
							  flags);
	else
		rc = getnameinfo((const struct sockaddr *) addr, salen,
						 node, nodelen,
						 service, servicelen,
						 flags);

	if (rc != 0)
	{
		if (node)
			strlcpy(node, "???", nodelen);
		if (service)
			strlcpy(service, "???", servicelen);
	}

	return rc;
}


/* -------
 *	getaddrinfo_unix - get unix socket info using IPv6-compatible API
 *
 *	Bugs: only one addrinfo is set even though hintsp is NULL or
 *		  ai_socktype is 0
 *		  AI_CANONNAME is not supported.
 * -------
 */
static int
getaddrinfo_unix(const char *path, const struct addrinfo *hintsp,
				 struct addrinfo **result)
{
	struct addrinfo hints = {0};
	struct addrinfo *aip;
	struct sockaddr_un *unp;

	*result = NULL;

	if (strlen(path) >= sizeof(unp->sun_path))
		return EAI_FAIL;

	if (hintsp == NULL)
	{
		hints.ai_family = AF_UNIX;
		hints.ai_socktype = SOCK_STREAM;
	}
	else
		memcpy(&hints, hintsp, sizeof(hints));

	if (hints.ai_socktype == 0)
		hints.ai_socktype = SOCK_STREAM;

	if (hints.ai_family != AF_UNIX)
	{
		/* shouldn't have been called */
		return EAI_FAIL;
	}

	aip = calloc(1, sizeof(struct addrinfo));
	if (aip == NULL)
		return EAI_MEMORY;

	unp = calloc(1, sizeof(struct sockaddr_un));
	if (unp == NULL)
	{
		free(aip);
		return EAI_MEMORY;
	}

	aip->ai_family = AF_UNIX;
	aip->ai_socktype = hints.ai_socktype;
	aip->ai_protocol = hints.ai_protocol;
	aip->ai_next = NULL;
	aip->ai_canonname = NULL;
	*result = aip;

	unp->sun_family = AF_UNIX;
	aip->ai_addr = (struct sockaddr *) unp;
	aip->ai_addrlen = sizeof(struct sockaddr_un);

	strcpy(unp->sun_path, path);

	/*
	 * If the supplied path starts with @, replace that with a zero byte for
	 * the internal representation.  In that mode, the entire sun_path is the
	 * address, including trailing zero bytes.  But we set the address length
	 * to only include the length of the original string.  That way the
	 * trailing zero bytes won't show up in any network or socket lists of the
	 * operating system.  This is just a convention, also followed by other
	 * packages.
	 */
	if (path[0] == '@')
	{
		unp->sun_path[0] = '\0';
		aip->ai_addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(path);
	}

	return 0;
}

/*
 * Convert an address to a hostname.
 */
static int
getnameinfo_unix(const struct sockaddr_un *sa, int salen,
				 char *node, int nodelen,
				 char *service, int servicelen,
				 int flags)
{
	int			ret;

	/* Invalid arguments. */
	if (sa == NULL || sa->sun_family != AF_UNIX ||
		(node == NULL && service == NULL))
		return EAI_FAIL;

	if (node)
	{
		ret = snprintf(node, nodelen, "%s", "[local]");
		if (ret < 0 || ret >= nodelen)
			return EAI_MEMORY;
	}

	if (service)
	{
		/*
		 * Check whether it looks like an abstract socket, but it could also
		 * just be an empty string.
		 */
		if (sa->sun_path[0] == '\0' && sa->sun_path[1] != '\0')
			ret = snprintf(service, servicelen, "@%s", sa->sun_path + 1);
		else
			ret = snprintf(service, servicelen, "%s", sa->sun_path);
		if (ret < 0 || ret >= servicelen)
			return EAI_MEMORY;
	}

	return 0;
}
