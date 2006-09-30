SET search_path = public;

DROP FUNCTION ssl_client_serial();
DROP FUNCTION ssl_is_used();
DROP FUNCTION ssl_client_cert_present();
DROP FUNCTION ssl_client_dn_field(text);
DROP FUNCTION ssl_issuer_field(text);
DROP FUNCTION ssl_client_dn();
DROP FUNCTION ssl_issuer_dn();
