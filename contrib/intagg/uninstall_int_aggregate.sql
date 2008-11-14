/* $PostgreSQL: pgsql/contrib/intagg/uninstall_int_aggregate.sql,v 1.4 2008/11/14 19:58:45 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION int_array_enum(int4[]);

DROP AGGREGATE int_array_aggregate (int4);

DROP FUNCTION int_agg_final_array (internal);

DROP FUNCTION int_agg_state (internal, int4);
