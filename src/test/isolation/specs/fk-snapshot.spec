setup
{
  CREATE TABLE pk_noparted (
	a			int		PRIMARY KEY
  );

  CREATE TABLE fk_parted_pk (
	a			int		PRIMARY KEY REFERENCES pk_noparted ON DELETE CASCADE
  ) PARTITION BY LIST (a);
  CREATE TABLE fk_parted_pk_1 PARTITION OF fk_parted_pk FOR VALUES IN (1);
  CREATE TABLE fk_parted_pk_2 PARTITION OF fk_parted_pk FOR VALUES IN (2);

  CREATE TABLE fk_noparted (
	a			int		REFERENCES fk_parted_pk ON DELETE NO ACTION INITIALLY DEFERRED
  );
  INSERT INTO pk_noparted VALUES (1);
  INSERT INTO fk_parted_pk VALUES (1);
  INSERT INTO fk_noparted VALUES (1);
}

teardown
{
  DROP TABLE pk_noparted, fk_parted_pk, fk_noparted;
}

session s1
step s1brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1brc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1ifp2	{ INSERT INTO fk_parted_pk VALUES (2); }
step s1ifp1	{ INSERT INTO fk_parted_pk VALUES (1); }
step s1dfp	{ DELETE FROM fk_parted_pk WHERE a = 1; }
step s1c	{ COMMIT; }
step s1sfp	{ SELECT * FROM fk_parted_pk; }
step s1sp	{ SELECT * FROM pk_noparted; }
step s1sfn	{ SELECT * FROM fk_noparted; }

session s2
step s2brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2brc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2ip2	{ INSERT INTO pk_noparted VALUES (2); }
step s2ifn2	{ INSERT INTO fk_noparted VALUES (2); }
step s2c	{ COMMIT; }
step s2sfp	{ SELECT * FROM fk_parted_pk; }
step s2sfn	{ SELECT * FROM fk_noparted; }

# inserting into referencing tables in transaction-snapshot mode
# PK table is non-partitioned
permutation s1brr s2brc s2ip2 s1sp s2c s1sp s1ifp2 s1c s1sfp
# PK table is partitioned: buggy, because s2's serialization transaction can
# see the uncommitted row thanks to the latest snapshot taken for
# partition lookup to work correctly also ends up getting used by the PK index
# scan
permutation s2ip2 s2brr s1brc s1ifp2 s2sfp s1c s2sfp s2ifn2 s2c s2sfn

# inserting into referencing tables in up-to-date snapshot mode
permutation s1brc s2brc s2ip2 s1sp s2c s1sp s1ifp2 s2brc s2sfp s1c s1sfp s2ifn2 s2c s2sfn

# deleting a referenced row and then inserting again in the same transaction; works
# the same no matter the snapshot mode
permutation s1brr s1dfp s1ifp1 s1c s1sfn
permutation s1brc s1dfp s1ifp1 s1c s1sfn
