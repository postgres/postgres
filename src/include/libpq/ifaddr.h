/*-------------------------------------------------------------------------
 *
 * ifaddr.h
 *	  IP netmask calculations, and enumerating network interfaces.
 *
 * Copyright (c) 2003-2020, PostgreSQL Global Development Group
 *
 * src/include/libpq/ifaddr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef IFADDR_H
#define IFADDR_H

#include "libpq/pqcomm.h"		/* pgrminclude ignore */

typedef void (*PgIfAddrCallback) (struct sockaddr *addr,
								  struct sockaddr *netmask,
								  void *cb_data);

extern int	pg_range_sockaddr(const struct sockaddr_storage *addr,
							  const struct sockaddr_storage *netaddr,
							  const struct sockaddr_storage *netmask);

extern int	pg_sockaddr_cidr_mask(struct sockaddr_storage *mask,
								  char *numbits, int family);

extern int	pg_foreach_ifaddr(PgIfAddrCallback callback, void *cb_data);

#endif							/* IFADDR_H */
