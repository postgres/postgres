
#include "postgres.h"

#include "keyring/keyring_kmip.h"
#include "catalog/keyring_min.h"
#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

void
kmip_ereport(bool throw_error, const char *msg, int errCode)
{
	int			ereport_level = throw_error ? ERROR : WARNING;

	if (errCode != 0)
	{
		ereport(ereport_level, (errmsg(msg, errCode)));
	}
	else
	{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
		/* TODO: how to do this properly? */
		elog(ereport_level, (msg));
#pragma GCC diagnostic pop
	}
}
