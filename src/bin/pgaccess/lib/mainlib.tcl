namespace eval Mainlib {

proc {cmd_Delete} {} {
global PgAcVar CurrentDB
if {$CurrentDB==""} return;
set objtodelete [get_dwlb_Selection]
if {$objtodelete==""} return;
set delmsg [format [intlmsg "You are going to delete\n\n %s \n\nProceed?"] $objtodelete]
if {[tk_messageBox -title [intlmsg "FINAL WARNING"] -parent .pgaw:Main -message $delmsg -type yesno -default no]=="no"} { return }
switch $PgAcVar(activetab) {
	Tables {
		sql_exec noquiet "drop table \"$objtodelete\""
		sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
		cmd_Tables
	}
	Schema {
		sql_exec quiet "delete from pga_schema where schemaname='$objtodelete'"
		cmd_Schema
	}
	Views {
		sql_exec noquiet "drop view \"$objtodelete\""
		sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
		cmd_Views
	}
	Queries {
		sql_exec quiet "delete from pga_queries where queryname='$objtodelete'"
		sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
		cmd_Queries
	}
	Scripts {
		sql_exec quiet "delete from pga_scripts where scriptname='$objtodelete'"
		cmd_Scripts
	}
	Forms {
		sql_exec quiet "delete from pga_forms where formname='$objtodelete'"
		cmd_Forms
	}
	Sequences {
		sql_exec quiet "drop sequence \"$objtodelete\""
		cmd_Sequences
	}
	Functions {
		delete_function $objtodelete
		cmd_Functions
	}
	Reports {
		sql_exec noquiet "delete from pga_reports where reportname='$objtodelete'"
		cmd_Reports
	}
	Users {
		sql_exec noquiet "drop user \"$objtodelete\""
		cmd_Users
	}
}
}

proc {cmd_Design} {} {
global PgAcVar CurrentDB
if {$CurrentDB==""} return;
if {[.pgaw:Main.lb curselection]==""} return;
set objname [.pgaw:Main.lb get [.pgaw:Main.lb curselection]]
set tablename $objname
switch $PgAcVar(activetab) {
	Tables  {
		Tables::design $objname
	}
	Schema  {
		Schema::open $objname
	}
	Queries {
		Queries::design $objname
	}
	Views {
		Views::design $objname
	}
	Scripts {
		Scripts::design $objname
	}
	Forms {
		Forms::design $objname
	}
	Functions {
		Functions::design $objname
	}
	Reports {
		Reports::design $objname
	}
	Users {
		Users::design $objname
	}
}
}

proc {cmd_Forms} {} {
global CurrentDB
	setCursor CLOCK
	.pgaw:Main.lb delete 0 end
	catch {
		wpg_select $CurrentDB "select formname from pga_forms order by formname" rec {
			.pgaw:Main.lb insert end $rec(formname)
		}
	}
	setCursor DEFAULT
}


proc {cmd_Functions} {} {
global CurrentDB
	set maxim 16384
	setCursor CLOCK
	catch {
		wpg_select $CurrentDB "select oid from pg_database where datname='template1'" rec {
			set maxim $rec(oid)
		}
	}
	.pgaw:Main.lb delete 0 end
	catch {
		wpg_select $CurrentDB "select proname from pg_proc where oid>$maxim order by proname" rec {
			.pgaw:Main.lb insert end $rec(proname)
		}	
	}
	setCursor DEFAULT
}


proc {cmd_Import_Export} {how} {
global PgAcVar CurrentDB
	if {$CurrentDB==""} return;
	Window show .pgaw:ImportExport
	set PgAcVar(impexp,tablename) {}
	set PgAcVar(impexp,filename) {}
	set PgAcVar(impexp,delimiter) {}
	if {$PgAcVar(activetab)=="Tables"} {
		set tn [get_dwlb_Selection]
		set PgAcVar(impexp,tablename) $tn
		if {$tn!=""} {set PgAcVar(impexp,filename) "$tn.txt"}
	}
	.pgaw:ImportExport.expbtn configure -text [intlmsg $how]
}


proc {cmd_New} {} {
global PgAcVar CurrentDB
if {$CurrentDB==""} return;
switch $PgAcVar(activetab) {
	Tables {
		Tables::new
	}
	Schema {
		Schema::new
	}
	Queries {
		Queries::new
	}
	Users {
		Users::new
	}
	Views {
		Views::new
	}
	Sequences {
		Sequences::new
	}
	Reports {
		Reports::new
	}
	Forms {
		Forms::new
	}
	Scripts {
		Scripts::new
	}
	Functions {
		Functions::new
	}
}
}


proc {cmd_Open} {} {
global PgAcVar CurrentDB
	if {$CurrentDB==""} return;
	set objname [get_dwlb_Selection]
	if {$objname==""} return;
	switch $PgAcVar(activetab) {
		Tables		{ Tables::open $objname }
		Schema		{ Schema::open $objname }
		Forms		{ Forms::open $objname }
		Scripts		{ Scripts::open $objname }
		Queries		{ Queries::open $objname }
		Views		{ Views::open $objname }
		Sequences	{ Sequences::open $objname }
		Functions	{ Functions::design $objname }
		Reports		{ Reports::open $objname }
	}
}



proc {cmd_Queries} {} {
global CurrentDB
	.pgaw:Main.lb delete 0 end
	catch {
		wpg_select $CurrentDB "select queryname from pga_queries order by queryname" rec {
			.pgaw:Main.lb insert end $rec(queryname)
		}
	}
}


proc {cmd_Rename} {} {
global PgAcVar CurrentDB
	if {$CurrentDB==""} return;
	if {$PgAcVar(activetab)=="Views"} return;
	if {$PgAcVar(activetab)=="Sequences"} return;
	if {$PgAcVar(activetab)=="Functions"} return;
	if {$PgAcVar(activetab)=="Users"} return;
	set temp [get_dwlb_Selection]
	if {$temp==""} {
		tk_messageBox -title [intlmsg Warning] -parent .pgaw:Main -message [intlmsg "Please select an object first!"]
		return;
	}
	set PgAcVar(Old_Object_Name) $temp
	Window show .pgaw:RenameObject
	wm transient .pgaw:RenameObject .pgaw:Main
}


proc {cmd_Reports} {} {
global CurrentDB
	setCursor CLOCK
	catch {
		wpg_select $CurrentDB "select reportname from pga_reports order by reportname" rec {
		.pgaw:Main.lb insert end "$rec(reportname)"
		}
	}
	setCursor DEFAULT
}

proc {cmd_Users} {} {
global CurrentDB
	setCursor CLOCK
	.pgaw:Main.lb delete 0 end
	catch {
		wpg_select $CurrentDB "select * from pg_user order by usename" rec {
			.pgaw:Main.lb insert end $rec(usename)
		}
	}
	setCursor DEFAULT
}


proc {cmd_Scripts} {} {
global CurrentDB
	setCursor CLOCK
	.pgaw:Main.lb delete 0 end
	catch {
		wpg_select $CurrentDB "select scriptname from pga_scripts order by scriptname" rec {
		.pgaw:Main.lb insert end $rec(scriptname)
		}
	}
	setCursor DEFAULT
}


proc {cmd_Sequences} {} {
global CurrentDB

setCursor CLOCK
.pgaw:Main.lb delete 0 end
catch {
	wpg_select $CurrentDB "select relname from pg_class where (relname not like 'pg_%') and (relkind='S') order by relname" rec {
		.pgaw:Main.lb insert end $rec(relname)
	}
}
setCursor DEFAULT
}

proc {cmd_Tables} {} {
global CurrentDB
	setCursor CLOCK
	.pgaw:Main.lb delete 0 end
	foreach tbl [Database::getTablesList] {.pgaw:Main.lb insert end $tbl}
	setCursor DEFAULT
}

proc {cmd_Schema} {} {
global CurrentDB
.pgaw:Main.lb delete 0 end
catch {
	wpg_select $CurrentDB "select schemaname from pga_schema order by schemaname" rec {
		.pgaw:Main.lb insert end $rec(schemaname)
	}
}
}

proc {cmd_Views} {} {
global CurrentDB
setCursor CLOCK
.pgaw:Main.lb delete 0 end
catch {
	wpg_select $CurrentDB "select c.relname,count(c.relname) from pg_class C, pg_rewrite R where (relname !~ '^pg_') and (r.ev_class = C.oid) and (r.ev_type = '1') group by relname" rec {
		if {$rec(count)!=0} {
			set itsaview($rec(relname)) 1
		}
	}
	wpg_select $CurrentDB "select relname from pg_class where (relname !~ '^pg_') and (relkind='r') and (relhasrules) order by relname" rec {
		if {[info exists itsaview($rec(relname))]} {
			.pgaw:Main.lb insert end $rec(relname)
		}
	}
}
setCursor DEFAULT
}

proc {delete_function} {objname} {
global CurrentDB
	wpg_select $CurrentDB "select proargtypes,pronargs from pg_proc where proname='$objname'" rec {
		set PgAcVar(function,parameters) $rec(proargtypes)
		set nrpar $rec(pronargs)
	}
	set lispar {}
	for {set i 0} {$i<$nrpar} {incr i} {
		lappend lispar [Database::getPgType [lindex $PgAcVar(function,parameters) $i]]
	}
	set lispar [join $lispar ,]
	sql_exec noquiet "drop function $objname ($lispar)"
}


proc {draw_tabs} {} {
global PgAcVar
	set ypos 85
	foreach tab $PgAcVar(tablist) {
		label .pgaw:Main.tab$tab -borderwidth 1  -anchor w -relief raised -text [intlmsg $tab]
		place .pgaw:Main.tab$tab -x 10 -y $ypos -height 25 -width 82 -anchor nw -bordermode ignore
		lower .pgaw:Main.tab$tab
		bind .pgaw:Main.tab$tab <Button-1> "Mainlib::tab_click $tab"
		incr ypos 25
	}
	set PgAcVar(activetab) ""
}


proc {get_dwlb_Selection} {} {
	set temp [.pgaw:Main.lb curselection]
	if {$temp==""} return "";
	return [.pgaw:Main.lb get $temp]
}




proc {sqlw_display} {msg} {
	if {![winfo exists .pgaw:SQLWindow]} {return}
	.pgaw:SQLWindow.f.t insert end "$msg\n\n"
	.pgaw:SQLWindow.f.t see end
	set nrlines [lindex [split [.pgaw:SQLWindow.f.t index end] .] 0]
	if {$nrlines>50} {
		.pgaw:SQLWindow.f.t delete 1.0 3.0
	}
}


proc {open_database} {} {
global PgAcVar CurrentDB
setCursor CLOCK
if {$PgAcVar(opendb,username)!=""} {
	if {$PgAcVar(opendb,host)!=""} {
		set connres [catch {set newdbc [pg_connect -conninfo "host=$PgAcVar(opendb,host) port=$PgAcVar(opendb,pgport) dbname=$PgAcVar(opendb,dbname) user=$PgAcVar(opendb,username) password=$PgAcVar(opendb,password)"]} msg]
	} else {
		set connres [catch {set newdbc [pg_connect -conninfo "dbname=$PgAcVar(opendb,dbname) user=$PgAcVar(opendb,username) password=$PgAcVar(opendb,password)"]} msg]
	}
} else {
	set connres [catch {set newdbc [pg_connect $PgAcVar(opendb,dbname) -host $PgAcVar(opendb,host) -port $PgAcVar(opendb,pgport)]} msg]
}
if {$connres} {
	setCursor DEFAULT
	showError [format [intlmsg "Error trying to connect to database '%s' on host %s \n\nPostgreSQL error message:%s"] $PgAcVar(opendb,dbname) $PgAcVar(opendb,host) $msg"]
	return $msg
} else {
	catch {pg_disconnect $CurrentDB}
	set CurrentDB $newdbc
	set PgAcVar(currentdb,host) $PgAcVar(opendb,host)
	set PgAcVar(currentdb,pgport) $PgAcVar(opendb,pgport)
	set PgAcVar(currentdb,dbname) $PgAcVar(opendb,dbname)
	set PgAcVar(currentdb,username) $PgAcVar(opendb,username)
	set PgAcVar(currentdb,password) $PgAcVar(opendb,password)
	set PgAcVar(statusline,dbname) $PgAcVar(currentdb,dbname)
	set PgAcVar(pref,lastdb) $PgAcVar(currentdb,dbname)
	set PgAcVar(pref,lasthost) $PgAcVar(currentdb,host)
	set PgAcVar(pref,lastport) $PgAcVar(currentdb,pgport)
	set PgAcVar(pref,lastusername) $PgAcVar(currentdb,username)
	Preferences::save
	catch {setCursor DEFAULT ; Window hide .pgaw:OpenDB}
	tab_click Tables
	# Check for pga_ tables
	foreach {table structure} {pga_queries {queryname varchar(64),querytype char(1),querycommand text,querytables text,querylinks text,queryresults text,querycomments text} pga_forms {formname varchar(64),formsource text} pga_scripts {scriptname varchar(64),scriptsource text} pga_reports {reportname varchar(64),reportsource text,reportbody text,reportprocs text,reportoptions text} pga_schema {schemaname varchar(64),schematables text,schemalinks text}} {
		set pgres [wpg_exec $CurrentDB "select relname from pg_class where relname='$table'"]
		if {$PgAcVar(pgsql,status)!="PGRES_TUPLES_OK"} {
			showError "[intlmsg {FATAL ERROR searching for PgAccess system tables}] : $PgAcVar(pgsql,errmsg)\nStatus:$PgAcVar(pgsql,status)"
			catch {pg_disconnect $CurrentDB}
			exit
		} elseif {[pg_result $pgres -numTuples]==0} {
			pg_result $pgres -clear
			sql_exec quiet "create table $table ($structure)"
			sql_exec quiet "grant ALL on $table to PUBLIC"
		} else {
			foreach fieldspec [split $structure ,] {
				set field [lindex [split $fieldspec] 0]
				set pgres [wpg_exec $CurrentDB "select \"$field\" from \"$table\""]
				if {$PgAcVar(pgsql,status)!="PGRES_TUPLES_OK"} {
					if {![regexp "attribute '$field' not found" $PgAcVar(pgsql,errmsg)]} {
						showError "[intlmsg {FATAL ERROR upgrading PgAccess table}] $table: $PgAcVar(pgsql,errmsg)\nStatus:$PgAcVar(pgsql,status)"
						catch {pg_disconnect $CurrentDB}
						exit
					} else {
						pg_result $pgres -clear
						sql_exec quiet "alter table \"$table\" add column $fieldspec "
					}
				}
			}
		}
		catch {pg_result $pgres -clear}
	}
	
	# searching for autoexec script
	wpg_select $CurrentDB "select * from pga_scripts where scriptname ~* '^autoexec$'" recd {
		eval $recd(scriptsource)
	}
	return ""
}
}


proc {tab_click} {tabname} {
global PgAcVar CurrentDB
	set w .pgaw:Main.tab$tabname
	if {$CurrentDB==""} return;
	set curtab $tabname
	#if {$PgAcVar(activetab)==$curtab} return;
	.pgaw:Main.btndesign configure -state disabled
	if {$PgAcVar(activetab)!=""} {
		place .pgaw:Main.tab$PgAcVar(activetab) -x 10
		.pgaw:Main.tab$PgAcVar(activetab) configure -font $PgAcVar(pref,font_normal)
	}
	$w configure -font $PgAcVar(pref,font_bold)
	place $w -x 7
	place .pgaw:Main.lmask -x 80 -y [expr 86+25*[lsearch -exact $PgAcVar(tablist) $curtab]]
	set PgAcVar(activetab) $curtab
	# Tabs where button Design is enabled
	if {[lsearch {Tables Schema Scripts Queries Functions Views Reports Forms Users} $PgAcVar(activetab)]!=-1} {
		.pgaw:Main.btndesign configure -state normal
	}
	.pgaw:Main.lb delete 0 end
	cmd_$curtab
}



}


proc vTclWindow.pgaw:Main {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:Main
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel \
		-background #efefef -cursor left_ptr
	wm focusmodel $base passive
	wm geometry $base 332x390+96+172
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "PostgreSQL access"
	bind $base <Key-F1> "Help::load index"
	label $base.labframe \
		-relief raised 
	listbox $base.lb \
		-background #fefefe \
		-selectbackground #c3c3c3 \
		-foreground black -highlightthickness 0 -selectborderwidth 0 \
		-yscrollcommand {.pgaw:Main.sb set} 
	bind $base.lb <Double-Button-1> {
		Mainlib::cmd_Open
	}
	button $base.btnnew \
		-borderwidth 1 -command Mainlib::cmd_New -text [intlmsg New]
	button $base.btnopen \
		-borderwidth 1 -command Mainlib::cmd_Open -text [intlmsg Open]
	button $base.btndesign \
		-borderwidth 1 -command Mainlib::cmd_Design -text [intlmsg Design]
	label $base.lmask \
		-borderwidth 0 \
		-text {  } 
	frame $base.fm \
        -borderwidth 1 -height 75 -relief raised -width 125 
	menubutton $base.fm.mndb \
		-borderwidth 1 -font $PgAcVar(pref,font_normal) \
		-menu .pgaw:Main.fm.mndb.01 -padx 4 -pady 3 -text [intlmsg Database]
	menu $base.fm.mndb.01 \
		-borderwidth 1 -font $PgAcVar(pref,font_normal) \
		-tearoff 0 
	$base.fm.mndb.01 add command \
		-command {Window show .pgaw:NewDatabase ; wm transient .pgaw:NewDatabase .pgaw:Main} -label [intlmsg New]
	$base.fm.mndb.01 add command \
		-command {
Window show .pgaw:OpenDB
set PgAcVar(opendb,host) $PgAcVar(currentdb,host)
set PgAcVar(opendb,pgport) $PgAcVar(currentdb,pgport)
focus .pgaw:OpenDB.f1.e3
wm transient .pgaw:OpenDB .pgaw:Main
.pgaw:OpenDB.f1.e3 selection range 0 end} \
		-label [intlmsg Open] -font $PgAcVar(pref,font_normal)
	$base.fm.mndb.01 add command \
		-command {.pgaw:Main.lb delete 0 end
set CurrentDB {}
set PgAcVar(currentdb,dbname) {}
set PgAcVar(statusline,dbname) {}} \
		-label [intlmsg Close]
	$base.fm.mndb.01 add command \
		-command Database::vacuum -label [intlmsg Vacuum]
	$base.fm.mndb.01 add separator
	$base.fm.mndb.01 add command \
		-command {Mainlib::cmd_Import_Export Import} -label [intlmsg {Import table}]
	$base.fm.mndb.01 add command \
		-command {Mainlib::cmd_Import_Export Export} -label [intlmsg {Export table}]
	$base.fm.mndb.01 add separator
	$base.fm.mndb.01 add command \
		-command Preferences::configure -label [intlmsg Preferences]
	$base.fm.mndb.01 add command \
		-command "Window show .pgaw:SQLWindow" -label [intlmsg "SQL window"]
	$base.fm.mndb.01 add separator
	$base.fm.mndb.01 add command \
		-command {
set PgAcVar(activetab) {}
Preferences::save
catch {pg_disconnect $CurrentDB}
exit} -label [intlmsg Exit]
	label $base.lshost \
		-relief groove -text localhost -textvariable PgAcVar(currentdb,host) 
	label $base.lsdbname \
		-anchor w \
		-relief groove -textvariable PgAcVar(statusline,dbname) 
	scrollbar $base.sb \
		-borderwidth 1 -command {.pgaw:Main.lb yview} -orient vert 
	menubutton $base.fm.mnob \
		-borderwidth 1 \
		-menu .pgaw:Main.fm.mnob.m -font $PgAcVar(pref,font_normal) -text [intlmsg Object]
	menu $base.fm.mnob.m \
		-borderwidth 1 -font $PgAcVar(pref,font_normal) \
		-tearoff 0 
	$base.fm.mnob.m add command \
		-command Mainlib::cmd_New -font $PgAcVar(pref,font_normal) -label [intlmsg New] 
	$base.fm.mnob.m add command \
		-command Mainlib::cmd_Delete -label [intlmsg Delete] 
	$base.fm.mnob.m add command \
		-command Mainlib::cmd_Rename -label [intlmsg Rename] 
	menubutton $base.fm.mnhelp \
		-borderwidth 1 \
		-menu .pgaw:Main.fm.mnhelp.m -font $PgAcVar(pref,font_normal) -text [intlmsg Help]
	menu $base.fm.mnhelp.m \
		-borderwidth 1 -font $PgAcVar(pref,font_normal) \
		-tearoff 0 
	$base.fm.mnhelp.m add command \
		-label [intlmsg Contents] -command {Help::load index}
	$base.fm.mnhelp.m add command \
		-label PostgreSQL  -command {Help::load postgresql}
	$base.fm.mnhelp.m add separator
	$base.fm.mnhelp.m add command \
		-command {Window show .pgaw:About} -label [intlmsg About]
	place $base.labframe \
		-x 80 -y 30 -width 246 -height 325 -anchor nw -bordermode ignore 
	place $base.lb \
		-x 90 -y 75 -width 210 -height 272 -anchor nw -bordermode ignore 
	place $base.btnnew \
		-x 89 -y 40 -width 75 -height 25 -anchor nw -bordermode ignore 
	place $base.btnopen \
		-x 166 -y 40 -width 75 -height 25 -anchor nw -bordermode ignore 
	place $base.btndesign \
		-x 243 -y 40 -width 76 -height 25 -anchor nw -bordermode ignore 
	place $base.lmask \
		-x 1550 -y 4500 -width 10 -height 23 -anchor nw -bordermode ignore 
	place $base.lshost \
		-x 3 -y 370 -width 91 -height 20 -anchor nw -bordermode ignore 
	place $base.lsdbname \
		-x 95 -y 370 -width 233 -height 20 -anchor nw -bordermode ignore 
	place $base.sb \
		-x 301 -y 74 -width 18 -height 274 -anchor nw -bordermode ignore 
	place $base.fm \
        -x 1 -y 0 -width 331 -height 25 -anchor nw -bordermode ignore 
	pack $base.fm.mndb \
        -in .pgaw:Main.fm -anchor center -expand 0 -fill none -side left 
	pack $base.fm.mnob \
        -in .pgaw:Main.fm -anchor center -expand 0 -fill none -side left 
	pack $base.fm.mnhelp \
        -in .pgaw:Main.fm -anchor center -expand 0 -fill none -side right 
}

proc vTclWindow.pgaw:ImportExport {base} {
	if {$base == ""} {
		set base .pgaw:ImportExport
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 287x151+259+304
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base [intlmsg "Import-Export table"]
	label $base.l1  -borderwidth 0 -text [intlmsg {Table name}]
	entry $base.e1  -background #fefefe -borderwidth 1 -textvariable PgAcVar(impexp,tablename) 
	label $base.l2  -borderwidth 0 -text [intlmsg {File name}]
	entry $base.e2  -background #fefefe -borderwidth 1 -textvariable PgAcVar(impexp,filename) 
	label $base.l3  -borderwidth 0 -text [intlmsg {Field delimiter}]
	entry $base.e3  -background #fefefe -borderwidth 1 -textvariable PgAcVar(impexp,delimiter) 
	button $base.expbtn  -borderwidth 1  -command {if {$PgAcVar(impexp,tablename)==""} {
	showError [intlmsg "You have to supply a table name!"]
} elseif {$PgAcVar(impexp,filename)==""} {
	showError [intlmsg "You have to supply a external file name!"]
} else {
	if {$PgAcVar(impexp,delimiter)==""} {
		set sup ""
	} else {
		set sup " USING DELIMITERS '$PgAcVar(impexp,delimiter)'"
	}
	if {[.pgaw:ImportExport.expbtn cget -text]=="Import"} {
		set oper "FROM"
	} else {
		set oper "TO"
	}
		if {$PgAcVar(impexp,withoids)} {
				set sup2 " WITH OIDS "
		} else {
				set sup2 ""
		}
	set sqlcmd "COPY \"$PgAcVar(impexp,tablename)\" $sup2 $oper '$PgAcVar(impexp,filename)'$sup"
	setCursor CLOCK
	if {[sql_exec noquiet $sqlcmd]} {
		tk_messageBox -title [intlmsg Information] -parent .pgaw:ImportExport -message [intlmsg "Operation completed!"]
		Window destroy .pgaw:ImportExport
	}
	setCursor DEFAULT
}}  -text Export 
	button $base.cancelbtn  -borderwidth 1 -command {Window destroy .pgaw:ImportExport} -text [intlmsg Cancel]
	checkbutton $base.oicb  -borderwidth 1  -text [intlmsg {with OIDs}] -variable PgAcVar(impexp,withoids) 
	place $base.l1  -x 15 -y 15 -anchor nw -bordermode ignore 
	place $base.e1  -x 115 -y 10 -height 22 -anchor nw -bordermode ignore 
	place $base.l2  -x 15 -y 45 -anchor nw -bordermode ignore 
	place $base.e2  -x 115 -y 40 -height 22 -anchor nw -bordermode ignore 
	place $base.l3  -x 15 -y 75 -height 18 -anchor nw -bordermode ignore 
	place $base.e3  -x 115 -y 74 -width 33 -height 22 -anchor nw -bordermode ignore 
	place $base.expbtn  -x 60 -y 110 -height 25 -width 75 -anchor nw -bordermode ignore 
	place $base.cancelbtn  -x 155 -y 110 -height 25 -width 75 -anchor nw -bordermode ignore 
	place $base.oicb  -x 170 -y 75 -anchor nw -bordermode ignore
}



proc vTclWindow.pgaw:RenameObject {base} {
	if {$base == ""} {
		set base .pgaw:RenameObject
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 272x105+294+262
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base [intlmsg "Rename"]
	label $base.l1  -borderwidth 0 -text [intlmsg {New name}]
	entry $base.e1  -background #fefefe -borderwidth 1 -textvariable PgAcVar(New_Object_Name) 
	button $base.b1  -borderwidth 1  -command {
			if {$PgAcVar(New_Object_Name)==""} {
				showError [intlmsg "You must give object a new name!"]
			} elseif {$PgAcVar(activetab)=="Tables"} {
				set retval [sql_exec noquiet "alter table \"$PgAcVar(Old_Object_Name)\" rename to \"$PgAcVar(New_Object_Name)\""]
				if {$retval} {
					sql_exec quiet "update pga_layout set tablename='$PgAcVar(New_Object_Name)' where tablename='$PgAcVar(Old_Object_Name)'"
					Mainlib::cmd_Tables
					Window destroy .pgaw:RenameObject
				}
			} elseif {$PgAcVar(activetab)=="Queries"} {
				set pgres [wpg_exec $CurrentDB "select * from pga_queries where queryname='$PgAcVar(New_Object_Name)'"]
				if {$PgAcVar(pgsql,status)!="PGRES_TUPLES_OK"} {
					showError "[intlmsg {Error retrieving from}] pga_queries\n$PgAcVar(pgsql,errmsg)\n$PgAcVar(pgsql,status)"
				} elseif {[pg_result $pgres -numTuples]>0} {
					showError [format [intlmsg "Query '%s' already exists!"] $PgAcVar(New_Object_Name)]
				} else {
					sql_exec noquiet "update pga_queries set queryname='$PgAcVar(New_Object_Name)' where queryname='$PgAcVar(Old_Object_Name)'"
					sql_exec noquiet "update pga_layout set tablename='$PgAcVar(New_Object_Name)' where tablename='$PgAcVar(Old_Object_Name)'"
					Mainlib::cmd_Queries
					Window destroy .pgaw:RenameObject
				}
				catch {pg_result $pgres -clear}
			} elseif {$PgAcVar(activetab)=="Forms"} {
				set pgres [wpg_exec $CurrentDB "select * from pga_forms where formname='$PgAcVar(New_Object_Name)'"]
				if {$PgAcVar(pgsql,status)!="PGRES_TUPLES_OK"} {
					showError "[intlmsg {Error retrieving from}] pga_forms\n$PgAcVar(pgsql,errmsg)\n$PgAcVar(pgsql,status)"
				} elseif {[pg_result $pgres -numTuples]>0} {
					showError [format [intlmsg "Form '%s' already exists!"] $PgAcVar(New_Object_Name)]
				} else {
					sql_exec noquiet "update pga_forms set formname='$PgAcVar(New_Object_Name)' where formname='$PgAcVar(Old_Object_Name)'"
					Mainlib::cmd_Forms
					Window destroy .pgaw:RenameObject
				}
				catch {pg_result $pgres -clear}
			} elseif {$PgAcVar(activetab)=="Scripts"} {
				set pgres [wpg_exec $CurrentDB "select * from pga_scripts where scriptname='$PgAcVar(New_Object_Name)'"]
				if {$PgAcVar(pgsql,status)!="PGRES_TUPLES_OK"} {
					showError "[intlmsg {Error retrieving from}] pga_scripts\n$PgAcVar(pgsql,errmsg)\n$PgAcVar(pgsql,status)"
				} elseif {[pg_result $pgres -numTuples]>0} {
					showError [format [intlmsg "Script '%s' already exists!"] $PgAcVar(New_Object_Name)]
				} else {
					sql_exec noquiet "update pga_scripts set scriptname='$PgAcVar(New_Object_Name)' where scriptname='$PgAcVar(Old_Object_Name)'"
					Mainlib::cmd_Scripts
					Window destroy .pgaw:RenameObject
				}
				catch {pg_result $pgres -clear}
			} elseif {$PgAcVar(activetab)=="Schema"} {
				set pgres [wpg_exec $CurrentDB "select * from pga_schema where schemaname='$PgAcVar(New_Object_Name)'"]
				if {$PgAcVar(pgsql,status)!="PGRES_TUPLES_OK"} {
					showError "[intlmsg {Error retrieving from}] pga_schema\n$PgAcVar(pgsql,errmsg)\n$PgAcVar(pgsql,status)"
				} elseif {[pg_result $pgres -numTuples]>0} {
					showError [format [intlmsg "Schema '%s' already exists!"] $PgAcVar(New_Object_Name)]
				} else {
					sql_exec noquiet "update pga_schema set schemaname='$PgAcVar(New_Object_Name)' where schemaname='$PgAcVar(Old_Object_Name)'"
					Mainlib::cmd_Schema
					Window destroy .pgaw:RenameObject
				}
				catch {pg_result $pgres -clear}
			}
	   } -text [intlmsg Rename]
	button $base.b2  -borderwidth 1 -command {Window destroy .pgaw:RenameObject} -text [intlmsg Cancel]
	place $base.l1  -x 15 -y 28 -anchor nw -bordermode ignore 
	place $base.e1  -x 100 -y 25 -anchor nw -bordermode ignore 
	place $base.b1  -x 55 -y 65 -width 80 -anchor nw -bordermode ignore 
	place $base.b2  -x 155 -y 65 -width 80 -anchor nw -bordermode ignore
}

proc vTclWindow.pgaw:NewDatabase {base} {
	if {$base == ""} {
		set base .pgaw:NewDatabase
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 272x105+294+262
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base [intlmsg "New"]
	label $base.l1  -borderwidth 0 -text [intlmsg {Name}]
	entry $base.e1  -background #fefefe -borderwidth 1 -textvariable PgAcVar(New_Database_Name) 
	button $base.b1  -borderwidth 1  -command {
		set retval [sql_exec noquiet "create database $PgAcVar(New_Database_Name)"]
		if {$retval} {
			Window destroy .pgaw:NewDatabase
		}
	} -text [intlmsg Create]
	button $base.b2  -borderwidth 1 -command {Window destroy .pgaw:NewDatabase} -text [intlmsg Cancel]
	place $base.l1  -x 15 -y 28 -anchor nw -bordermode ignore 
	place $base.e1  -x 100 -y 25 -anchor nw -bordermode ignore 
	place $base.b1  -x 55 -y 65 -width 80 -anchor nw -bordermode ignore 
	place $base.b2  -x 155 -y 65 -width 80 -anchor nw -bordermode ignore
}


proc vTclWindow.pgaw:GetParameter {base} {
	if {$base == ""} {
		set base .pgaw:GetParameter
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	set sw [winfo screenwidth .]
	set sh [winfo screenheight .]
	set x [expr ($sw - 297)/2]
	set y [expr ($sh - 98)/2]
	wm geometry $base 297x98+$x+$y
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Input parameter"]
	label $base.l1 \
		-anchor nw -borderwidth 1 \
		-justify left -relief sunken -textvariable PgAcVar(getqueryparam,msg) -wraplength 200 
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(getqueryparam,var) 
	bind $base.e1 <Key-KP_Enter> {
		set PgAcVar(getqueryparam,result) 1
destroy .pgaw:GetParameter
	}
	bind $base.e1 <Key-Return> {
		set PgAcVar(getqueryparam,result) 1
destroy .pgaw:GetParameter
	}
	button $base.bok \
		-borderwidth 1 -command {set PgAcVar(getqueryparam,result) 1
destroy .pgaw:GetParameter} -text Ok 
	button $base.bcanc \
		-borderwidth 1 -command {set PgAcVar(getqueryparam,result) 0
destroy .pgaw:GetParameter} -text [intlmsg Cancel]
	place $base.l1 \
		-x 10 -y 5 -width 201 -height 53 -anchor nw -bordermode ignore 
	place $base.e1 \
		-x 10 -y 65 -width 200 -height 24 -anchor nw -bordermode ignore 
	place $base.bok \
		-x 225 -y 5 -width 61 -height 26 -anchor nw -bordermode ignore 
	place $base.bcanc \
		-x 225 -y 35 -width 61 -height 26 -anchor nw -bordermode ignore 
}


proc vTclWindow.pgaw:SQLWindow {base} {
	if {$base == ""} {
		set base .pgaw:SQLWindow
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 551x408+192+169
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base [intlmsg "SQL window"]
	frame $base.f \
		-borderwidth 1 -height 392 -relief raised -width 396 
	scrollbar $base.f.01 \
		-borderwidth 1 -command {.pgaw:SQLWindow.f.t xview} -orient horiz \
		-width 10 
	scrollbar $base.f.02 \
		-borderwidth 1 -command {.pgaw:SQLWindow.f.t yview} -orient vert -width 10 
	text $base.f.t \
		-borderwidth 1 \
		-height 200 -width 200 -wrap word \
		-xscrollcommand {.pgaw:SQLWindow.f.01 set} \
		-yscrollcommand {.pgaw:SQLWindow.f.02 set} 
	button $base.b1 \
		-borderwidth 1 -command {.pgaw:SQLWindow.f.t delete 1.0 end} -text [intlmsg Clean]
	button $base.b2 \
		-borderwidth 1 -command {destroy .pgaw:SQLWindow} -text [intlmsg Close] 
	grid columnconf $base 0 -weight 1
	grid columnconf $base 1 -weight 1
	grid rowconf $base 0 -weight 1
	grid $base.f \
		-in .pgaw:SQLWindow -column 0 -row 0 -columnspan 2 -rowspan 1 
	grid columnconf $base.f 0 -weight 1
	grid rowconf $base.f 0 -weight 1
	grid $base.f.01 \
		-in .pgaw:SQLWindow.f -column 0 -row 1 -columnspan 1 -rowspan 1 -sticky ew 
	grid $base.f.02 \
		-in .pgaw:SQLWindow.f -column 1 -row 0 -columnspan 1 -rowspan 1 -sticky ns 
	grid $base.f.t \
		-in .pgaw:SQLWindow.f -column 0 -row 0 -columnspan 1 -rowspan 1 \
		-sticky nesw 
	grid $base.b1 \
		-in .pgaw:SQLWindow -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.b2 \
		-in .pgaw:SQLWindow -column 1 -row 1 -columnspan 1 -rowspan 1 
}

proc vTclWindow.pgaw:About {base} {
	if {$base == ""} {
		set base .pgaw:About
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 471x177+168+243
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base [intlmsg "About"]
	label $base.l1  -borderwidth 3 -font -Adobe-Helvetica-Bold-R-Normal-*-*-180-*-*-*-*-*  -relief ridge -text PgAccess 
	label $base.l2  -relief groove  -text [intlmsg "A Tcl/Tk interface to\nPostgreSQL\nby Constantin Teodorescu"]
	label $base.l3  -borderwidth 0 -relief sunken -text {v 0.98.5}
	label $base.l4  -relief groove  -text "[intlmsg {You will always get the latest version at:}]
http://www.flex.ro/pgaccess

[intlmsg {Suggestions at}] : teo@flex.ro"
	button $base.b1  -borderwidth 1 -command {Window destroy .pgaw:About} -text Ok 
	place $base.l1  -x 10 -y 10 -width 196 -height 103 -anchor nw -bordermode ignore 
	place $base.l2  -x 10 -y 115 -width 198 -height 55 -anchor nw -bordermode ignore 
	place $base.l3  -x 145 -y 80 -anchor nw -bordermode ignore 
	place $base.l4  -x 215 -y 10 -width 246 -height 103 -anchor nw -bordermode ignore 
	place $base.b1  -x 295 -y 130 -width 105 -height 28 -anchor nw -bordermode ignore
}

proc vTclWindow.pgaw:OpenDB {base} {
	if {$base == ""} {
		set base .pgaw:OpenDB
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 283x172+119+210
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Open database"]
	frame $base.f1 \
		-borderwidth 2 -height 75 -width 125 
	label $base.f1.l1 \
		-borderwidth 0 -relief raised -text [intlmsg Host]
	entry $base.f1.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(opendb,host) -width 200 
	bind $base.f1.e1 <Key-KP_Enter> {
		focus .pgaw:OpenDB.f1.e2
	}
	bind $base.f1.e1 <Key-Return> {
		focus .pgaw:OpenDB.f1.e2
	}
	label $base.f1.l2 \
		-borderwidth 0 -relief raised -text [intlmsg Port]
	entry $base.f1.e2 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(opendb,pgport) -width 200 
	bind $base.f1.e2 <Key-Return> {
		focus .pgaw:OpenDB.f1.e3
	}
	label $base.f1.l3 \
		-borderwidth 0 -relief raised -text [intlmsg Database]
	entry $base.f1.e3 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(opendb,dbname) -width 200 
	bind $base.f1.e3 <Key-Return> {
		focus .pgaw:OpenDB.f1.e4
	}
	label $base.f1.l4 \
		-borderwidth 0 -relief raised -text [intlmsg Username]
	entry $base.f1.e4 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(opendb,username) \
		-width 200 
	bind $base.f1.e4 <Key-Return> {
		focus .pgaw:OpenDB.f1.e5
	}
	label $base.f1.ls2 \
		-borderwidth 0 -relief raised -text { } 
	label $base.f1.l5 \
		-borderwidth 0 -relief raised -text [intlmsg Password]
	entry $base.f1.e5 \
		-background #fefefe -borderwidth 1 -show x -textvariable PgAcVar(opendb,password) \
		-width 200 
	bind $base.f1.e5 <Key-Return> {
		focus .pgaw:OpenDB.fb.btnopen
	}
	frame $base.fb \
		-height 75 -relief groove -width 125 
	button $base.fb.btnopen \
		-borderwidth 1 -command Mainlib::open_database -padx 9 \
		-pady 3 -text [intlmsg Open]
	button $base.fb.btncancel \
		-borderwidth 1 -command {Window hide .pgaw:OpenDB} \
		-padx 9 -pady 3 -text [intlmsg Cancel]
	place $base.f1 \
		-x 9 -y 5 -width 265 -height 126 -anchor nw -bordermode ignore 
	grid columnconf $base.f1 2 -weight 1
	grid $base.f1.l1 \
		-in .pgaw:OpenDB.f1 -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e1 \
		-in .pgaw:OpenDB.f1 -column 2 -row 0 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.l2 \
		-in .pgaw:OpenDB.f1 -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e2 \
		-in .pgaw:OpenDB.f1 -column 2 -row 2 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.l3 \
		-in .pgaw:OpenDB.f1 -column 0 -row 4 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e3 \
		-in .pgaw:OpenDB.f1 -column 2 -row 4 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.l4 \
		-in .pgaw:OpenDB.f1 -column 0 -row 6 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e4 \
		-in .pgaw:OpenDB.f1 -column 2 -row 6 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.ls2 \
		-in .pgaw:OpenDB.f1 -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f1.l5 \
		-in .pgaw:OpenDB.f1 -column 0 -row 7 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e5 \
		-in .pgaw:OpenDB.f1 -column 2 -row 7 -columnspan 1 -rowspan 1 -pady 2 
	place $base.fb \
		-x 0 -y 135 -width 283 -height 40 -anchor nw -bordermode ignore 
	grid $base.fb.btnopen \
		-in .pgaw:OpenDB.fb -column 0 -row 0 -columnspan 1 -rowspan 1 -padx 5 
	grid $base.fb.btncancel \
		-in .pgaw:OpenDB.fb -column 1 -row 0 -columnspan 1 -rowspan 1 -padx 5 
}


