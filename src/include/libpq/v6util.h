#ifndef V6UTIL_H
#define V6UTIL_H
void  freeaddrinfo2(int hint_ai_family, struct addrinfo *ai);
int   getaddrinfo2(const char* hostname, const char* servname,
		   const struct addrinfo* hintp, struct addrinfo **result);
char* SockAddr_ntop(const SockAddr* sa, char* dst, size_t cnt, int v4conv);
int   SockAddr_pton(SockAddr* sa, const char* src, size_t cnt);
int   isAF_INETx(const SockAddr* sa);
int   isAF_INETx2(int family);
int   rangeSockAddr(const SockAddr* addr, const SockAddr* netaddr, const SockAddr* netmask);
int   rangeSockAddrAF_INET(const SockAddr* addr, const SockAddr* netaddr, 
			   const SockAddr* netmask);
int   rangeSockAddrAF_INET6(const SockAddr* addr, const SockAddr* netaddr, 
			    const SockAddr* netmask);
void  convSockAddr6to4(const SockAddr* src, SockAddr* dst);

#endif /* V6UTIL_H */
