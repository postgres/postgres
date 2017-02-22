--
-- COMMENT_ON
--

COMMENT ON SCHEMA foo IS 'This is schema foo';
COMMENT ON TYPE enum_test IS 'ENUM test';
COMMENT ON TYPE int2range  IS 'RANGE test';
COMMENT ON DOMAIN japanese_postal_code IS 'DOMAIN test';
COMMENT ON SEQUENCE fkey_table_seq IS 'SEQUENCE test';
COMMENT ON TABLE datatype_table IS 'This table should contain all native datatypes';
COMMENT ON VIEW datatype_view IS 'This is a view';
COMMENT ON FUNCTION c_function_test() IS 'FUNCTION test';
COMMENT ON TRIGGER trigger_1 ON datatype_table IS 'TRIGGER test';
COMMENT ON RULE rule_1 ON datatype_table IS 'RULE test';
