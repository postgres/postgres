/* $Header: /cvsroot/pgsql/src/include/getaddrinfo.h,v 1.1 2003/03/29 11:31:51 petere Exp $ */

#ifndef GETADDRINFO_H
#define GETADDRINFO_H

#include "c.h"
#include <netdb.h>


struct addrinfo {
	int     ai_flags;
	int     ai_family;
	int     ai_socktype;
	int     ai_protocol;
	size_t  ai_addrlen;
	struct sockaddr *ai_addr;
	char   *ai_canonname;
	struct addrinfo *ai_next;
};


int getaddrinfo(const char *node, const char *service,
				const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);


#define EAI_BADFLAGS	-1
#define EAI_NONAME		-2
#define EAI_AGAIN		-3
#define EAI_FAIL		-4
#define EAI_NODATA		-5
#define EAI_FAMILY		-6
#define EAI_SOCKTYPE	-7
#define EAI_SERVICE		-8
#define EAI_ADDRFAMILY	-9
#define EAI_MEMORY		-10
#define EAI_SYSTEM		-11

#define AI_PASSIVE		0x0001
#define AI_NUMERICHOST	0x0004

#endif /* GETADDRINFO_H */
