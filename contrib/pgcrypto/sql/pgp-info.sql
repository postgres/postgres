--
-- PGP info functions
--

-- pgp_key_id

select pgp_key_id(dearmor(pubkey)) from keytbl where id=1;
select pgp_key_id(dearmor(pubkey)) from keytbl where id=2;
select pgp_key_id(dearmor(pubkey)) from keytbl where id=3;
select pgp_key_id(dearmor(pubkey)) from keytbl where id=4; -- should fail
select pgp_key_id(dearmor(pubkey)) from keytbl where id=5;
select pgp_key_id(dearmor(pubkey)) from keytbl where id=6;

select pgp_key_id(dearmor(seckey)) from keytbl where id=1;
select pgp_key_id(dearmor(seckey)) from keytbl where id=2;
select pgp_key_id(dearmor(seckey)) from keytbl where id=3;
select pgp_key_id(dearmor(seckey)) from keytbl where id=4; -- should fail
select pgp_key_id(dearmor(seckey)) from keytbl where id=5;
select pgp_key_id(dearmor(seckey)) from keytbl where id=6;

select pgp_key_id(dearmor(data)) as data_key_id
from encdata order by id;
