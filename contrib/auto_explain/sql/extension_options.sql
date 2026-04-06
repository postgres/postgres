--
-- Tests for auto_explain.log_extension_options.
--

LOAD 'auto_explain';
LOAD 'pg_overexplain';

-- Various legal values with assorted quoting and whitespace choices.
SET auto_explain.log_extension_options = '';
SET auto_explain.log_extension_options = 'debug, RANGE_TABLE';
SET auto_explain.log_extension_options = 'debug TRUE  ';
SET auto_explain.log_extension_options = '   debug 1,RAnge_table "off"';
SET auto_explain.log_extension_options = $$"debug" tRuE, range_table 'false'$$;

-- Syntax errors.
SET auto_explain.log_extension_options = ',';
SET auto_explain.log_extension_options = ', range_table';
SET auto_explain.log_extension_options = 'range_table, ';
SET auto_explain.log_extension_options = 'range_table true false';
SET auto_explain.log_extension_options = '"range_table';
SET auto_explain.log_extension_options = 'range_table 3.1415nine';
SET auto_explain.log_extension_options = 'range_table "true';
SET auto_explain.log_extension_options = $$range_table 'true$$;
SET auto_explain.log_extension_options = $$'$$;

-- Unacceptable option values.
SET auto_explain.log_extension_options = 'range_table maybe';
SET auto_explain.log_extension_options = 'range_table 2';
SET auto_explain.log_extension_options = 'range_table "0"';
SET auto_explain.log_extension_options = 'range_table 3.14159';

-- Supply enough options to force the option array to be reallocated.
SET auto_explain.log_extension_options = 'debug, debug, debug, debug, debug, debug, debug, debug, debug, debug false';
