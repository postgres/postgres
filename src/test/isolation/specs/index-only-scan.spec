# index-only scan test
#
# This test tries to expose problems with the interaction between index-only
# scans and SSI.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
  CREATE TABLE tabx (id int NOT NULL);
  INSERT INTO tabx SELECT generate_series(1,10000);
  ALTER TABLE tabx ADD PRIMARY KEY (id);
  CREATE TABLE taby (id int NOT NULL);
  INSERT INTO taby SELECT generate_series(1,10000);
  ALTER TABLE taby ADD PRIMARY KEY (id);
}
setup { VACUUM FREEZE ANALYZE tabx; }
setup { VACUUM FREEZE ANALYZE taby; }

teardown
{
  DROP TABLE tabx;
  DROP TABLE taby;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET LOCAL seq_page_cost = 0.1;
  SET LOCAL random_page_cost = 0.1;
  SET LOCAL cpu_tuple_cost = 0.03;
}
step rxwy1 { DELETE FROM taby WHERE id = (SELECT min(id) FROM tabx); }
step c1 { COMMIT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET LOCAL seq_page_cost = 0.1;
  SET LOCAL random_page_cost = 0.1;
  SET LOCAL cpu_tuple_cost = 0.03;
}
step rywx2 { DELETE FROM tabx WHERE id = (SELECT min(id) FROM taby); }
step c2 { COMMIT; }
