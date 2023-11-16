--
-- PGP encrypt using MD5
--

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret.', 'key', 's2k-digest-algo=md5'),
	'key', 'expect-s2k-digest-algo=md5');
