namespace eval Views {

proc {new} {} {
global PgAcVar
	set PgAcVar(query,oid) 0
	set PgAcVar(query,name) {}
	Window show .pgaw:QueryBuilder
	set PgAcVar(query,asview) 1
	.pgaw:QueryBuilder.saveAsView configure -state disabled
}


proc {open} {viewname} {
global PgAcVar
	if {$viewname==""} return;
	set wn [Tables::getNewWindowName]
	Tables::createWindow
	set PgAcVar(mw,$wn,query) "select * from \"$viewname\""
	set PgAcVar(mw,$wn,isaquery) 0
	set PgAcVar(mw,$wn,updatable) 0
	Tables::loadLayout $wn $viewname
	Tables::selectRecords $wn $PgAcVar(mw,$wn,query)
}


proc {design} {viewname} {
global PgAcVar CurrentDB
	set vd {}
	wpg_select $CurrentDB "select pg_get_viewdef('$viewname')as vd" tup {
		set vd $tup(vd)
	}
	if {$vd==""} {
		showError "[intlmsg {Error retrieving view definition for}] '$viewname'!"
		return
	}
	Window show .pgaw:QueryBuilder
	.pgaw:QueryBuilder.text1 delete 0.0 end
	.pgaw:QueryBuilder.text1 insert end $vd
	set PgAcVar(query,asview) 1
	.pgaw:QueryBuilder.saveAsView configure -state disabled
	set PgAcVar(query,name) $viewname
}


}
