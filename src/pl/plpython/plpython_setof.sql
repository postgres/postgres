
CREATE FUNCTION test_setof() returns setof text
	AS
'if GD.has_key("calls"):
	GD["calls"] = GD["calls"] + 1
	if GD["calls"] > 2:
		return None
else:
	GD["calls"] = 1
return str(GD["calls"])'
	LANGUAGE 'plpython';
