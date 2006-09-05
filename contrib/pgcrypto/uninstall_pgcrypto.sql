
SET search_path = public;

DROP FUNCTION digest(text, text);
DROP FUNCTION digest(bytea, text);

DROP FUNCTION hmac(text, text, text);
DROP FUNCTION hmac(bytea, bytea, text);

DROP FUNCTION crypt(text, text);
DROP FUNCTION gen_salt(text);
DROP FUNCTION gen_salt(text, int4);

DROP FUNCTION encrypt(bytea, bytea, text);
DROP FUNCTION decrypt(bytea, bytea, text);
DROP FUNCTION encrypt_iv(bytea, bytea, bytea, text);
DROP FUNCTION decrypt_iv(bytea, bytea, bytea, text);

DROP FUNCTION gen_random_bytes(int4);

DROP FUNCTION pgp_sym_encrypt(text, text);
DROP FUNCTION pgp_sym_encrypt_bytea(bytea, text);
DROP FUNCTION pgp_sym_encrypt(text, text, text);
DROP FUNCTION pgp_sym_encrypt_bytea(bytea, text, text);
DROP FUNCTION pgp_sym_decrypt(bytea, text);
DROP FUNCTION pgp_sym_decrypt_bytea(bytea, text);
DROP FUNCTION pgp_sym_decrypt(bytea, text, text);
DROP FUNCTION pgp_sym_decrypt_bytea(bytea, text, text);

DROP FUNCTION pgp_pub_encrypt(text, bytea);
DROP FUNCTION pgp_pub_encrypt_bytea(bytea, bytea);
DROP FUNCTION pgp_pub_encrypt(text, bytea, text);
DROP FUNCTION pgp_pub_encrypt_bytea(bytea, bytea, text);
DROP FUNCTION pgp_pub_decrypt(bytea, bytea);
DROP FUNCTION pgp_pub_decrypt_bytea(bytea, bytea);
DROP FUNCTION pgp_pub_decrypt(bytea, bytea, text);
DROP FUNCTION pgp_pub_decrypt_bytea(bytea, bytea, text);
DROP FUNCTION pgp_pub_decrypt(bytea, bytea, text, text);
DROP FUNCTION pgp_pub_decrypt_bytea(bytea, bytea, text, text);

DROP FUNCTION pgp_key_id(bytea);
DROP FUNCTION armor(bytea);
DROP FUNCTION dearmor(text);

