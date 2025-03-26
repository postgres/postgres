/*
 * module for PostgreSQL to access client SSL certificate information
 *
 * Written by Victor B. Wagner <vitus@cryptocom.ru>, Cryptocom LTD
 * This file is distributed under BSD-style license.
 *
 * contrib/sslinfo/sslinfo.c
 */

#include "postgres.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>

#include "access/htup_details.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(
					.name = "sslinfo",
					.version = PG_VERSION
);

static Datum X509_NAME_field_to_text(X509_NAME *name, text *fieldName);
static Datum ASN1_STRING_to_text(ASN1_STRING *str);

/*
 * Function context for data persisting over repeated calls.
 */
typedef struct
{
	TupleDesc	tupdesc;
} SSLExtensionInfoContext;

/*
 * Indicates whether current session uses SSL
 *
 * Function has no arguments.  Returns bool.  True if current session
 * is SSL session and false if it is local or non-ssl session.
 */
PG_FUNCTION_INFO_V1(ssl_is_used);
Datum
ssl_is_used(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(MyProcPort->ssl_in_use);
}


/*
 * Returns SSL version currently in use.
 */
PG_FUNCTION_INFO_V1(ssl_version);
Datum
ssl_version(PG_FUNCTION_ARGS)
{
	const char *version;

	if (!MyProcPort->ssl_in_use)
		PG_RETURN_NULL();

	version = be_tls_get_version(MyProcPort);
	if (version == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(version));
}


/*
 * Returns SSL cipher currently in use.
 */
PG_FUNCTION_INFO_V1(ssl_cipher);
Datum
ssl_cipher(PG_FUNCTION_ARGS)
{
	const char *cipher;

	if (!MyProcPort->ssl_in_use)
		PG_RETURN_NULL();

	cipher = be_tls_get_cipher(MyProcPort);
	if (cipher == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(cipher));
}


/*
 * Indicates whether current client provided a certificate
 *
 * Function has no arguments.  Returns bool.  True if current session
 * is SSL session and client certificate is verified, otherwise false.
 */
PG_FUNCTION_INFO_V1(ssl_client_cert_present);
Datum
ssl_client_cert_present(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(MyProcPort->peer_cert_valid);
}


/*
 * Returns serial number of certificate used to establish current
 * session
 *
 * Function has no arguments.  It returns the certificate serial
 * number as numeric or null if current session doesn't use SSL or if
 * SSL connection is established without sending client certificate.
 */
PG_FUNCTION_INFO_V1(ssl_client_serial);
Datum
ssl_client_serial(PG_FUNCTION_ARGS)
{
	char decimal[NAMEDATALEN];
	Datum		result;

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	be_tls_get_peer_serial(MyProcPort, decimal, NAMEDATALEN);

	if (!*decimal)
		PG_RETURN_NULL();

	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(decimal),
								 ObjectIdGetDatum(0),
								 Int32GetDatum(-1));
	return result;
}


/*
 * Converts OpenSSL ASN1_STRING structure into text
 *
 * Converts ASN1_STRING into text, converting all the characters into
 * current database encoding if possible.  Any invalid characters are
 * replaced by question marks.
 *
 * Parameter: str - OpenSSL ASN1_STRING structure.  Memory management
 * of this structure is responsibility of caller.
 *
 * Returns Datum, which can be directly returned from a C language SQL
 * function.
 */
static Datum
ASN1_STRING_to_text(ASN1_STRING *str)
{
	BIO		   *membuf;
	size_t		size;
	char		nullterm;
	char	   *sp;
	char	   *dp;
	text	   *result;

	membuf = BIO_new(BIO_s_mem());
	if (membuf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create OpenSSL BIO structure")));
	(void) BIO_set_close(membuf, BIO_CLOSE);
	ASN1_STRING_print_ex(membuf, str,
						 ((ASN1_STRFLGS_RFC2253 & ~ASN1_STRFLGS_ESC_MSB)
						  | ASN1_STRFLGS_UTF8_CONVERT));
	/* ensure null termination of the BIO's content */
	nullterm = '\0';
	BIO_write(membuf, &nullterm, 1);
	size = BIO_get_mem_data(membuf, &sp);
	dp = pg_any_to_server(sp, size - 1, PG_UTF8);
	result = cstring_to_text(dp);
	if (dp != sp)
		pfree(dp);
	if (BIO_free(membuf) != 1)
		elog(ERROR, "could not free OpenSSL BIO structure");

	PG_RETURN_TEXT_P(result);
}


/*
 * Returns specified field of specified X509_NAME structure
 *
 * Common part of ssl_client_dn and ssl_issuer_dn functions.
 *
 * Parameter: X509_NAME *name - either subject or issuer of certificate
 * Parameter: text fieldName  - field name string like 'CN' or commonName
 *			  to be looked up in the OpenSSL ASN1 OID database
 *
 * Returns result of ASN1_STRING_to_text applied to appropriate
 * part of name
 */
static Datum
X509_NAME_field_to_text(X509_NAME *name, text *fieldName)
{
	char	   *string_fieldname;
	int			nid,
				index;
	ASN1_STRING *data;

	string_fieldname = text_to_cstring(fieldName);
	nid = OBJ_txt2nid(string_fieldname);
	if (nid == NID_undef)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid X.509 field name: \"%s\"",
						string_fieldname)));
	pfree(string_fieldname);
	index = X509_NAME_get_index_by_NID(name, nid, -1);
	if (index < 0)
		return (Datum) 0;
	data = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index));
	return ASN1_STRING_to_text(data);
}


/*
 * Returns specified field of client certificate distinguished name
 *
 * Receives field name (like 'commonName' and 'emailAddress') and
 * returns appropriate part of certificate subject converted into
 * database encoding.
 *
 * Parameter: fieldname text - will be looked up in OpenSSL object
 * identifier database
 *
 * Returns text string with appropriate value.
 *
 * Throws an error if argument cannot be converted into ASN1 OID by
 * OpenSSL.  Returns null if no client certificate is present, or if
 * there is no field with such name in the certificate.
 */
PG_FUNCTION_INFO_V1(ssl_client_dn_field);
Datum
ssl_client_dn_field(PG_FUNCTION_ARGS)
{
	text	   *fieldname = PG_GETARG_TEXT_PP(0);
	Datum		result;

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	result = X509_NAME_field_to_text(X509_get_subject_name(MyProcPort->peer), fieldname);

	if (!result)
		PG_RETURN_NULL();
	else
		return result;
}


/*
 * Returns specified field of client certificate issuer name
 *
 * Receives field name (like 'commonName' and 'emailAddress') and
 * returns appropriate part of certificate subject converted into
 * database encoding.
 *
 * Parameter: fieldname text - would be looked up in OpenSSL object
 * identifier database
 *
 * Returns text string with appropriate value.
 *
 * Throws an error if argument cannot be converted into ASN1 OID by
 * OpenSSL.  Returns null if no client certificate is present, or if
 * there is no field with such name in the certificate.
 */
PG_FUNCTION_INFO_V1(ssl_issuer_field);
Datum
ssl_issuer_field(PG_FUNCTION_ARGS)
{
	text	   *fieldname = PG_GETARG_TEXT_PP(0);
	Datum		result;

	if (!(MyProcPort->peer))
		PG_RETURN_NULL();

	result = X509_NAME_field_to_text(X509_get_issuer_name(MyProcPort->peer), fieldname);

	if (!result)
		PG_RETURN_NULL();
	else
		return result;
}


/*
 * Returns current client certificate subject as one string
 *
 * This function returns distinguished name (subject) of the client
 * certificate used in the current SSL connection, converting it into
 * the current database encoding.
 *
 * Returns text datum.
 */
PG_FUNCTION_INFO_V1(ssl_client_dn);
Datum
ssl_client_dn(PG_FUNCTION_ARGS)
{
	char		subject[NAMEDATALEN];

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	be_tls_get_peer_subject_name(MyProcPort, subject, NAMEDATALEN);

	if (!*subject)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(subject));
}


/*
 * Returns current client certificate issuer as one string
 *
 * This function returns issuer's distinguished name of the client
 * certificate used in the current SSL connection, converting it into
 * the current database encoding.
 *
 * Returns text datum.
 */
PG_FUNCTION_INFO_V1(ssl_issuer_dn);
Datum
ssl_issuer_dn(PG_FUNCTION_ARGS)
{
	char		issuer[NAMEDATALEN];

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	be_tls_get_peer_issuer_name(MyProcPort, issuer, NAMEDATALEN);

	if (!*issuer)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(issuer));
}


/*
 * Returns information about available SSL extensions.
 *
 * Returns setof record made of the following values:
 * - name of the extension.
 * - value of the extension.
 * - critical status of the extension.
 */
PG_FUNCTION_INFO_V1(ssl_extension_info);
Datum
ssl_extension_info(PG_FUNCTION_ARGS)
{
	X509	   *cert = MyProcPort->peer;
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;
	MemoryContext oldcontext;
	SSLExtensionInfoContext *fctx;

	if (SRF_IS_FIRSTCALL())
	{

		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * Switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence */
		fctx = (SSLExtensionInfoContext *) palloc(sizeof(SSLExtensionInfoContext));

		/* Construct tuple descriptor */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context that cannot accept type record")));
		fctx->tupdesc = BlessTupleDesc(tupdesc);

		/* Set max_calls as a count of extensions in certificate */
		max_calls = cert != NULL ? X509_get_ext_count(cert) : 0;

		if (max_calls > 0)
		{
			/* got results, keep track of them */
			funcctx->max_calls = max_calls;
			funcctx->user_fctx = fctx;
		}
		else
		{
			/* fast track when no results */
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * Initialize per-call variables.
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	fctx = funcctx->user_fctx;

	/* do while there are more left to send */
	if (call_cntr < max_calls)
	{
		Datum		values[3];
		bool		nulls[3];
		char	   *buf;
		HeapTuple	tuple;
		Datum		result;
		BIO		   *membuf;
		X509_EXTENSION *ext;
		ASN1_OBJECT *obj;
		int			nid;
		int			len;

		/* need a BIO for this */
		membuf = BIO_new(BIO_s_mem());
		if (membuf == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create OpenSSL BIO structure")));

		/* Get the extension from the certificate */
		ext = X509_get_ext(cert, call_cntr);
		obj = X509_EXTENSION_get_object(ext);

		/* Get the extension name */
		nid = OBJ_obj2nid(obj);
		if (nid == NID_undef)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unknown OpenSSL extension in certificate at position %d",
							call_cntr)));
		values[0] = CStringGetTextDatum(OBJ_nid2sn(nid));
		nulls[0] = false;

		/* Get the extension value */
		if (X509V3_EXT_print(membuf, ext, 0, 0) <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not print extension value in certificate at position %d",
							call_cntr)));
		len = BIO_get_mem_data(membuf, &buf);
		values[1] = PointerGetDatum(cstring_to_text_with_len(buf, len));
		nulls[1] = false;

		/* Get critical status */
		values[2] = BoolGetDatum(X509_EXTENSION_get_critical(ext));
		nulls[2] = false;

		/* Build tuple */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		if (BIO_free(membuf) != 1)
			elog(ERROR, "could not free OpenSSL BIO structure");

		SRF_RETURN_NEXT(funcctx, result);
	}

	/* All done */
	SRF_RETURN_DONE(funcctx);
}
