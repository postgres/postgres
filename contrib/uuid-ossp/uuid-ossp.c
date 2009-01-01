/*-------------------------------------------------------------------------
 *
 * UUID generation functions using the OSSP UUID library
 *
 * Copyright (c) 2007-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/contrib/uuid-ossp/uuid-ossp.c,v 1.10 2009/01/01 18:21:19 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

/*
 * There's some confusion over the location of the uuid.h header file.
 * On Debian, it's installed as ossp/uuid.h, while on Fedora, or if you
 * install ossp-uuid from a tarball, it's installed as uuid.h. Don't know
 * what other systems do.
 */
#ifdef HAVE_OSSP_UUID_H
#include <ossp/uuid.h>
#else
#ifdef HAVE_UUID_H
#include <uuid.h>
#else
#error OSSP uuid.h not found
#endif
#endif

/* better both be 16 */
#if (UUID_LEN != UUID_LEN_BIN)
#error UUID length mismatch
#endif


PG_MODULE_MAGIC;


Datum		uuid_nil(PG_FUNCTION_ARGS);
Datum		uuid_ns_dns(PG_FUNCTION_ARGS);
Datum		uuid_ns_url(PG_FUNCTION_ARGS);
Datum		uuid_ns_oid(PG_FUNCTION_ARGS);
Datum		uuid_ns_x500(PG_FUNCTION_ARGS);

Datum		uuid_generate_v1(PG_FUNCTION_ARGS);
Datum		uuid_generate_v1mc(PG_FUNCTION_ARGS);
Datum		uuid_generate_v3(PG_FUNCTION_ARGS);
Datum		uuid_generate_v4(PG_FUNCTION_ARGS);
Datum		uuid_generate_v5(PG_FUNCTION_ARGS);


PG_FUNCTION_INFO_V1(uuid_nil);
PG_FUNCTION_INFO_V1(uuid_ns_dns);
PG_FUNCTION_INFO_V1(uuid_ns_url);
PG_FUNCTION_INFO_V1(uuid_ns_oid);
PG_FUNCTION_INFO_V1(uuid_ns_x500);

PG_FUNCTION_INFO_V1(uuid_generate_v1);
PG_FUNCTION_INFO_V1(uuid_generate_v1mc);
PG_FUNCTION_INFO_V1(uuid_generate_v3);
PG_FUNCTION_INFO_V1(uuid_generate_v4);
PG_FUNCTION_INFO_V1(uuid_generate_v5);

static void
pguuid_complain(uuid_rc_t rc)
{
	char	*err = uuid_error(rc);

	if (err != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("OSSP uuid library failure: %s", err)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("OSSP uuid library failure: error code %d", rc)));
}

static char *
uuid_to_string(const uuid_t * uuid)
{
	char	   *buf = palloc(UUID_LEN_STR + 1);
	void	   *ptr = buf;
	size_t		len = UUID_LEN_STR + 1;
	uuid_rc_t	rc;

	rc = uuid_export(uuid, UUID_FMT_STR, &ptr, &len);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);

	return buf;
}


static void
string_to_uuid(const char *str, uuid_t * uuid)
{
	uuid_rc_t	rc;

	rc = uuid_import(uuid, UUID_FMT_STR, str, UUID_LEN_STR + 1);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
}


static Datum
special_uuid_value(const char *name)
{
	uuid_t	   *uuid;
	char	   *str;
	uuid_rc_t	rc;

	rc = uuid_create(&uuid);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	rc = uuid_load(uuid, name);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	str = uuid_to_string(uuid);
	rc = uuid_destroy(uuid);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);

	return DirectFunctionCall1(uuid_in, CStringGetDatum(str));
}


Datum
uuid_nil(PG_FUNCTION_ARGS)
{
	return special_uuid_value("nil");
}


Datum
uuid_ns_dns(PG_FUNCTION_ARGS)
{
	return special_uuid_value("ns:DNS");
}


Datum
uuid_ns_url(PG_FUNCTION_ARGS)
{
	return special_uuid_value("ns:URL");
}


Datum
uuid_ns_oid(PG_FUNCTION_ARGS)
{
	return special_uuid_value("ns:OID");
}


Datum
uuid_ns_x500(PG_FUNCTION_ARGS)
{
	return special_uuid_value("ns:X500");
}


static Datum
uuid_generate_internal(int mode, const uuid_t * ns, const char *name)
{
	uuid_t	   *uuid;
	char	   *str;
	uuid_rc_t	rc;

	rc = uuid_create(&uuid);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	rc = uuid_make(uuid, mode, ns, name);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	str = uuid_to_string(uuid);
	rc = uuid_destroy(uuid);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);

	return DirectFunctionCall1(uuid_in, CStringGetDatum(str));
}


Datum
uuid_generate_v1(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V1, NULL, NULL);
}


Datum
uuid_generate_v1mc(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V1 | UUID_MAKE_MC, NULL, NULL);
}


static Datum
uuid_generate_v35_internal(int mode, pg_uuid_t *ns, text *name)
{
	uuid_t	   *ns_uuid;
	Datum		result;
	uuid_rc_t	rc;

	rc = uuid_create(&ns_uuid);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	string_to_uuid(DatumGetCString(DirectFunctionCall1(uuid_out, UUIDPGetDatum(ns))),
				   ns_uuid);

	result = uuid_generate_internal(mode,
									ns_uuid,
									text_to_cstring(name));

	rc = uuid_destroy(ns_uuid);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);

	return result;
}


Datum
uuid_generate_v3(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *ns = PG_GETARG_UUID_P(0);
	text	   *name = PG_GETARG_TEXT_P(1);

	return uuid_generate_v35_internal(UUID_MAKE_V3, ns, name);
}


Datum
uuid_generate_v4(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V4, NULL, NULL);
}


Datum
uuid_generate_v5(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *ns = PG_GETARG_UUID_P(0);
	text	   *name = PG_GETARG_TEXT_P(1);

	return uuid_generate_v35_internal(UUID_MAKE_V5, ns, name);
}
