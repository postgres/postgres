/* $Header: /cvsroot/pgsql/src/port/getaddrinfo.c,v 1.1 2003/03/29 11:31:52 petere Exp $ */

#include "c.h"
#include "getaddrinfo.h"
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


int
getaddrinfo(const char *node, const char *service,
			const struct addrinfo *hints,
			struct addrinfo **res)
{
	struct addrinfo *ai;
	struct sockaddr_in sin, *psin;

	if (!hints || (hints->ai_family != AF_INET && hints->ai_family != AF_UNSPEC))
		return EAI_FAMILY;

	if (hints->ai_socktype != SOCK_STREAM)
		return EAI_SOCKTYPE;

	if (!node && !service)
		return EAI_NONAME;

	if (node)
	{
		if (node[0] == '\0')
			sin.sin_addr.s_addr = htonl(INADDR_ANY);
		else if (hints->ai_flags & AI_NUMERICHOST)
		{
			inet_aton(node, &sin.sin_addr);
		}
		else
		{
			struct hostent *hp;

			hp = gethostbyname(node);
			if (hp == NULL)
			{
				switch (h_errno)
				{
					case HOST_NOT_FOUND:
						return EAI_NONAME;
					case NO_DATA:
						return EAI_NODATA;
					case TRY_AGAIN:
						return EAI_AGAIN;
					case NO_RECOVERY:
					default:
						return EAI_FAIL;
				}
			}
			if (hp->h_addrtype != AF_INET)
				return EAI_ADDRFAMILY;

			memmove(&(sin.sin_addr), hp->h_addr, hp->h_length);
		}
	}
	else
	{
		if (hints->ai_flags & AI_PASSIVE)
			sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}

	if (service)
		sin.sin_port = htons((unsigned short)atoi(service));

	ai = malloc(sizeof(*ai));
	if (!ai)
		return EAI_MEMORY;
	psin = malloc(sizeof(*psin));
	if (!psin)
	{
		free(ai);
		return EAI_MEMORY;
	}

	memcpy(psin, &sin, sizeof(sin));

	ai->ai_family = hints->ai_family;
	ai->ai_socktype = hints->ai_socktype;
	ai->ai_protocol = hints->ai_protocol;
	ai->ai_addrlen = sizeof(*psin);
	ai->ai_addr = (struct sockaddr *) psin;
	ai->ai_canonname = NULL;
	ai->ai_next = NULL;

	*res = ai;

	return 0;
}


void
freeaddrinfo(struct addrinfo *res)
{
	if (res)
	{
		if (res->ai_addr)
			free(res->ai_addr);
		free(res);
	}
}


const char*
gai_strerror(int errcode)
{
	int hcode;

	switch (errcode)
	{
		case EAI_NONAME:
			hcode = HOST_NOT_FOUND;
			break;
		case EAI_NODATA:
			hcode = NO_DATA;
			break;
		case EAI_AGAIN:
			hcode = TRY_AGAIN;
			break;
		case EAI_FAIL:
		default:
			hcode = NO_RECOVERY;
			break;
	}

	return hstrerror(hcode);
}
