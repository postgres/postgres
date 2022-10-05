CREATE SCHEMA IF NOT EXISTS _masking_function;
CREATE OR REPLACE FUNCTION _masking_function.default(in text, out text)
    AS $$ SELECT $1 || ' default' $$
              LANGUAGE SQL;

CREATE OR REPLACE FUNCTION _masking_function.default(in int, out text)
    AS $$ SELECT $1 || ' default' $$
              LANGUAGE SQL;
