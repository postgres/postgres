#!/bin/sh
# the next line restarts using wish \
exec wish "$0" "$@"

image create bitmap dnarw -data  {
#define down_arrow_width 15
#define down_arrow_height 15
static char down_arrow_bits[] = {
	0x00,0x80,0x00,0x80,0x00,0x80,0x00,0x80,
	0x00,0x80,0xf8,0x8f,0xf0,0x87,0xe0,0x83,
	0xc0,0x81,0x80,0x80,0x00,0x80,0x00,0x80,
	0x00,0x80,0x00,0x80,0x00,0x80
	}
}


proc {intlmsg} {msg} {
global PgAcVar Messages
	if {$PgAcVar(pref,language)=="english"} { return $msg }
	if { ! [array exists Messages] } { return $msg }
	if { ! [info exists Messages($msg)] } { return $msg }
	return $Messages($msg)
}

proc {PgAcVar:clean} {prefix} {
global PgAcVar
	foreach key [array names PgAcVar $prefix] {
		set PgAcVar($key) {}
		unset PgAcVar($key)
	}
}


proc {find_PGACCESS_HOME} {} {
global PgAcVar env
	if {! [info exists env(PGACCESS_HOME)]} {
		set home [file dirname [info script]]
		switch [file pathtype $home] {
			absolute {set env(PGACCESS_HOME) $home}
			relative {set env(PGACCESS_HOME) [file join [pwd] $home]}
			volumerelative {
				set curdir [pwd]
				cd $home
				set env(PGACCESS_HOME) [file join [pwd] [file dirname [file join [lrange [file split $home] 1 end]]]]
				cd $curdir
			}
		}
	}
	if {![file isdir $env(PGACCESS_HOME)]} {
		set PgAcVar(PGACCESS_HOME) [pwd]
	} else {
		set PgAcVar(PGACCESS_HOME) $env(PGACCESS_HOME)
	}
}


proc init {argc argv} {
global PgAcVar CurrentDB
	find_PGACCESS_HOME
	# Loading all defined namespaces
	foreach module {mainlib database tables queries visualqb forms views functions reports scripts users sequences schema help preferences} {
		source [file join $PgAcVar(PGACCESS_HOME) lib $module.tcl]
	}
	set PgAcVar(currentdb,host) localhost
	set PgAcVar(currentdb,pgport) 5432
	set CurrentDB {}
	set PgAcVar(tablist) [list Tables Queries Views Sequences Functions Reports Forms Scripts Users Schema]
	set PgAcVar(activetab) {}
	set PgAcVar(query,tables) {}
	set PgAcVar(query,links) {}
	set PgAcVar(query,results) {}
	set PgAcVar(mwcount) 0
	Preferences::load
}

proc {wpg_exec} {db cmd} {
global PgAcVar
	set PgAcVar(pgsql,cmd) "never executed"
	set PgAcVar(pgsql,status) "no status yet"
	set PgAcVar(pgsql,errmsg) "no error message yet"
	if {[catch {
		Mainlib::sqlw_display $cmd
		set PgAcVar(pgsql,cmd) $cmd
		set PgAcVar(pgsql,res) [pg_exec $db $cmd]
		set PgAcVar(pgsql,status) [pg_result $PgAcVar(pgsql,res) -status]
		set PgAcVar(pgsql,errmsg) [pg_result $PgAcVar(pgsql,res) -error]
	} tclerrmsg]} {
		showError [format [intlmsg "Tcl error executing pg_exec %s\n\n%s"] $cmd $tclerrmsg]
		return 0
	}
	return $PgAcVar(pgsql,res)
}


proc {wpg_select} {args} {
	Mainlib::sqlw_display "[lindex $args 1]"
	uplevel pg_select $args
}


proc {create_drop_down} {base x y w} {
global PgAcVar
	if {[winfo exists $base.ddf]} return;
	frame $base.ddf -borderwidth 1 -height 75 -relief raised -width 55
	listbox $base.ddf.lb -background #fefefe -foreground #000000 -selectbackground #c3c3c3 -borderwidth 1  -font $PgAcVar(pref,font_normal)  -highlightthickness 0 -selectborderwidth 0 -yscrollcommand [subst {$base.ddf.sb set}]
	scrollbar $base.ddf.sb -borderwidth 1 -command [subst {$base.ddf.lb yview}] -highlightthickness 0 -orient vert
	place $base.ddf -x $x -y $y -width $w -height 185 -anchor nw -bordermode ignore
	place $base.ddf.lb -x 1 -y 1 -width [expr $w-18] -height 182 -anchor nw -bordermode ignore
	place $base.ddf.sb -x [expr $w-15] -y 1 -width 14 -height 183 -anchor nw -bordermode ignore
}


proc {setCursor} {{type NORMAL}} {
	if {[lsearch -exact "CLOCK WAIT WATCH" [string toupper $type]] != -1} {
		set type watch
	} else {
		set type left_ptr
	}
	foreach wn [winfo children .] {
		catch {$wn configure -cursor $type}
	}
	update ; update idletasks 
}


proc {parameter} {msg} {
global PgAcVar
	Window show .pgaw:GetParameter
	focus .pgaw:GetParameter.e1
	set PgAcVar(getqueryparam,var) ""
	set PgAcVar(getqueryparam,flag) 0
	set PgAcVar(getqueryparam,msg) $msg
	bind .pgaw:GetParameter <Destroy> "set PgAcVar(getqueryparam,flag) 1"
	grab .pgaw:GetParameter
	tkwait variable PgAcVar(getqueryparam,flag)
	if {$PgAcVar(getqueryparam,result)} {
		return $PgAcVar(getqueryparam,var)
	} else {
		return ""
	}
}


proc {showError} {emsg} {
   bell ; tk_messageBox -title [intlmsg Error] -icon error -message $emsg
}


proc {sql_exec} {how cmd} {
global PgAcVar CurrentDB
	if {[set pgr [wpg_exec $CurrentDB $cmd]]==0} {
		return 0
	}
	if {($PgAcVar(pgsql,status)=="PGRES_COMMAND_OK") || ($PgAcVar(pgsql,status)=="PGRES_TUPLES_OK")} {
		pg_result $pgr -clear
		return 1
	}	
	if {$how != "quiet"} {
		showError [format [intlmsg "Error executing query\n\n%s\n\nPostgreSQL error message:\n%s\nPostgreSQL status:%s"] $cmd $PgAcVar(pgsql,errmsg) $PgAcVar(pgsql,status)]
	}
	pg_result $pgr -clear
	return 0
}



proc {main} {argc argv} {
global PgAcVar CurrentDB tcl_platform
	load libpgtcl[info sharedlibextension]
	catch {Mainlib::draw_tabs}
	set PgAcVar(opendb,username) {}
	set PgAcVar(opendb,password) {}
	if {$argc>0} {
		set PgAcVar(opendb,dbname) [lindex $argv 0]
		set PgAcVar(opendb,host) localhost
		set PgAcVar(opendb,pgport) 5432
		Mainlib::open_database
	} elseif {$PgAcVar(pref,autoload) && ($PgAcVar(pref,lastdb)!="")} {
		set PgAcVar(opendb,dbname) $PgAcVar(pref,lastdb)
		set PgAcVar(opendb,host) $PgAcVar(pref,lasthost)
		set PgAcVar(opendb,pgport) $PgAcVar(pref,lastport)
		catch {set PgAcVar(opendb,username) $PgAcVar(pref,lastusername)}
		if {[set openmsg [Mainlib::open_database]]!=""} {
			if {[regexp "no password supplied" $openmsg]} {
				Window show .pgaw:OpenDB
				focus .pgaw:OpenDB.f1.e5
				wm transient .pgaw:OpenDB .pgaw:Main
			}
		}
		
	}
	wm protocol .pgaw:Main WM_DELETE_WINDOW {
		catch {pg_disconnect $CurrentDB}
		exit
	}
}


proc {Window} {args} {
global vTcl
	set cmd [lindex $args 0]
	set name [lindex $args 1]
	set newname [lindex $args 2]
	set rest [lrange $args 3 end]
	if {$name == "" || $cmd == ""} {return}
	if {$newname == ""} {
		set newname $name
	}
	set exists [winfo exists $newname]
	switch $cmd {
		show {
			if {$exists == "1" && $name != "."} {wm deiconify $name; return}
			if {[info procs vTclWindow(pre)$name] != ""} {
				eval "vTclWindow(pre)$name $newname $rest"
			}
			if {[info procs vTclWindow$name] != ""} {
				eval "vTclWindow$name $newname $rest"
			}
			if {[info procs vTclWindow(post)$name] != ""} {
				eval "vTclWindow(post)$name $newname $rest"
			}
		}
		hide    { if $exists {wm withdraw $newname; return} }
		iconify { if $exists {wm iconify $newname; return} }
		destroy { if $exists {destroy $newname; return} }
	}
}

proc vTclWindow. {base} {
	if {$base == ""} {
		set base .
	}
	wm focusmodel $base passive
	wm geometry $base 1x1+0+0
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm withdraw $base
	wm title $base "vt.tcl"
}


init $argc $argv

Window show .
Window show .pgaw:Main

main $argc $argv

