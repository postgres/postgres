namespace eval Queries {


proc {new} {} {
global PgAcVar
		Window show .pgaw:QueryBuilder
		PgAcVar:clean query,*
		set PgAcVar(query,oid) 0
		set PgAcVar(query,name) {}
		set PgAcVar(query,asview) 0
		set PgAcVar(query,tables) {}
		set PgAcVar(query,links) {}
		set PgAcVar(query,results) {}
		.pgaw:QueryBuilder.saveAsView configure -state normal
}


proc {open} {queryname} {
global PgAcVar
	if {! [loadQuery $queryname]} return;
	if {$PgAcVar(query,type)=="S"} then {
		set wn [Tables::getNewWindowName]
		set PgAcVar(mw,$wn,query) [subst $PgAcVar(query,sqlcmd)]
		set PgAcVar(mw,$wn,updatable) 0
		set PgAcVar(mw,$wn,isaquery) 1
		Tables::createWindow
		wm title $wn "Query result: $PgAcVar(query,name)"
		Tables::loadLayout $wn $PgAcVar(query,name)
		Tables::selectRecords $wn $PgAcVar(mw,$wn,query)
	} else {
		set answ [tk_messageBox -title [intlmsg Warning] -type yesno -message "This query is an action query!\n\n[string range $qcmd 0 30] ...\n\nDo you want to execute it?"]
		if {$answ} {
			if {[sql_exec noquiet $qcmd]} {
				tk_messageBox -title Information -message "Your query has been executed without error!"
			}
		}
	}
}


proc {design} {queryname} {
global PgAcVar
	if {! [loadQuery $queryname]} return;
	Window show .pgaw:QueryBuilder
	.pgaw:QueryBuilder.text1 delete 0.0 end
	.pgaw:QueryBuilder.text1 insert end $PgAcVar(query,sqlcmd)
	.pgaw:QueryBuilder.text2 delete 0.0 end
	.pgaw:QueryBuilder.text2 insert end $PgAcVar(query,comments)	
}


proc {loadQuery} {queryname} {
global PgAcVar CurrentDB
	set PgAcVar(query,name) $queryname
	if {[set pgres [wpg_exec $CurrentDB "select querycommand,querytype,querytables,querylinks,queryresults,querycomments,oid from pga_queries where queryname='$PgAcVar(query,name)'"]]==0} then {
		showError [intlmsg "Error retrieving query definition"]
		return 0
	}
	if {[pg_result $pgres -numTuples]==0} {
		showError [format [intlmsg "Query '%s' was not found!"] $PgAcVar(query,name)]
		pg_result $pgres -clear
		return 0
	}
	set tuple [pg_result $pgres -getTuple 0]
	set PgAcVar(query,sqlcmd)   [lindex $tuple 0]
	set PgAcVar(query,type)     [lindex $tuple 1]
	set PgAcVar(query,tables)   [lindex $tuple 2]
	set PgAcVar(query,links)    [lindex $tuple 3]
	set PgAcVar(query,results)  [lindex $tuple 4]
	set PgAcVar(query,comments) [lindex $tuple 5]
	set PgAcVar(query,oid)      [lindex $tuple 6]
	pg_result $pgres -clear
	return 1
}


proc {visualDesigner} {} {
global PgAcVar
	Window show .pgaw:VisualQuery
	VisualQueryBuilder::loadVisualLayout
	focus .pgaw:VisualQuery.fb.entt
}


proc {save} {} {
global PgAcVar CurrentDB
if {$PgAcVar(query,name)==""} then {
	showError [intlmsg "You have to supply a name for this query!"]
	focus .pgaw:QueryBuilder.eqn
} else {
	set qcmd [.pgaw:QueryBuilder.text1 get 1.0 end]
	set PgAcVar(query,comments) [.pgaw:QueryBuilder.text2 get 1.0 end]
	regsub -all "\n" $qcmd " " qcmd
	if {$qcmd==""} then {
	showError [intlmsg "This query has no commands?"]
	} else {
		if { [lindex [split [string toupper [string trim $qcmd]]] 0] == "SELECT" } {
			set qtype S
		} else {
			set qtype A
		}
		if {$PgAcVar(query,asview)} {
			wpg_select $CurrentDB "select pg_get_viewdef('$PgAcVar(query,name)') as vd" tup {
				if {$tup(vd)!="Not a view"} {
					if {[tk_messageBox -title [intlmsg Warning] -message [format [intlmsg "View '%s' already exists!\nOverwrite ?"] $PgAcVar(query,name)] -type yesno -default no]=="yes"} {
						set pg_res [wpg_exec $CurrentDB "drop view \"$PgAcVar(query,name)\""]
						if {$PgAcVar(pgsql,status)!="PGRES_COMMAND_OK"} {
							showError "[intlmsg {Error deleting view}] '$PgAcVar(query,name)'"
						}
					}
				}
			}
			set pgres [wpg_exec $CurrentDB "create view \"$PgAcVar(query,name)\" as $qcmd"]
			if {$PgAcVar(pgsql,status)!="PGRES_COMMAND_OK"} {
				showError "[intlmsg {Error defining view}]\n\n$PgAcVar(pgsql,errmsg)"
			} else {
				Mainlib::tab_click Views
				Window destroy .pgaw:QueryBuilder
			}
			catch {pg_result $pgres -clear}
		} else {
			regsub -all "'" $qcmd "''" qcmd
			regsub -all "'" $PgAcVar(query,comments) "''" PgAcVar(query,comments)
			regsub -all "'" $PgAcVar(query,results) "''" PgAcVar(query,results)
			setCursor CLOCK
			if {$PgAcVar(query,oid)==0} then {
				set pgres [wpg_exec $CurrentDB "insert into pga_queries values ('$PgAcVar(query,name)','$qtype','$qcmd','$PgAcVar(query,tables)','$PgAcVar(query,links)','$PgAcVar(query,results)','$PgAcVar(query,comments)')"]
			} else {
				set pgres [wpg_exec $CurrentDB "update pga_queries set queryname='$PgAcVar(query,name)',querytype='$qtype',querycommand='$qcmd',querytables='$PgAcVar(query,tables)',querylinks='$PgAcVar(query,links)',queryresults='$PgAcVar(query,results)',querycomments='$PgAcVar(query,comments)' where oid=$PgAcVar(query,oid)"]
			}
			setCursor DEFAULT
			if {$PgAcVar(pgsql,status)!="PGRES_COMMAND_OK"} then {
				showError "[intlmsg {Error executing query}]\n$PgAcVar(pgsql,errmsg)"
			} else {
				Mainlib::tab_click Queries
				if {$PgAcVar(query,oid)==0} {set PgAcVar(query,oid) [pg_result $pgres -oid]}
			}
		}
		catch {pg_result $pgres -clear}
	}
}
}


proc {execute} {} {
global PgAcVar
set qcmd [.pgaw:QueryBuilder.text1 get 0.0 end]
regsub -all "\n" [string trim $qcmd] " " qcmd
if {[lindex [split [string toupper $qcmd]] 0]!="SELECT"} {
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:QueryBuilder -message [intlmsg "This is an action query!\n\nExecute it?"] -type yesno -default no]=="yes"} {
		sql_exec noquiet $qcmd
	}
} else {
	set wn [Tables::getNewWindowName]
	set PgAcVar(mw,$wn,query) [subst $qcmd]
	set PgAcVar(mw,$wn,updatable) 0
	set PgAcVar(mw,$wn,isaquery) 1
	Tables::createWindow
	Tables::loadLayout $wn $PgAcVar(query,name)
	Tables::selectRecords $wn $PgAcVar(mw,$wn,query)
}
}

proc {close} {} {
global PgAcVar
	.pgaw:QueryBuilder.saveAsView configure -state normal
	set PgAcVar(query,asview) 0
	set PgAcVar(query,name) {}
	.pgaw:QueryBuilder.text1 delete 1.0 end
	Window destroy .pgaw:QueryBuilder
}


}


proc vTclWindow.pgaw:QueryBuilder {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:QueryBuilder
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 542x364+150+150
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Query builder"]
	bind $base <Key-F1> "Help::load queries"
	label $base.lqn  -borderwidth 0 -text [intlmsg {Query name}]
	entry $base.eqn  -background #fefefe -borderwidth 1 -foreground #000000  -highlightthickness 1 -selectborderwidth 0 -textvariable PgAcVar(query,name) 
	text $base.text1  -background #fefefe -borderwidth 1  -font $PgAcVar(pref,font_normal) -foreground #000000 -highlightthickness 1 -wrap word 
	label $base.lcomm -borderwidth 0 -text [intlmsg Comments]
	text $base.text2  -background #fefefe -borderwidth 1  -font $PgAcVar(pref,font_normal) -foreground #000000 -highlightthickness 1 -wrap word 
	checkbutton $base.saveAsView  -borderwidth 1  -text [intlmsg {Save this query as a view}] -variable PgAcVar(query,asview) 
	frame $base.frb \
		-height 75 -relief groove -width 125 
	button $base.frb.savebtn -command {Queries::save} \
		-borderwidth 1 -text [intlmsg {Save query definition}]
	button $base.frb.execbtn -command {Queries::execute} \
		-borderwidth 1 -text [intlmsg {Execute query}]
	button $base.frb.pgaw:VisualQueryshow -command {Queries::visualDesigner} \
		-borderwidth 1 -text [intlmsg {Visual designer}]
	button $base.frb.termbtn -command {Queries::close} \
		-borderwidth 1 -text [intlmsg Close]
	place $base.lqn  -x 5 -y 5 -anchor nw -bordermode ignore 
	place $base.eqn  -x 100 -y 1 -width 335 -height 24 -anchor nw -bordermode ignore 
	place $base.frb \
		-x 5 -y 55 -width 530 -height 35 -anchor nw -bordermode ignore 
	pack $base.frb.savebtn \
		-in $base.frb -anchor center -expand 0 -fill none -side left 
	pack $base.frb.execbtn \
		-in $base.frb -anchor center -expand 0 -fill none -side left 
	pack $base.frb.pgaw:VisualQueryshow \
		-in $base.frb -anchor center -expand 0 -fill none -side left 
	pack $base.frb.termbtn \
		-in $base.frb -anchor center -expand 0 -fill none -side right 
	place $base.text1  -x 5 -y 90 -width 530 -height 160 -anchor nw -bordermode ignore 
	place $base.lcomm -x 5 -y 255
	place $base.text2  -x 5 -y 270 -width 530 -height 86 -anchor nw -bordermode ignore 
	place $base.saveAsView  -x 5 -y 30 -height 25 -anchor nw -bordermode ignore 
}

