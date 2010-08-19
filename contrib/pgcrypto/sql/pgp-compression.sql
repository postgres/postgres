--
-- PGP compression support
--

select pgp_sym_decrypt(dearmor('
-----BEGIN PGP MESSAGE-----

ww0ECQMCsci6AdHnELlh0kQB4jFcVwHMJg0Bulop7m3Mi36s15TAhBo0AnzIrRFrdLVCkKohsS6+
DMcmR53SXfLoDJOv/M8uKj3QSq7oWNIp95pxfA==
=tbSn
-----END PGP MESSAGE-----
'), 'key', 'expect-compress-algo=1');

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key', 'compress-algo=0'),
	'key', 'expect-compress-algo=0');

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key', 'compress-algo=1'),
	'key', 'expect-compress-algo=1');

select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key', 'compress-algo=2'),
	'key', 'expect-compress-algo=2');

-- level=0 should turn compression off
select pgp_sym_decrypt(
	pgp_sym_encrypt('Secret message', 'key',
			'compress-algo=2, compress-level=0'),
	'key', 'expect-compress-algo=0');
