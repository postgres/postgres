/* contrib/intagg/uninstall_int_aggregate.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION int_array_enum(int4[]);

DROP AGGREGATE int_array_aggregate (int4);

DROP FUNCTION int_agg_final_array (internal);

DROP FUNCTION int_agg_state (internal, int4);
