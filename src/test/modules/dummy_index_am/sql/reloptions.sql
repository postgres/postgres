-- Tests for relation options
CREATE EXTENSION dummy_index_am;

CREATE TABLE dummy_test_tab (i int4);

-- Silence validation checks for strings
SET client_min_messages TO 'warning';

-- Test with default values.
CREATE INDEX dummy_test_idx ON dummy_test_tab
  USING dummy_index_am (i);
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';
DROP INDEX dummy_test_idx;

-- Test with full set of options.
-- Allow validation checks for strings, just for the index creation
SET client_min_messages TO 'notice';
CREATE INDEX dummy_test_idx ON dummy_test_tab
  USING dummy_index_am (i) WITH (
  option_bool = false,
  option_int = 5,
  option_real = 3.1,
  option_enum = 'two',
  option_string_val = NULL,
  option_string_null = 'val');
-- Silence again validation checks for strings until the end of the test.
SET client_min_messages TO 'warning';
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';

-- ALTER INDEX .. SET
ALTER INDEX dummy_test_idx SET (option_int = 10);
ALTER INDEX dummy_test_idx SET (option_bool = true);
ALTER INDEX dummy_test_idx SET (option_real = 3.2);
ALTER INDEX dummy_test_idx SET (option_string_val = 'val2');
ALTER INDEX dummy_test_idx SET (option_string_null = NULL);
ALTER INDEX dummy_test_idx SET (option_enum = 'one');
ALTER INDEX dummy_test_idx SET (option_enum = 'three');
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';

-- ALTER INDEX .. RESET
ALTER INDEX dummy_test_idx RESET (option_int);
ALTER INDEX dummy_test_idx RESET (option_bool);
ALTER INDEX dummy_test_idx RESET (option_real);
ALTER INDEX dummy_test_idx RESET (option_enum);
ALTER INDEX dummy_test_idx RESET (option_string_val);
ALTER INDEX dummy_test_idx RESET (option_string_null);
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';

-- Cross-type checks for reloption values
-- Integer
ALTER INDEX dummy_test_idx SET (option_int = 3.3); -- ok
ALTER INDEX dummy_test_idx SET (option_int = true); -- error
ALTER INDEX dummy_test_idx SET (option_int = 'val3'); -- error
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';
ALTER INDEX dummy_test_idx RESET (option_int);
-- Boolean
ALTER INDEX dummy_test_idx SET (option_bool = 4); -- error
ALTER INDEX dummy_test_idx SET (option_bool = 1); -- ok, as true
ALTER INDEX dummy_test_idx SET (option_bool = 3.4); -- error
ALTER INDEX dummy_test_idx SET (option_bool = 'val4'); -- error
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';
ALTER INDEX dummy_test_idx RESET (option_bool);
-- Float
ALTER INDEX dummy_test_idx SET (option_real = 4); -- ok
ALTER INDEX dummy_test_idx SET (option_real = true); -- error
ALTER INDEX dummy_test_idx SET (option_real = 'val5'); -- error
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';
ALTER INDEX dummy_test_idx RESET (option_real);
-- Enum
ALTER INDEX dummy_test_idx SET (option_enum = 'one'); -- ok
ALTER INDEX dummy_test_idx SET (option_enum = 0); -- error
ALTER INDEX dummy_test_idx SET (option_enum = true); -- error
ALTER INDEX dummy_test_idx SET (option_enum = 'three'); -- error
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';
ALTER INDEX dummy_test_idx RESET (option_enum);
-- String
ALTER INDEX dummy_test_idx SET (option_string_val = 4); -- ok
ALTER INDEX dummy_test_idx SET (option_string_val = 3.5); -- ok
ALTER INDEX dummy_test_idx SET (option_string_val = true); -- ok, as "true"
SELECT unnest(reloptions) FROM pg_class WHERE relname = 'dummy_test_idx';
ALTER INDEX dummy_test_idx RESET (option_string_val);

DROP INDEX dummy_test_idx;
