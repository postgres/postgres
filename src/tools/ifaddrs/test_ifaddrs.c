/*
 * src/tools/ifaddrs/test_ifaddrs.c
 *
 *
 *	test_ifaddrs.c
 *		test pg_foreach_ifaddr()
 */

#include "postgres.h"

#include <netinet/in.h>
#include <arpa/inet.h>

#include "libpq/ifaddr.h"


static void
print_addr(struct sockaddr *addr)
{
	char		buffer[256];
	int			ret,
				len;

	switch (addr->sa_family)
	{
		case AF_INET:
			len = sizeof(struct sockaddr_in);
			break;
		case AF_INET6:
			len = sizeof(struct sockaddr_in6);
			break;
		default:
			len = sizeof(struct sockaddr_storage);
			break;
	}

	ret = getnameinfo(addr, len, buffer, sizeof(buffer), NULL, 0,
					  NI_NUMERICHOST);
	if (ret != 0)
		printf("[unknown: family %d]", addr->sa_family);
	else
		printf("%s", buffer);
}

static void
callback(struct sockaddr *addr, struct sockaddr *mask, void *unused)
{
	printf("addr: ");
	print_addr(addr);
	printf("  mask: ");
	print_addr(mask);
	printf("\n");
}

int
main(int argc, char *argv[])
{
#ifdef WIN32
	WSADATA		wsaData;

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		fprintf(stderr, "WSAStartup failed\n");
		return 1;
	}
#endif

	if (pg_foreach_ifaddr(callback, NULL) < 0)
		fprintf(stderr, "pg_foreach_ifaddr failed: %m\n");
	return 0;
}
