/* -------------------------------------------------------------------------
 *
 * auth_delay.c
 *
 * Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/auth_delay/auth_delay.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "libpq/auth.h"
#include "port.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/* GUC Variables */
static int	auth_delay_milliseconds;

/* Original Hook */
static ClientAuthentication_hook_type original_client_auth_hook = NULL;

/*
 * Check authentication
 */
static void
auth_delay_checks(Port *port, int status)
{
	/*
	 * Any other plugins which use ClientAuthentication_hook.
	 */
	if (original_client_auth_hook)
		original_client_auth_hook(port, status);

	/*
	 * Inject a short delay if authentication failed.
	 */
	if (status != STATUS_OK)
	{
		pg_usleep(1000L * auth_delay_milliseconds);
	}
}

/*
 * Module Load Callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables */
	DefineCustomIntVariable("auth_delay.milliseconds",
							"Milliseconds to delay before reporting authentication failure",
							NULL,
							&auth_delay_milliseconds,
							0,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("auth_delay");

	/* Install Hooks */
	original_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = auth_delay_checks;
}
