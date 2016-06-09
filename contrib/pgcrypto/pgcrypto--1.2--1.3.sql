/* contrib/pgcrypto/pgcrypto--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgcrypto UPDATE TO '1.3'" to load this file. \quit

ALTER FUNCTION digest(text, text) PARALLEL SAFE;
ALTER FUNCTION digest(bytea, text) PARALLEL SAFE;
ALTER FUNCTION hmac(text, text, text) PARALLEL SAFE;
ALTER FUNCTION hmac(bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION crypt(text, text) PARALLEL SAFE;
ALTER FUNCTION gen_salt(text) PARALLEL SAFE;
ALTER FUNCTION gen_salt(text, int4) PARALLEL SAFE;
ALTER FUNCTION encrypt(bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION decrypt(bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION encrypt_iv(bytea, bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION decrypt_iv(bytea, bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION gen_random_bytes(int4) PARALLEL SAFE;
ALTER FUNCTION gen_random_uuid() PARALLEL SAFE;
ALTER FUNCTION pgp_sym_encrypt(text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_encrypt_bytea(bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_encrypt(text, text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_encrypt_bytea(bytea, text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_decrypt(bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_decrypt_bytea(bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_decrypt(bytea, text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_sym_decrypt_bytea(bytea, text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_encrypt(text, bytea) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_encrypt_bytea(bytea, bytea) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_encrypt(text, bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_encrypt_bytea(bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_decrypt(bytea, bytea) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_decrypt_bytea(bytea, bytea) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_decrypt(bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_decrypt_bytea(bytea, bytea, text) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_decrypt(bytea, bytea, text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_pub_decrypt_bytea(bytea, bytea, text, text) PARALLEL SAFE;
ALTER FUNCTION pgp_key_id(bytea) PARALLEL SAFE;
ALTER FUNCTION armor(bytea) PARALLEL SAFE;
ALTER FUNCTION armor(bytea, text[], text[]) PARALLEL SAFE;
ALTER FUNCTION dearmor(text) PARALLEL SAFE;
ALTER FUNCTION pgp_armor_headers(text) PARALLEL SAFE;
