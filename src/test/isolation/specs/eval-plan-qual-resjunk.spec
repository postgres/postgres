# A view whose query has resjunk columns (here a sort key that isn't part of
# the output).  The whole-row Var used for ROW_MARK_COPY has the view's
# rowtype, i.e. excludes the resjunk columns, while the subquery scan's slot,
# described by the subquery's targetlist, includes them.  EPQ must be able to
# restore the whole-row datum into that slot.

setup
{
  CREATE TABLE epq_rj_lock (id int PRIMARY KEY, n int);
  INSERT INTO epq_rj_lock VALUES (1, 0);
  CREATE TABLE epq_rj_data (id int, t text);
  INSERT INTO epq_rj_data VALUES (1, 'x');
  CREATE VIEW epq_rj_view AS SELECT id FROM epq_rj_data ORDER BY t;
}

teardown
{
  DROP VIEW epq_rj_view;
  DROP TABLE epq_rj_data, epq_rj_lock;
}

session s1
setup { SET default_transaction_isolation = 'read committed'; }
step begin1 { BEGIN; }
step update1 { UPDATE epq_rj_lock SET n = n + 1 WHERE id = 1; }
step commit1 { COMMIT; }

session s2
setup { SET default_transaction_isolation = 'read committed'; }
step read2
{
  SELECT l.id, l.n
  FROM epq_rj_lock l JOIN epq_rj_view v ON v.id = l.id
  FOR UPDATE OF l;
}
step modify2
{
  UPDATE epq_rj_lock l SET n = n + 10
  FROM epq_rj_view v WHERE v.id = l.id
  RETURNING l.id, l.n;
}
step explain2
{
  EXPLAIN (COSTS OFF)
  SELECT l.id, l.n
  FROM epq_rj_lock l JOIN epq_rj_view v ON v.id = l.id
  FOR UPDATE OF l;
}

# Without a concurrent update, no EPQ reconstruction is needed.
permutation begin1 update1 commit1 read2

# EPQ reconstructs the view's row from its whole-row datum.  The view's ORDER
# BY keeps it from being pulled up.
permutation explain2 begin1 update1 read2 commit1
permutation begin1 update1 modify2 commit1
