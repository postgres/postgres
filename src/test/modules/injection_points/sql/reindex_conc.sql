-- Tests for REINDEX CONCURRENTLY
CREATE EXTENSION injection_points;

-- Check safety of indexes with predicates and expressions.
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-conc-index-safe', 'notice');
SELECT injection_points_attach('reindex-conc-index-not-safe', 'notice');

CREATE SCHEMA reindex_inj;
CREATE TABLE reindex_inj.tbl(i int primary key, updated_at timestamp);

CREATE UNIQUE INDEX ind_simple ON reindex_inj.tbl(i);
CREATE UNIQUE INDEX ind_expr ON reindex_inj.tbl(ABS(i));
CREATE UNIQUE INDEX ind_pred ON reindex_inj.tbl(i) WHERE mod(i, 2) = 0;
CREATE UNIQUE INDEX ind_expr_pred ON reindex_inj.tbl(abs(i)) WHERE mod(i, 2) = 0;

REINDEX INDEX CONCURRENTLY reindex_inj.ind_simple;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_expr;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_pred;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_expr_pred;

-- Cleanup
SELECT injection_points_detach('reindex-conc-index-safe');
SELECT injection_points_detach('reindex-conc-index-not-safe');
DROP TABLE reindex_inj.tbl;
DROP SCHEMA reindex_inj;

DROP EXTENSION injection_points;
