SET search_path = public;

DROP FUNCTION int_array_enum(int4[]);

DROP AGGREGATE int_array_aggregate (int4);

DROP FUNCTION int_agg_final_array (int4[]);

DROP FUNCTION int_agg_state (int4[], int4);
