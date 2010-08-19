--
-- PGP encrypt
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

select pgp_sym_decrypt(pgp_sym_encrypt('Secret.', 'key'), 'key');

-- check whether the defaults are ok
select pgp_sym_decrypt(pgp_sym_encrypt('Secret.', 'key'),
 	'key', 'expect-cipher-algo=aes128,
		expect-disable-mdc=0,
		expect-sess-key=0,
		expect-s2k-mode=3,
		expect-s2k-digest-algo=sha1,
		expect-compress-algo=0
		');

-- maybe the expect- stuff simply does not work
select pgp_sym_decrypt(pgp_sym_encrypt('Secret.', 'key'),
 	'key', 'expect-cipher-algo=bf,
		expect-disable-mdc=1,
		expect-sess-key=1,
		expect-s2k-mode=0,
		expect-s2k-digest-algo=md5,
		expect-compress-algo=1
		');

-- bytea as text
select pgp_sym_decrypt(pgp_sym_encrypt_bytea('Binary', 'baz'), 'baz');

-- text as bytea
select pgp_sym_decrypt_bytea(pgp_sym_encrypt('Text', 'baz'), 'baz');


-- algorithm change
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'cipher-algo=bf'),
 	'key', 'expect-cipher-algo=bf');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'cipher-algo=aes'),
 	'key', 'expect-cipher-algo=aes128');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'cipher-algo=aes192'),
 	'key', 'expect-cipher-algo=aes192');

-- s2k change
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-mode=0'),
 	'key', 'expect-s2k-mode=0');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-mode=1'),
 	'key', 'expect-s2k-mode=1');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-mode=3'),
 	'key', 'expect-s2k-mode=3');

-- s2k digest change
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-digest-algo=md5'),
 	'key', 'expect-s2k-digest-algo=md5');
select pgp_sym_decrypt(
		pgp_sym_encrypt('Secret.', 'key', 's2k-digest-algo=sha1'),
 	'key', 'expect-s2k-digest-algo=sha1');

-- sess key
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=0'),
 	'key', 'expect-sess-key=0');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1'),
 	'key', 'expect-sess-key=1');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1, cipher-algo=bf'),
 	'key', 'expect-sess-key=1, expect-cipher-algo=bf');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1, cipher-algo=aes192'),
 	'key', 'expect-sess-key=1, expect-cipher-algo=aes192');
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 'sess-key=1, cipher-algo=aes256'),
 	'key', 'expect-sess-key=1, expect-cipher-algo=aes256');

-- no mdc
select pgp_sym_decrypt(
		pgp_sym_encrypt('Secret.', 'key', 'disable-mdc=1'),
 	'key', 'expect-disable-mdc=1');

-- crlf
select encode(pgp_sym_decrypt_bytea(
	pgp_sym_encrypt(E'1\n2\n3\r\n', 'key', 'convert-crlf=1'),
 	'key'), 'hex');

-- conversion should be lossless
select encode(digest(pgp_sym_decrypt(
  pgp_sym_encrypt(E'\r\n0\n1\r\r\n\n2\r', 'key', 'convert-crlf=1'),
 	'key', 'convert-crlf=1'), 'sha1'), 'hex') as result,
  encode(digest(E'\r\n0\n1\r\r\n\n2\r', 'sha1'), 'hex') as expect;
