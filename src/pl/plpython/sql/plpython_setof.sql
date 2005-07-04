
CREATE or replace FUNCTION test_setof() returns setof text
	AS
'if GD.has_key("calls"):
	GD["calls"] = GD["calls"] + 1
	if GD["calls"] > 2:
		del GD["calls"]
		return plpy.EndOfSet
else:
	GD["calls"] = 1
return str(GD["calls"])'
	LANGUAGE plpythonu;
