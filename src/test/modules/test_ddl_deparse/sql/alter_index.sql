-- Test index visibility commands
CREATE TABLE visibility_test (id int);
CREATE INDEX idx_vis ON visibility_test(id);

ALTER INDEX idx_vis INVISIBLE;
ALTER INDEX idx_vis VISIBLE;

ALTER INDEX idx_vis INVISIBLE;
REINDEX INDEX idx_vis;

CREATE SCHEMA visibility_schema;
CREATE TABLE visibility_schema.test2 (id int);
CREATE INDEX idx_vis2 ON visibility_schema.test2(id);
ALTER INDEX visibility_schema.idx_vis2 INVISIBLE;

-- Clean up
DROP SCHEMA visibility_schema CASCADE;
DROP TABLE visibility_test CASCADE;
