/* $PostgreSQL: pgsql/contrib/intagg/uninstall_int_aggregate.sql,v 1.3 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION int_array_enum(int4[]);

DROP AGGREGATE int_array_aggregate (int4);

DROP FUNCTION int_agg_final_array (int4[]);

DROP FUNCTION int_agg_state (int4[], int4);
