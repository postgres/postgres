namespace eval Database {

proc {getTablesList} {} {
global CurrentDB PgAcVar
	set tlist {}
	if {[catch {
		wpg_select $CurrentDB "select c.relname,count(c.relname) from pg_class C, pg_rewrite R where (r.ev_class = C.oid) and (r.ev_type = '1') group by relname" rec {
			if {$rec(count)!=0} {
				set itsaview($rec(relname)) 1
			}
		}
		if {! $PgAcVar(pref,systemtables)} {
			wpg_select $CurrentDB "select relname from pg_class where (relname !~ '^pg_') and (relkind='r') order by relname" rec {
				if {![regexp "^pga_" $rec(relname)]} then {
					if {![info exists itsaview($rec(relname))]} {
						lappend tlist $rec(relname)
					}
				}
			}
		} else {
			wpg_select $CurrentDB "select relname from pg_class where (relkind='r') order by relname" rec {
				if {![info exists itsaview($rec(relname))]} {
					lappend tlist $rec(relname)
				}
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
