--
-- Materialized views
--

CREATE MATERIALIZED VIEW pg_class_mv AS
  SELECT * FROM pg_class LIMIT 1 WITH NO DATA;

REFRESH MATERIALIZED VIEW pg_class_mv;
