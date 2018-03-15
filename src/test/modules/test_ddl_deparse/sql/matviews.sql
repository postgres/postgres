--
-- Materialized views
--

CREATE MATERIALIZED VIEW pg_class_mv AS
  SELECT * FROM datatype_table LIMIT 1 WITH NO DATA;

REFRESH MATERIALIZED VIEW pg_class_mv;
