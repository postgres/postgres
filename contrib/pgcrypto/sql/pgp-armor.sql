--
-- PGP Armor
--

select armor('');
select armor('test');
select encode(dearmor(armor('')), 'escape');
select encode(dearmor(armor('zooka')), 'escape');

select armor('0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef');

-- lots formatting
select encode(dearmor(' a pgp msg:

-----BEGIN PGP MESSAGE-----
Comment: Some junk

em9va2E=

  =D5cR

-----END PGP MESSAGE-----'), 'escape');

-- lots messages
select encode(dearmor('
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
'), 'escape');

-- bad crc
select dearmor('
-----BEGIN PGP MESSAGE-----

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- corrupt (no space after the colon)
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
foo:

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- corrupt (no empty line)
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- no headers
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- header with empty value
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
foo: 

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- simple
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
fookey: foovalue
barkey: barvalue

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- insane keys, part 1
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
insane:key : 

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- insane keys, part 2
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
insane:key : text value here

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- long value
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
long: this value is more than 76 characters long, but it should still parse correctly as that''s permitted by RFC 4880

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- long value, split up
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
long: this value is more than 76 characters long, but it should still 
long: parse correctly as that''s permitted by RFC 4880

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- long value, split up, part 2
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
long: this value is more than 
long: 76 characters long, but it should still 
long: parse correctly as that''s permitted by RFC 4880

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

-- long value, split up, part 3
select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
emptykey: 
long: this value is more than 
emptykey: 
long: 76 characters long, but it should still 
emptykey: 
long: parse correctly as that''s permitted by RFC 4880
emptykey: 

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
');

select * from pgp_armor_headers('
-----BEGIN PGP MESSAGE-----
Comment: dat1.blowfish.sha1.mdc.s2k3.z0

jA0EBAMCfFNwxnvodX9g0jwB4n4s26/g5VmKzVab1bX1SmwY7gvgvlWdF3jKisvS
yA6Ce1QTMK3KdL2MPfamsTUSAML8huCJMwYQFfE=
=JcP+
-----END PGP MESSAGE-----
');

-- test CR+LF line endings
select * from pgp_armor_headers(replace('
-----BEGIN PGP MESSAGE-----
fookey: foovalue
barkey: barvalue

em9va2E=
=ZZZZ
-----END PGP MESSAGE-----
', E'\n', E'\r\n'));

-- test header generation
select armor('zooka', array['foo'], array['bar']);
select armor('zooka', array['Version', 'Comment'], array['Created by pgcrypto', 'PostgreSQL, the world''s most advanced open source database']);
select * from pgp_armor_headers(
  armor('zooka', array['Version', 'Comment'],
                 array['Created by pgcrypto', 'PostgreSQL, the world''s most advanced open source database']));

-- error/corner cases
select armor('', array['foo'], array['too', 'many']);
select armor('', array['too', 'many'], array['foo']);
select armor('', array[['']], array['foo']);
select armor('', array['foo'], array[['']]);
select armor('', array[null], array['foo']);
select armor('', array['foo'], array[null]);
select armor('', '[0:0]={"foo"}', array['foo']);
select armor('', array['foo'], '[0:0]={"foo"}');
select armor('', array[E'embedded\nnewline'], array['foo']);
select armor('', array['foo'], array[E'embedded\nnewline']);
select armor('', array['embedded: colon+space'], array['foo']);
