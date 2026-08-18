# A foreign key may use a cross-type equality operator: a "date" primary key
# and a "timestamp" referencing column give "=(date,timestamp without time
# zone)", whose left input is the PK type and whose right input is the FK type.
#
# When the referenced row is updated while a check is locking it, the check
# has to re-check against the new version of the row.  That re-check must
# still pass each value to the side of the operator that expects it.  A date
# counts days and a timestamp counts microseconds, so reading one as the other
# compares two unrelated numbers.  Both types are pass-by-value, so nothing
# here turns on how a value is stored, only on which side it is read from.
#
# Below the referenced key is present the whole time -- s1 moves it away and
# puts it back inside one transaction -- so the INSERT must succeed, exactly as
# it does for the same-type case in the second permutation.

setup
{
  CREATE TABLE fkct_pk (k date PRIMARY KEY, payload text);
  CREATE TABLE fkct_fk (id int, t timestamp REFERENCES fkct_pk(k));
  INSERT INTO fkct_pk VALUES ('2020-01-01', 'p1');

  CREATE TABLE fkct_pk_same (k timestamp PRIMARY KEY, payload text);
  CREATE TABLE fkct_fk_same (id int, t timestamp REFERENCES fkct_pk_same(k));
  INSERT INTO fkct_pk_same VALUES ('2020-01-01', 'p1');
}

teardown
{
  DROP TABLE fkct_fk, fkct_pk, fkct_fk_same, fkct_pk_same;
}

session s1
step s1b      { BEGIN; }
step s1away   { UPDATE fkct_pk SET k = '2020-06-01' WHERE payload = 'p1'; }
step s1back   { UPDATE fkct_pk SET k = '2020-01-01' WHERE payload = 'p1'; }
step s1aways  { UPDATE fkct_pk_same SET k = '2020-06-01' WHERE payload = 'p1'; }
step s1backs  { UPDATE fkct_pk_same SET k = '2020-01-01' WHERE payload = 'p1'; }
step s1c      { COMMIT; }

# Two rows in one statement, so the checks are batched -- that is what reaches
# the re-check path under test.
session s2
step s2ins    { INSERT INTO fkct_fk SELECT g, '2020-01-01'::timestamp FROM generate_series(1,2) g; }
step s2inss   { INSERT INTO fkct_fk_same SELECT g, '2020-01-01'::timestamp FROM generate_series(1,2) g; }
step s2sel    { SELECT k FROM fkct_pk; }

permutation s1b s1away s1back s2ins s1c s2sel

# The mirror image: s1 leaves the key where it moved it, so the INSERT must
# fail.  Making the re-check accept every concurrently updated tuple would
# satisfy the permutation above while breaking this one.
permutation s1b s1away s2ins s1c s2sel

permutation s1b s1aways s1backs s2inss s1c
