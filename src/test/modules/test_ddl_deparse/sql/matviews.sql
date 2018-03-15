--
-- Materialized views
--

CREATE MATERIALIZED VIEW ddl_deparse_mv AS
  SELECT * FROM datatype_table LIMIT 1 WITH NO DATA;

REFRESH MATERIALIZED VIEW ddl_deparse_mv;
