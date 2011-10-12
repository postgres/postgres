/* contrib/pgcrypto/pgcrypto--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgcrypto" to load this file. \quit

ALTER EXTENSION pgcrypto ADD function digest(text,text);
ALTER EXTENSION pgcrypto ADD function digest(bytea,text);
ALTER EXTENSION pgcrypto ADD function hmac(text,text,text);
ALTER EXTENSION pgcrypto ADD function hmac(bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function crypt(text,text);
ALTER EXTENSION pgcrypto ADD function gen_salt(text);
ALTER EXTENSION pgcrypto ADD function gen_salt(text,integer);
ALTER EXTENSION pgcrypto ADD function encrypt(bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function decrypt(bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function encrypt_iv(bytea,bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function decrypt_iv(bytea,bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function gen_random_bytes(integer);
ALTER EXTENSION pgcrypto ADD function pgp_sym_encrypt(text,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_encrypt_bytea(bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_encrypt(text,text,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_encrypt_bytea(bytea,text,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_decrypt(bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_decrypt_bytea(bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_decrypt(bytea,text,text);
ALTER EXTENSION pgcrypto ADD function pgp_sym_decrypt_bytea(bytea,text,text);
ALTER EXTENSION pgcrypto ADD function pgp_pub_encrypt(text,bytea);
ALTER EXTENSION pgcrypto ADD function pgp_pub_encrypt_bytea(bytea,bytea);
ALTER EXTENSION pgcrypto ADD function pgp_pub_encrypt(text,bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_pub_encrypt_bytea(bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_pub_decrypt(bytea,bytea);
ALTER EXTENSION pgcrypto ADD function pgp_pub_decrypt_bytea(bytea,bytea);
ALTER EXTENSION pgcrypto ADD function pgp_pub_decrypt(bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_pub_decrypt_bytea(bytea,bytea,text);
ALTER EXTENSION pgcrypto ADD function pgp_pub_decrypt(bytea,bytea,text,text);
ALTER EXTENSION pgcrypto ADD function pgp_pub_decrypt_bytea(bytea,bytea,text,text);
ALTER EXTENSION pgcrypto ADD function pgp_key_id(bytea);
ALTER EXTENSION pgcrypto ADD function armor(bytea);
ALTER EXTENSION pgcrypto ADD function dearmor(text);
