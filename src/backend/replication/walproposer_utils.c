#include "replication/walproposer.h"
#include "common/logging.h"
#include "common/ip.h"
#include <netinet/tcp.h>
#include <unistd.h>

int CompareNodeId(NodeId* id1, NodeId* id2)
{
	return
		(id1->term < id2->term)
		? -1
		: (id1->term > id2->term)
		   ? 1
   		   : memcmp(&id1->uuid, &id1->uuid, sizeof(pg_uuid_t));
}

int
CompareLsn(const void *a, const void *b)
{
	XLogRecPtr	lsn1 = *((const XLogRecPtr *) a);
	XLogRecPtr	lsn2 = *((const XLogRecPtr *) b);

	if (lsn1 < lsn2)
		return -1;
	else if (lsn1 == lsn2)
		return 0;
	else
		return 1;
}

static bool
SetSocketOptions(pgsocket sock)
{
	int on = 1;
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on, sizeof(on)) < 0)
	{
		elog(WARNING, "setsockopt(TCP_NODELAY) failed: %m");
		closesocket(sock);
		return false;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
				   (char *) &on, sizeof(on)) < 0)
	{
		elog(WARNING, "setsockopt(SO_REUSEADDR) failed: %m");
		closesocket(sock);
		return false;
	}
	if (!pg_set_noblock(sock))
	{
		elog(WARNING, "faied to switch socket to non-blocking mode: %m");
		closesocket(sock);
		return false;
	}
	return true;
}

pgsocket
ConnectSocketAsync(char const* host, char const* port, bool* established)
{
	struct addrinfo *addrs = NULL,
		*addr,
		hints;
	int	ret;
	pgsocket sock = PGINVALID_SOCKET;

	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;
	ret = pg_getaddrinfo_all(host, port, &hints, &addrs);
	if (ret || !addrs)
	{
		elog(WARNING, "Could not resolve \"%s\": %s",
					 host, gai_strerror(ret));
		return -1;
	}
	for (addr = addrs; addr; addr = addr->ai_next)
	{
		sock = socket(addr->ai_family, SOCK_STREAM, 0);
		if (sock == PGINVALID_SOCKET)
		{
			elog(WARNING, "could not create socket: %m");
			continue;
		}
		if (!SetSocketOptions(sock))
			continue;

		/*
		 * Bind it to a kernel assigned port on localhost and get the assigned
		 * port via getsockname().
		 */
		while ((ret = connect(sock, addr->ai_addr, addr->ai_addrlen)) < 0 && errno == EINTR);
		if (ret < 0)
		{
			if (errno == EINPROGRESS)
			{
				*established = false;
				break;
			}
			elog(WARNING, "Could not establish connection to %s:%s: %m",
						 host, port);
			closesocket(sock);
		}
		else
		{
			*established = true;
			break;
		}
	}
	return sock;
}
ssize_t
ReadSocketAsync(pgsocket sock, void* buf, size_t size)
{
	size_t offs = 0;

	while (size != offs)
	{
		ssize_t rc = recv(sock, (char*)buf + offs, size - offs, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return offs;
			elog(WARNING, "Socket write failed: %m");
			return -1;
		}
		else if (rc == 0)
		{
			elog(WARNING, "Connection was closed by peer");
			return -1;
		}
		offs += rc;
	}
	return offs;
}

ssize_t
WriteSocketAsync(pgsocket sock, void const* buf, size_t size)
{
	size_t offs = 0;

	while (size != offs)
	{
		ssize_t rc = send(sock, (char const*)buf + offs, size - offs, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return offs;
			elog(WARNING, "Socket write failed: %m");
			return -1;
		}
		else if (rc == 0)
		{
			elog(WARNING, "Connection was closed by peer");
			return -1;
		}
		offs += rc;
	}
	return offs;
}

bool
WriteSocket(pgsocket sock, void const* buf, size_t size)
{
	char* src = (char*)buf;

	while (size != 0)
	{
		ssize_t rc = send(sock, src, size, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			elog(WARNING, "Socket write failed: %m");
			return false;
		}
		else if (rc == 0)
		{
			elog(WARNING, "Connection was closed by peer");
			return false;
		}
		size -= rc;
		src += rc;
	}
	return true;
}

/*
 * Convert a character which represents a hexadecimal digit to an integer.
 *
 * Returns -1 if the character is not a hexadecimal digit.
 */
static int
HexDecodeChar(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/*
 * Decode a hex string into a byte string, 2 hex chars per byte.
 *
 * Returns false if invalid characters are encountered; otherwise true.
 */
bool
HexDecodeString(uint8 *result, char *input, int nbytes)
{
	int			i;

	for (i = 0; i < nbytes; ++i)
	{
		int			n1 = HexDecodeChar(input[i * 2]);
		int			n2 = HexDecodeChar(input[i * 2 + 1]);

		if (n1 < 0 || n2 < 0)
			return false;
		result[i] = n1 * 16 + n2;
	}

	return true;
}

