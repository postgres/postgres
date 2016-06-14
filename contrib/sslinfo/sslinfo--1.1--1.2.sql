/* contrib/sslinfo/sslinfo--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION sslinfo UPDATE TO '1.2'" to load this file. \quit

ALTER FUNCTION ssl_client_serial() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_is_used() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_version() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_cipher() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_client_cert_present() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_client_dn_field(text) PARALLEL RESTRICTED;
ALTER FUNCTION ssl_issuer_field(text) PARALLEL RESTRICTED;
ALTER FUNCTION ssl_client_dn() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_issuer_dn() PARALLEL RESTRICTED;
ALTER FUNCTION ssl_extension_info() PARALLEL RESTRICTED;
