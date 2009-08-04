--
-- PGP Public Key Encryption
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- successful encrypt/decrypt
select pgp_pub_decrypt(
	pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
	dearmor(seckey))
from keytbl where keytbl.id=1;

select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=2;

select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=3;

select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=6;

-- try with rsa-sign only
select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=4;

-- try with secret key
select pgp_pub_decrypt(
		pgp_pub_encrypt('Secret msg', dearmor(seckey)),
		dearmor(seckey))
from keytbl where keytbl.id=1;

-- does text-to-bytea works
select pgp_pub_decrypt_bytea(
		pgp_pub_encrypt('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=1;

-- and bytea-to-text?
select pgp_pub_decrypt(
		pgp_pub_encrypt_bytea('Secret msg', dearmor(pubkey)),
		dearmor(seckey))
from keytbl where keytbl.id=1;


