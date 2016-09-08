--
-- MONEY
--

CREATE TABLE money_data (m money);

INSERT INTO money_data VALUES ('123');
SELECT * FROM money_data;
SELECT m + '123' FROM money_data;
SELECT m + '123.45' FROM money_data;
SELECT m - '123.45' FROM money_data;
SELECT m * 2 FROM money_data;
SELECT m / 2 FROM money_data;

-- All true
SELECT m = '$123.00' FROM money_data;
SELECT m != '$124.00' FROM money_data;
SELECT m <= '$123.00' FROM money_data;
SELECT m >= '$123.00' FROM money_data;
SELECT m < '$124.00' FROM money_data;
SELECT m > '$122.00' FROM money_data;

-- All false
SELECT m = '$123.01' FROM money_data;
SELECT m != '$123.00' FROM money_data;
SELECT m <= '$122.99' FROM money_data;
SELECT m >= '$123.01' FROM money_data;
SELECT m > '$124.00' FROM money_data;
SELECT m < '$122.00' FROM money_data;

SELECT cashlarger(m, '$124.00') FROM money_data;
SELECT cashsmaller(m, '$124.00') FROM money_data;
SELECT cash_words(m) FROM money_data;
SELECT cash_words(m + '1.23') FROM money_data;

DELETE FROM money_data;
INSERT INTO money_data VALUES ('$123.45');
SELECT * FROM money_data;

DELETE FROM money_data;
INSERT INTO money_data VALUES ('$123.451');
SELECT * FROM money_data;

DELETE FROM money_data;
INSERT INTO money_data VALUES ('$123.454');
SELECT * FROM money_data;

DELETE FROM money_data;
INSERT INTO money_data VALUES ('$123.455');
SELECT * FROM money_data;

DELETE FROM money_data;
INSERT INTO money_data VALUES ('$123.456');
SELECT * FROM money_data;

DELETE FROM money_data;
INSERT INTO money_data VALUES ('$123.459');
SELECT * FROM money_data;

-- input checks
SELECT '1234567890'::money;
SELECT '12345678901234567'::money;
SELECT '123456789012345678'::money;
SELECT '9223372036854775807'::money;
SELECT '-12345'::money;
SELECT '-1234567890'::money;
SELECT '-12345678901234567'::money;
SELECT '-123456789012345678'::money;
SELECT '-9223372036854775808'::money;

-- special characters
SELECT '(1)'::money;
SELECT '($123,456.78)'::money;

-- documented minimums and maximums
SELECT '-92233720368547758.08'::money;
SELECT '92233720368547758.07'::money;

SELECT '-92233720368547758.09'::money;
SELECT '92233720368547758.08'::money;

-- rounding
SELECT '-92233720368547758.085'::money;
SELECT '92233720368547758.075'::money;

-- Cast int4/int8 to money
SELECT 1234567890::money;
SELECT 12345678901234567::money;
SELECT (-12345)::money;
SELECT (-1234567890)::money;
SELECT (-12345678901234567)::money;
SELECT 1234567890::int4::money;
SELECT 12345678901234567::int8::money;
SELECT (-1234567890)::int4::money;
SELECT (-12345678901234567)::int8::money;
