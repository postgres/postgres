CREATE EXTENSION plsample;
-- Create and test some dummy functions
CREATE FUNCTION plsample_result_text(a1 numeric, a2 text, a3 integer[])
RETURNS TEXT
AS $$
  Example of source with text result.
$$ LANGUAGE plsample;
SELECT plsample_result_text(1.23, 'abc', '{4, 5, 6}');

CREATE FUNCTION plsample_result_void(a1 text[])
RETURNS VOID
AS $$
  Example of source with void result.
$$ LANGUAGE plsample;
SELECT plsample_result_void('{foo, bar, hoge}');
