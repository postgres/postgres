/*-------------------------------------------------------------------------
 *
 * fe-gssapi-common.c
 *     The front-end (client) GSSAPI common code
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *      src/interfaces/libpq/fe-gssapi-common.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "fe-gssapi-common.h"

#include "libpq-int.h"
#include "pqexpbuffer.h"

/*
 * Fetch all errors of a specific type and append to "str".
 * Each error string is preceded by a space.
 */
static void
pg_GSS_error_int(PQExpBuffer str, OM_uint32 stat, int type)
{
	OM_uint32	lmin_s;
	gss_buffer_desc lmsg;
	OM_uint32	msg_ctx = 0;

	do
	{
		if (gss_display_status(&lmin_s, stat, type, GSS_C_NO_OID,
							   &msg_ctx, &lmsg) != GSS_S_COMPLETE)
			break;
		appendPQExpBufferChar(str, ' ');
		appendBinaryPQExpBuffer(str, lmsg.value, lmsg.length);
		gss_release_buffer(&lmin_s, &lmsg);
	} while (msg_ctx);
}

/*
 * GSSAPI errors contain two parts; put both into conn->errorMessage.
 */
void
pg_GSS_error(const char *mprefix, PGconn *conn,
			 OM_uint32 maj_stat, OM_uint32 min_stat)
{
	appendPQExpBuffer(&conn->errorMessage, "%s:", mprefix);
	pg_GSS_error_int(&conn->errorMessage, maj_stat, GSS_C_GSS_CODE);
	appendPQExpBufferChar(&conn->errorMessage, ':');
	pg_GSS_error_int(&conn->errorMessage, min_stat, GSS_C_MECH_CODE);
	appendPQExpBufferChar(&conn->errorMessage, '\n');
}

/*
 * Check if we can acquire credentials at all (and yield them if so).
 */
bool
pg_GSS_have_cred_cache(gss_cred_id_t *cred_out)
{
	OM_uint32	major,
				minor;
	gss_cred_id_t cred = GSS_C_NO_CREDENTIAL;

	major = gss_acquire_cred(&minor, GSS_C_NO_NAME, 0, GSS_C_NO_OID_SET,
							 GSS_C_INITIATE, &cred, NULL, NULL);
	if (major != GSS_S_COMPLETE)
	{
		*cred_out = NULL;
		return false;
	}
	*cred_out = cred;
	return true;
}

/*
 * Try to load service name for a connection
 */
int
pg_GSS_load_servicename(PGconn *conn)
{
	OM_uint32	maj_stat,
				min_stat;
	int			maxlen;
	gss_buffer_desc temp_gbuf;
	char	   *host;

	if (conn->gtarg_nam != NULL)
		/* Already taken care of - move along */
		return STATUS_OK;

	host = PQhost(conn);
	if (!(host && host[0] != '\0'))
	{
		libpq_append_conn_error(conn, "host name must be specified");
		return STATUS_ERROR;
	}

	/*
	 * Import service principal name so the proper ticket can be acquired by
	 * the GSSAPI system.
	 */
	maxlen = strlen(conn->krbsrvname) + strlen(host) + 2;
	temp_gbuf.value = (char *) malloc(maxlen);
	if (!temp_gbuf.value)
	{
		libpq_append_conn_error(conn, "out of memory");
		return STATUS_ERROR;
	}
	snprintf(temp_gbuf.value, maxlen, "%s@%s",
			 conn->krbsrvname, host);
	temp_gbuf.length = strlen(temp_gbuf.value);

	maj_stat = gss_import_name(&min_stat, &temp_gbuf,
							   GSS_C_NT_HOSTBASED_SERVICE, &conn->gtarg_nam);
	free(temp_gbuf.value);

	if (maj_stat != GSS_S_COMPLETE)
	{
		pg_GSS_error(libpq_gettext("GSSAPI name import error"),
					 conn,
					 maj_stat, min_stat);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}
