--
-- This is created by pgsql/contrib/findoidjoins/make_oidjoin_check
--
SELECT	ctid, aggfnoid 
FROM	pg_catalog.pg_aggregate fk 
WHERE	aggfnoid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.aggfnoid);
SELECT	ctid, aggtransfn 
FROM	pg_catalog.pg_aggregate fk 
WHERE	aggtransfn != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.aggtransfn);
SELECT	ctid, aggfinalfn 
FROM	pg_catalog.pg_aggregate fk 
WHERE	aggfinalfn != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.aggfinalfn);
SELECT	ctid, aggsortop 
FROM	pg_catalog.pg_aggregate fk 
WHERE	aggsortop != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.aggsortop);
SELECT	ctid, aggtranstype 
FROM	pg_catalog.pg_aggregate fk 
WHERE	aggtranstype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.aggtranstype);
SELECT	ctid, aminsert 
FROM	pg_catalog.pg_am fk 
WHERE	aminsert != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.aminsert);
SELECT	ctid, ambeginscan 
FROM	pg_catalog.pg_am fk 
WHERE	ambeginscan != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.ambeginscan);
SELECT	ctid, amgettuple 
FROM	pg_catalog.pg_am fk 
WHERE	amgettuple != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amgettuple);
SELECT	ctid, amgetmulti 
FROM	pg_catalog.pg_am fk 
WHERE	amgetmulti != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amgetmulti);
SELECT	ctid, amrescan 
FROM	pg_catalog.pg_am fk 
WHERE	amrescan != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amrescan);
SELECT	ctid, amendscan 
FROM	pg_catalog.pg_am fk 
WHERE	amendscan != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amendscan);
SELECT	ctid, ammarkpos 
FROM	pg_catalog.pg_am fk 
WHERE	ammarkpos != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.ammarkpos);
SELECT	ctid, amrestrpos 
FROM	pg_catalog.pg_am fk 
WHERE	amrestrpos != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amrestrpos);
SELECT	ctid, ambuild 
FROM	pg_catalog.pg_am fk 
WHERE	ambuild != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.ambuild);
SELECT	ctid, ambulkdelete 
FROM	pg_catalog.pg_am fk 
WHERE	ambulkdelete != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.ambulkdelete);
SELECT	ctid, amvacuumcleanup 
FROM	pg_catalog.pg_am fk 
WHERE	amvacuumcleanup != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amvacuumcleanup);
SELECT	ctid, amcostestimate 
FROM	pg_catalog.pg_am fk 
WHERE	amcostestimate != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amcostestimate);
SELECT	ctid, amopclaid 
FROM	pg_catalog.pg_amop fk 
WHERE	amopclaid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_opclass pk WHERE pk.oid = fk.amopclaid);
SELECT	ctid, amopsubtype 
FROM	pg_catalog.pg_amop fk 
WHERE	amopsubtype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.amopsubtype);
SELECT	ctid, amopopr 
FROM	pg_catalog.pg_amop fk 
WHERE	amopopr != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.amopopr);
SELECT	ctid, amopclaid 
FROM	pg_catalog.pg_amproc fk 
WHERE	amopclaid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_opclass pk WHERE pk.oid = fk.amopclaid);
SELECT	ctid, amprocsubtype 
FROM	pg_catalog.pg_amproc fk 
WHERE	amprocsubtype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.amprocsubtype);
SELECT	ctid, amproc 
FROM	pg_catalog.pg_amproc fk 
WHERE	amproc != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.amproc);
SELECT	ctid, attrelid 
FROM	pg_catalog.pg_attribute fk 
WHERE	attrelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.attrelid);
SELECT	ctid, atttypid 
FROM	pg_catalog.pg_attribute fk 
WHERE	atttypid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.atttypid);
SELECT	ctid, castsource 
FROM	pg_catalog.pg_cast fk 
WHERE	castsource != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.castsource);
SELECT	ctid, casttarget 
FROM	pg_catalog.pg_cast fk 
WHERE	casttarget != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.casttarget);
SELECT	ctid, castfunc 
FROM	pg_catalog.pg_cast fk 
WHERE	castfunc != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.castfunc);
SELECT	ctid, relnamespace 
FROM	pg_catalog.pg_class fk 
WHERE	relnamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.relnamespace);
SELECT	ctid, reltype 
FROM	pg_catalog.pg_class fk 
WHERE	reltype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.reltype);
SELECT	ctid, relam 
FROM	pg_catalog.pg_class fk 
WHERE	relam != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_am pk WHERE pk.oid = fk.relam);
SELECT	ctid, reltablespace 
FROM	pg_catalog.pg_class fk 
WHERE	reltablespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_tablespace pk WHERE pk.oid = fk.reltablespace);
SELECT	ctid, reltoastrelid 
FROM	pg_catalog.pg_class fk 
WHERE	reltoastrelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.reltoastrelid);
SELECT	ctid, reltoastidxid 
FROM	pg_catalog.pg_class fk 
WHERE	reltoastidxid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.reltoastidxid);
SELECT	ctid, connamespace 
FROM	pg_catalog.pg_constraint fk 
WHERE	connamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.connamespace);
SELECT	ctid, contypid 
FROM	pg_catalog.pg_constraint fk 
WHERE	contypid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.contypid);
SELECT	ctid, connamespace 
FROM	pg_catalog.pg_conversion fk 
WHERE	connamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.connamespace);
SELECT	ctid, conproc 
FROM	pg_catalog.pg_conversion fk 
WHERE	conproc != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.conproc);
SELECT	ctid, dattablespace 
FROM	pg_catalog.pg_database fk 
WHERE	dattablespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_tablespace pk WHERE pk.oid = fk.dattablespace);
SELECT	ctid, classid 
FROM	pg_catalog.pg_depend fk 
WHERE	classid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.classid);
SELECT	ctid, refclassid 
FROM	pg_catalog.pg_depend fk 
WHERE	refclassid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.refclassid);
SELECT	ctid, classoid 
FROM	pg_catalog.pg_description fk 
WHERE	classoid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.classoid);
SELECT	ctid, indexrelid 
FROM	pg_catalog.pg_index fk 
WHERE	indexrelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.indexrelid);
SELECT	ctid, indrelid 
FROM	pg_catalog.pg_index fk 
WHERE	indrelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.indrelid);
SELECT	ctid, lanvalidator 
FROM	pg_catalog.pg_language fk 
WHERE	lanvalidator != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.lanvalidator);
SELECT	ctid, opcamid 
FROM	pg_catalog.pg_opclass fk 
WHERE	opcamid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_am pk WHERE pk.oid = fk.opcamid);
SELECT	ctid, opcnamespace 
FROM	pg_catalog.pg_opclass fk 
WHERE	opcnamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.opcnamespace);
SELECT	ctid, opcintype 
FROM	pg_catalog.pg_opclass fk 
WHERE	opcintype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.opcintype);
SELECT	ctid, oprnamespace 
FROM	pg_catalog.pg_operator fk 
WHERE	oprnamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.oprnamespace);
SELECT	ctid, oprleft 
FROM	pg_catalog.pg_operator fk 
WHERE	oprleft != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.oprleft);
SELECT	ctid, oprright 
FROM	pg_catalog.pg_operator fk 
WHERE	oprright != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.oprright);
SELECT	ctid, oprresult 
FROM	pg_catalog.pg_operator fk 
WHERE	oprresult != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.oprresult);
SELECT	ctid, oprcom 
FROM	pg_catalog.pg_operator fk 
WHERE	oprcom != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprcom);
SELECT	ctid, oprnegate 
FROM	pg_catalog.pg_operator fk 
WHERE	oprnegate != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprnegate);
SELECT	ctid, oprlsortop 
FROM	pg_catalog.pg_operator fk 
WHERE	oprlsortop != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprlsortop);
SELECT	ctid, oprrsortop 
FROM	pg_catalog.pg_operator fk 
WHERE	oprrsortop != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprrsortop);
SELECT	ctid, oprltcmpop 
FROM	pg_catalog.pg_operator fk 
WHERE	oprltcmpop != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprltcmpop);
SELECT	ctid, oprgtcmpop 
FROM	pg_catalog.pg_operator fk 
WHERE	oprgtcmpop != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprgtcmpop);
SELECT	ctid, oprcode 
FROM	pg_catalog.pg_operator fk 
WHERE	oprcode != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.oprcode);
SELECT	ctid, oprrest 
FROM	pg_catalog.pg_operator fk 
WHERE	oprrest != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.oprrest);
SELECT	ctid, oprjoin 
FROM	pg_catalog.pg_operator fk 
WHERE	oprjoin != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.oprjoin);
SELECT	ctid, pronamespace 
FROM	pg_catalog.pg_proc fk 
WHERE	pronamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.pronamespace);
SELECT	ctid, prolang 
FROM	pg_catalog.pg_proc fk 
WHERE	prolang != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_language pk WHERE pk.oid = fk.prolang);
SELECT	ctid, prorettype 
FROM	pg_catalog.pg_proc fk 
WHERE	prorettype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.prorettype);
SELECT	ctid, ev_class 
FROM	pg_catalog.pg_rewrite fk 
WHERE	ev_class != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.ev_class);
SELECT	ctid, starelid 
FROM	pg_catalog.pg_statistic fk 
WHERE	starelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.starelid);
SELECT	ctid, staop1 
FROM	pg_catalog.pg_statistic fk 
WHERE	staop1 != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.staop1);
SELECT	ctid, staop2 
FROM	pg_catalog.pg_statistic fk 
WHERE	staop2 != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.staop2);
SELECT	ctid, staop3 
FROM	pg_catalog.pg_statistic fk 
WHERE	staop3 != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.staop3);
SELECT	ctid, tgrelid 
FROM	pg_catalog.pg_trigger fk 
WHERE	tgrelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.tgrelid);
SELECT	ctid, tgfoid 
FROM	pg_catalog.pg_trigger fk 
WHERE	tgfoid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.tgfoid);
SELECT	ctid, typnamespace 
FROM	pg_catalog.pg_type fk 
WHERE	typnamespace != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_namespace pk WHERE pk.oid = fk.typnamespace);
SELECT	ctid, typrelid 
FROM	pg_catalog.pg_type fk 
WHERE	typrelid != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_class pk WHERE pk.oid = fk.typrelid);
SELECT	ctid, typelem 
FROM	pg_catalog.pg_type fk 
WHERE	typelem != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.typelem);
SELECT	ctid, typinput 
FROM	pg_catalog.pg_type fk 
WHERE	typinput != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.typinput);
SELECT	ctid, typoutput 
FROM	pg_catalog.pg_type fk 
WHERE	typoutput != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.typoutput);
SELECT	ctid, typreceive 
FROM	pg_catalog.pg_type fk 
WHERE	typreceive != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.typreceive);
SELECT	ctid, typsend 
FROM	pg_catalog.pg_type fk 
WHERE	typsend != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_proc pk WHERE pk.oid = fk.typsend);
SELECT	ctid, typbasetype 
FROM	pg_catalog.pg_type fk 
WHERE	typbasetype != 0 AND 
	NOT EXISTS(SELECT 1 FROM pg_catalog.pg_type pk WHERE pk.oid = fk.typbasetype);
