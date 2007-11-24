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

