/*
 * dummy_seclabel.c
 *
 * Dummy security label provider.
 *
 * This module does not provide anything worthwhile from a security
 * perspective, but allows regression testing independent of platform-specific
 * features like SELinux.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */
#include "postgres.h"

#include "commands/seclabel.h"
#include "miscadmin.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/* Entrypoint of the module */
void		_PG_init(void);

static void
dummy_object_relabel(const ObjectAddress *object, const char *seclabel)
{
	if (seclabel == NULL ||
		strcmp(seclabel, "unclassified") == 0 ||
		strcmp(seclabel, "classified") == 0)
		return;

	if (strcmp(seclabel, "secret") == 0 ||
		strcmp(seclabel, "top secret") == 0)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("only superuser can set '%s' label", seclabel)));
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_NAME),
			 errmsg("'%s' is not a valid security label", seclabel)));
}

void
_PG_init(void)
{
	register_label_provider("dummy", dummy_object_relabel);
}
