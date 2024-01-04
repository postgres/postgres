/*-------------------------------------------------------------------------
 *
 * ifaddr.c
 *	  IP netmask calculations, and enumerating network interfaces.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/ifaddr.c
 *
 * This file and the IPV6 implementation were initially provided by
 * Nigel Kukard <nkukard@lbsd.net>, Linux Based Systems Design
 * http://www.lbsd.net.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/file.h>

#include "libpq/ifaddr.h"
#include "port/pg_bswap.h"

static int	range_sockaddr_AF_INET(const struct sockaddr_in *addr,
								   const struct sockaddr_in *netaddr,
								   const struct sockaddr_in *netmask);

static int	range_sockaddr_AF_INET6(const struct sockaddr_in6 *addr,
									const struct sockaddr_in6 *netaddr,
									const struct sockaddr_in6 *netmask);


/*
 * pg_range_sockaddr - is addr within the subnet specified by netaddr/netmask ?
 *
 * Note: caller must already have verified that all three addresses are
 * in the same address family; and AF_UNIX addresses are not supported.
 */
int
pg_range_sockaddr(const struct sockaddr_storage *addr,
				  const struct sockaddr_storage *netaddr,
				  const struct sockaddr_storage *netmask)
{
	if (addr->ss_family == AF_INET)
		return range_sockaddr_AF_INET((const struct sockaddr_in *) addr,
									  (const struct sockaddr_in *) netaddr,
									  (const struct sockaddr_in *) netmask);
	else if (addr->ss_family == AF_INET6)
		return range_sockaddr_AF_INET6((const struct sockaddr_in6 *) addr,
									   (const struct sockaddr_in6 *) netaddr,
									   (const struct sockaddr_in6 *) netmask);
	else
		return 0;
}

static int
range_sockaddr_AF_INET(const struct sockaddr_in *addr,
					   const struct sockaddr_in *netaddr,
					   const struct sockaddr_in *netmask)
{
	if (((addr->sin_addr.s_addr ^ netaddr->sin_addr.s_addr) &
		 netmask->sin_addr.s_addr) == 0)
		return 1;
	else
		return 0;
}

static int
range_sockaddr_AF_INET6(const struct sockaddr_in6 *addr,
						const struct sockaddr_in6 *netaddr,
						const struct sockaddr_in6 *netmask)
{
	int			i;

	for (i = 0; i < 16; i++)
	{
		if (((addr->sin6_addr.s6_addr[i] ^ netaddr->sin6_addr.s6_addr[i]) &
			 netmask->sin6_addr.s6_addr[i]) != 0)
			return 0;
	}

	return 1;
}

/*
 *	pg_sockaddr_cidr_mask - make a network mask of the appropriate family
 *	  and required number of significant bits
 *
 * numbits can be null, in which case the mask is fully set.
 *
 * The resulting mask is placed in *mask, which had better be big enough.
 *
 * Return value is 0 if okay, -1 if not.
 */
int
pg_sockaddr_cidr_mask(struct sockaddr_storage *mask, char *numbits, int family)
{
	long		bits;
	char	   *endptr;

	if (numbits == NULL)
	{
		bits = (family == AF_INET) ? 32 : 128;
	}
	else
	{
		bits = strtol(numbits, &endptr, 10);
		if (*numbits == '\0' || *endptr != '\0')
			return -1;
	}

	switch (family)
	{
		case AF_INET:
			{
				struct sockaddr_in mask4;
				long		maskl;

				if (bits < 0 || bits > 32)
					return -1;
				memset(&mask4, 0, sizeof(mask4));
				/* avoid "x << 32", which is not portable */
				if (bits > 0)
					maskl = (0xffffffffUL << (32 - (int) bits))
						& 0xffffffffUL;
				else
					maskl = 0;
				mask4.sin_addr.s_addr = pg_hton32(maskl);
				memcpy(mask, &mask4, sizeof(mask4));
				break;
			}

		case AF_INET6:
			{
				struct sockaddr_in6 mask6;
				int			i;

				if (bits < 0 || bits > 128)
					return -1;
				memset(&mask6, 0, sizeof(mask6));
				for (i = 0; i < 16; i++)
				{
					if (bits <= 0)
						mask6.sin6_addr.s6_addr[i] = 0;
					else if (bits >= 8)
						mask6.sin6_addr.s6_addr[i] = 0xff;
					else
					{
						mask6.sin6_addr.s6_addr[i] =
							(0xff << (8 - (int) bits)) & 0xff;
					}
					bits -= 8;
				}
				memcpy(mask, &mask6, sizeof(mask6));
				break;
			}

		default:
			return -1;
	}

	mask->ss_family = family;
	return 0;
}


/*
 * Run the callback function for the addr/mask, after making sure the
 * mask is sane for the addr.
 */
static void
run_ifaddr_callback(PgIfAddrCallback callback, void *cb_data,
					struct sockaddr *addr, struct sockaddr *mask)
{
	struct sockaddr_storage fullmask;

	if (!addr)
		return;

	/* Check that the mask is valid */
	if (mask)
	{
		if (mask->sa_family != addr->sa_family)
		{
			mask = NULL;
		}
		else if (mask->sa_family == AF_INET)
		{
			if (((struct sockaddr_in *) mask)->sin_addr.s_addr == INADDR_ANY)
				mask = NULL;
		}
		else if (mask->sa_family == AF_INET6)
		{
			if (IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6 *) mask)->sin6_addr))
				mask = NULL;
		}
	}

	/* If mask is invalid, generate our own fully-set mask */
	if (!mask)
	{
		pg_sockaddr_cidr_mask(&fullmask, NULL, addr->sa_family);
		mask = (struct sockaddr *) &fullmask;
	}

	(*callback) (addr, mask, cb_data);
}

#ifdef WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

/*
 * Enumerate the system's network interface addresses and call the callback
 * for each one.  Returns 0 if successful, -1 if trouble.
 *
 * This version is for Win32.  Uses the Winsock 2 functions (ie: ws2_32.dll)
 */
int
pg_foreach_ifaddr(PgIfAddrCallback callback, void *cb_data)
{
	INTERFACE_INFO *ptr,
			   *ii = NULL;
	unsigned long length,
				i;
	unsigned long n_ii = 0;
	SOCKET		sock;
	int			error;

	sock = WSASocket(AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
	if (sock == INVALID_SOCKET)
		return -1;

	while (n_ii < 1024)
	{
		n_ii += 64;
		ptr = realloc(ii, sizeof(INTERFACE_INFO) * n_ii);
		if (!ptr)
		{
			free(ii);
			closesocket(sock);
			errno = ENOMEM;
			return -1;
		}

		ii = ptr;
		if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, 0, 0,
					 ii, n_ii * sizeof(INTERFACE_INFO),
					 &length, 0, 0) == SOCKET_ERROR)
		{
			error = WSAGetLastError();
			if (error == WSAEFAULT || error == WSAENOBUFS)
				continue;		/* need to make the buffer bigger */
			closesocket(sock);
			free(ii);
			return -1;
		}

		break;
	}

	for (i = 0; i < length / sizeof(INTERFACE_INFO); ++i)
		run_ifaddr_callback(callback, cb_data,
							(struct sockaddr *) &ii[i].iiAddress,
							(struct sockaddr *) &ii[i].iiNetmask);

	closesocket(sock);
	free(ii);
	return 0;
}
#elif HAVE_GETIFADDRS			/* && !WIN32 */

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

/*
 * Enumerate the system's network interface addresses and call the callback
 * for each one.  Returns 0 if successful, -1 if trouble.
 *
 * This version uses the getifaddrs() interface, which is available on
 * BSDs, macOS, Solaris, illumos and Linux.
 */
int
pg_foreach_ifaddr(PgIfAddrCallback callback, void *cb_data)
{
	struct ifaddrs *ifa,
			   *l;

	if (getifaddrs(&ifa) < 0)
		return -1;

	for (l = ifa; l; l = l->ifa_next)
		run_ifaddr_callback(callback, cb_data,
							l->ifa_addr, l->ifa_netmask);

	freeifaddrs(ifa);
	return 0;
}
#else							/* !HAVE_GETIFADDRS && !WIN32 */

#include <sys/ioctl.h>
#include <net/if.h>

#if defined(SIOCGIFCONF)

/*
 * Remaining Unixes use SIOCGIFCONF. Some only return IPv4 information
 * here, so this is the least preferred method. Note that there is no
 * standard way to iterate the struct ifreq returned in the array.
 * On some OSs the structures are padded large enough for any address,
 * on others you have to calculate the size of the struct ifreq.
 */

/* Some OSs have _SIZEOF_ADDR_IFREQ, so just use that */
#ifndef _SIZEOF_ADDR_IFREQ

/* Calculate based on sockaddr.sa_len */
#ifdef HAVE_STRUCT_SOCKADDR_SA_LEN
#define _SIZEOF_ADDR_IFREQ(ifr) \
		((ifr).ifr_addr.sa_len > sizeof(struct sockaddr) ? \
		 (sizeof(struct ifreq) - sizeof(struct sockaddr) + \
		  (ifr).ifr_addr.sa_len) : sizeof(struct ifreq))

/* Padded ifreq structure, simple */
#else
#define _SIZEOF_ADDR_IFREQ(ifr) \
	sizeof (struct ifreq)
#endif
#endif							/* !_SIZEOF_ADDR_IFREQ */

/*
 * Enumerate the system's network interface addresses and call the callback
 * for each one.  Returns 0 if successful, -1 if trouble.
 *
 * This version uses ioctl(SIOCGIFCONF).
 */
int
pg_foreach_ifaddr(PgIfAddrCallback callback, void *cb_data)
{
	struct ifconf ifc;
	struct ifreq *ifr,
			   *end,
				addr,
				mask;
	char	   *ptr,
			   *buffer = NULL;
	size_t		n_buffer = 1024;
	pgsocket	sock;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == PGINVALID_SOCKET)
		return -1;

	while (n_buffer < 1024 * 100)
	{
		n_buffer += 1024;
		ptr = realloc(buffer, n_buffer);
		if (!ptr)
		{
			free(buffer);
			close(sock);
			errno = ENOMEM;
			return -1;
		}

		memset(&ifc, 0, sizeof(ifc));
		ifc.ifc_buf = buffer = ptr;
		ifc.ifc_len = n_buffer;

		if (ioctl(sock, SIOCGIFCONF, &ifc) < 0)
		{
			if (errno == EINVAL)
				continue;
			free(buffer);
			close(sock);
			return -1;
		}

		/*
		 * Some Unixes try to return as much data as possible, with no
		 * indication of whether enough space allocated. Don't believe we have
		 * it all unless there's lots of slop.
		 */
		if (ifc.ifc_len < n_buffer - 1024)
			break;
	}

	end = (struct ifreq *) (buffer + ifc.ifc_len);
	for (ifr = ifc.ifc_req; ifr < end;)
	{
		memcpy(&addr, ifr, sizeof(addr));
		memcpy(&mask, ifr, sizeof(mask));
		if (ioctl(sock, SIOCGIFADDR, &addr, sizeof(addr)) == 0 &&
			ioctl(sock, SIOCGIFNETMASK, &mask, sizeof(mask)) == 0)
			run_ifaddr_callback(callback, cb_data,
								&addr.ifr_addr, &mask.ifr_addr);
		ifr = (struct ifreq *) ((char *) ifr + _SIZEOF_ADDR_IFREQ(*ifr));
	}

	free(buffer);
	close(sock);
	return 0;
}
#else							/* !defined(SIOCGIFCONF) */

/*
 * Enumerate the system's network interface addresses and call the callback
 * for each one.  Returns 0 if successful, -1 if trouble.
 *
 * This version is our fallback if there's no known way to get the
 * interface addresses.  Just return the standard loopback addresses.
 */
int
pg_foreach_ifaddr(PgIfAddrCallback callback, void *cb_data)
{
	struct sockaddr_in addr;
	struct sockaddr_storage mask;
	struct sockaddr_in6 addr6;

	/* addr 127.0.0.1/8 */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = pg_ntoh32(0x7f000001);
	memset(&mask, 0, sizeof(mask));
	pg_sockaddr_cidr_mask(&mask, "8", AF_INET);
	run_ifaddr_callback(callback, cb_data,
						(struct sockaddr *) &addr,
						(struct sockaddr *) &mask);

	/* addr ::1/128 */
	memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_addr.s6_addr[15] = 1;
	memset(&mask, 0, sizeof(mask));
	pg_sockaddr_cidr_mask(&mask, "128", AF_INET6);
	run_ifaddr_callback(callback, cb_data,
						(struct sockaddr *) &addr6,
						(struct sockaddr *) &mask);

	return 0;
}
#endif							/* !defined(SIOCGIFCONF) */

#endif							/* !HAVE_GETIFADDRS */
