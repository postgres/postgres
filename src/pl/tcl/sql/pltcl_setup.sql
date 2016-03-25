--
-- Create the tables used in the test queries
--
-- T_pkey1 is the primary key table for T_dta1. Entries from T_pkey1
-- Cannot be changed or deleted if they are referenced from T_dta1.
--
-- T_pkey2 is the primary key table for T_dta2. If the key values in
-- T_pkey2 are changed, the references in T_dta2 follow. If entries
-- are deleted, the referencing entries from T_dta2 are deleted too.
-- The values for field key2 in T_pkey2 are silently converted to
-- upper case on insert/update.
--
create table T_pkey1 (
    key1	int4,
    key2	char(20),
    txt		char(40)
);

create table T_pkey2 (
    key1	int4,
    key2	char(20),
    txt		char(40)
);

create table T_dta1 (
    tkey	char(10),
    ref1	int4,
    ref2	char(20)
);

create table T_dta2 (
    tkey	char(10),
    ref1	int4,
    ref2	char(20)
);


--
-- Function to check key existence in T_pkey1
--
create function check_pkey1_exists(int4, bpchar) returns bool as E'
    if {![info exists GD]} {
        set GD(plan) [spi_prepare				\\
	    "select 1 from T_pkey1				\\
	        where key1 = \\$1 and key2 = \\$2"		\\
	    {int4 bpchar}]
    }

    set n [spi_execp -count 1 $GD(plan) [list $1 $2]]

    if {$n > 0} {
        return "t"
    }
    return "f"
' language pltcl;


-- dump trigger data

CREATE TABLE trigger_test
    (i int, v text );

CREATE VIEW trigger_test_view AS SELECT * FROM trigger_test;

CREATE FUNCTION trigger_data() returns trigger language pltcl as $_$

	if { [info exists TG_relid] } {
	set TG_relid "bogus:12345"
	}

	set dnames [info locals {[a-zA-Z]*} ]

	foreach key [lsort $dnames] {

		if { [array exists $key] } {
			set str "{"
			foreach akey [lsort [ array names $key ] ] {
				if {[string length $str] > 1} { set str "$str, " }
				set cmd "($akey)"
				set cmd "set val \$$key$cmd"
				eval $cmd
				set str "$str$akey: $val"
			}
			set str "$str}"
		elog NOTICE "$key: $str"
		} else {
			set val [eval list "\$$key" ]
		elog NOTICE "$key: $val"
		}
	}


	return OK

$_$;

CREATE TRIGGER show_trigger_data_trig
BEFORE INSERT OR UPDATE OR DELETE ON trigger_test
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

CREATE TRIGGER show_trigger_data_view_trig
INSTEAD OF INSERT OR UPDATE OR DELETE ON trigger_test_view
FOR EACH ROW EXECUTE PROCEDURE trigger_data(24,'skidoo view');

--
-- Trigger function on every change to T_pkey1
--
create function trig_pkey1_before() returns trigger as E'
    #
    # Create prepared plans on the first call
    #
    if {![info exists GD]} {
	#
	# Plan to check for duplicate key in T_pkey1
	#
        set GD(plan_pkey1) [spi_prepare				\\
	    "select check_pkey1_exists(\\$1, \\$2) as ret"	\\
	    {int4 bpchar}]
	#
	# Plan to check for references from T_dta1
	#
        set GD(plan_dta1) [spi_prepare				\\
	    "select 1 from T_dta1				\\
	        where ref1 = \\$1 and ref2 = \\$2"		\\
	    {int4 bpchar}]
    }

    #
    # Initialize flags
    #
    set check_old_ref 0
    set check_new_dup 0

    switch $TG_op {
        INSERT {
	    #
	    # Must check for duplicate key on INSERT
	    #
	    set check_new_dup 1
	}
	UPDATE {
	    #
	    # Must check for duplicate key on UPDATE only if
	    # the key changes. In that case we must check for
	    # references to OLD values too.
	    #
	    if {[string compare $NEW(key1) $OLD(key1)] != 0} {
	        set check_old_ref 1
		set check_new_dup 1
	    }
	    if {[string compare $NEW(key2) $OLD(key2)] != 0} {
	        set check_old_ref 1
		set check_new_dup 1
	    }
	}
	DELETE {
	    #
	    # Must only check for references to OLD on DELETE
	    #
	    set check_old_ref 1
	}
    }

    if {$check_new_dup} {
	#
	# Check for duplicate key
	#
        spi_execp -count 1 $GD(plan_pkey1) [list $NEW(key1) $NEW(key2)]
	if {$ret == "t"} {
	    elog ERROR \\
	        "duplicate key ''$NEW(key1)'', ''$NEW(key2)'' for T_pkey1"
	}
    }

    if {$check_old_ref} {
	#
	# Check for references to OLD
	#
        set n [spi_execp -count 1 $GD(plan_dta1) [list $OLD(key1) $OLD(key2)]]
	if {$n > 0} {
	    elog ERROR \\
	        "key ''$OLD(key1)'', ''$OLD(key2)'' referenced by T_dta1"
	}
    }

    #
    # Anything is fine - let operation pass through
    #
    return OK
' language pltcl;


create trigger pkey1_before before insert or update or delete on T_pkey1
	for each row execute procedure
	trig_pkey1_before();


--
-- Trigger function to check for duplicate keys in T_pkey2
-- and to force key2 to be upper case only without leading whitespaces
--
create function trig_pkey2_before() returns trigger as E'
    #
    # Prepare plan on first call
    #
    if {![info exists GD]} {
        set GD(plan_pkey2) [spi_prepare				\\
	    "select 1 from T_pkey2				\\
	        where key1 = \\$1 and key2 = \\$2"		\\
	    {int4 bpchar}]
    }

    #
    # Convert key2 value
    #
    set NEW(key2) [string toupper [string trim $NEW(key2)]]

    #
    # Check for duplicate key
    #
    set n [spi_execp -count 1 $GD(plan_pkey2) [list $NEW(key1) $NEW(key2)]]
    if {$n > 0} {
	elog ERROR \\
	    "duplicate key ''$NEW(key1)'', ''$NEW(key2)'' for T_pkey2"
    }

    #
    # Return modified tuple in NEW
    #
    return [array get NEW]
' language pltcl;


create trigger pkey2_before before insert or update on T_pkey2
	for each row execute procedure
	trig_pkey2_before();


--
-- Trigger function to force references from T_dta2 follow changes
-- in T_pkey2 or be deleted too. This must be done AFTER the changes
-- in T_pkey2 are done so the trigger for primkey check on T_dta2
-- fired on our updates will see the new key values in T_pkey2.
--
create function trig_pkey2_after() returns trigger as E'
    #
    # Prepare plans on first call
    #
    if {![info exists GD]} {
	#
	# Plan to update references from T_dta2
	#
        set GD(plan_dta2_upd) [spi_prepare			\\
	    "update T_dta2 set ref1 = \\$3, ref2 = \\$4		\\
	        where ref1 = \\$1 and ref2 = \\$2"		\\
	    {int4 bpchar int4 bpchar}]
	#
	# Plan to delete references from T_dta2
	#
        set GD(plan_dta2_del) [spi_prepare			\\
	    "delete from T_dta2 				\\
	        where ref1 = \\$1 and ref2 = \\$2"		\\
	    {int4 bpchar}]
    }

    #
    # Initialize flags
    #
    set old_ref_follow 0
    set old_ref_delete 0

    switch $TG_op {
	UPDATE {
	    #
	    # On update we must let old references follow
	    #
	    set NEW(key2) [string toupper $NEW(key2)]

	    if {[string compare $NEW(key1) $OLD(key1)] != 0} {
	        set old_ref_follow 1
	    }
	    if {[string compare $NEW(key2) $OLD(key2)] != 0} {
	        set old_ref_follow 1
	    }
	}
	DELETE {
	    #
	    # On delete we must delete references too
	    #
	    set old_ref_delete 1
	}
    }

    if {$old_ref_follow} {
	#
	# Let old references follow and fire NOTICE message if
	# there where some
	#
        set n [spi_execp $GD(plan_dta2_upd) \\
	    [list $OLD(key1) $OLD(key2) $NEW(key1) $NEW(key2)]]
	if {$n > 0} {
	    elog NOTICE \\
		"updated $n entries in T_dta2 for new key in T_pkey2"
        }
    }

    if {$old_ref_delete} {
	#
	# delete references and fire NOTICE message if
	# there where some
	#
        set n [spi_execp $GD(plan_dta2_del) \\
	    [list $OLD(key1) $OLD(key2)]]
	if {$n > 0} {
	    elog NOTICE \\
		"deleted $n entries from T_dta2"
        }
    }

    return OK
' language pltcl;


create trigger pkey2_after after update or delete on T_pkey2
	for each row execute procedure
	trig_pkey2_after();


--
-- Generic trigger function to check references in T_dta1 and T_dta2
--
create function check_primkey() returns trigger as E'
    #
    # For every trigger/relation pair we create
    # a saved plan and hold them in GD
    #
    set plankey [list "plan" $TG_name $TG_relid]
    set planrel [list "relname" $TG_relid]

    #
    # Extract the pkey relation name
    #
    set keyidx [expr [llength $args] / 2]
    set keyrel [string tolower [lindex $args $keyidx]]

    if {![info exists GD($plankey)]} {
	#
	# We must prepare a new plan. Build up a query string
	# for the primary key check.
	#
	set keylist [lrange $args [expr $keyidx + 1] end]

        set query "select 1 from $keyrel"
	set qual " where"
	set typlist ""
	set idx 1
	foreach key $keylist {
	    set key [string tolower $key]
	    #
	    # Add the qual part to the query string
	    #
	    append query "$qual $key = \\$$idx"
	    set qual " and"

	    #
	    # Lookup the fields type in pg_attribute
	    #
	    set n [spi_exec "select T.typname			\\
	        from pg_catalog.pg_type T, pg_catalog.pg_attribute A, pg_catalog.pg_class C	\\
		where C.relname  = ''[quote $keyrel]''		\\
		  and C.oid      = A.attrelid			\\
		  and A.attname  = ''[quote $key]''		\\
		  and A.atttypid = T.oid"]
	    if {$n != 1} {
	        elog ERROR "table $keyrel doesn''t have a field named $key"
	    }

	    #
	    # Append the fields type to the argument type list
	    #
	    lappend typlist $typname
	    incr idx
	}

	#
	# Prepare the plan
	#
	set GD($plankey) [spi_prepare $query $typlist]

	#
	# Lookup and remember the table name for later error messages
	#
	spi_exec "select relname from pg_catalog.pg_class	\\
		where oid = ''$TG_relid''::oid"
	set GD($planrel) $relname
    }

    #
    # Build the argument list from the NEW row
    #
    incr keyidx -1
    set arglist ""
    foreach arg [lrange $args 0 $keyidx] {
        lappend arglist $NEW($arg)
    }

    #
    # Check for the primary key
    #
    set n [spi_execp -count 1 $GD($plankey) $arglist]
    if {$n <= 0} {
        elog ERROR "key for $GD($planrel) not in $keyrel"
    }

    #
    # Anything is fine
    #
    return OK
' language pltcl;


create trigger dta1_before before insert or update on T_dta1
	for each row execute procedure
	check_primkey('ref1', 'ref2', 'T_pkey1', 'key1', 'key2');


create trigger dta2_before before insert or update on T_dta2
	for each row execute procedure
	check_primkey('ref1', 'ref2', 'T_pkey2', 'key1', 'key2');


create function tcl_composite_arg_ref1(T_dta1) returns int as '
    return $1(ref1)
' language pltcl;

create function tcl_composite_arg_ref2(T_dta1) returns text as '
    return $1(ref2)
' language pltcl;

create function tcl_argisnull(text) returns bool as '
    argisnull 1
' language pltcl;

create function tcl_lastoid(tabname text) returns int8 as '
    spi_exec "insert into $1 default values"
    spi_lastoid
' language pltcl;


create function tcl_int4add(int4,int4) returns int4 as '
    return [expr $1 + $2]
' language pltcl;

-- We use split(n) as a quick-and-dirty way of parsing the input array
-- value, which comes in as a string like '{1,2}'.  There are better ways...

create function tcl_int4_accum(int4[], int4) returns int4[] as '
    set state [split $1 "{,}"]
    set newsum [expr {[lindex $state 1] + $2}]
    set newcnt [expr {[lindex $state 2] + 1}]
    return "{$newsum,$newcnt}"
' language pltcl;

create function tcl_int4_avg(int4[]) returns int4 as '
    set state [split $1 "{,}"]
    if {[lindex $state 2] == 0} { return_null }
    return [expr {[lindex $state 1] / [lindex $state 2]}]
' language pltcl;

create aggregate tcl_avg (
		sfunc = tcl_int4_accum,
		basetype = int4,
		stype = int4[],
		finalfunc = tcl_int4_avg,
		initcond = '{0,0}'
	);

create aggregate tcl_sum (
		sfunc = tcl_int4add,
		basetype = int4,
		stype = int4,
		initcond1 = 0
	);

create function tcl_int4lt(int4,int4) returns bool as '
    if {$1 < $2} {
        return t
    }
    return f
' language pltcl;

create function tcl_int4le(int4,int4) returns bool as '
    if {$1 <= $2} {
        return t
    }
    return f
' language pltcl;

create function tcl_int4eq(int4,int4) returns bool as '
    if {$1 == $2} {
        return t
    }
    return f
' language pltcl;

create function tcl_int4ge(int4,int4) returns bool as '
    if {$1 >= $2} {
        return t
    }
    return f
' language pltcl;

create function tcl_int4gt(int4,int4) returns bool as '
    if {$1 > $2} {
        return t
    }
    return f
' language pltcl;

create operator @< (
		leftarg = int4,
		rightarg = int4,
		procedure = tcl_int4lt
	);

create operator @<= (
		leftarg = int4,
		rightarg = int4,
		procedure = tcl_int4le
	);

create operator @= (
		leftarg = int4,
		rightarg = int4,
		procedure = tcl_int4eq
	);

create operator @>= (
		leftarg = int4,
		rightarg = int4,
		procedure = tcl_int4ge
	);

create operator @> (
		leftarg = int4,
		rightarg = int4,
		procedure = tcl_int4gt
	);

create function tcl_int4cmp(int4,int4) returns int4 as '
    if {$1 < $2} {
        return -1
    }
    if {$1 > $2} {
        return 1
    }
    return 0
' language pltcl;

CREATE OPERATOR CLASS tcl_int4_ops
	FOR TYPE int4 USING btree AS
	OPERATOR 1  @<,
	OPERATOR 2  @<=,
	OPERATOR 3  @=,
	OPERATOR 4  @>=,
	OPERATOR 5  @>,
	FUNCTION 1  tcl_int4cmp(int4,int4) ;

--
-- Test usage of Tcl's "clock" command.  In recent Tcl versions this
-- command fails without working "unknown" support, so it's a good canary
-- for initialization problems.
--
create function tcl_date_week(int4,int4,int4) returns text as $$
    return [clock format [clock scan "$2/$3/$1"] -format "%U"]
$$ language pltcl immutable;

select tcl_date_week(2010,1,24);
select tcl_date_week(2001,10,24);

-- test pltcl event triggers
create or replace function tclsnitch() returns event_trigger language pltcl as $$
  elog NOTICE "tclsnitch: $TG_event $TG_tag"
$$;

create event trigger tcl_a_snitch on ddl_command_start execute procedure tclsnitch();
create event trigger tcl_b_snitch on ddl_command_end execute procedure tclsnitch();

create or replace function foobar() returns int language sql as $$select 1;$$;
alter function foobar() cost 77;
drop function foobar();

create table foo();
drop table foo;

drop event trigger tcl_a_snitch;
drop event trigger tcl_b_snitch;

-- test use of errorCode in error handling

create function tcl_error_handling_test() returns text as $$
    global errorCode
    if {[catch { spi_exec "select no_such_column from foo;" }]} {
        array set errArray $errorCode
        if {$errArray(condition) == "undefined_table"} {
            return "expected error: $errArray(message)"
        } else {
            return "unexpected error: $errArray(condition) $errArray(message)"
        }
    } else {
        return "no error"
    }
$$ language pltcl;

select tcl_error_handling_test();

create temp table foo(f1 int);

select tcl_error_handling_test();

drop table foo;
