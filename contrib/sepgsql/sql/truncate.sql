--
-- Regression Test for TRUNCATE
--

--
-- Setup
--
CREATE TABLE julio_claudians (name text, birth_date date);
SECURITY LABEL ON TABLE julio_claudians IS 'system_u:object_r:sepgsql_regtest_foo_table_t:s0';
INSERT INTO julio_claudians VALUES ('Augustus', 'September 23, 63 BC'), ('Tiberius', 'November 16, 42 BC'), ('Caligula', 'August 31, 0012'), ('Claudius', 'August 1, 0010'), ('Nero', 'December 15, 0037');

CREATE TABLE flavians (name text, birth_date date);
SECURITY LABEL ON TABLE flavians IS 'system_u:object_r:sepgsql_table_t:s0';

INSERT INTO flavians VALUES ('Vespasian', 'November 17, 0009'), ('Titus', 'December 30, 0039'), ('Domitian', 'October 24, 0051');

SELECT * from julio_claudians;
SELECT * from flavians;

TRUNCATE TABLE julio_claudians;			-- ok
TRUNCATE TABLE flavians;			-- failed

SELECT * from julio_claudians;
SELECT * from flavians;
