--
-- CREATE_VIEW
--

CREATE VIEW static_view AS
  SELECT 'foo'::TEXT AS col;

CREATE OR REPLACE VIEW static_view AS
  SELECT 'bar'::TEXT AS col;

CREATE VIEW datatype_view AS
  SELECT * FROM datatype_table;

CREATE RECURSIVE VIEW nums_1_100 (n) AS
    VALUES (1)
UNION ALL
    SELECT n+1 FROM nums_1_100 WHERE n < 100;
