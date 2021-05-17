#include "safekeeper.h"
#include "common/logging.h"
#include "common/file_perm.h"
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

static bool
SetSocketOptions(pgsocket sock)
{
	int on = 1;
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on, sizeof(on)) < 0)
	{
		pg_log_error("setsockopt(TCP_NODELAY) failed: %m");
		closesocket(sock);
		return false;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
				   (char *) &on, sizeof(on)) < 0)
	{
		pg_log_error("setsockopt(SO_REUSEADDR) failed: %m");
		closesocket(sock);
		return false;
	}
	if (!pg_set_noblock(sock))
	{
		pg_log_error("faied to switch socket to non-blocking mode: %m");
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
		pg_log_error("Could not resolve \"%s\": %s",
					 host, gai_strerror(ret));
		return -1;
	}
	for (addr = addrs; addr; addr = addr->ai_next)
	{
		sock = socket(addr->ai_family, SOCK_STREAM, 0);
		if (sock == PGINVALID_SOCKET)
		{
			pg_log_error("could not create socket: %m");
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
			pg_log_error("Could not establish connection to %s:%s: %m",
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

pgsocket
CreateSocket(char const* host, char const* port, int n_peers)
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
		pg_log_error("Could not resolve \"%s\": %s",
					 host, gai_strerror(ret));
		return -1;
	}
	for (addr = addrs; addr; addr = addr->ai_next)
	{
		sock = socket(addr->ai_family, SOCK_STREAM, 0);
		if (sock == PGINVALID_SOCKET)
		{
			pg_log_error("could not create socket: %m");
			continue;
		}
		if (!SetSocketOptions(sock))
			continue;

		/*
		 * Bind it to a kernel assigned port on localhost and get the assigned
		 * port via getsockname().
		 */
		if (bind(sock, addr->ai_addr, addr->ai_addrlen) < 0)
		{
			pg_log_error("Could not bind socket: %m");
			closesocket(sock);
			continue;
		}
		ret = listen(sock, n_peers);
		if (ret < 0)
		{
			pg_log_error("Could not listen: %m");
			closesocket(sock);
			continue;
		}
		break;
	}
	return sock;
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
			pg_log_error("Socket write failed: %m");
			return false;
		}
		else if (rc == 0)
		{
			pg_log_error("Connection was closed by peer");
			return false;
		}
		size -= rc;
		src += rc;
	}
	return true;
}

bool
ReadSocket(pgsocket sock, void* buf, size_t size)
{
	char* dst = (char*)buf;

	while (size != 0)
	{
		ssize_t rc = recv(sock, dst, size, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			pg_log_error("Socket read failed: %m");
			return false;
		}
		else if (rc == 0)
		{
			pg_log_error("Connection was closed by peer");
			return false;
		}
		size -= rc;
		dst += rc;
	}
	return true;
}

bool
ReadSocketNowait(pgsocket sock, void* buf, size_t size)
{
	while (true)
	{
		ssize_t rc = recv(sock, buf, size, MSG_DONTWAIT);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			pg_log_error("Socket read failed: %m");
		}
		else if (rc == 0)
			pg_log_error("Connection was closed by peer");
		else if ((size_t)rc == size)
			return true;
		else
			pg_log_error("Read only %d bytes instread of %d", (int)rc, (int)size);
		return false;
	}
	return true;
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
			pg_log_error("Socket write failed: %m");
			return -1;
		}
		else if (rc == 0)
		{
			pg_log_error("Connection was closed by peer");
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
			pg_log_error("Socket write failed: %m");
			return -1;
		}
		else if (rc == 0)
		{
			pg_log_error("Connection was closed by peer");
			return -1;
		}
		offs += rc;
	}
	return offs;
}

bool
SaveData(int file, void const* data, size_t size, bool do_sync)
{
	if ((size_t)pg_pwrite(file, data, size, 0) != size)
	{
		pg_log_error("Failed to write file: %m");
		return false;
	}
	if (do_sync && fsync(file) < 0)
	{
		pg_log_error("Failed to fsync file: %m");
		return false;
	}
	return true;
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

/*
 * Converts an int32 to network byte order.
 */
void
fe_sendint32(int32 i, char *buf)
{
	uint32		n32 = pg_hton32(i);

	memcpy(buf, &n32, sizeof(n32));
}

/*
 * Converts an int32 from network byte order to native format.
 */
int32
fe_recvint32(char *buf)
{
	uint32		n32;

	memcpy(&n32, buf, sizeof(n32));

	return pg_ntoh32(n32);
}

/*
 * Converts an int16 to network byte order.
 */
void
fe_sendint16(int16 i, char *buf)
{
	uint16		n16 = pg_hton16(i);

	memcpy(buf, &n16, sizeof(n16));
}

/*
 * Converts an int16 from network byte order to native format.
 */
int16
fe_recvint16(char *buf)
{
	uint16		n16;

	memcpy(&n16, buf, sizeof(n16));

	return pg_ntoh16(n16);
}

PGconn *
ConnectSafekeeper(char const* host, char const* port)
{
	const char* const keywords[] = {"dbname", "host", "port", NULL};
	const char* const values[] = {"replication", host, port, NULL};
	PGconn* conn = PQconnectdbParams(keywords, values, true);

	/*
	 * If there is too little memory even to allocate the PGconn object
	 * and PQconnectdbParams returns NULL, we call exit(1) directly.
	 */
	if (!conn)
	{
		pg_log_error("could not connect to safekeeper %s:%s", host, port);
		return NULL;
	}

	if (PQstatus(conn) != CONNECTION_OK)
	{
		pg_log_error("Safekeeper %s:%s: %s", host, port, PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}
	return conn;
}
