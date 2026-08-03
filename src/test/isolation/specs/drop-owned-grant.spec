# Test locking of role membership grants during concurrent DROP OWNED BY.

setup
{
	CREATE ROLE regress_dropowned_role;
	CREATE ROLE regress_dropowned_member;
	CREATE ROLE regress_dropowned_grantor;
	GRANT regress_dropowned_role TO regress_dropowned_grantor
		WITH ADMIN OPTION;
	SET ROLE regress_dropowned_grantor;
	GRANT regress_dropowned_role TO regress_dropowned_member;
	RESET ROLE;
}

teardown
{
	DROP ROLE regress_dropowned_member;
	DROP ROLE regress_dropowned_grantor;
	DROP ROLE regress_dropowned_role;
}

session s1
step s1b	{ BEGIN; }
step s1d	{ DROP OWNED BY regress_dropowned_grantor; }
step s1c	{ COMMIT; }

session s2
step s2d	{ DROP OWNED BY regress_dropowned_grantor; }

permutation s1b s1d s2d s1c
