#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#include <sys/file.h>

#include "libpq/libpq.h"
#include "miscadmin.h"






#ifdef HAVE_UNIX_SOCKETS
static int ga_unix(const char* path, const struct addrinfo* hintsp, 
		   struct addrinfo** result);
#endif   /* HAVE_UNIX_SOCKETS */


int getaddrinfo2(const char* hostname, const char* servname,
		 const struct addrinfo* hintp, struct addrinfo **result)
{
#ifdef HAVE_UNIX_SOCKETS
  if( hintp != NULL && hintp->ai_family == AF_UNIX){
    return ga_unix(servname, hintp, result);
  }
  else {
#endif   /* HAVE_UNIX_SOCKETS */
    return getaddrinfo(hostname, servname, hintp, result);
#ifdef HAVE_UNIX_SOCKETS
  }
#endif   /* HAVE_UNIX_SOCKETS */
}

void freeaddrinfo2(int hint_ai_family, struct addrinfo *ai)
{
#ifdef HAVE_UNIX_SOCKETS
  if(hint_ai_family == AF_UNIX){
    struct addrinfo *p;
    while(ai != NULL){
      p = ai;
      ai = ai->ai_next;
      free(p->ai_addr);
      free(p);
    }
  }
  else {
#endif   /* HAVE_UNIX_SOCKETS */
    freeaddrinfo(ai);
#ifdef HAVE_UNIX_SOCKETS
  }
#endif   /* HAVE_UNIX_SOCKETS */
}


#ifdef HAVE_UNIX_SOCKETS
/**
 *  Bug:  only one addrinfo is set even though hintsp is NULL or 
 *        ai_socktype is 0
 *        AI_CANNONNAME does not support.
 */
static int ga_unix(const char* path, const struct addrinfo* hintsp, 
		   struct addrinfo** result)
{
  struct addrinfo     hints;
  struct addrinfo*    aip;
  struct sockaddr_un* unp;
  memset(&hints, 0, sizeof(hints));


  if(hintsp == NULL){
    hints.ai_family   = AF_UNIX;
    hints.ai_socktype = SOCK_STREAM;
  }
  else {
    memcpy(&hints, hintsp, sizeof(hints));
  }
  if(hints.ai_socktype == 0){
    hints.ai_socktype = SOCK_STREAM;
  }

  if(!(hints.ai_family == AF_UNIX)){
    printf("hints.ai_family is invalied ga_unix()\n");
    return EAI_ADDRFAMILY;
  }


  aip = calloc(1, sizeof(struct addrinfo));
  if(aip == NULL){
    return EAI_MEMORY;
  }

  aip->ai_family    = AF_UNIX;
  aip->ai_socktype  = hints.ai_socktype;
  aip->ai_protocol  = hints.ai_protocol;
  aip->ai_next      = NULL;
  aip->ai_canonname = NULL;
  *result = aip;

  unp = calloc(1, sizeof(struct sockaddr_un));
  if(aip == NULL){
    return EAI_MEMORY;
  }
  
  unp->sun_family = AF_UNIX;
  aip->ai_addr    = (struct sockaddr*) unp;
  aip->ai_addrlen = sizeof(struct sockaddr_un);

  if(strlen(path) >= sizeof(unp->sun_path)){
    return EAI_SERVICE;
  }
  strcpy(unp->sun_path, path);

#if SALEN
  unp->sun_len = sizeof(struct sockaddr_un);
#endif /* SALEN */

  if(hints.ai_flags & AI_PASSIVE){
    unlink(unp->sun_path);
  }
  
  return 0;
}
#endif   /* HAVE_UNIX_SOCKETS */





/**
 * SockAddr_ntop - set IP address string from SockAddr
 *
 * parameters...  sa    : SockAddr union
 *                dst   : buffer for address string
 *                cnt   : sizeof dst
 *                v4conv: non-zero: if address is IPv4 mapped IPv6 address then
 *                        convert to IPv4 address.
 * returns... pointer to dst
 * if sa.sa_family is not AF_INET or AF_INET6 dst is set as empy string.
 */
char* SockAddr_ntop(const SockAddr* sa, char* dst, size_t cnt, int v4conv)
{
  switch(sa->sa.sa_family){
  case AF_INET:
    inet_ntop(AF_INET, &sa->in.sin_addr, dst, cnt);
    break;
  case AF_INET6:
    inet_ntop(AF_INET6, &sa->in6.sin6_addr, dst, cnt);
    if(v4conv && IN6_IS_ADDR_V4MAPPED(&sa->in6.sin6_addr) ){
      strcpy(dst, dst + 7);
    }
    break;
  default:
    dst[0] = '\0';
    break;
  }
  return dst;
}

int SockAddr_pton(SockAddr* sa, const char* src, size_t cnt)
{
  int i;
  int family = AF_INET;
  for(i = 0; i < cnt; i++){
    if(src[i] == ':'){
      family = AF_INET6;
      break;
    }
  }

  sa->sa.sa_family = family;
  switch(family){
  case AF_INET:
    return inet_pton(AF_INET, src, &sa->in.sin_addr);
  case AF_INET6:
    return inet_pton(AF_INET6, src, &sa->in6.sin6_addr);
    break;
  default:
    return -1;
  }
}




/**
 *  isAF_INETx - check to see if sa is AF_INET or AF_INET6
 *
 *  parameters...    sa : SockAddr union
 *  returns... 
 *                if sa->sa.sa_famil is AF_INET or AF_INET6 then
 *                   return 1
 *                 else 
 *                   return 0
 */
int   isAF_INETx(const SockAddr* sa)
{
  if(sa->sa.sa_family == AF_INET  ||
     sa->sa.sa_family == AF_INET6   
     ){
    return 1;
  }
  else {
    return 0;
  }
}

int  isAF_INETx2(int family)
{
  if(family == AF_INET  ||
     family == AF_INET6   
     ){
    return 1;
  }
  else {
    return 0;
  }
}


int rangeSockAddr(const SockAddr* addr, const SockAddr* netaddr, const SockAddr* netmask)
{
  if(addr->sa.sa_family == AF_INET){
    return rangeSockAddrAF_INET(addr, netaddr, netmask);
  }
  else if(addr->sa.sa_family == AF_INET6){
    return rangeSockAddrAF_INET6(addr, netaddr, netmask);
  }
  else {
    return 0;
  }
}

int rangeSockAddrAF_INET(const SockAddr* addr, const SockAddr* netaddr, 
			 const SockAddr* netmask)
{
  if(addr->sa.sa_family != AF_INET    || 
     netaddr->sa.sa_family != AF_INET ||
     netmask->sa.sa_family != AF_INET  ){
    return 0;
  }
  if( ((addr->in.sin_addr.s_addr ^  netaddr->in.sin_addr.s_addr ) &
       netmask->in.sin_addr.s_addr) == 0){
    return 1;
  }
  else {
    return 0;
  }
}

int rangeSockAddrAF_INET6(const SockAddr* addr, const SockAddr* netaddr, 
			  const SockAddr* netmask)
{
  int i;

  if(IN6_IS_ADDR_V4MAPPED(&addr->in6.sin6_addr) ){
    SockAddr addr4;
    convSockAddr6to4(addr, &addr4);
    if(rangeSockAddrAF_INET(&addr4, netaddr, netmask)){
      return 1;
    }
  }

  if(netaddr->sa.sa_family != AF_INET6 ||
     netmask->sa.sa_family != AF_INET6  ){
    return 0;
  }

  for( i = 0; i < 16; i++){
    if( ((addr->in6.sin6_addr.s6_addr[i] ^ netaddr->in6.sin6_addr.s6_addr[i] ) &
	 netmask->in6.sin6_addr.s6_addr[i] ) != 0){
      return 0;
    }
  }

  return 1;
}

void convSockAddr6to4(const SockAddr* src, SockAddr* dst)
{
  char addr_str[INET6_ADDRSTRLEN];

  dst->in.sin_family = AF_INET;
  dst->in.sin_port  = src->in6.sin6_port;

  dst->in.sin_addr.s_addr = src->in6.sin6_addr.s6_addr32[3];
  SockAddr_ntop(src, addr_str, INET6_ADDRSTRLEN, 0);
}


