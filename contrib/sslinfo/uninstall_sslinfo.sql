/* $PostgreSQL: pgsql/contrib/sslinfo/uninstall_sslinfo.sql,v 1.3 2007/11/13 04:24:29 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION ssl_client_serial();
DROP FUNCTION ssl_is_used();
DROP FUNCTION ssl_client_cert_present();
DROP FUNCTION ssl_client_dn_field(text);
DROP FUNCTION ssl_issuer_field(text);
DROP FUNCTION ssl_client_dn();
DROP FUNCTION ssl_issuer_dn();
