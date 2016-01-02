/*-------------------------------------------------------------------------
 *
 * UUID generation functions using the BSD, E2FS or OSSP UUID library
 *
 * Copyright (c) 2007-2016, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2009 Andrew Gierth
 *
 * contrib/uuid-ossp/uuid-ossp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

/*
 * It's possible that there's more than one uuid.h header file present.
 * We expect configure to set the HAVE_ symbol for only the one we want.
 *
 * BSD includes a uuid_hash() function that conflicts with the one in
 * builtins.h; we #define it out of the way.
 */
#define uuid_hash bsd_uuid_hash

#ifdef HAVE_UUID_H
#include <uuid.h>
#endif
#ifdef HAVE_OSSP_UUID_H
#include <ossp/uuid.h>
#endif
#ifdef HAVE_UUID_UUID_H
#include <uuid/uuid.h>
#endif

#undef uuid_hash

/*
 * Some BSD variants offer md5 and sha1 implementations but Linux does not,
 * so we use a copy of the ones from pgcrypto.  Not needed with OSSP, though.
 */
#ifndef HAVE_UUID_OSSP
#include "md5.h"
#include "sha1.h"
#endif


/* Check our UUID length against OSSP's; better both be 16 */
#if defined(HAVE_UUID_OSSP) && (UUID_LEN != UUID_LEN_BIN)
#error UUID length mismatch
#endif

/* Define some constants like OSSP's, to make the code more readable */
#ifndef HAVE_UUID_OSSP
#define UUID_MAKE_MC 0
#define UUID_MAKE_V1 1
#define UUID_MAKE_V2 2
#define UUID_MAKE_V3 3
#define UUID_MAKE_V4 4
#define UUID_MAKE_V5 5
#endif

/*
 * A DCE 1.1 compatible source representation of UUIDs, derived from
 * the BSD implementation.  BSD already has this; OSSP doesn't need it.
 */
#ifdef HAVE_UUID_E2FS
typedef struct
{
	uint32_t	time_low;
	uint16_t	time_mid;
	uint16_t	time_hi_and_version;
	uint8_t		clock_seq_hi_and_reserved;
	uint8_t		clock_seq_low;
	uint8_t		node[6];
} dce_uuid_t;
#else
#define dce_uuid_t uuid_t
#endif

/* If not OSSP, we need some endianness-manipulation macros */
#ifndef HAVE_UUID_OSSP

#define UUID_TO_NETWORK(uu) \
do { \
	uu.time_low = htonl(uu.time_low); \
	uu.time_mid = htons(uu.time_mid); \
	uu.time_hi_and_version = htons(uu.time_hi_and_version); \
} while (0)

#define UUID_TO_LOCAL(uu) \
do { \
	uu.time_low = ntohl(uu.time_low); \
	uu.time_mid = ntohs(uu.time_mid); \
	uu.time_hi_and_version = ntohs(uu.time_hi_and_version); \
} while (0)

#define UUID_V3_OR_V5(uu, v) \
do { \
	uu.time_hi_and_version &= 0x0FFF; \
	uu.time_hi_and_version |= (v << 12); \
	uu.clock_seq_hi_and_reserved &= 0x3F; \
	uu.clock_seq_hi_and_reserved |= 0x80; \
} while(0)

#endif   /* !HAVE_UUID_OSSP */

PG_MODULE_MAGIC;

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

#ifdef HAVE_UUID_OSSP

static void
pguuid_complain(uuid_rc_t rc)
{
	char	   *err = uuid_error(rc);

	if (err != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("OSSP uuid library failure: %s", err)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("OSSP uuid library failure: error code %d", rc)));
}

/*
 * We create a uuid_t object just once per session and re-use it for all
 * operations in this module.  OSSP UUID caches the system MAC address and
 * other state in this object.  Reusing the object has a number of benefits:
 * saving the cycles needed to fetch the system MAC address over and over,
 * reducing the amount of entropy we draw from /dev/urandom, and providing a
 * positive guarantee that successive generated V1-style UUIDs don't collide.
 * (On a machine fast enough to generate multiple UUIDs per microsecond,
 * or whatever the system's wall-clock resolution is, we'd otherwise risk
 * collisions whenever random initialization of the uuid_t's clock sequence
 * value chanced to produce duplicates.)
 *
 * However: when we're doing V3 or V5 UUID creation, uuid_make needs two
 * uuid_t objects, one holding the namespace UUID and one for the result.
 * It's unspecified whether it's safe to use the same uuid_t for both cases,
 * so let's cache a second uuid_t for use as the namespace holder object.
 */
static uuid_t *
get_cached_uuid_t(int which)
{
	static uuid_t *cached_uuid[2] = {NULL, NULL};

	if (cached_uuid[which] == NULL)
	{
		uuid_rc_t	rc;

		rc = uuid_create(&cached_uuid[which]);
		if (rc != UUID_RC_OK)
		{
			cached_uuid[which] = NULL;
			pguuid_complain(rc);
		}
	}
	return cached_uuid[which];
}

static char *
uuid_to_string(const uuid_t *uuid)
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
string_to_uuid(const char *str, uuid_t *uuid)
{
	uuid_rc_t	rc;

	rc = uuid_import(uuid, UUID_FMT_STR, str, UUID_LEN_STR + 1);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
}


static Datum
special_uuid_value(const char *name)
{
	uuid_t	   *uuid = get_cached_uuid_t(0);
	char	   *str;
	uuid_rc_t	rc;

	rc = uuid_load(uuid, name);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	str = uuid_to_string(uuid);

	return DirectFunctionCall1(uuid_in, CStringGetDatum(str));
}

/* len is unused with OSSP, but we want to have the same number of args */
static Datum
uuid_generate_internal(int mode, const uuid_t *ns, const char *name, int len)
{
	uuid_t	   *uuid = get_cached_uuid_t(0);
	char	   *str;
	uuid_rc_t	rc;

	rc = uuid_make(uuid, mode, ns, name);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	str = uuid_to_string(uuid);

	return DirectFunctionCall1(uuid_in, CStringGetDatum(str));
}


static Datum
uuid_generate_v35_internal(int mode, pg_uuid_t *ns, text *name)
{
	uuid_t	   *ns_uuid = get_cached_uuid_t(1);

	string_to_uuid(DatumGetCString(DirectFunctionCall1(uuid_out,
													   UUIDPGetDatum(ns))),
				   ns_uuid);

	return uuid_generate_internal(mode,
								  ns_uuid,
								  text_to_cstring(name),
								  0);
}

#else							/* !HAVE_UUID_OSSP */

static Datum
uuid_generate_internal(int v, unsigned char *ns, char *ptr, int len)
{
	char		strbuf[40];

	switch (v)
	{
		case 0:			/* constant-value uuids */
			strlcpy(strbuf, ptr, 37);
			break;

		case 1:			/* time/node-based uuids */
			{
#ifdef HAVE_UUID_E2FS
				uuid_t		uu;

				uuid_generate_time(uu);
				uuid_unparse(uu, strbuf);

				/*
				 * PTR, if set, replaces the trailing characters of the uuid;
				 * this is to support v1mc, where a random multicast MAC is
				 * used instead of the physical one
				 */
				if (ptr && len <= 36)
					strcpy(strbuf + (36 - len), ptr);
#else							/* BSD */
				uuid_t		uu;
				uint32_t	status = uuid_s_ok;
				char	   *str = NULL;

				uuid_create(&uu, &status);

				if (status == uuid_s_ok)
				{
					uuid_to_string(&uu, &str, &status);
					if (status == uuid_s_ok)
					{
						strlcpy(strbuf, str, 37);

						/*
						 * PTR, if set, replaces the trailing characters of
						 * the uuid; this is to support v1mc, where a random
						 * multicast MAC is used instead of the physical one
						 */
						if (ptr && len <= 36)
							strcpy(strbuf + (36 - len), ptr);
					}
					if (str)
						free(str);
				}

				if (status != uuid_s_ok)
					ereport(ERROR,
							(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
							 errmsg("uuid library failure: %d",
									(int) status)));
#endif
				break;
			}

		case 3:			/* namespace-based MD5 uuids */
		case 5:			/* namespace-based SHA1 uuids */
			{
				dce_uuid_t	uu;
#ifdef HAVE_UUID_BSD
				uint32_t	status = uuid_s_ok;
				char	   *str = NULL;
#endif

				if (v == 3)
				{
					MD5_CTX		ctx;

					MD5Init(&ctx);
					MD5Update(&ctx, ns, sizeof(uu));
					MD5Update(&ctx, (unsigned char *) ptr, len);
					/* we assume sizeof MD5 result is 16, same as UUID size */
					MD5Final((unsigned char *) &uu, &ctx);
				}
				else
				{
					SHA1_CTX	ctx;
					unsigned char sha1result[SHA1_RESULTLEN];

					SHA1Init(&ctx);
					SHA1Update(&ctx, ns, sizeof(uu));
					SHA1Update(&ctx, (unsigned char *) ptr, len);
					SHA1Final(sha1result, &ctx);
					memcpy(&uu, sha1result, sizeof(uu));
				}

				/* the calculated hash is using local order */
				UUID_TO_NETWORK(uu);
				UUID_V3_OR_V5(uu, v);

#ifdef HAVE_UUID_E2FS
				/* uuid_unparse expects local order */
				UUID_TO_LOCAL(uu);
				uuid_unparse((unsigned char *) &uu, strbuf);
#else							/* BSD */
				uuid_to_string(&uu, &str, &status);

				if (status == uuid_s_ok)
					strlcpy(strbuf, str, 37);

				if (str)
					free(str);

				if (status != uuid_s_ok)
					ereport(ERROR,
							(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
							 errmsg("uuid library failure: %d",
									(int) status)));
#endif
				break;
			}

		case 4:			/* random uuid */
		default:
			{
#ifdef HAVE_UUID_E2FS
				uuid_t		uu;

				uuid_generate_random(uu);
				uuid_unparse(uu, strbuf);
#else							/* BSD */
				snprintf(strbuf, sizeof(strbuf),
						 "%08lx-%04x-%04x-%04x-%04x%08lx",
						 (unsigned long) arc4random(),
						 (unsigned) (arc4random() & 0xffff),
						 (unsigned) ((arc4random() & 0xfff) | 0x4000),
						 (unsigned) ((arc4random() & 0x3fff) | 0x8000),
						 (unsigned) (arc4random() & 0xffff),
						 (unsigned long) arc4random());
#endif
				break;
			}
	}

	return DirectFunctionCall1(uuid_in, CStringGetDatum(strbuf));
}

#endif   /* HAVE_UUID_OSSP */


Datum
uuid_nil(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("nil");
#else
	return uuid_generate_internal(0, NULL,
								  "00000000-0000-0000-0000-000000000000", 36);
#endif
}


Datum
uuid_ns_dns(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:DNS");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b810-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_ns_url(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:URL");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b811-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_ns_oid(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:OID");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b812-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_ns_x500(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:X500");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b814-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_generate_v1(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V1, NULL, NULL, 0);
}


Datum
uuid_generate_v1mc(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	char	   *buf = NULL;
#elif defined(HAVE_UUID_E2FS)
	char		strbuf[40];
	char	   *buf;
	uuid_t		uu;

	uuid_generate_random(uu);

	/* set IEEE802 multicast and local-admin bits */
	((dce_uuid_t *) &uu)->node[0] |= 0x03;

	uuid_unparse(uu, strbuf);
	buf = strbuf + 24;
#else							/* BSD */
	char		buf[16];

	/* set IEEE802 multicast and local-admin bits */
	snprintf(buf, sizeof(buf), "-%04x%08lx",
			 (unsigned) ((arc4random() & 0xffff) | 0x0300),
			 (unsigned long) arc4random());
#endif

	return uuid_generate_internal(UUID_MAKE_V1 | UUID_MAKE_MC, NULL,
								  buf, 13);
}


Datum
uuid_generate_v3(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *ns = PG_GETARG_UUID_P(0);
	text	   *name = PG_GETARG_TEXT_P(1);

#ifdef HAVE_UUID_OSSP
	return uuid_generate_v35_internal(UUID_MAKE_V3, ns, name);
#else
	return uuid_generate_internal(UUID_MAKE_V3, (unsigned char *) ns,
								  VARDATA(name), VARSIZE(name) - VARHDRSZ);
#endif
}


Datum
uuid_generate_v4(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V4, NULL, NULL, 0);
}


Datum
uuid_generate_v5(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *ns = PG_GETARG_UUID_P(0);
	text	   *name = PG_GETARG_TEXT_P(1);

#ifdef HAVE_UUID_OSSP
	return uuid_generate_v35_internal(UUID_MAKE_V5, ns, name);
#else
	return uuid_generate_internal(UUID_MAKE_V5, (unsigned char *) ns,
								  VARDATA(name), VARSIZE(name) - VARHDRSZ);
#endif
}
