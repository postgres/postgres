#ifndef IP_H
#define IP_H
#include "c.h"
#include <sys/socket.h>
#include <netdb.h>
#include "libpq/pqcomm.h"
#ifndef HAVE_GETADDRINFO
#include "getaddrinfo.h"
#endif

int   getaddrinfo2(const char *hostname, const char *servname,
				   const struct addrinfo *hintp, struct addrinfo **result);
void  freeaddrinfo2(int hint_ai_family, struct addrinfo *ai);

char *SockAddr_ntop(const SockAddr *sa, char *dst, size_t cnt, int v4conv);
int   SockAddr_pton(SockAddr *sa, const char *src);
int   isAF_INETx(const int family);
int   rangeSockAddr(const SockAddr *addr, const SockAddr *netaddr,
			const SockAddr *netmask);
int   rangeSockAddrAF_INET(const SockAddr *addr, const SockAddr *netaddr, 
			   const SockAddr *netmask);
#ifdef HAVE_IPV6
int   rangeSockAddrAF_INET6(const SockAddr *addr, const SockAddr *netaddr, 
			    const SockAddr *netmask);
void  convSockAddr6to4(const SockAddr *src, SockAddr *dst);
#endif

#endif /* IP_H */
