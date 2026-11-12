--
-- check static and global data (SD and GD)
--

CREATE FUNCTION global_test_one() returns text
    AS
'if "global_test" not in SD:
	SD["global_test"] = "set by global_test_one"
if "global_test" not in GD:
	GD["global_test"] = "set by global_test_one"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE plpython3u;

CREATE FUNCTION global_test_two() returns text
    AS
'if "global_test" not in SD:
	SD["global_test"] = "set by global_test_two"
if "global_test" not in GD:
	GD["global_test"] = "set by global_test_two"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE plpython3u;


CREATE FUNCTION static_test() returns int4
    AS
'if "call" in SD:
	SD["call"] = SD["call"] + 1
else:
	SD["call"] = 1
return SD["call"]
'
    LANGUAGE plpython3u;


SELECT static_test();
SELECT static_test();
SELECT global_test_one();
SELECT global_test_two();

-- SD is shared by all calls of a function, whatever the input collation,
-- and survives CREATE OR REPLACE FUNCTION
CREATE FUNCTION static_test_coll(t text) RETURNS int4
AS $$
SD["n"] = SD.get("n", 0) + 1
return SD["n"]
$$ LANGUAGE plpython3u;

SELECT static_test_coll('a'), static_test_coll('a' COLLATE "C");

CREATE OR REPLACE FUNCTION static_test_coll(t text) RETURNS int4
AS $$
SD["n"] = SD.get("n", 0) + 1
return SD["n"]
$$ LANGUAGE plpython3u;

SELECT static_test_coll('a');
