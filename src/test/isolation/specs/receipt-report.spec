# Daily Report of Receipts test.
#
# This test doesn't persist a bad state in the database; rather, it
# provides a view of the data which is not consistent with any
# order of execution of the serializable transactions.  It
# demonstrates a situation where the deposit date for receipts could
# be changed and a report of the closed day's receipts subsequently
# run which will miss a receipt from the date which has been closed.
#
# There are only six permutations which must cause a serialization failure.
# Failure cases are where s1 overlaps both s2 and s3, but s2 commits before
# s3 executes its first SELECT.
#
# As long as s3 is declared READ ONLY there should be no false positives.
# If s3 were changed to READ WRITE, we would currently expect 42 false
# positives.  Further work dealing with de facto READ ONLY transactions
# may be able to reduce or eliminate those false positives.

setup
{
  CREATE TABLE ctl (k text NOT NULL PRIMARY KEY, deposit_date date NOT NULL);
  INSERT INTO ctl VALUES ('receipt', DATE '2008-12-22');
  CREATE TABLE receipt (receipt_no int NOT NULL PRIMARY KEY, deposit_date date NOT NULL, amount numeric(13,2));
  INSERT INTO receipt VALUES (1, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 1.00);
  INSERT INTO receipt VALUES (2, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 2.00);
}

teardown
{
  DROP TABLE ctl, receipt;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rxwy1	{ INSERT INTO receipt VALUES (3, (SELECT deposit_date FROM ctl WHERE k = 'receipt'), 4.00); }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step wx2	{ UPDATE ctl SET deposit_date = DATE '2008-12-23' WHERE k = 'receipt'; }
step c2		{ COMMIT; }

session s3
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE, READ ONLY; }
step rx3	{ SELECT * FROM ctl WHERE k = 'receipt'; }
step ry3	{ SELECT * FROM receipt WHERE deposit_date = DATE '2008-12-22'; }
step c3		{ COMMIT; }
