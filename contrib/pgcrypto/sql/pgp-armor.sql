--
-- PGP Armor
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

select armor('');
select armor('test');
select dearmor(armor(''));
select dearmor(armor('zooka'));

select armor('0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef');

-- lots formatting
select dearmor(' a pgp msg:

-----BEGIN PGP MESSAGE-----
Comment: Some junk

em9va2E=

  =D5cR

-----END PGP MESSAGE-----');

-- lots messages
select dearmor('
wrong packet:
  -----BEGIN PGP MESSAGE-----

  d3Jvbmc=
  =vCYP
  -----END PGP MESSAGE-----

right packet:
-----BEGIN PGP MESSAGE-----

cmlnaHQ=
=nbpj
-----END PGP MESSAGE-----

use only first packet
-----BEGIN PGP MESSAGE-----

d3Jvbmc=
=vCYP
-----END PGP MESSAGE-----
');

-- bad crc
select dearmor('
-----BEGIN PGP MESSAGE-----

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');
