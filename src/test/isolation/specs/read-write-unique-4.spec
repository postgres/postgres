# Read-write-unique test.
# Implementing a gapless sequence of ID numbers for each year.

setup
{
  CREATE TABLE invoice (
    year int,
    invoice_number int,
    PRIMARY KEY (year, invoice_number)
  );

  INSERT INTO invoice VALUES (2016, 1), (2016, 2);
}

teardown
{
  DROP TABLE invoice;
}

session s1
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r1 { SELECT COALESCE(MAX(invoice_number) + 1, 1) FROM invoice WHERE year = 2016; }
step w1 { INSERT INTO invoice VALUES (2016, 3); }
step c1 { COMMIT; }

session s2
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r2 { SELECT COALESCE(MAX(invoice_number) + 1, 1) FROM invoice WHERE year = 2016; }
step w2 { INSERT INTO invoice VALUES (2016, 3); }
step c2 { COMMIT; }

# if they both read first then there should be an SSI conflict
permutation r1 r2 w1 w2 c1 c2

# cases where one session doesn't explicitly read before writing:

# if s2 doesn't explicitly read, then trying to insert the value
# generates a unique constraint violation after s1 commits, as if s2
# ran after s1
permutation r1 w1 w2 c1 c2

# if s1 doesn't explicitly read, but s2 does, then s1 inserts and
# commits first, should s2 experience an SSI failure instead of a
# unique constraint violation?  there is no serial order of operations
# (s1, s2) or (s2, s1) where s1 succeeds, and s2 doesn't see the row
# in an explicit select but then fails to insert due to unique
# constraint violation
permutation r2 w1 w2 c1 c2
