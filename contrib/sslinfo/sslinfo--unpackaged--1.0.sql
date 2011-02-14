/* contrib/sslinfo/sslinfo--unpackaged--1.0.sql */

ALTER EXTENSION sslinfo ADD function ssl_client_serial();
ALTER EXTENSION sslinfo ADD function ssl_is_used();
ALTER EXTENSION sslinfo ADD function ssl_version();
ALTER EXTENSION sslinfo ADD function ssl_cipher();
ALTER EXTENSION sslinfo ADD function ssl_client_cert_present();
ALTER EXTENSION sslinfo ADD function ssl_client_dn_field(text);
ALTER EXTENSION sslinfo ADD function ssl_issuer_field(text);
ALTER EXTENSION sslinfo ADD function ssl_client_dn();
ALTER EXTENSION sslinfo ADD function ssl_issuer_dn();
