/* contrib/sslinfo/sslinfo--unpackaged--1.0.sql */

ALTER EXTENSION sslinfo ADD function ssl_client_serial();
ALTER EXTENSION sslinfo ADD function ssl_is_used();
ALTER EXTENSION sslinfo ADD function ssl_client_cert_present();
ALTER EXTENSION sslinfo ADD function ssl_client_dn_field(text);
ALTER EXTENSION sslinfo ADD function ssl_issuer_field(text);
ALTER EXTENSION sslinfo ADD function ssl_client_dn();
ALTER EXTENSION sslinfo ADD function ssl_issuer_dn();

-- These functions were not in 9.0:

CREATE FUNCTION ssl_version() RETURNS text
AS 'MODULE_PATHNAME', 'ssl_version'
LANGUAGE C STRICT;

CREATE FUNCTION ssl_cipher() RETURNS text
AS 'MODULE_PATHNAME', 'ssl_cipher'
LANGUAGE C STRICT;
