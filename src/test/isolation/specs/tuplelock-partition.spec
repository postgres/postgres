# Test tuple locking on INSERT ON CONFLICT UPDATE on a partitioned table.

setup
{
   DROP TABLE IF EXISTS parttab;
   CREATE TABLE parttab (col1 text, key INTEGER PRIMARY KEY, col2 text) PARTITION BY LIST (key);
   CREATE TABLE parttab1 (key INTEGER PRIMARY KEY, col1 text, col2 text);
   CREATE TABLE parttab2 (key INTEGER PRIMARY KEY, col1 text, col2 text);
   ALTER TABLE parttab ATTACH PARTITION parttab1 FOR VALUES IN (1);
   ALTER TABLE parttab ATTACH PARTITION parttab2 FOR VALUES IN (2);
   INSERT INTO parttab (key, col1, col2) VALUES (1, 'a', 'b');
}

teardown
{
   DROP TABLE parttab;
}

session "s1"
step "s1b"               { BEGIN; }
step "s1update_nokey"  { INSERT INTO parttab (key, col1, col2) VALUES (1, 'a', 'b') ON CONFLICT (key) DO UPDATE SET col1 = 'x', col2 = 'y'; }
step "s1update_key"  { INSERT INTO parttab (key, col1, col2) VALUES (1, 'a', 'b') ON CONFLICT (key) DO UPDATE SET key=1; }
step "s1c"               { COMMIT; }

session "s2"
step "s2locktuple"  { SELECT * FROM parttab FOR KEY SHARE; }

# INSERT ON CONFLICT UPDATE, performs an UPDATE on non-key columns
permutation "s1b" "s1update_nokey" "s2locktuple" "s1c"

# INSERT ON CONFLICT UPDATE, performs an UPDATE on key column
permutation "s1b" "s1update_key" "s2locktuple" "s1c"
