# Verify that SPLIT operation locks DML operations with partitioned table

setup
{
  DROP TABLE IF EXISTS tpart;
  CREATE TABLE tpart(i int, t text) partition by range(i);
  CREATE TABLE tpart_00_10 PARTITION OF tpart FOR VALUES FROM (0) TO (10);
  CREATE TABLE tpart_10_20 PARTITION OF tpart FOR VALUES FROM (10) TO (20);
  CREATE TABLE tpart_20_30 PARTITION OF tpart FOR VALUES FROM (20) TO (30);
  CREATE TABLE tpart_default PARTITION OF tpart DEFAULT;
  INSERT INTO tpart VALUES (5, 'text05');
  INSERT INTO tpart VALUES (15, 'text15');
  INSERT INTO tpart VALUES (25, 'text25');
  INSERT INTO tpart VALUES (35, 'text35');
}

teardown
{
  DROP TABLE tpart;
}

session s1
step s1b	{ BEGIN; }
step s1brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1bs	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1splt	{ ALTER TABLE tpart SPLIT PARTITION tpart_10_20 INTO
			   (PARTITION tpart_10_15 FOR VALUES FROM (10) TO (15),
			    PARTITION tpart_15_20 FOR VALUES FROM (15) TO (20)); }
step s1c	{ COMMIT; }


session s2
step s2b	{ BEGIN; }
step s2brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2bs	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2i	{ INSERT INTO tpart VALUES (1, 'text01'); }
step s2c	{ COMMIT; }
step s2s	{ SELECT * FROM tpart; }


# s1 starts SPLIT PARTITION then s2 trying to insert row and
# waits until s1 finished SPLIT operation.

permutation s1b s1splt s2b s2i s1c s2c s2s
permutation s1b s1splt s2brr s2i s1c s2c s2s
permutation s1b s1splt s2bs s2i s1c s2c s2s

permutation s1brr s1splt s2b s2i s1c s2c s2s
permutation s1brr s1splt s2brr s2i s1c s2c s2s
permutation s1brr s1splt s2bs s2i s1c s2c s2s

permutation s1bs s1splt s2b s2i s1c s2c s2s
permutation s1bs s1splt s2brr s2i s1c s2c s2s
permutation s1bs s1splt s2bs s2i s1c s2c s2s
