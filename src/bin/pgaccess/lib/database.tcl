namespace eval Database {

proc {getTablesList} {} {
global CurrentDB PgAcVar
	set tlist {}
	if {[catch {
		# As of Postgres 7.1, testing for view-ness is not needed
		# because relkind = 'r' eliminates views.  But we should
		# leave the code in for awhile yet, so as not to fail when
		# running against older releases.
		wpg_select $CurrentDB "select viewname from pg_views" rec {
			set itsaview($rec(viewname)) 1
		}
		if {! $PgAcVar(pref,systemtables)} {
			set sysconstraint "and (relname !~ '^pg_') and (relname !~ '^pga_')"
		} else {
			set sysconstraint ""
		}
		wpg_select $CurrentDB "select relname from pg_class where (relkind='r') $sysconstraint order by relname" rec {
			if {![info exists itsaview($rec(relname))]} {
				lappend tlist $rec(relname)
			}
		}
	} gterrmsg]} {
		showError $gterrmsg
	}
	return $tlist
}


proc {vacuum} {} {
global PgAcVar CurrentDB
	if {$CurrentDB==""} return;
	set PgAcVar(statusline,dbname) [format [intlmsg "vacuuming database %s ..."] $PgAcVar(currentdb,dbname)]
	setCursor CLOCK
	set pgres [wpg_exec $CurrentDB "vacuum;"]
	catch {pg_result $pgres -clear}
	setCursor DEFAULT
	set PgAcVar(statusline,dbname) $PgAcVar(currentdb,dbname)
}


proc {getPgType} {oid} {
global CurrentDB
	set temp "unknown"
	wpg_select $CurrentDB "select typname from pg_type where oid=$oid" rec {
		set temp $rec(typname)
	}
	return $temp
}


proc {executeUpdate} {sqlcmd} {
global CurrentDB
	return [sql_exec noquiet $sqlcmd]
}

}
