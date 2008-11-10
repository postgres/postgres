/*
 * module for PostgreSQL to access client SSL certificate information
 *
 * Written by Victor B. Wagner <vitus@cryptocom.ru>, Cryptocom LTD
 * This file is distributed under BSD-style license.
 *
 * $PostgreSQL: pgsql/contrib/sslinfo/sslinfo.c,v 1.5.2.1 2008/11/10 14:57:53 tgl Exp $
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/numeric.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "mb/pg_wchar.h"

#include <openssl/x509.h>
#include <openssl/asn1.h>


PG_MODULE_MAGIC;


Datum		ssl_is_used(PG_FUNCTION_ARGS);
Datum		ssl_client_cert_present(PG_FUNCTION_ARGS);
Datum		ssl_client_serial(PG_FUNCTION_ARGS);
Datum		ssl_client_dn_field(PG_FUNCTION_ARGS);
Datum		ssl_issuer_field(PG_FUNCTION_ARGS);
Datum		ssl_client_dn(PG_FUNCTION_ARGS);
Datum		ssl_issuer_dn(PG_FUNCTION_ARGS);
Datum		X509_NAME_field_to_text(X509_NAME *name, text *fieldName);
Datum		X509_NAME_to_text(X509_NAME *name);
Datum		ASN1_STRING_to_text(ASN1_STRING *str);


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
	PG_RETURN_BOOL(MyProcPort->ssl != NULL);
}


/*
 * Indicates whether current client have provided a certificate
 *
 * Function has no arguments.  Returns bool.  True if current session
 * is SSL session and client certificate is verified, otherwise false.
 */
PG_FUNCTION_INFO_V1(ssl_client_cert_present);
Datum
ssl_client_cert_present(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(MyProcPort->peer != NULL);
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
	Datum		result;
	Port	   *port = MyProcPort;
	X509	   *peer = port->peer;
	ASN1_INTEGER *serial = NULL;
	BIGNUM	   *b;
	char	   *decimal;

	if (!peer)
		PG_RETURN_NULL();
	serial = X509_get_serialNumber(peer);
	b = ASN1_INTEGER_to_BN(serial, NULL);
	decimal = BN_bn2dec(b);

	BN_free(b);
	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(decimal),
								 ObjectIdGetDatum(0),
								 Int32GetDatum(-1));
	OPENSSL_free(decimal);
	return result;
}


/*
 * Converts OpenSSL ASN1_STRING structure into text
 *
 * Converts ASN1_STRING into text, converting all the characters into
 * current database encoding if possible.  Any invalid characters are
 * replaced by question marks.
 *
 * Parameter: str - OpenSSL ASN1_STRING structure.	Memory managment
 * of this structure is responsibility of caller.
 *
 * Returns Datum, which can be directly returned from a C language SQL
 * function.
 */
Datum
ASN1_STRING_to_text(ASN1_STRING *str)
{
	BIO		   *membuf = NULL;
	size_t		size,
				outlen;
	char	   *sp;
	char	   *dp;
	text	   *result;

	membuf = BIO_new(BIO_s_mem());
	(void) BIO_set_close(membuf, BIO_CLOSE);
	ASN1_STRING_print_ex(membuf, str,
						 ((ASN1_STRFLGS_RFC2253 & ~ASN1_STRFLGS_ESC_MSB)
						  | ASN1_STRFLGS_UTF8_CONVERT));

	outlen = 0;
	BIO_write(membuf, &outlen, 1);
	size = BIO_get_mem_data(membuf, &sp);
	dp = (char *) pg_do_encoding_conversion((unsigned char *) sp,
											size - 1,
											PG_UTF8,
											GetDatabaseEncoding());
	outlen = strlen(dp);
	result = palloc(VARHDRSZ + outlen);
	memcpy(VARDATA(result), dp, outlen);
	if (dp != sp)
		pfree(dp);

	BIO_free(membuf);
	VARATT_SIZEP(result) = outlen + VARHDRSZ;
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
Datum
X509_NAME_field_to_text(X509_NAME *name, text *fieldName)
{
	char	   *sp;
	char	   *string_fieldname;
	char	   *dp;
	size_t		name_len = VARSIZE(fieldName) - VARHDRSZ;
	int			nid,
				index,
				i;
	ASN1_STRING *data;

	string_fieldname = palloc(name_len + 1);
	sp = VARDATA(fieldName);
	dp = string_fieldname;
	for (i = 0; i < name_len; i++)
		*dp++ = *sp++;
	*dp = '\0';
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
	text	   *fieldname = PG_GETARG_TEXT_P(0);
	Datum		result;

	if (!(MyProcPort->peer))
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
	text	   *fieldname = PG_GETARG_TEXT_P(0);
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
 * Equivalent of X509_NAME_oneline that respects encoding
 *
 * This function converts X509_NAME structure to the text variable
 * converting all textual data into current database encoding.
 *
 * Parameter: X509_NAME *name X509_NAME structure to be converted
 *
 * Returns: text datum which contains string representation of
 * X509_NAME
 */
Datum
X509_NAME_to_text(X509_NAME *name)
{
	BIO		   *membuf = BIO_new(BIO_s_mem());
	int			i,
				nid,
				count = X509_NAME_entry_count(name);
	X509_NAME_ENTRY *e;
	ASN1_STRING *v;

	const char *field_name;
	size_t		size,
				outlen;
	char	   *sp;
	char	   *dp;
	text	   *result;

	(void) BIO_set_close(membuf, BIO_CLOSE);
	for (i = 0; i < count; i++)
	{
		e = X509_NAME_get_entry(name, i);
		nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(e));
		v = X509_NAME_ENTRY_get_data(e);
		field_name = OBJ_nid2sn(nid);
		if (!field_name)
			field_name = OBJ_nid2ln(nid);
		BIO_printf(membuf, "/%s=", field_name);
		ASN1_STRING_print_ex(membuf, v,
							 ((ASN1_STRFLGS_RFC2253 & ~ASN1_STRFLGS_ESC_MSB)
							  | ASN1_STRFLGS_UTF8_CONVERT));
	}

	i = 0;
	BIO_write(membuf, &i, 1);
	size = BIO_get_mem_data(membuf, &sp);
	dp = (char *) pg_do_encoding_conversion((unsigned char *) sp,
											size - 1,
											PG_UTF8,
											GetDatabaseEncoding());
	outlen = strlen(dp);
	result = palloc(VARHDRSZ + outlen);
	memcpy(VARDATA(result), dp, outlen);
	if (dp != sp)
		pfree(dp);

	BIO_free(membuf);
	VARATT_SIZEP(result) = outlen + VARHDRSZ;
	PG_RETURN_TEXT_P(result);
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
	if (!(MyProcPort->peer))
		PG_RETURN_NULL();
	return X509_NAME_to_text(X509_get_subject_name(MyProcPort->peer));
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
	if (!(MyProcPort->peer))
		PG_RETURN_NULL();
	return X509_NAME_to_text(X509_get_issuer_name(MyProcPort->peer));
}
