--
-- check static and global data (SD and GD)
--

CREATE FUNCTION global_test_one() returns text
    AS
'if not SD.has_key("global_test"):
	SD["global_test"] = "set by global_test_one"
if not GD.has_key("global_test"):
	GD["global_test"] = "set by global_test_one"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE plpythonu;

CREATE FUNCTION global_test_two() returns text
    AS
'if not SD.has_key("global_test"):
	SD["global_test"] = "set by global_test_two"
if not GD.has_key("global_test"):
	GD["global_test"] = "set by global_test_two"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE plpythonu;


CREATE FUNCTION static_test() returns int4
    AS
'if SD.has_key("call"):
	SD["call"] = SD["call"] + 1
else:
	SD["call"] = 1
return SD["call"]
'
    LANGUAGE plpythonu;


SELECT static_test();
SELECT static_test();
SELECT global_test_one();
SELECT global_test_two();
