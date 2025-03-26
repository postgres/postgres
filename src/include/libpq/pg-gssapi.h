/*-------------------------------------------------------------------------
 *
 * pg-gssapi.h
 *       Definitions for including GSSAPI headers
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pg-gssapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_GSSAPI_H
#define PG_GSSAPI_H

#ifdef ENABLE_GSS

/* IWYU pragma: begin_exports */
#if defined(HAVE_GSSAPI_H)
#include <gssapi.h>
#include <gssapi_ext.h>
#else
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>
#endif
/* IWYU pragma: end_exports */

/*
* On Windows, <wincrypt.h> includes a #define for X509_NAME, which breaks our
* ability to use OpenSSL's version of that symbol if <wincrypt.h> is pulled
* in after <openssl/ssl.h> ... and, at least on some builds, it is.  We
* can't reliably fix that by re-ordering #includes, because libpq/libpq-be.h
* #includes <openssl/ssl.h>.  Instead, just zap the #define again here.
*/
#ifdef X509_NAME
#undef X509_NAME
#endif

#endif							/* ENABLE_GSS */

#endif							/* PG_GSSAPI_H */
