#!/usr/bin/wish

global widget;

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

proc {set_default_fonts} {} {
global pref tcl_platform
if {[string toupper $tcl_platform(platform)]=="WINDOWS"} {
	set pref(font_normal) {"MS Sans Serif" 8}
	set pref(font_bold) {"MS Sans Serif" 8 bold}
	set pref(font_fix) {Terminal 8}
	set pref(font_italic) {"MS Sans Serif" 8 italic}
} else {
	set pref(font_normal) -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
	set pref(font_bold) -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
	set pref(font_italic) -Adobe-Helvetica-Medium-O-Normal-*-*-120-*-*-*-*-*
	set pref(font_fix) -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
}
}

proc {set_gui_pref} {} {
global pref
foreach wid {Label Text Button Listbox Checkbutton Radiobutton} {
	option add *$wid.font $pref(font_normal)
}
option add *Entry.background #fefefe
option add *Entry.foreground #000000
}

proc {load_pref} {} {
global pref
set_default_fonts
set_gui_pref
set retval [catch {set fid [open "~/.pgaccessrc" r]}]
if {$retval} {
	set pref(rows) 200
	set pref(tvfont) clean
	set pref(autoload) 1
	set pref(lastdb) {}
	set pref(lasthost) localhost
	set pref(lastport) 5432
	set pref(username) {}
	set pref(password) {}
} else {
	while {![eof $fid]} {
		set pair [gets $fid]
		set pref([lindex $pair 0]) [lindex $pair 1]
	}
	close $fid
	set_gui_pref
}
}

proc init {argc argv} {
global dbc host pport tablist mw fldval activetab qlvar mwcount pref
load_pref
set host localhost
set pport 5432
set dbc {}
set tablist [list Tables Queries Views Sequences Functions Reports Forms Scripts Users]
set activetab {}
set qlvar(yoffs) 360
set qlvar(xoffs) 50
set qlvar(reswidth) 150
set qlvar(resfields) {}
set qlvar(ressort) {}
set qlvar(rescriteria) {}
set qlvar(restables) {}
set qlvar(critedit) 0
set qlvar(links) {}
set qlvar(ntables) 0
set qlvar(newtablename) {}
set mwcount 0
}

init $argc $argv

proc {sqlw_display} {msg} {
	if {![winfo exists .sqlw]} {return}
	.sqlw.f.t insert end "$msg\n\n"
	.sqlw.f.t see end
	set nrlines [lindex [split [.sqlw.f.t index end] .] 0]
	if {$nrlines>50} {
		.sqlw.f.t delete 1.0 3.0
	}
}

proc {wpg_exec} {db cmd} {
global pgsql
	set pgsql(cmd) "never executed"
	set pgsql(status) "no status yet"
	set pgsql(errmsg) "no error message yet"
	if {[catch {
		sqlw_display $cmd
		set pgsql(cmd) $cmd
		set pgsql(res) [pg_exec $db $cmd]
		set pgsql(status) [pg_result $pgsql(res) -status]
		set pgsql(errmsg) [pg_result $pgsql(res) -error]
	} tclerrmsg]} {
		show_error "Tcl error executing pg_exec $cmd\n\n$tclerrmsg"
		return 0
	}
	return $pgsql(res)
}

proc {wpg_select} {args} {
	sqlw_display "[lindex $args 1]"
	uplevel pg_select $args
}

proc {anfw:add} {} {
global anfw pgsql tiw
	if {$anfw(name)==""} {
		show_error "Empty field name ?"
		focus .anfw.e1
		return
	}		
	if {$anfw(type)==""} {
		show_error "No field type ?"
		focus .anfw.e2
		return
	}
	if {![sql_exec quiet "alter table \"$tiw(tablename)\" add column \"$anfw(name)\" $anfw(type)"]} {
		show_error "Cannot add column\n\nPostgreSQL error: $pgsql(errmsg)"
		return
	}
	Window destroy .anfw
	sql_exec quiet "update pga_layout set colnames=colnames || ' {$anfw(name)}', colwidth=colwidth || ' 150',nrcols=nrcols+1 where tablename='$tiw(tablename)'"
	show_table_information $tiw(tablename)
}

proc {add_new_field} {} {
global ntw
if {$ntw(fldname)==""} {
	show_error "Enter a field name"
	focus .nt.e2
	return
}
if {$ntw(fldtype)==""} {
	show_error "The field type is not specified!"
	return
}
if {($ntw(fldtype)=="varchar")&&($ntw(fldsize)=="")} {
	focus .nt.e3
	show_error "You must specify field size!"
	return
}
if {$ntw(fldsize)==""} then {set sup ""} else {set sup "($ntw(fldsize))"}
if {[regexp $ntw(fldtype) "varchartextdatetime"]} {set supc "'"} else {set supc ""}
if {$ntw(defaultval)==""} then {set sup2 ""} else {set sup2 " DEFAULT $supc$ntw(defaultval)$supc"}
# Checking for field name collision
set inspos end
for {set i 0} {$i<[.nt.lb size]} {incr i} {
	set linie [.nt.lb get $i]
	if {$ntw(fldname)==[string trim [string range $linie 2 33]]} {
		if {[tk_messageBox -title Warning -parent .nt -message "There is another field with the same name: \"$ntw(fldname)\"!\n\nReplace it ?" -type yesno -default yes]=="no"} return
		.nt.lb delete $i
		set inspos $i
		break
	}	 
  }
.nt.lb insert $inspos [format "%1s %-32.32s %-14s%-16s" $ntw(pk) $ntw(fldname) $ntw(fldtype)$sup $sup2$ntw(notnull)]
focus .nt.e2
set ntw(fldname) {}
set ntw(fldsize) {}
set ntw(defaultval) {}
set ntw(pk) " "
}

proc {create_table} {} {
global dbc ntw
if {$ntw(newtablename)==""} then {
	show_error "You must supply a name for your table!"
	focus .nt.etabn
	return
}
if {[.nt.lb size]==0} then {
	show_error "Your table has no fields!"
	focus .nt.e2
	return
}
set fl {}
set pkf {}
foreach line [.nt.lb get 0 end] {
	set fldname "\"[string trim [string range $line 2 33]]\""
	lappend fl "$fldname [string trim [string range $line 35 end]]"
	if {[string range $line 0 0]=="*"} {
		lappend pkf "$fldname"
	}
}
set temp "create table \"$ntw(newtablename)\" ([join $fl ,]"
if {$ntw(constraint)!=""} then {set temp "$temp, constraint \"$ntw(constraint)\""}
if {$ntw(check)!=""} then {set temp "$temp check ($ntw(check))"}
if {[llength $pkf]>0} then {set temp "$temp, primary key([join $pkf ,])"}
set temp "$temp)"
if {$ntw(fathername)!=""} then {set temp "$temp inherits ($ntw(fathername))"}
cursor_clock
if {[sql_exec noquiet $temp]} {
	Window destroy .nt
	cmd_Tables
}
cursor_normal
}

proc {cmd_Delete} {} {
global dbc activetab
if {$dbc==""} return;
set objtodelete [get_dwlb_Selection]
if {$objtodelete==""} return;
set temp {}
switch $activetab {
	Tables {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete table:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop table \"$objtodelete\""
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Tables
		}
	}
	Views {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete view:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop view \"$objtodelete\""
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Views
		}
	}
	Queries {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete query:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_queries where queryname='$objtodelete'"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Queries
		}
	}
	Scripts {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete script:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_scripts where scriptname='$objtodelete'"
			cmd_Scripts
		}
	}
	Forms {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete form:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_forms where formname='$objtodelete'"
			cmd_Forms
		}
	}
	Sequences {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete sequence:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "drop sequence \"$objtodelete\""
			cmd_Sequences
		}
	}
	Functions {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete function:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			delete_function $objtodelete
			cmd_Functions
		}
	}
	Reports {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete report:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "delete from pga_reports where reportname='$objtodelete'"
			cmd_Reports
		}
	}
	Users {
		if {[tk_messageBox -title "FINAL WARNING" -parent .dw -message "You are going to delete user:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop user \"$objtodelete\""
			cmd_Users
		}
	}
}
if {$temp==""} return;
}

proc {cmd_Design} {} {
global dbc activetab rbvar uw
if {$dbc==""} return;
if {[.dw.lb curselection]==""} return;
set objname [.dw.lb get [.dw.lb curselection]]
set tablename $objname
switch $activetab {
	Queries {open_query design}
	Scripts {design_script $objname}
	Forms {fd_load_form $objname design}
	Reports {
		Window show .rb
		tkwait visibility .rb
		rb_init
		set rbvar(reportname) $objname
		rb_load_report
		set rbvar(justpreview) 0
	}
	Users {
		Window show .uw
		tkwait visibility .uw
		wm transient .uw .dw
		wm title .uw "Design user"
		set uw(username) $objname
		set uw(password) {} ; set uw(verify) {}
		pg_select $dbc "select *,date(valuntil) as valdata from pg_user where usename='$objname'" tup {
			if {$tup(usesuper)=="t"} {
				set uw(createuser) CREATEUSER
			} else {
				set uw(createuser) NOCREATEUSER
			}
			if {$tup(usecreatedb)=="t"} {
				set uw(createdb) CREATEDB
			} else {
				set uw(createdb) NOCREATEDB
			}
			if {$tup(valuntil)!=""} {
				set uw(valid) $tup(valdata)
			} else {
				set uw(valid) {}
			}
		}
		.uw.e1 configure -state disabled
		.uw.b1 configure -text Alter
		focus .uw.e2
	}
}
}

proc {cmd_Forms} {} {
global dbc
cursor_clock
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select formname from pga_forms order by formname" rec {
		.dw.lb insert end $rec(formname)
	}
}
cursor_normal
}

proc {cmd_Functions} {} {
global dbc
set maxim 0
set pgid 0
cursor_clock
catch {
	wpg_select $dbc "select proowner,count(*) from pg_proc group by proowner" rec {
		if {$rec(count)>$maxim} {
			set maxim $rec(count)
			set pgid $rec(proowner)
		}
	}
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select proname from pg_proc where prolang=14 and proowner<>$pgid order by proname" rec {
		.dw.lb insert end $rec(proname)
	}	
}
cursor_normal
}
}

proc {cmd_Import_Export} {how} {
global dbc ie_tablename ie_filename activetab
if {$dbc==""} return;
Window show .iew
set ie_tablename {}
set ie_filename {}
set ie_delimiter {}
if {$activetab=="Tables"} {
	set tn [get_dwlb_Selection]
	set ie_tablename $tn
	if {$tn!=""} {set ie_filename "$tn.txt"}
}
.iew.expbtn configure -text $how
}

proc {cmd_Information} {} {
global dbc tiw activetab
if {$dbc==""} return;
if {$activetab!="Tables"} return;
show_table_information [get_dwlb_Selection]
}

proc {cmd_New} {} {
global dbc activetab queryname queryoid cbv funcpar funcname funcret rbvar uw
if {$dbc==""} return;
switch $activetab {
	Tables {
		Window show .nt
		focus .nt.etabn
	}
	Queries {
		Window show .qb
		set queryoid 0
		set queryname {}
			set cbv 0
		.qb.cbv configure -state normal
	}
	Users {
		Window show .uw
		wm transient .uw .dw
		set uw(username) {}
		set uw(password) {}
		set uw(createdb) NOCREATEDB
		set uw(createuser) NOCREATEUSER
		set uw(verify) {}
		set uw(valid) {}
		focus .uw.e1
	}
	Views {
	set queryoid 0
	set queryname {}
		Window show .qb
		set cbv 1
		.qb.cbv configure -state disabled
	}
	Sequences {
	Window show .sqf
	focus .sqf.e1
	}
	Reports {
	Window show .rb ; tkwait visibility .rb ; rb_init ; set rbvar(reportname) {} ; set rbvar(justpreview) 0
	focus .rb.e2
	}
	Forms {
		Window show .fd
		Window show .fdtb
		Window show .fdmenu
		Window show .fda
		fd_init
	}
	Scripts {
		design_script {}
	}
	Functions {
	Window show .fw
	set funcname {}
	set funcpar {}
	set funcret {}
	place .fw.okbtn -y 255
	.fw.okbtn configure -state normal
	.fw.okbtn configure -text Define
	.fw.text1 delete 1.0 end
	focus .fw.e1
	}
}
}

proc {cmd_Open} {} {
global dbc activetab
if {$dbc==""} return;
set objname [get_dwlb_Selection]
if {$objname==""} return;
switch $activetab {
	Tables {open_table $objname}
	Forms {open_form $objname}
	Scripts {execute_script $objname}
	Queries {open_query view}
	Views {open_view}
	Sequences {open_sequence $objname}
	Functions {open_function $objname}
	Reports {open_report $objname}
}
}

proc {cmd_Preferences} {} {
Window show .pw
}

proc {cmd_Queries} {} {
global dbc
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select queryname from pga_queries order by queryname" rec {
		.dw.lb insert end $rec(queryname)
	}
}
}

proc {uw:create_user} {} {
global dbc uw
set uw(username) [string trim $uw(username)]
set uw(password) [string trim $uw(password)]
set uw(verify) [string trim $uw(verify)]
if {$uw(username)==""} {
	show_error "User without name!"
	focus .uw.e1
	return
}
if {$uw(password)!=$uw(verify)} {
	show_error "Passwords do not match!"
	set uw(password) {} ; set uw(verify) {}
	focus .uw.e2
	return
}
set cmd "[.uw.b1 cget -text] user \"$uw(username)\""
if {$uw(password)!=""} {
	set cmd "$cmd WITH PASSWORD \"$uw(password)\" "
}
set cmd "$cmd $uw(createdb) $uw(createuser)"
if {$uw(valid)!=""} {
	set cmd "$cmd VALID UNTIL '$uw(valid)'"
}
if {[sql_exec noquiet $cmd]} {
	Window destroy .uw
	cmd_Users
}
}

proc {cmd_Rename} {} {
global dbc oldobjname activetab
if {$dbc==""} return;
if {$activetab=="Views"} return;
if {$activetab=="Sequences"} return;
if {$activetab=="Functions"} return;
if {$activetab=="Users"} return;
set temp [get_dwlb_Selection]
if {$temp==""} {
	tk_messageBox -title Warning -parent .dw -message "Please select an object first !"
	return;
}
set oldobjname $temp
Window show .rf
}

proc {cmd_Reports} {} {
global dbc
cursor_clock
catch {
	wpg_select $dbc "select reportname from pga_reports order by reportname" rec {
	.dw.lb insert end "$rec(reportname)"
	}
}
cursor_normal
}

proc {cmd_Users} {} {
global dbc
cursor_clock
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select * from pg_user order by usename" rec {
		.dw.lb insert end $rec(usename)
	}
}
cursor_normal
}

proc {cmd_Scripts} {} {
global dbc
cursor_clock
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select scriptname from pga_scripts order by scriptname" rec {
	.dw.lb insert end $rec(scriptname)
	}
}
cursor_normal
}

proc {cmd_Sequences} {} {
global dbc

cursor_clock
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select relname from pg_class where (relname not like 'pg_%') and (relkind='S') order by relname" rec {
		.dw.lb insert end $rec(relname)
	}
}
cursor_normal
}

proc {cmd_Tables} {} {
global dbc
cursor_clock
.dw.lb delete 0 end
foreach tbl [get_tables] {.dw.lb insert end $tbl}
cursor_normal
}

proc {cmd_Views} {} {
global dbc

cursor_clock
.dw.lb delete 0 end
catch {
	wpg_select $dbc "select relname from pg_class where (relname !~ '^pg_') and (relkind='r') and (relhasrules) order by relname" rec {
		.dw.lb insert end $rec(relname)
	}
}
cursor_normal
}

proc {create_drop_down} {base x y w} {
global pref
if {[winfo exists $base.ddf]} {
    return
}
frame $base.ddf -borderwidth 1 -height 75 -relief raised -width 55
listbox $base.ddf.lb -background #fefefe -borderwidth 1  -font $pref(font_normal)  -highlightthickness 0 -selectborderwidth 0 -yscrollcommand [subst {$base.ddf.sb set}]
scrollbar $base.ddf.sb -borderwidth 1 -command [subst {$base.ddf.lb yview}] -highlightthickness 0 -orient vert
place $base.ddf -x $x -y $y -width $w -height 185 -anchor nw -bordermode ignore
place $base.ddf.lb -x 1 -y 1 -width [expr $w-18] -height 182 -anchor nw -bordermode ignore
place $base.ddf.sb -x [expr $w-15] -y 1 -width 14 -height 183 -anchor nw -bordermode ignore
}

proc {cursor_normal} {} {
	foreach wn [winfo children .] {
		catch {$wn configure -cursor left_ptr}
	}
	update ; update idletasks 
}

proc {cursor_clock} {} {
	foreach wn [winfo children .] {
		catch {$wn configure -cursor watch}
	}
	update ; update idletasks 
}

proc {delete_function} {objname} {
global dbc
wpg_select $dbc "select proargtypes,pronargs from pg_proc where proname='$objname'" rec {
	set funcpar $rec(proargtypes)
	set nrpar $rec(pronargs)
}
set lispar {}
for {set i 0} {$i<$nrpar} {incr i} {
	lappend lispar [get_pgtype [lindex $funcpar $i]]
}
set lispar [join $lispar ,]
sql_exec noquiet "drop function $objname ($lispar)"
}

proc {design_script} {sname} {
global dbc scriptname
Window show .sw
set scriptname $sname
.sw.src delete 1.0 end
if {[string length $sname]==0} return;
wpg_select $dbc "select * from pga_scripts where scriptname='$sname'" rec {
	.sw.src insert end $rec(scriptsource)    
}
}

proc {drag_it} {w x y} {
global draglocation
	set dlo ""
	catch { set dlo $draglocation(obj) }
	if {$dlo != ""} {
		set dx [expr $x - $draglocation(x)]
		set dy [expr $y - $draglocation(y)]
		$w move $dlo $dx $dy
		set draglocation(x) $x
		set draglocation(y) $y
	}
}

proc {drag_start} {wn w x y} {
global draglocation
catch {unset draglocation}
set object [$w find closest $x $y]
if {[lsearch [$wn.c gettags $object] movable]==-1} return;
$wn.c bind movable <Leave> {}
set draglocation(obj) $object
set draglocation(x) $x
set draglocation(y) $y
set draglocation(start) $x
}

proc {drag_stop} {wn w x y} {
global draglocation mw dbc
	set dlo ""
	catch { set dlo $draglocation(obj) }
	if {$dlo != ""} {
		$wn.c bind movable <Leave> "$wn configure -cursor left_ptr"
		$wn configure -cursor left_ptr
		set ctr [get_tag_info $wn $draglocation(obj) v]
		set diff [expr $x-$draglocation(start)]
		if {$diff==0} return;
		set newcw {}
		for {set i 0} {$i<$mw($wn,colcount)} {incr i} {
			if {$i==$ctr} {
				lappend newcw [expr [lindex $mw($wn,colwidth) $i]+$diff]
			} else {
				lappend newcw [lindex $mw($wn,colwidth) $i]
			}
		}
		set mw($wn,colwidth) $newcw
		$wn.c itemconfigure c$ctr -width [expr [lindex $mw($wn,colwidth) $ctr]-5]
		mw_draw_headers $wn
		mw_draw_hgrid $wn
		if {$mw($wn,crtrow)!=""} {mw_show_record $wn $mw($wn,crtrow)}
		for {set i [expr $ctr+1]} {$i<$mw($wn,colcount)} {incr i} {
			$wn.c move c$i $diff 0
		}
		cursor_clock
		sql_exec quiet "update pga_layout set colwidth='$mw($wn,colwidth)' where tablename='$mw($wn,layout_name)'"
		cursor_normal
	}
}

proc {draw_tabs} {} {
global tablist activetab
set ypos 85
foreach tab $tablist {
	label .dw.tab$tab -borderwidth 1  -anchor w -relief raised -text $tab
	place .dw.tab$tab -x 10 -y $ypos -height 25 -width 82 -anchor nw -bordermode ignore
	lower .dw.tab$tab
	bind .dw.tab$tab <Button-1> {tab_click %W}
	incr ypos 25
}
set activetab ""
}

proc {execute_script} {scriptname} {
global dbc
	set ss {}
	wpg_select $dbc "select * from pga_scripts where scriptname='$scriptname'" rec {
		set ss $rec(scriptsource)
	}
    if {[string length $ss] > 0} {
		eval $ss
    }
}

proc {fd_change_coord} {} {
global fdvar fdobj
set i $fdvar(moveitemobj)
set c $fdobj($i,c)
set c [list $fdvar(c_left) $fdvar(c_top) [expr $fdvar(c_left)+$fdvar(c_width)] [expr $fdvar(c_top)+$fdvar(c_height)]]
set fdobj($i,c) $c
.fd.c delete o$i
fd_draw_object $i
fd_draw_hookers $i
}

proc {fd_delete_object} {} {
global fdvar
set i $fdvar(moveitemobj)
.fd.c delete o$i
.fd.c delete hook
set j [lsearch $fdvar(objlist) $i]
set fdvar(objlist) [lreplace $fdvar(objlist) $j $j]
}

proc {fd_draw_hook} {x y} {
.fd.c create rectangle [expr $x-2] [expr $y-2] [expr $x+2] [expr $y+2] -fill black -tags hook
}

proc {fd_draw_hookers} {i} {
global fdobj
foreach {x1 y1 x2 y2} $fdobj($i,c) {}
.fd.c delete hook
fd_draw_hook $x1 $y1
fd_draw_hook $x1 $y2
fd_draw_hook $x2 $y1
fd_draw_hook $x2 $y2
}

proc {fd_draw_object} {i} {
global fdvar fdobj pref
set c $fdobj($i,c)
foreach {x1 y1 x2 y2} $c {}
.fd.c delete o$i
switch $fdobj($i,t) {
	button {
		fd_draw_rectangle $x1 $y1 $x2 $y2 raised #a0a0a0 o$i
		.fd.c create text [expr ($x1+$x2)/2] [expr ($y1+$y2)/2] -text $fdobj($i,l) -font $pref(font_normal) -tags o$i
	}
	entry {
		fd_draw_rectangle $x1 $y1 $x2 $y2 sunken white o$i
	}
	label {
		.fd.c create text $x1 $y1 -text $fdobj($i,l) -font $pref(font_normal) -anchor nw -tags o$i
	}
	checkbox {
		fd_draw_rectangle [expr $x1+2] [expr $y1+5] [expr $x1+12] [expr $y1+15] raised #a0a0a0 o$i
		.fd.c create text [expr $x1+20] [expr $y1+3] -text $fdobj($i,l) -anchor nw -font $pref(font_normal) -tags o$i
	}
	radio {
		.fd.c create oval [expr $x1+4] [expr $y1+5] [expr $x1+14] [expr $y1+15] -fill white -tags o$i
		.fd.c create text [expr $x1+24] [expr $y1+3] -text $fdobj($i,l) -anchor nw -font $pref(font_normal) -tags o$i
	}
	query {
		.fd.c create oval $x1 $y1 [expr $x1+20] [expr $y1+20] -fill white -tags o$i
		.fd.c create text [expr $x1+5] [expr $y1+4] -text Q  -anchor nw -font $pref(font_normal) -tags o$i
	}
	listbox {
		fd_draw_rectangle $x1 $y1 [expr $x2-12] $y2 sunken white o$i
		fd_draw_rectangle [expr $x2-11] $y1 $x2 $y2 sunken gray o$i
		.fd.c create line [expr $x2-5] $y1 $x2 [expr $y1+10] -fill #808080 -tags o$i
		.fd.c create line [expr $x2-10] [expr $y1+9] $x2 [expr $y1+9] -fill #808080 -tags o$i
		.fd.c create line [expr $x2-10] [expr $y1+9] [expr $x2-5] $y1 -fill white -tags o$i
		.fd.c create line [expr $x2-5] $y2 $x2 [expr $y2-10] -fill #808080 -tags o$i
		.fd.c create line [expr $x2-10] [expr $y2-9] $x2 [expr $y2-9] -fill white -tags o$i
		.fd.c create line [expr $x2-10] [expr $y2-9] [expr $x2-5] $y2 -fill white -tags o$i
	}
}
.fd.c raise hook
}

proc {fd_draw_rectangle} {x1 y1 x2 y2 relief color tag} {
if {$relief=="raised"} {
	set c1 white
	set c2 #606060
} else {
	set c1 #606060
	set c2 white
}
if {$color != "none"} {
	.fd.c create rectangle $x1 $y1 $x2 $y2 -outline "" -fill $color -tags $tag
}
.fd.c create line $x1 $y1 $x2 $y1 -fill $c1 -tags $tag
.fd.c create line $x1 $y1 $x1 $y2 -fill $c1 -tags $tag
.fd.c create line $x1 $y2 $x2 $y2 -fill $c2 -tags $tag
.fd.c create line $x2 $y1 $x2 [expr 1+$y2] -fill $c2 -tags $tag
}

proc {fd_init} {} {
global fdvar fdobj
catch {unset fdvar}
catch {unset fdobj}
catch {.fd.c delete all}
set fdvar(forminame) {udf0}
set fdvar(formname) "New form"
set fdvar(objnum) 0
set fdvar(objlist) {}
set fdvar(oper) none
set fdvar(tool) point
}

proc {fd_item_click} {x y} {
global fdvar fdobj
set fdvar(oper) none
set fdvar(moveitemobj) {}
set il [.fd.c find overlapping $x $y $x $y]
if {[llength $il]==0} return
set tl [.fd.c gettags [lindex $il 0]]
set i [lsearch -glob $tl o*]
if {$i==-1} return
set objnum [string range [lindex $tl $i] 1 end]
set fdvar(moveitemobj) $objnum
set fdvar(moveitemx) $x
set fdvar(moveitemy) $y
set fdvar(oper) move
fd_show_attributes $objnum
fd_draw_hookers $objnum
}

proc {fd_load_form} {name mode} {
global fdvar fdobj dbc
fd_init
set fdvar(formname) $name
if {$mode=="design"} {
	Window show .fd
	Window show .fdmenu
	Window show .fda
	Window show .fdtb
}
#set fid [open "$name.form" r]
#set info [gets $fid]
#close $fid
set res [wpg_exec $dbc "select * from pga_forms where formname='$fdvar(formname)'"]
set info [lindex [pg_result $res -getTuple 0] 1]
pg_result $res -clear
set fdvar(forminame) [lindex $info 0]
set fdvar(objnum) [lindex $info 1]
set fdvar(objlist) [lindex $info 2]
set fdvar(geometry) [lindex $info 3]
set j 0
foreach objinfo [lrange $info 4 end] {
	foreach {t n c x l v} $objinfo {}
	set i [lindex $fdvar(objlist) $j]
	set fdobj($i,t) $t
	set fdobj($i,n) $n
	set fdobj($i,c) $c
	set fdobj($i,l) $l
	set fdobj($i,x) $x
	set fdobj($i,v) $v
	if {$mode=="design"} {fd_draw_object $i}
	incr j
}
if {$mode=="design"} {wm geometry .fd $fdvar(geometry)}
}

proc {fd_mouse_down} {x y} {
global fdvar
set x [expr 3*int($x/3)]
set y [expr 3*int($y/3)]
set fdvar(xstart) $x
set fdvar(ystart) $y
if {$fdvar(tool)=="point"} {
	fd_item_click $x $y
	return
}
set fdvar(oper) draw
}

proc {fd_mouse_move} {x y} {
global fdvar
#set fdvar(msg) "x=$x y=$y"
set x [expr 3*int($x/3)]
set y [expr 3*int($y/3)]
set oper ""
catch {set oper $fdvar(oper)}
if {$oper=="draw"} {
	catch {.fd.c delete curdraw}
	.fd.c create rectangle $fdvar(xstart) $fdvar(ystart) $x $y -tags curdraw
	return
}
if {$oper=="move"} {
	set dx [expr $x-$fdvar(moveitemx)]
	set dy [expr $y-$fdvar(moveitemy)]
	.fd.c move o$fdvar(moveitemobj) $dx $dy
	.fd.c move hook $dx $dy
	set fdvar(moveitemx) $x
	set fdvar(moveitemy) $y
}
}

proc {fd_mouse_up} {x y} {
global fdvar fdobj
set x [expr 3*int($x/3)]
set y [expr 3*int($y/3)]
if {$fdvar(oper)=="move"} {
	set fdvar(moveitem) {}
	set fdvar(oper) none
	set oc $fdobj($fdvar(moveitemobj),c)
	set dx [expr $x - $fdvar(xstart)]
	set dy [expr $y - $fdvar(ystart)]
	set newcoord [list [expr $dx+[lindex $oc 0]] [expr $dy+[lindex $oc 1]] [expr $dx+[lindex $oc 2]] [expr $dy+[lindex $oc 3]]]
	set fdobj($fdvar(moveitemobj),c) $newcoord
	fd_show_attributes $fdvar(moveitemobj)
	fd_draw_hookers $fdvar(moveitemobj)
	return
}
if {$fdvar(oper)!="draw"} return
set fdvar(oper) none
.fd.c delete curdraw
# Check for x2<x1 or y2<y1
if {$x<$fdvar(xstart)} {set temp $x ; set x $fdvar(xstart) ; set fdvar(xstart) $temp}
if {$y<$fdvar(ystart)} {set temp $y ; set y $fdvar(ystart) ; set fdvar(ystart) $temp}
# Check for too small sizes
if {[expr $x-$fdvar(xstart)]<20} {set x [expr $fdvar(xstart)+20]}
if {[expr $y-$fdvar(ystart)]<10} {set y [expr $fdvar(ystart)+10]}
incr fdvar(objnum)
set i $fdvar(objnum)
lappend fdvar(objlist) $i
# t=type , c=coords , n=name , l=label
set fdobj($i,t) $fdvar(tool)
set fdobj($i,c) [list $fdvar(xstart) $fdvar(ystart) $x $y]
set fdobj($i,n) $fdvar(tool)$i
set fdobj($i,l) $fdvar(tool)$i
set fdobj($i,x) {}
set fdobj($i,v) {}
fd_draw_object $i
fd_show_attributes $i
set fdvar(moveitemobj) $i
fd_draw_hookers $i
set fdvar(tool) point
}

proc {fd_save_form} {name} {
global fdvar fdobj dbc
if {[tk_messageBox -title Warning -message "Do you want to save the form into the database ?" -type yesno -default yes]=="no"} {return 1}
if {[string length $fdvar(forminame)]==0} {
	tk_messageBox -title Warning -message "Forms need an internal name, only literals, low case"
	return 0
}
if {[string length $fdvar(formname)]==0} {
	tk_messageBox -title Warning -message "Form must have a name"
	return 0
}
set info [list $fdvar(forminame) $fdvar(objnum) $fdvar(objlist) [wm geometry .fd]]
foreach i $fdvar(objlist) {
	lappend info [list $fdobj($i,t) $fdobj($i,n) $fdobj($i,c) $fdobj($i,x) $fdobj($i,l) $fdobj($i,v)]
}
sql_exec noquiet "delete from pga_forms where formname='$fdvar(formname)'"
regsub -all "'" $info "''" info
sql_exec noquiet "insert into pga_forms values ('$fdvar(formname)','$info')"
cmd_Forms
return 1
}

proc {fd_set_command} {} {
global fdobj fdvar
set i $fdvar(moveitemobj)
set fdobj($i,x) $fdvar(c_cmd)
}

proc {fd_set_name} {} {
global fdvar fdobj
set i $fdvar(moveitemobj)
foreach k $fdvar(objlist) {
	if {($fdobj($k,n)==$fdvar(c_name)) && ($i!=$k)} {
		tk_messageBox -title Warning -message "There is another object (a $fdobj($k,t)) with the same name. Please change it!"
		return
	}
}
set fdobj($i,n) $fdvar(c_name)
fd_show_attributes $i
}

proc {fd_set_text} {} {
global fdvar fdobj
set fdobj($fdvar(moveitemobj),l) $fdvar(c_text)
fd_draw_object $fdvar(moveitemobj)
}

proc {fd_show_attributes} {i} {
global fdvar fdobj
set fdvar(c_info) "$fdobj($i,t) .$fdvar(forminame).$fdobj($i,n)"
set fdvar(c_name) $fdobj($i,n)
set c $fdobj($i,c)
set fdvar(c_top) [lindex $c 1]
set fdvar(c_left) [lindex $c 0]
set fdvar(c_width) [expr [lindex $c 2]-[lindex $c 0]]
set fdvar(c_height) [expr [lindex $c 3]-[lindex $c 1]]
set fdvar(c_cmd) {}
catch {set fdvar(c_cmd) $fdobj($i,x)}
set fdvar(c_var) {}
catch {set fdvar(c_var) $fdobj($i,v)}
set fdvar(c_text) {}
catch {set fdvar(c_text) $fdobj($i,l)}
}

proc {fd_test} {} {
global fdvar fdobj dbc datasets pref
set basewp $fdvar(forminame)
set base .$fdvar(forminame)
if {[winfo exists $base]} {
   wm deiconify $base; return
}
toplevel $base -class Toplevel
wm focusmodel $base passive
wm geometry $base $fdvar(geometry)
wm maxsize $base 785 570
wm minsize $base 1 1
wm overrideredirect $base 0
wm resizable $base 1 1
wm deiconify $base
wm title $base $fdvar(formname)
foreach item $fdvar(objlist) {
set coord $fdobj($item,c)
set name $fdobj($item,n)
set wh "-width [expr 3+[lindex $coord 2]-[lindex $coord 0]]  -height [expr 3+[lindex $coord 3]-[lindex $coord 1]]"
set visual 1
switch $fdobj($item,t) {
	button {
		set cmd {}
		catch {set cmd $fdobj($item,x)}
		button $base.$name  -borderwidth 1 -padx 0 -pady 0 -text "$fdobj($item,l)" -font $pref(font_normal) -command [subst {$cmd}]
	}
	checkbox {
		checkbutton  $base.$name -onvalue t -offvalue f -font $pref(font_normal) -text "$fdobj($item,l)" -variable "$fdobj($item,v)" -borderwidth 1
		set wh {}
	}
	query {
		set visual 0
	set datasets($base.$name,sql) $fdobj($item,x)
		eval "proc $base.$name:open {} {\
			global dbc datasets tup$basewp$name ;\
			catch {unset tup$basewp$name} ;\
			set wn \[focus\] ; cursor_clock ;\
			set res \[wpg_exec \$dbc \"\$datasets($base.$name,sql)\"\] ;\
			pg_result \$res -assign tup$basewp$name ;\
			set fl {} ;\
			foreach fd \[pg_result \$res -lAttributes\] {lappend fl \[lindex \$fd 0\]} ;\
			set datasets($base.$name,fields) \$fl ;\
			set datasets($base.$name,recno) 0 ;\
			set datasets($base.$name,nrecs) \[pg_result \$res -numTuples\] ;\
			cursor_normal ;\
		}"
		eval "proc $base.$name:setsql {sqlcmd} {\
			global datasets ;\
			set datasets($base.$name,sql) \$sqlcmd ;\
		}"
		eval "proc $base.$name:nrecords {} {\
			global datasets ;\
			return \$datasets($base.$name,nrecs) ;\
		}"
		eval "proc $base.$name:crtrecord {} {\
			global datasets ;\
			return \$datasets($base.$name,recno) ;\
		}"
		eval "proc $base.$name:moveto {newrecno} {\
			global datasets ;\
			set datasets($base.$name,recno) \$newrecno ;\
		}"
		eval "proc $base.$name:close {} {
			global tup$basewp$name ;\
			catch {unset tup$basewp$name };\
		}"
		eval "proc $base.$name:fields {} {\
			global datasets ;\
			return \$datasets($base.$name,fields) ;\
		}"
		eval "proc $base.$name:fill {lb fld} {\
			global datasets tup$basewp$name ;\
			\$lb delete 0 end ;\
			for {set i 0} {\$i<\$datasets($base.$name,nrecs)} {incr i} {\
				\$lb insert end \$tup$basewp$name\(\$i,\$fld\) ;\
			}
		}"
		eval "proc $base.$name:movefirst {} {global datasets ; set datasets($base.$name,recno) 0}"
		eval "proc $base.$name:movenext {} {global datasets ; incr datasets($base.$name,recno) ; if {\$datasets($base.$name,recno)==\[$base.$name:nrecords\]} {$base.$name:movelast}}"
		eval "proc $base.$name:moveprevious {} {global datasets ; incr datasets($base.$name,recno) -1 ; if {\$datasets($base.$name,recno)==-1} {$base.$name:movefirst}}"
		eval "proc $base.$name:movelast {} {global datasets ; set datasets($base.$name,recno) \[expr \[$base.$name:nrecords\] -1\]}"
		eval "proc $base.$name:updatecontrols {} {\
			global datasets tup$basewp$name ;\
			set i \$datasets($base.$name,recno) ;\
			foreach fld \$datasets($base.$name,fields) {\
				catch {\
					upvar $basewp$name\(\$fld\) dbvar ;\
					set dbvar \$tup$basewp$name\(\$i,\$fld\) ;\
				}\
			}\
		}"
		eval "proc $base.$name:clearcontrols {} {\
			global datasets ;\
			catch { foreach fld \$datasets($base.$name,fields) {\
				catch {\
					upvar $basewp$name\(\$fld\) dbvar ;\
					set dbvar {} ;\
				}\
			}}\
		}"
	}
	radio {
		radiobutton  $base.$name -font $pref(font_normal) -text "$fdobj($item,l)" -variable "$fdobj($item,v)" -value "$name" -borderwidth 1
		set wh {}
	}
	entry {
		set var {} ; catch {set var $fdobj($item,v)}
		entry $base.$name -bo 1 -ba white -selectborderwidth 0  -highlightthickness 0 
		if {$var!=""} {$base.$name configure -textvar $var}
	}
	label {
		set wh {}
		label $base.$name -font $pref(font_normal) -anchor nw -padx 0 -pady 0 -text $fdobj($item,l)
	set var {} ; catch {set var $fdobj($item,v)}
	if {$var!=""} {$base.$name configure -textvar $var}
	}
	listbox {
		listbox $base.$name -borderwidth 1 -background white  -highlightthickness 0 -selectborderwidth 0 -font $pref(font_normal) -yscrollcommand [subst {$base.sb$name set}]
	scrollbar $base.sb$name -borderwidth 1 -command [subst {$base.$name yview}] -orient vert  -highlightthickness 0
	eval [subst "place $base.sb$name -x [expr [lindex $coord 2]-14] -y [expr [lindex $coord 1]-1] -width 16 -height [expr 3+[lindex $coord 3]-[lindex $coord 1]] -anchor nw -bordermode ignore"]
	}
}
if $visual {eval [subst "place $base.$name  -x [expr [lindex $coord 0]-1] -y [expr [lindex $coord 1]-1] -anchor nw $wh -bordermode ignore"]}
}
}



proc {get_dwlb_Selection} {} {
set temp [.dw.lb curselection]
if {$temp==""} return "";
return [.dw.lb get $temp]
}

proc {get_pgtype} {oid} {
global dbc
set temp "unknown"
wpg_select $dbc "select typname from pg_type where oid=$oid" rec {
	set temp $rec(typname)
}
return $temp
}

proc {get_tables} {} {
global dbc
set tbl {}
catch {
	wpg_select $dbc "select * from pg_class where (relname !~ '^pg_') and (relkind='r') order by relname" rec {
		if {![regexp "^pga_" $rec(relname)]} then {lappend tbl $rec(relname)}
	}
}
return $tbl
}

proc {get_tag_info} {wn itemid prefix} {
set taglist [$wn.c itemcget $itemid -tags]
set i [lsearch -glob $taglist $prefix*]
set thetag [lindex $taglist $i]
return [string range $thetag 1 end]
}

proc {mw_canvas_click} {wn x y} {
global mw
if {![mw_exit_edit $wn]} return
# Determining row
for {set row 0} {$row<$mw($wn,nrecs)} {incr row} {
	if {[lindex $mw($wn,rowy) $row]>$y} break
}
incr row -1
if {$y>[lindex $mw($wn,rowy) $mw($wn,last_rownum)]} {set row $mw($wn,last_rownum)}
if {$row<0} return
set mw($wn,row_edited) $row
set mw($wn,crtrow) $row
mw_show_record $wn $row
if {$mw($wn,errorsavingnew)} return
# Determining column
set posx [expr -$mw($wn,leftoffset)]
set col 0
foreach cw $mw($wn,colwidth) {
	incr posx [expr $cw+2]
	if {$x<$posx} break
	incr col
}
set itlist [$wn.c find withtag r$row]
foreach item $itlist {
	if {[get_tag_info $wn $item c]==$col} {
		mw_start_edit $wn $item $x $y
		break
	}
}
}

proc {mw_delete_record} {wn} {
global dbc mw
if {!$mw($wn,updatable)} return;
if {![mw_exit_edit $wn]} return;
set taglist [$wn.c gettags hili]
if {[llength $taglist]==0} return;
set rowtag [lindex $taglist [lsearch -regexp $taglist "^r"]]
set row [string range $rowtag 1 end]
set oid [lindex $mw($wn,keylist) $row]
if {[tk_messageBox -title "FINAL WARNING" -icon question -parent $wn -message "Delete current record ?" -type yesno -default no]=="no"} return
if {[sql_exec noquiet "delete from \"$mw($wn,tablename)\" where oid=$oid"]} {
	$wn.c delete hili
}
}

proc {mw_draw_headers} {wn} {
global mw pref
$wn.c delete header
set posx [expr 5-$mw($wn,leftoffset)]
for {set i 0} {$i<$mw($wn,colcount)} {incr i} {
	set xf [expr $posx+[lindex $mw($wn,colwidth) $i]]
	$wn.c create rectangle $posx 1 $xf 22 -fill #CCCCCC -outline "" -width 0 -tags header
	$wn.c create text [expr $posx+[lindex $mw($wn,colwidth) $i]*1.0/2] 14 -text [lindex $mw($wn,colnames) $i] -tags header -fill navy -font $pref(font_normal)
	$wn.c create line $posx 22 [expr $xf-1] 22 -fill #AAAAAA -tags header
	$wn.c create line [expr $xf-1] 5 [expr $xf-1] 22 -fill #AAAAAA -tags header
	$wn.c create line [expr $xf+1] 5 [expr $xf+1] 22 -fill white -tags header
	$wn.c create line $xf -15000 $xf 15000 -fill #CCCCCC -tags [subst {header movable v$i}]
	set posx [expr $xf+2]
}
set mw($wn,r_edge) $posx
$wn.c bind movable <Button-1> "drag_start $wn %W %x %y"
$wn.c bind movable <B1-Motion> {drag_it %W %x %y}
$wn.c bind movable <ButtonRelease-1> "drag_stop $wn %W %x %y"
$wn.c bind movable <Enter> "$wn configure -cursor left_side"
$wn.c bind movable <Leave> "$wn configure -cursor left_ptr"
}

proc {mw_draw_hgrid} {wn} {
global mw
$wn.c delete hgrid
set posx 10
for {set j 0} {$j<$mw($wn,colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr [lindex $mw($wn,colwidth) $j]+2]
	set textwidth($j) [expr [lindex $mw($wn,colwidth) $j]-5]
}
incr posx -6
for {set i 0} {$i<$mw($wn,nrecs)} {incr i} {
	$wn.c create line [expr -$mw($wn,leftoffset)] [lindex $mw($wn,rowy) [expr $i+1]] [expr $posx-$mw($wn,leftoffset)] [lindex $mw($wn,rowy) [expr $i+1]] -fill gray -tags [subst {hgrid g$i}]
}
if {$mw($wn,updatable)} {
	set i $mw($wn,nrecs)
	set posy [expr 14+[lindex $mw($wn,rowy) $mw($wn,nrecs)]]
	$wn.c create line [expr -$mw($wn,leftoffset)] $posy [expr $posx-$mw($wn,leftoffset)] $posy -fill gray -tags [subst {hgrid g$i}]
}
}

proc {mw_draw_new_record} {wn} {
global mw pref
set posx [expr 10-$mw($wn,leftoffset)]
set posy [lindex $mw($wn,rowy) $mw($wn,last_rownum)]
if {$pref(tvfont)=="helv"} {
	set tvfont $pref(font_normal)
} else {
	set tvfont $pref(font_fix)
}
if {$mw($wn,updatable)} {
  for {set j 0} {$j<$mw($wn,colcount)} {incr j} {
	$wn.c create text $posx $posy -text * -tags [subst {r$mw($wn,nrecs) c$j q new unt}]  -anchor nw -font $tvfont -width [expr [lindex $mw($wn,colwidth) $j]-5]
	incr posx [expr [lindex $mw($wn,colwidth) $j]+2]
  }
  incr posy 14
  $wn.c create line [expr -$mw($wn,leftoffset)] $posy [expr $mw($wn,r_edge)-$mw($wn,leftoffset)] $posy -fill gray -tags [subst {hgrid g$mw($wn,nrecs)}]
}
}

proc {mw_edit_text} {wn c k} {
global mw
set bbin [$wn.c bbox r$mw($wn,row_edited)]
switch $k {
	BackSpace { set dp [expr [$wn.c index $mw($wn,id_edited) insert]-1];if {$dp>=0} {$wn.c dchars $mw($wn,id_edited) $dp $dp; set mw($wn,dirtyrec) 1}}
	Home {$wn.c icursor $mw($wn,id_edited) 0}
	End {$wn.c icursor $mw($wn,id_edited) end}
	Left {$wn.c icursor $mw($wn,id_edited) [expr [$wn.c index $mw($wn,id_edited) insert]-1]}
	Delete {}
	Right {$wn.c icursor $mw($wn,id_edited) [expr [$wn.c index $mw($wn,id_edited) insert]+1]}
	Return {if {[mw_exit_edit $wn]} {$wn.c focus {}}}
	Escape {set mw($wn,dirtyrec) 0; $wn.c itemconfigure $mw($wn,id_edited) -text $mw($wn,text_initial_value); $wn.c focus {}}
	default {if {[string compare $c " "]>-1} {$wn.c insert $mw($wn,id_edited) insert $c;set mw($wn,dirtyrec) 1}}
}
set bbout [$wn.c bbox r$mw($wn,row_edited)]
set dy [expr [lindex $bbout 3]-[lindex $bbin 3]]
if {$dy==0} return
set re $mw($wn,row_edited)
$wn.c move g$re 0 $dy
for {set i [expr 1+$re]} {$i<=$mw($wn,nrecs)} {incr i} {
	$wn.c move r$i 0 $dy
	$wn.c move g$i 0 $dy
	set rh [lindex $mw($wn,rowy) $i]
	incr rh $dy
	set mw($wn,rowy) [lreplace $mw($wn,rowy) $i $i $rh]
}
mw_show_record $wn $mw($wn,row_edited)
# Delete is trapped by window interpreted as record delete
#    Delete {$wn.c dchars $mw($wn,id_edited) insert insert; set mw($wn,dirtyrec) 1}
}

proc {mw_exit_edit} {wn} {
global mw dbc
# User has edited the text ?
if {!$mw($wn,dirtyrec)} {
	# No, unfocus text
	$wn.c focus {}
	# For restoring * to the new record position
	if {$mw($wn,id_edited)!=""} {
		if {[lsearch [$wn.c gettags $mw($wn,id_edited)] new]!=-1} {
			$wn.c itemconfigure $mw($wn,id_edited) -text $mw($wn,text_initial_value)
		}
	}
	set mw($wn,id_edited) {};set mw($wn,text_initial_value) {}
	return 1
}
# Trimming the spaces
set fldval [string trim [$wn.c itemcget $mw($wn,id_edited) -text]]
$wn.c itemconfigure $mw($wn,id_edited) -text $fldval
if {[string compare $mw($wn,text_initial_value) $fldval]==0} {
	set mw($wn,dirtyrec) 0
	$wn.c focus {}
	set mw($wn,id_edited) {};set mw($wn,text_initial_value) {}
	return 1
}
cursor_clock
set oid [lindex $mw($wn,keylist) $mw($wn,row_edited)]
set fld [lindex $mw($wn,colnames) [get_tag_info $wn $mw($wn,id_edited) c]]
set fillcolor black
if {$mw($wn,row_edited)==$mw($wn,last_rownum)} {
	set fillcolor red
	set sfp [lsearch $mw($wn,newrec_fields) "\"$fld\""]
	if {$sfp>-1} {
		set mw($wn,newrec_fields) [lreplace $mw($wn,newrec_fields) $sfp $sfp]
		set mw($wn,newrec_values) [lreplace $mw($wn,newrec_values) $sfp $sfp]
	}			
	lappend mw($wn,newrec_fields) "\"$fld\""
	lappend mw($wn,newrec_values) '$fldval'
	# Remove the untouched tag from the object
	$wn.c dtag $mw($wn,id_edited) unt
		$wn.c itemconfigure $mw($wn,id_edited) -fill red
	set retval 1
} else {
	set mw($wn,msg) "Updating record ..."
	after 1000 "set mw($wn,msg) {}"
	regsub -all ' $fldval  \\' sqlfldval
	set retval [sql_exec noquiet "update \"$mw($wn,tablename)\" set \"$fld\"='$sqlfldval' where oid=$oid"]
}
cursor_normal
if {!$retval} {
	set mw($wn,msg) ""
	focus $wn.c
	return 0
}
set mw($wn,dirtyrec) 0
$wn.c focus {}
set mw($wn,id_edited) {};set mw($wn,text_initial_value) {}
return 1
}

proc {mw_load_layout} {wn layoutname} {
global dbc mw
cursor_clock
set mw($wn,layout_name) $layoutname
catch {unset mw($wn,colcount) mw($wn,colnames) mw($wn,colwidth)}
set mw($wn,layout_found) 0
set pgres [wpg_exec $dbc "select *,oid from pga_layout where tablename='$layoutname' order by oid desc"]
set pgs [pg_result $pgres -status]
if {$pgs!="PGRES_TUPLES_OK"} {
	# Probably table pga_layout isn't yet defined
	sql_exec noquiet "create table pga_layout (tablename varchar(64),nrcols int2,colnames text,colwidth text)"
	sql_exec quiet "grant ALL on pga_layout to PUBLIC"
} else {
	set nrlay [pg_result $pgres -numTuples]
	if {$nrlay>=1} {
		set layoutinfo [pg_result $pgres -getTuple 0]
		set mw($wn,colcount) [lindex $layoutinfo 1]
		set mw($wn,colnames)  [lindex $layoutinfo 2]
		set mw($wn,colwidth) [lindex $layoutinfo 3]
		set goodoid [lindex $layoutinfo 4]
		set mw($wn,layout_found) 1
	}
	if {$nrlay>1} {
		show_error "Multiple ($nrlay) layout info found\n\nPlease report the bug!"
		sql_exec quiet "delete from pga_layout where (tablename='$mw($wn,tablename)') and (oid<>$goodoid)"
	}
}
pg_result $pgres -clear
}

proc {mw_pan_left} {wn } {
global mw
if {![mw_exit_edit $wn]} return;
if {$mw($wn,leftcol)==[expr $mw($wn,colcount)-1]} return;
set diff [expr 2+[lindex $mw($wn,colwidth) $mw($wn,leftcol)]]
incr mw($wn,leftcol)
incr mw($wn,leftoffset) $diff
$wn.c move header -$diff 0
$wn.c move q -$diff 0
$wn.c move hgrid -$diff 0
}

proc {mw_pan_right} {wn} {
global mw
if {![mw_exit_edit $wn]} return;
if {$mw($wn,leftcol)==0} return;
incr mw($wn,leftcol) -1
set diff [expr 2+[lindex $mw($wn,colwidth) $mw($wn,leftcol)]]
incr mw($wn,leftoffset) -$diff
$wn.c move header $diff 0
$wn.c move q $diff 0
$wn.c move hgrid $diff 0
}

proc {mw_save_new_record} {wn} {
global dbc mw
if {![mw_exit_edit $wn]} {return 0}
if {$mw($wn,newrec_fields)==""} {return 1}
set mw($wn,msg) "Saving new record ..."
after 1000 "set mw($wn,msg) {}"
set pgres [wpg_exec $dbc "insert into \"$mw($wn,tablename)\" ([join $mw($wn,newrec_fields) ,]) values ([join $mw($wn,newrec_values) ,])" ]
if {[pg_result $pgres -status]!="PGRES_COMMAND_OK"} {
	set errmsg [pg_result $pgres -error]
	show_error "Error inserting new record\n\n$errmsg"
	return 0
}
set oid [pg_result $pgres -oid]
lappend mw($wn,keylist) $oid
pg_result $pgres -clear
# Get bounds of the last record
set lrbb [$wn.c bbox new]
lappend mw($wn,rowy) [lindex $lrbb 3]
$wn.c itemconfigure new -fill black
$wn.c dtag q new
# Replace * from untouched new row elements with "  "
foreach item [$wn.c find withtag unt] {
	$wn.c itemconfigure $item -text "  "
}
$wn.c dtag q unt
incr mw($wn,last_rownum)
incr mw($wn,nrecs)
mw_draw_new_record $wn
set mw($wn,newrec_fields) {}
set mw($wn,newrec_values) {}
return 1
}

proc {mw_scroll_window} {wn par1 args} {
global mw
if {![mw_exit_edit $wn]} return;
if {$par1=="scroll"} {
	set newtop $mw($wn,toprec)
	if {[lindex $args 1]=="units"} {
		incr newtop [lindex $args 0]
	} else {
		incr newtop [expr [lindex $args 0]*25]
		if {$newtop<0} {set newtop 0}
		if {$newtop>=[expr $mw($wn,nrecs)-1]} {set newtop [expr $mw($wn,nrecs)-1]}
	}
} elseif {$par1=="moveto"} {
	set newtop [expr int([lindex $args 0]*$mw($wn,nrecs))]
} else {
	return
}
if {$newtop<0} return;
if {$newtop>=[expr $mw($wn,nrecs)-1]} return;
set dy [expr [lindex $mw($wn,rowy) $mw($wn,toprec)]-[lindex $mw($wn,rowy) $newtop]]
$wn.c move q 0 $dy
$wn.c move hgrid 0 $dy
set newrowy {}
foreach y $mw($wn,rowy) {lappend newrowy [expr $y+$dy]}
set mw($wn,rowy) $newrowy
set mw($wn,toprec) $newtop
mw_set_scrollbar $wn
}

proc {mw_select_records} {wn sql} {
global dbc field mw pgsql pref
set mw($wn,newrec_fields) {}
set mw($wn,newrec_values) {}
if {![mw_exit_edit $wn]} return;
$wn.c delete q
$wn.c delete header
$wn.c delete hgrid
$wn.c delete new
set mw($wn,leftcol) 0
set mw($wn,leftoffset) 0
set mw($wn,crtrow) {}
set mw($wn,msg) "Accessing data. Please wait ..."
$wn.f1.b1 configure -state disabled
cursor_clock
set is_error 1
if {[sql_exec noquiet "BEGIN"]} {
	if {[sql_exec noquiet "declare mycursor cursor for $sql"]} {
		set pgres [wpg_exec $dbc "fetch $pref(rows) in mycursor"]
		if {$pgsql(status)=="PGRES_TUPLES_OK"} {
			set is_error 0
		}
	}
}
if {$is_error} {
	sql_exec quiet "END"
	set mw($wn,msg) {}
	$wn.f1.b1 configure -state normal
	cursor_normal
	set mw($wn,msg) "Error executing : $sql"
	return
}
if {$mw($wn,updatable)} then {set shift 1} else {set shift 0}
#
# checking at least the numer of fields
set attrlist [pg_result $pgres -lAttributes]
if {$mw($wn,layout_found)} then {
	if {  ($mw($wn,colcount) != [expr [llength $attrlist]-$shift]) ||
		  ($mw($wn,colcount) != [llength $mw($wn,colnames)]) ||
		  ($mw($wn,colcount) != [llength $mw($wn,colwidth)]) } then {
		# No. of columns don't match, something is wrong
		# tk_messageBox -title Information -message "Layout info changed !\nRescanning..."
		set mw($wn,layout_found) 0
		sql_exec quiet "delete from pga_layout where tablename='$mw($wn,layout_name)'"
	}
}
# Always take the col. names from the result
set mw($wn,colcount) [llength $attrlist]
if {$mw($wn,updatable)} then {incr mw($wn,colcount) -1}
set mw($wn,colnames) {}
# In defmw($wn,colwidth) prepare mw($wn,colwidth) (in case that not layout_found)
set defmw($wn,colwidth) {}
for {set i 0} {$i<$mw($wn,colcount)} {incr i} {
	lappend mw($wn,colnames) [lindex [lindex $attrlist [expr {$i+$shift}]] 0]
	lappend defmw($wn,colwidth) 150
}
if {!$mw($wn,layout_found)} {
	set mw($wn,colwidth) $defmw($wn,colwidth)
	sql_exec quiet "insert into pga_layout values ('$mw($wn,layout_name)',$mw($wn,colcount),'$mw($wn,colnames)','$mw($wn,colwidth)')"
	set mw($wn,layout_found) 1
}
set mw($wn,nrecs) [pg_result $pgres -numTuples]
if {$mw($wn,nrecs)>$pref(rows)} {
	set mw($wn,msg) "Only first $pref(rows) records from $mw($wn,nrecs) have been loaded"
	set mw($wn,nrecs) $pref(rows)
}
set tagoid {}
if {$pref(tvfont)=="helv"} {
	set tvfont $pref(font_normal)
} else {
	set tvfont $pref(font_fix)
}
# Computing column's left edge
set posx 10
for {set j 0} {$j<$mw($wn,colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr {[lindex $mw($wn,colwidth) $j]+2}]
	set textwidth($j) [expr {[lindex $mw($wn,colwidth) $j]-5}]
}
incr posx -6
set posy 24
mw_draw_headers $wn
set mw($wn,updatekey) oid
set mw($wn,keylist) {}
set mw($wn,rowy) {24}
set mw($wn,msg) "Loading maximum $pref(rows) records ..."
set wupdatable $mw($wn,updatable)
for {set i 0} {$i<$mw($wn,nrecs)} {incr i} {
	set curtup [pg_result $pgres -getTuple $i]
	if {$wupdatable} then {lappend mw($wn,keylist) [lindex $curtup 0]}
	for {set j 0} {$j<$mw($wn,colcount)} {incr j} {
		$wn.c create text $ledge($j) $posy -text [lindex $curtup [expr {$j+$shift}]] -tags [subst {r$i c$j q}] -anchor nw -font $tvfont -width $textwidth($j) -fill black
	}
	set bb [$wn.c bbox r$i]
	incr posy [expr {[lindex $bb 3]-[lindex $bb 1]}]
	lappend mw($wn,rowy) $posy
	$wn.c create line 0 [lindex $bb 3] $posx [lindex $bb 3] -fill gray -tags [subst {hgrid g$i}]
	if {$i==25} {update; update idletasks}
}
after 3000 "set mw($wn,msg) {}"
set mw($wn,last_rownum) $i
# Defining position for input data
mw_draw_new_record $wn
pg_result $pgres -clear
sql_exec quiet "END"
set mw($wn,toprec) 0
mw_set_scrollbar $wn
if {$mw($wn,updatable)} then {
	$wn.c bind q <Key> "mw_edit_text $wn %A %K"
} else {
	$wn.c bind q <Key> {}
}
set mw($wn,dirtyrec) 0
$wn.c raise header
$wn.f1.b1 configure -state normal
cursor_normal
}

proc {mw_set_scrollbar} {wn} {
global mw
if {$mw($wn,nrecs)==0} return;
$wn.sb set [expr $mw($wn,toprec)*1.0/$mw($wn,nrecs)] [expr ($mw($wn,toprec)+27.0)/$mw($wn,nrecs)]
}

proc {mw_reload} {wn} {
global mw
set nq $mw($wn,query)
if {($mw($wn,isaquery)) && ("$mw($wn,filter)$mw($wn,sortfield)"!="")} {
	show_error "Sorting and filtering not (yet) available from queries!\n\nPlease enter them in the query definition!"
	set mw($wn,sortfield) {}
	set mw($wn,filter) {}
} else {
	if {$mw($wn,filter)!=""} {
		set nq "$mw($wn,query) where ($mw($wn,filter))"
	} else {
		set nq $mw($wn,query)
	}
	if {$mw($wn,sortfield)!=""} {
		set nq "$nq order by $mw($wn,sortfield)"
	}
}
if {[mw_save_new_record $wn]} {mw_select_records $wn $nq}
}

proc {mw_show_record} {wn row} {
global mw
set mw($wn,errorsavingnew) 0
if {$mw($wn,newrec_fields)!=""} {
	if {$row!=$mw($wn,last_rownum)} {
		if {![mw_save_new_record $wn]} {
					set mw($wn,errorsavingnew) 1
					return
				}
	}
}
set y1 [lindex $mw($wn,rowy) $row]
set y2 [lindex $mw($wn,rowy) [expr $row+1]]
if {$y2==""} {set y2 [expr $y1+14]}
$wn.c dtag hili hili
$wn.c addtag hili withtag r$row
# Making a rectangle arround the record
set x 3
foreach wi $mw($wn,colwidth) {incr x [expr $wi+2]}
$wn.c delete crtrec
$wn.c create rectangle [expr -1-$mw($wn,leftoffset)] $y1 [expr $x-$mw($wn,leftoffset)] $y2 -fill #EEEEEE -outline {} -tags {q crtrec}
$wn.c lower crtrec
}

proc {mw_start_edit} {wn id x y} {
global mw
if {!$mw($wn,updatable)} return
set mw($wn,id_edited) $id
set mw($wn,dirtyrec) 0
set mw($wn,text_initial_value) [$wn.c itemcget $id -text]
focus $wn.c
$wn.c focus $id
$wn.c icursor $id @$x,$y
if {$mw($wn,row_edited)==$mw($wn,nrecs)} {
	if {[$wn.c itemcget $id -text]=="*"} {
		$wn.c itemconfigure $id -text ""
		$wn.c icursor $id 0
	}
}
}

proc {open_database} {} {
global dbc host pport dbname username password newusername newpassword sdbname newdbname newhost newpport pref pgsql
cursor_clock
if {$newusername!=""} {
	set connres [catch {set newdbc [pg_connect -conninfo "host=$newhost port=$newpport dbname=$newdbname user=$newusername password=$newpassword"]} msg]
} else {
	set connres [catch {set newdbc [pg_connect $newdbname -host $newhost -port $newpport]} msg]
}
if {$connres} {
	cursor_normal
	show_error "Error trying to connect to database \"$newdbname\" on host $newhost\n\nPostgreSQL error message: $msg"
	return $msg
} else {
	catch {pg_disconnect $dbc}
	set dbc $newdbc
	set host $newhost
	set pport $newpport
	set dbname $newdbname
	set username $newusername
	set password $newpassword
	set sdbname $dbname
	set pref(lastdb) $dbname
	set pref(lasthost) $host
	set pref(lastport) $pport
	set pref(lastusername) $username
	save_pref
	catch {cursor_normal ; Window hide .dbod}
	tab_click .dw.tabTables
	# Check for pga_ tables
	foreach {table structure} { pga_queries {queryname varchar(64),querytype char(1),querycommand text} pga_forms {formname varchar(64),formsource text} pga_scripts {scriptname varchar(64),scriptsource text} pga_reports {reportname varchar(64),reportsource text,reportbody text,reportprocs text,reportoptions text}} {
		set pgres [wpg_exec $dbc "select relname from pg_class where relname='$table'"]
		if {$pgsql(status)!="PGRES_TUPLES_OK"} {
			show_error "FATAL ERROR searching for PgAccess system tables : $pgsql(errmsg)\nStatus:$pgsql(status)"
			catch {pg_disconnect $dbc}
			exit
		} elseif {[pg_result $pgres -numTuples]==0} {
			pg_result $pgres -clear
			sql_exec quiet "create table $table ($structure)"
			sql_exec quiet "grant ALL on $table to PUBLIC"
		}
		catch {pg_result $pgres -clear}
	}
	# searching for autoexec script
	wpg_select $dbc "select * from pga_scripts where scriptname ~* '^autoexec$'" recd {
		eval $recd(scriptsource)
	}
	return ""
}
}

proc {open_form} {formname} {
	 fd_load_form $formname run
	 fd_test
}

proc {open_function} {objname} {
global dbc funcname funcpar funcret
Window show .fw
place .fw.okbtn -y 400
.fw.okbtn configure -state disabled
.fw.text1 delete 1.0 end
wpg_select $dbc "select * from pg_proc where proname='$objname'" rec {
	set funcname $objname
	set temppar $rec(proargtypes)
	set funcret [get_pgtype $rec(prorettype)]
	set funcnrp $rec(pronargs)
	.fw.text1 insert end $rec(prosrc)
}
set funcpar {}
for {set i 0} {$i<$funcnrp} {incr i} {
	lappend funcpar [get_pgtype [lindex $temppar $i]]
}
set funcpar [join $funcpar ,]
}

proc {open_report} {objname} {
global dbc rbvar
Window show .rb
#tkwait visibility .rb
Window hide .rb
Window show .rpv
rb_init
set rbvar(reportname) $objname
rb_load_report
tkwait visibility .rpv
set rbvar(justpreview) 1
rb_preview
}

proc {open_query} {how} {
global dbc queryname mw queryoid

if {[.dw.lb curselection]==""} return;
set queryname [.dw.lb get [.dw.lb curselection]]
if {[set pgres [wpg_exec $dbc "select querycommand,querytype,oid from pga_queries where queryname='$queryname'"]]==0} then {
	show_error "Error retrieving query definition"
	return
}
if {[pg_result $pgres -numTuples]==0} {
	show_error "Query $queryname was not found!"
	pg_result $pgres -clear
	return
}
set tuple [pg_result $pgres -getTuple 0]
set qcmd [lindex $tuple 0]
set qtype [lindex $tuple 1]
set queryoid [lindex $tuple 2]
pg_result $pgres -clear
if {$how=="design"} {
	Window show .qb
	.qb.text1 delete 0.0 end
	.qb.text1 insert end $qcmd
} else {
	if {$qtype=="S"} then {
		set wn [mw_get_new_name]
		set mw($wn,query) [subst $qcmd]
		set mw($wn,updatable) 0
		set mw($wn,isaquery) 1
		mw_create_window
		wm title $wn "Query result: $queryname"
		mw_load_layout $wn $queryname
		mw_select_records $wn $mw($wn,query)
	} else {
		set answ [tk_messageBox -title Warning -type yesno -message "This query is an action query!\n\n[string range $qcmd 0 30] ...\n\nDo you want to execute it?"]
		if {$answ} {
			if {[sql_exec noquiet $qcmd]} {
				tk_messageBox -title Information -message "Your query has been executed without error!"
			}
		}
	}
}
}

proc {mw_free_variables} {wn} {
global mw
	foreach varname [array names mw $wn,*] {
		unset mw($varname)
	}
}

proc {mw_get_new_name} {} {
global mw mwcount
incr mwcount
set wn .mw$mwcount
set mw($wn,dirtyrec) 0
set mw($wn,id_edited) {}
set mw($wn,filter) {}
set mw($wn,sortfield) {}
return .mw$mwcount
}

proc {open_sequence} {objname} {
global dbc seq_name seq_inc seq_start seq_minval seq_maxval
Window show .sqf
set flag 1
wpg_select $dbc "select * from \"$objname\"" rec {
	set flag 0
	set seq_name $objname
	set seq_inc $rec(increment_by)
	set seq_start $rec(last_value)
	.sqf.l3 configure -text "Last value"
	set seq_minval $rec(min_value)
	set seq_maxval $rec(max_value)
	.sqf.defbtn configure -state disabled
	place .sqf.defbtn -x 40 -y 300
}
if {$flag} {
	show_error "Sequence $objname not found!"
} else {
	for {set i 1} {$i<6} {incr i} {
		.sqf.e$i configure -state disabled
	}
	focus .sqf.closebtn
}
}

proc {open_table} {objname} {
global mw sortfield filter
set sortfield {}
set filter {}
set wn [mw_get_new_name]
mw_create_window
set mw($wn,tablename) $objname
mw_load_layout $wn $objname
set mw($wn,query) "select oid,\"$objname\".* from \"$objname\""
set mw($wn,updatable) 1
set mw($wn,isaquery) 0
mw_select_records $wn $mw($wn,query)
catch {wm title $wn "Table viewer : $objname"}
}

proc {open_view} {} {
global mw
set vn [get_dwlb_Selection]
if {$vn==""} return;
set wn [mw_get_new_name]
mw_create_window
set mw($wn,query) "select * from \"$vn\""
set mw($wn,isaquery) 0
set mw($wn,updatable) 0
mw_load_layout $wn $vn
mw_select_records $wn $mw($wn,query)
}

proc {rename_column} {} {
global dbc tiw
	if {[string length [string trim $tiw(new_cn)]]==0} {
		show_error "Field name not entered!"
		return
	}
	set old_name [string trim [string range $tiw(old_cn) 0 31]]
	set tiw(new_cn) [string trim $tiw(new_cn)]
	if {$old_name == $tiw(new_cn)} {
		show_error "New name is the same as the old one !"
		return
	}
	foreach line [.tiw.lb get 0 end] {
		if {[string trim [string range $line 0 31]]==$tiw(new_cn)} {
			show_error "Colum name \"$tiw(new_cn)\" already exists in this table!"
			return
		}
	}
	if {[sql_exec noquiet "alter table \"$tiw(tablename)\" rename column \"$old_name\" to \"$tiw(new_cn)\""]} {
		set temp $tiw(col_id)
		.tiw.lb delete $temp $temp
		.tiw.lb insert $temp "[format %-32.32s $tiw(new_cn)] [string range $tiw(old_cn) 33 end]"
		Window destroy .rcw
	}
}

proc {parameter} {msg} {
global gpw
Window show .gpw
focus .gpw.e1
set gpw(var) ""
set gpw(flag) 0
set gpw(msg) $msg
bind .gpw <Destroy> "set gpw(flag) 1"
grab .gpw
tkwait variable gpw(flag)
if {$gpw(result)} {
	return $gpw(var)
} else {
	return ""
}
}

proc {ql_add_new_table} {} {
global qlvar dbc

if {$qlvar(newtablename)==""} return
set fldlist {}
cursor_clock
wpg_select $dbc "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$qlvar(newtablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
		lappend fldlist $rec(attname)
}
cursor_normal
if {$fldlist==""} {
	show_error "Table $qlvar(newtablename) not found!"
	return
}
set qlvar(tablename$qlvar(ntables)) $qlvar(newtablename)
set qlvar(tablestruct$qlvar(ntables)) $fldlist
set qlvar(tablealias$qlvar(ntables)) "t$qlvar(ntables)"
set qlvar(ali_t$qlvar(ntables)) $qlvar(newtablename)
incr qlvar(ntables)
if {$qlvar(ntables)==1} {
   ql_draw_lizzard
} else {
   ql_draw_table [expr $qlvar(ntables)-1]
}
set qlvar(newtablename) {}
focus .ql.entt
}

proc {ql_compute_sql} {} {
global qlvar
set sqlcmd "select "
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
	if {$sqlcmd!="select "} {set sqlcmd "$sqlcmd, "}
	set sqlcmd "$sqlcmd[lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i]"
}
set tables {}
for {set i 0} {$i<$qlvar(ntables)} {incr i} {
	set thename {}
	catch {set thename $qlvar(tablename$i)}
	if {$thename!=""} {lappend tables "\"$qlvar(tablename$i)\" $qlvar(tablealias$i)"}
}
set sqlcmd "$sqlcmd from [join $tables ,] "
set sup1 {}
if {[llength $qlvar(links)]>0} {
	set sup1 "where "
	foreach link $qlvar(links) {
		if {$sup1!="where "} {set sup1 "$sup1 and "}
		set sup1 "$sup1 ([lindex $link 0].[lindex $link 1]=[lindex $link 2].[lindex $link 3])"
	}
}
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
	set crit [lindex $qlvar(rescriteria) $i]
	if {$crit!=""} {
		if {$sup1==""} {set sup1 "where "}
		if {[string length $sup1]>6} {set sup1 "$sup1 and "}
		set sup1 "$sup1 ([lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i] $crit) "        
	}        
}
set sqlcmd "$sqlcmd $sup1"
set sup2 {}
for {set i 0} {$i<[llength $qlvar(ressort)]} {incr i} {
	set how [lindex $qlvar(ressort) $i]
	if {$how!="unsorted"} {
		if {$how=="Ascending"} {set how asc} else {set how desc}
		if {$sup2==""} {set sup2 " order by "} else {set sup2 "$sup2,"}
		set sup2 "$sup2 [lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i] $how "
	}
}
set sqlcmd "$sqlcmd $sup2"
set qlvar(sql) $sqlcmd
#tk_messageBox -message $sqlcmd
return $sqlcmd
}

proc {ql_delete_object} {} {
global qlvar
# Checking if there 
set obj [.ql.c find withtag hili]
if {$obj==""} return
# Is object a link ?
if {[ql_get_tag_info $obj link]=="s"} {
	if {[tk_messageBox -title WARNING -icon question -parent .ql -message "Remove link ?" -type yesno -default no]=="no"} return
	set linkid [ql_get_tag_info $obj lkid]
	set qlvar(links) [lreplace $qlvar(links) $linkid $linkid]
	.ql.c delete links
	ql_draw_links
	return
}
# Is object a result field ?
if {[ql_get_tag_info $obj res]=="f"} {
	set col [ql_get_tag_info $obj col]
	if {$col==""} return
	if {[tk_messageBox -title WARNING -icon question -parent .ql -message "Remove field from result ?" -type yesno -default no]=="no"} return
	set qlvar(resfields) [lreplace $qlvar(resfields) $col $col]
	set qlvar(restables) [lreplace $qlvar(restables) $col $col]
	set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $col $col]
	ql_draw_res_panel
	return
}
# Is object a table ?
set tablealias [ql_get_tag_info $obj tab]
set tablename $qlvar(ali_$tablealias)
if {"$tablename"==""} return
if {[tk_messageBox -title WARNING -icon question -parent .ql -message "Remove table $tablename from query ?" -type yesno -default no]=="no"} return
for {set i [expr [llength $qlvar(restables)]-1]} {$i>=0} {incr i -1} {
	if {"$tablename"==[lindex $qlvar(restables) $i]} {
	   set qlvar(resfields) [lreplace $qlvar(resfields) $i $i]
	   set qlvar(restables) [lreplace $qlvar(restables) $i $i]
	   set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $i $i]
	}
}
for {set i [expr [llength $qlvar(links)]-1]} {$i>=0} {incr i -1} {
	set thelink [lindex $qlvar(links) $i]
	if {($tablealias==[lindex $thelink 0]) || ($tablealias==[lindex $thelink 2])} {
		set qlvar(links) [lreplace $qlvar(links) $i $i]
	}
}
for {set i 0} {$i<$qlvar(ntables)} {incr i} {
	set temp {}
	catch {set temp $qlvar(tablename$i)}
	if {"$temp"=="$tablename"} {
		unset qlvar(tablename$i)
		unset qlvar(tablestruct$i)
		unset qlvar(tablealias$i)
		break
	}
}
#incr qlvar(ntables) -1
.ql.c delete tab$tablealias
.ql.c delete links
ql_draw_links
ql_draw_res_panel
}

proc {ql_dragit} {w x y} {
global draginfo
if {"$draginfo(obj)" != ""} {
	set dx [expr $x - $draginfo(x)]
	set dy [expr $y - $draginfo(y)]
	if {$draginfo(is_a_table)} {
		set taglist [.ql.c gettags $draginfo(obj)]
		set tabletag [lindex $taglist [lsearch -regexp $taglist "^tab"]]
		$w move $tabletag $dx $dy
		ql_draw_links
	} else {
		$w move $draginfo(obj) $dx $dy
	}
	set draginfo(x) $x
	set draginfo(y) $y
}
}

proc {ql_dragstart} {w x y} {
global draginfo
catch {unset draginfo}
set draginfo(obj) [$w find closest $x $y]
if {[ql_get_tag_info $draginfo(obj) r]=="ect"} {
	# If it'a a rectangle, exit
	set draginfo(obj) {}
	return
}
.ql configure -cursor hand1
.ql.c raise $draginfo(obj)
set draginfo(table) 0
if {[ql_get_tag_info $draginfo(obj) table]=="header"} {
	set draginfo(is_a_table) 1
	.ql.c itemconfigure [.ql.c find withtag hili] -fill black
	.ql.c dtag [.ql.c find withtag hili] hili
	.ql.c addtag hili withtag $draginfo(obj)
	.ql.c itemconfigure hili -fill blue
} else {
	set draginfo(is_a_table) 0
}
set draginfo(x) $x
set draginfo(y) $y
set draginfo(sx) $x
set draginfo(sy) $y
}

proc {ql_dragstop} {x y} {
global draginfo qlvar
# when click Close, ql window is destroyed but event ButtonRelease-1 is fired
if {![winfo exists .ql]} return;
.ql configure -cursor left_ptr
set este {}
catch {set este $draginfo(obj)}
if {$este==""} return
# Re-establish the normal paint order so
# information won't be overlapped by table rectangles
# or link linkes
.ql.c lower $draginfo(obj)
.ql.c lower rect
.ql.c lower links
set qlvar(panstarted) 0
if {$draginfo(is_a_table)} {
	set draginfo(obj) {}
	.ql.c delete links
	ql_draw_links
	return
}
.ql.c move $draginfo(obj) [expr $draginfo(sx)-$x] [expr $draginfo(sy)-$y]
if {($y>$qlvar(yoffs)) && ($x>$qlvar(xoffs))} {
	# Drop position : inside the result panel
	# Compute the offset of the result panel due to panning
	set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
	set newfld [.ql.c itemcget $draginfo(obj) -text]
	set tabtag [ql_get_tag_info $draginfo(obj) tab]
	set col [expr int(($x-$qlvar(xoffs)-$resoffset)/$qlvar(reswidth))]
	set qlvar(resfields) [linsert $qlvar(resfields) $col $newfld]
	set qlvar(ressort) [linsert $qlvar(ressort) $col unsorted]
	set qlvar(rescriteria) [linsert $qlvar(rescriteria) $col {}]
	set qlvar(restables) [linsert $qlvar(restables) $col $tabtag]
	ql_draw_res_panel    
} else {
	# Drop position : in the table panel
	set droptarget [.ql.c find overlapping $x $y $x $y]
	set targettable {}
	foreach item $droptarget {
		set targettable [ql_get_tag_info $item tab]
		set targetfield [ql_get_tag_info $item f-]
		if {($targettable!="") && ($targetfield!="")} {
			set droptarget $item
			break
		}
	}
	# check if target object isn't a rectangle
	if {[ql_get_tag_info $droptarget rec]=="t"} {set targettable {}}
	if {$targettable!=""} {
		# Target has a table
		# See about originate table
		set sourcetable [ql_get_tag_info $draginfo(obj) tab]
		if {$sourcetable!=""} {
			# Source has also a tab .. tag
			set sourcefield [ql_get_tag_info $draginfo(obj) f-]
			if {$sourcetable!=$targettable} {
				lappend qlvar(links) [list $sourcetable $sourcefield $targettable $targetfield $draginfo(obj) $droptarget]
				ql_draw_links
			}
		}
	}
}
# Erase information about onbject beeing dragged
set draginfo(obj) {}
}

proc {ql_draw_links} {} {
global qlvar
.ql.c delete links
set i 0
foreach link $qlvar(links) {
	# Compute the source and destination right edge
	set sre [lindex [.ql.c bbox tab[lindex $link 0]] 2]
	set dre [lindex [.ql.c bbox tab[lindex $link 2]] 2]
	# Compute field bound boxes
	set sbbox [.ql.c bbox [lindex $link 4]]
	set dbbox [.ql.c bbox [lindex $link 5]]
	# Compute the auxiliary lines
	if {[lindex $sbbox 2] < [lindex $dbbox 0]} {
		# Source object is on the left of target object
		set x1 $sre
		set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
		.ql.c create line $x1 $y1 [expr $x1+10] $y1 -tags [subst {links lkid$i}] -width 3
		set x2 [lindex $dbbox 0]
		set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
		.ql.c create line [expr $x2-10] $y2 $x2 $y2 -tags [subst {links lkid$i}] -width 3
		.ql.c create line [expr $x1+10] $y1 [expr $x2-10] $y2 -tags [subst {links lkid$i}] -width 2
	} else {
		# source object is on the right of target object
		set x1 [lindex $sbbox 0]
		set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
		.ql.c create line $x1 $y1 [expr $x1-10] $y1 -tags [subst {links lkid$i}] -width 3
		set x2 $dre
		set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
		.ql.c create line $x2 $y2 [expr $x2+10] $y2 -width 3 -tags [subst {links lkid$i}]
		.ql.c create line [expr $x1-10] $y1 [expr $x2+10] $y2 -tags [subst {links lkid$i}] -width 2
	}
	incr i
}
.ql.c lower links
.ql.c bind links <Button-1> {ql_link_click %x %y}
}

proc {ql_draw_lizzard} {} {
global qlvar pref
.ql.c delete all
set posx 20
for {set it 0} {$it<$qlvar(ntables)} {incr it} {
	ql_draw_table $it
}
.ql.c lower rect
.ql.c create line 0 $qlvar(yoffs) 10000 $qlvar(yoffs) -width 3
.ql.c create rectangle 0 $qlvar(yoffs) 10000 5000 -fill #FFFFFF
for {set i [expr 15+$qlvar(yoffs)]} {$i<500} {incr i 15} {
	.ql.c create line $qlvar(xoffs) $i 10000 $i -fill #CCCCCC -tags {resgrid}
}    
for {set i $qlvar(xoffs)} {$i<10000} {incr i $qlvar(reswidth)} {
	.ql.c create line $i [expr 1+$qlvar(yoffs)] $i 10000 -fill #cccccc -tags {resgrid}
}
# Make a marker for result panel offset calculations (due to panning)
.ql.c create line $qlvar(xoffs) $qlvar(yoffs) $qlvar(xoffs) 500 -tags {resmarker resgrid}
.ql.c create rectangle 0 $qlvar(yoffs) $qlvar(xoffs) 5000 -fill #EEEEEE -tags {reshdr}
.ql.c create text 5 [expr 1+$qlvar(yoffs)] -text Field: -anchor nw -font $pref(font_normal) -tags {reshdr}
.ql.c create text 5 [expr 16+$qlvar(yoffs)] -text Table: -anchor nw -font $pref(font_normal) -tags {reshdr}
.ql.c create text 5 [expr 31+$qlvar(yoffs)] -text Sort: -anchor nw -font $pref(font_normal) -tags {reshdr}
.ql.c create text 5 [expr 46+$qlvar(yoffs)] -text Criteria: -anchor nw -font $pref(font_normal) -tags {reshdr}
.ql.c bind mov <Button-1> {ql_dragstart %W %x %y}
.ql.c bind mov <B1-Motion> {ql_dragit %W %x %y}
bind .ql <ButtonRelease-1> {ql_dragstop %x %y}
bind .ql <Button-1> {qlc_click %x %y %W}
bind .ql <B1-Motion> {ql_pan %x %y}
bind .ql <Key-Delete> {ql_delete_object}
}

proc {ql_draw_res_panel} {} {
global qlvar pref
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
.ql.c delete resp
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
	.ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 1+$qlvar(yoffs)] -text [lindex $qlvar(resfields) $i] -anchor nw -tags [subst {resf resp col$i}] -font $pref(font_normal)
	.ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 16+$qlvar(yoffs)] -text $qlvar(ali_[lindex $qlvar(restables) $i]) -anchor nw -tags {resp rest} -font $pref(font_normal)
	.ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 31+$qlvar(yoffs)] -text [lindex $qlvar(ressort) $i] -anchor nw -tags {resp sort} -font $pref(font_normal)
	if {[lindex $qlvar(rescriteria) $i]!=""} {
		.ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)]  [expr $qlvar(yoffs)+46+15*0] -anchor nw -text [lindex $qlvar(rescriteria) $i]  -font $pref(font_normal) -tags [subst {resp cr-c$i-r0}]
	}
}
.ql.c raise reshdr
.ql.c bind resf <Button-1> {ql_resfield_click %x %y}
.ql.c bind sort <Button-1> {ql_swap_sort %W %x %y}
}

proc {ql_draw_table} {it} {
global qlvar pref

set posy 10
set allbox [.ql.c bbox rect]
if {$allbox==""} {set posx 10} else {set posx [expr 20+[lindex $allbox 2]]}
set tablename $qlvar(tablename$it)
set tablealias $qlvar(tablealias$it)
.ql.c create text $posx $posy -text "$tablename" -anchor nw -tags [subst {tab$tablealias f-oid mov tableheader}] -font $pref(font_bold)
incr posy 16
foreach fld $qlvar(tablestruct$it) {
   .ql.c create text $posx $posy -text $fld -fill #010101 -anchor nw -tags [subst {f-$fld tab$tablealias mov}] -font $pref(font_normal)
   incr posy 14
}
set reg [.ql.c bbox tab$tablealias]
.ql.c create rectangle [lindex $reg 0] [lindex $reg 1] [lindex $reg 2] [lindex $reg 3] -fill #EEEEEE -tags [subst {rect tab$tablealias}]
.ql.c create line [lindex $reg 0] [expr [lindex $reg 1]+15] [lindex $reg 2] [expr [lindex $reg 1]+15] -tags [subst {rect tab$tablealias}]
.ql.c lower tab$tablealias
.ql.c lower rect
}

proc {ql_get_tag_info} {obj prefix} {
set taglist [.ql.c gettags $obj]
set tagpos [lsearch -regexp $taglist "^$prefix"]
if {$tagpos==-1} {return ""}
set thattag [lindex $taglist $tagpos]
return [string range $thattag [string length $prefix] end]
}

proc {ql_init} {} {
global qlvar
catch {unset qlvar}
set qlvar(yoffs) 360
set qlvar(xoffs) 50
set qlvar(reswidth) 150
set qlvar(resfields) {}
set qlvar(ressort) {}
set qlvar(rescriteria) {}
set qlvar(restables) {}
set qlvar(critedit) 0
set qlvar(links) {}
set qlvar(ntables) 0
set qlvar(newtablename) {}
}

proc {ql_link_click} {x y} {
global qlvar

set obj [.ql.c find closest $x $y 1 links]
if {[ql_get_tag_info $obj link]!="s"} return
.ql.c itemconfigure [.ql.c find withtag hili] -fill black
.ql.c dtag [.ql.c find withtag hili] hili
.ql.c addtag hili withtag $obj
.ql.c itemconfigure $obj -fill blue
}

proc {ql_pan} {x y} {
global qlvar
set panstarted 0
catch {set panstarted $qlvar(panstarted) }
if {!$panstarted} return
set dx [expr $x-$qlvar(panstartx)]
set dy [expr $y-$qlvar(panstarty)]
set qlvar(panstartx) $x
set qlvar(panstarty) $y
if {$qlvar(panobject)=="tables"} {
	.ql.c move mov $dx $dy
	.ql.c move links $dx $dy
	.ql.c move rect $dx $dy
} else {
	.ql.c move resp $dx 0
	.ql.c move resgrid $dx 0
	.ql.c raise reshdr
}
}

proc {ql_resfield_click} {x y} {
global qlvar

set obj [.ql.c find closest $x $y]
if {[ql_get_tag_info $obj res]!="f"} return
.ql.c itemconfigure [.ql.c find withtag hili] -fill black
.ql.c dtag [.ql.c find withtag hili] hili
.ql.c addtag hili withtag $obj
.ql.c itemconfigure $obj -fill blue
}

proc {ql_show_sql} {} {
global qlvar pref

set sqlcmd [ql_compute_sql]
.ql.c delete sqlpage
.ql.c create rectangle 0 0 2000 [expr $qlvar(yoffs)-1] -fill #ffffff -tags {sqlpage}
.ql.c create text 10 10 -text $sqlcmd -anchor nw -width 550 -tags {sqlpage} -font $pref(font_normal)
.ql.c bind sqlpage <Button-1> {.ql.c delete sqlpage}
}

proc {ql_swap_sort} {w x y} {
global qlvar
set obj [$w find closest $x $y]
set taglist [.ql.c gettags $obj]
if {[lsearch $taglist sort]==-1} return
set cum [.ql.c itemcget $obj -text]
if {$cum=="unsorted"} {
	set cum Ascending
} elseif {$cum=="Ascending"} {
	set cum Descending
} else {
	set cum unsorted
}
set col [expr int(($x-$qlvar(xoffs))/$qlvar(reswidth))]
set qlvar(ressort) [lreplace $qlvar(ressort) $col $col $cum]
.ql.c itemconfigure $obj -text $cum
}

proc {qlc_click} {x y w} {
global qlvar pref
set qlvar(panstarted) 0
if {$w==".ql.c"} {
	set canpan 1
	if {$y<$qlvar(yoffs)} {
		if {[llength [.ql.c find overlapping $x $y $x $y]]!=0} {set canpan 0}
			set qlvar(panobject) tables
	} else {
		set qlvar(panobject) result
	}
	if {$canpan} {
		.ql configure -cursor hand1
		set qlvar(panstartx) $x
		set qlvar(panstarty) $y
		set qlvar(panstarted) 1
	}
}
set isedit 0
catch {set isedit $qlvar(critedit)}
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
if {$isedit} {
	set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $qlvar(critcol) $qlvar(critcol) $qlvar(critval)]
	.ql.c delete cr-c$qlvar(critcol)-r$qlvar(critrow)
	.ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$qlvar(critcol)*$qlvar(reswidth)] [expr $qlvar(yoffs)+46+15*$qlvar(critrow)] -anchor nw -text $qlvar(critval) -font $pref(font_normal) -tags [subst {resp cr-c$qlvar(critcol)-r$qlvar(critrow)}]
	set qlvar(critedit) 0
}
catch {destroy .ql.entc}
if {$y<[expr $qlvar(yoffs)+46]} return
if {$x<[expr $qlvar(xoffs)+5]} return
set col [expr int(($x-$qlvar(xoffs)-$resoffset)/$qlvar(reswidth))]
if {$col>=[llength $qlvar(resfields)]} return
set nx [expr $col*$qlvar(reswidth)+8+$qlvar(xoffs)+$resoffset]
set ny [expr $qlvar(yoffs)+76]
# Get the old criteria value
set qlvar(critval) [lindex $qlvar(rescriteria) $col]
entry .ql.entc -textvar qlvar(critval) -borderwidth 0 -background #FFFFFF -highlightthickness 0 -selectborderwidth 0  -font $pref(font_normal)
place .ql.entc -x $nx -y $ny -height 14
focus .ql.entc
bind .ql.entc <Button-1> {set qlvar(panstarted) 0}
set qlvar(critcol) $col
set qlvar(critrow) 0
set qlvar(critedit) 1
}

proc {rb_add_field} {} {
global rbvar pref
set fldname [.rb.lb get [.rb.lb curselection]]
set newid [.rb.c create text $rbvar(xf_auto) [expr $rbvar(y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font $pref(font_normal)]
.rb.c create text $rbvar(xf_auto) [expr $rbvar(y_pghdr)+5] -text $fldname -tags [subst {f-$fldname t_f rg_detail mov ro}] -anchor nw -font $pref(font_normal)
set bb [.rb.c bbox $newid]
incr rbvar(xf_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}

proc {rb_add_label} {} {
global rbvar pref
set fldname $rbvar(labeltext)
set newid [.rb.c create text $rbvar(xl_auto) [expr $rbvar(y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font $pref(font_normal)]
set bb [.rb.c bbox $newid]
incr rbvar(xl_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}

proc {rb_change_object_font} {} {
global rbvar
.rb.c itemconfigure hili -font -Adobe-[.rb.bfont cget -text]-[rb_get_bold]-[rb_get_italic]-Normal--*-$rbvar(pointsize)-*-*-*-*-*-*
}

proc {rb_delete_object} {} {
if {[tk_messageBox -title Warning -parent .rb -message "Delete current report object?" -type yesno -default no]=="no"} return;
.rb.c delete hili
}

proc {rb_dragit} {w x y} {
global draginfo rbvar
# Showing current region
foreach rg $rbvar(regions) {
	set rbvar(msg) $rbvar(e_$rg)
	if {$rbvar(y_$rg)>$y} break;
}
set temp {}
catch {set temp $draginfo(obj)}
if {"$temp" != ""} {
	set dx [expr $x - $draginfo(x)]
	set dy [expr $y - $draginfo(y)]
	if {$draginfo(region)!=""} {
		set x $draginfo(x) ; $w move bg_$draginfo(region) 0 $dy
	} else {
		$w move $draginfo(obj) $dx $dy
	}
	set draginfo(x) $x
	set draginfo(y) $y
}
}

proc {rb_dragstart} {w x y} {
global draginfo rbvar
focus .rb.c
catch {unset draginfo}
set obj {}
# Only movable objects start dragging
foreach id [$w find overlapping $x $y $x $y] {
	if {[rb_has_tag $id mov]} {
		set obj $id
		break
	}
}
if {$obj==""} return;
set draginfo(obj) $obj
set taglist [.rb.c itemcget $obj -tags]
set i [lsearch -glob $taglist bg_*]
if {$i==-1} {
	set draginfo(region) {}
} else {
	set draginfo(region) [string range [lindex $taglist $i] 3 64]
} 
.rb configure -cursor hand1
.rb.c itemconfigure [.rb.c find withtag hili] -fill black
.rb.c dtag [.rb.c find withtag hili] hili
.rb.c addtag hili withtag $draginfo(obj)
.rb.c itemconfigure hili -fill blue
set draginfo(x) $x
set draginfo(y) $y
set draginfo(sx) $x
set draginfo(sy) $y
# Setting font information
if {[.rb.c type hili]=="text"} {
	set fnta [split [.rb.c itemcget hili -font] -]
	.rb.bfont configure -text [lindex $fnta 2]
	if {[lindex $fnta 3]=="Medium"} then {.rb.lbold configure -relief raised} else {.rb.lbold configure -relief sunken}
	if {[lindex $fnta 4]=="R"} then {.rb.lita configure -relief raised} else {.rb.lita configure -relief sunken}
	set rbvar(pointsize) [lindex $fnta 8]
	if {[rb_has_tag $obj t_f]} {set rbvar(info) "Database field"}
	if {[rb_has_tag $obj t_l]} {set rbvar(info) "Label"}
	if {[.rb.c itemcget $obj -anchor]=="nw"} then {.rb.balign configure -text left} else {.rb.balign configure -text right}
}
}

proc {rb_dragstop} {x y} {
global draginfo rbvar
# when click Close, ql window is destroyed but event ButtonRelease-1 is fired
if {![winfo exists .rb]} return;
.rb configure -cursor left_ptr
set este {}
catch {set este $draginfo(obj)}
if {$este==""} return
# Erase information about object beeing dragged
if {$draginfo(region)!=""} {
	set dy 0
	foreach rg $rbvar(regions) {
		.rb.c move rg_$rg 0 $dy
		if {$rg==$draginfo(region)} {
			set dy [expr $y-$rbvar(y_$draginfo(region))]
		}
		incr rbvar(y_$rg) $dy
	}
#    .rb.c move det 0 [expr $y-$rbvar(y_$draginfo(region))]
	set rbvar(y_$draginfo(region)) $y
	rb_draw_regions
} else {
	# Check if object beeing dragged is inside the canvas
	set bb [.rb.c bbox $draginfo(obj)]
	if {[lindex $bb 0] < 5} {
		.rb.c move $draginfo(obj) [expr 5-[lindex $bb 0]] 0
	}
}
set draginfo(obj) {}
unset draginfo
}

proc {rb_draw_regions} {} {
global rbvar
foreach rg $rbvar(regions) {
	.rb.c delete bg_$rg
	.rb.c create line 0 $rbvar(y_$rg) 5000 $rbvar(y_$rg) -tags [subst {bg_$rg}]
	.rb.c create rectangle 6 [expr $rbvar(y_$rg)-3] 12 [expr $rbvar(y_$rg)+3] -fill black -tags [subst {bg_$rg mov reg}]
	.rb.c lower bg_$rg
}
}

proc {rb_flip_align} {} {
set bb [.rb.c bbox hili]
if {[.rb.balign cget -text]=="left"} then {
	.rb.balign configure -text right
	.rb.c itemconfigure hili -anchor ne
	.rb.c move hili [expr [lindex $bb 2]-[lindex $bb 0]-3] 0
} else {
	.rb.balign configure -text left
	.rb.c itemconfigure hili -anchor nw
	.rb.c move hili [expr [lindex $bb 0]-[lindex $bb 2]+3] 0
}
}

proc {rb_get_bold} {} {
if {[.rb.lbold cget -relief]=="raised"} then {return Medium} else {return Bold}
}

proc {rb_get_italic} {} {
if {[.rb.lita cget -relief]=="raised"} then {return R} else {return O}
}

proc {rb_get_report_fields} {} {
global dbc rbvar
.rb.lb delete 0 end
if {$rbvar(tablename)==""} return ;
#cursor_clock
wpg_select $dbc "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$rbvar(tablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
	.rb.lb insert end $rec(attname)
}
#cursor_normal
}

proc {rb_has_tag} {id tg} {
if {[lsearch [.rb.c itemcget $id -tags] $tg]==-1} then {return 0 } else {return 1}
}

proc {rb_init} {} {
global rbvar
set rbvar(xl_auto) 10
set rbvar(xf_auto) 10
set rbvar(regions) {rpthdr pghdr detail pgfoo rptfoo}
set rbvar(y_rpthdr) 30
set rbvar(y_pghdr) 60
set rbvar(y_detail) 90
set rbvar(y_pgfoo) 120
set rbvar(y_rptfoo) 150
set rbvar(e_rpthdr) {Report header}
set rbvar(e_pghdr) {Page header}
set rbvar(e_detail) {Detail record}
set rbvar(e_pgfoo) {Page footer}
set rbvar(e_rptfoo) {Report footer}
rb_draw_regions
}

proc {rb_load_report} {} {
global rbvar dbc
.rb.c delete all
wpg_select $dbc "select * from pga_reports where reportname='$rbvar(reportname)'" rcd {
	eval $rcd(reportbody)
}
rb_get_report_fields
rb_draw_regions
}

proc {rb_preview} {} {
global dbc rbvar
Window show .rpv
.rpv.fr.c delete all
set ol [.rb.c find withtag ro]
set fields {}
foreach objid $ol {
	set tags [.rb.c itemcget $objid -tags]
	lappend fields [string range [lindex $tags [lsearch -glob $tags f-*]] 2 64]
	lappend fields [lindex [.rb.c coords $objid] 0]
	lappend fields [lindex [.rb.c coords $objid] 1]
	lappend fields $objid
	lappend fields [lindex $tags [lsearch -glob $tags t_*]]
}
# Parsing page header
set py 10
foreach {field x y objid objtype} $fields {
	if {$objtype=="t_l"} {
		.rpv.fr.c create text $x [expr $py+$y] -text [.rb.c itemcget $objid -text]  -font [.rb.c itemcget $objid -font] -anchor nw
	}
}
incr py [expr $rbvar(y_pghdr)-$rbvar(y_rpthdr)]
# Parsing detail group
set di [lsearch $rbvar(regions) detail]
set y_hi $rbvar(y_detail)
set y_lo $rbvar(y_[lindex $rbvar(regions) [expr $di-1]])
wpg_select $dbc "select * from \"$rbvar(tablename)\"" rec {
	foreach {field x y objid objtype} $fields {
		if {($y>=$y_lo) && ($y<=$y_hi)} then {
		if {$objtype=="t_f"} {
			.rpv.fr.c create text $x [expr $py+$y] -text $rec($field) -font [.rb.c itemcget $objid -font] -anchor [.rb.c itemcget $objid -anchor]
		}
		if {$objtype=="t_l"} {
			.rpv.fr.c create text $x [expr $py+$y] -text [.rb.c itemcget $objid -text]  -font [.rb.c itemcget $objid -font] -anchor nw
		}
		}
	}
	incr py [expr $rbvar(y_detail)-$rbvar(y_pghdr)]
}
.rpv.fr.c configure -scrollregion [subst {0 0 1000 $py}]
}

proc {rb_print_report} {} {
set bb [.rpv.fr.c bbox all]
.rpv.fr.c postscript -file "pgaccess-report.ps" -width [expr 10+[lindex $bb 2]-[lindex $bb 0]] -height [expr 10+[lindex $bb 3]-[lindex $bb 1]]
tk_messageBox -title Information -parent .rb -message "The printed image in Postscript is in the file pgaccess-report.ps"
}

proc {rb_save_report} {} {
global rbvar
set prog "set rbvar(tablename) \"$rbvar(tablename)\""
foreach region $rbvar(regions) {
	set prog "$prog ; set rbvar(y_$region) $rbvar(y_$region)"
}
foreach obj [.rb.c find all] {
	if {[.rb.c type $obj]=="text"} {
		set bb [.rb.c bbox $obj]
		if {[.rb.c itemcget $obj -anchor]=="nw"} then {set x [expr [lindex $bb 0]+1]} else {set x [expr [lindex $bb 2]-2]}
		set prog "$prog ; .rb.c create text $x [lindex $bb 1] -font [.rb.c itemcget $obj -font] -anchor [.rb.c itemcget $obj -anchor] -text {[.rb.c itemcget $obj -text]} -tags {[.rb.c itemcget $obj -tags]}"
	}
}
sql_exec noquiet "delete from pga_reports where reportname='$rbvar(reportname)'"
sql_exec noquiet "insert into pga_reports (reportname,reportsource,reportbody) values ('$rbvar(reportname)','$rbvar(tablename)','$prog')"
}

proc {save_pref} {} {
global pref
catch {
	set fid [open "~/.pgaccessrc" w]
	foreach {opt val} [array get pref] { puts $fid "$opt {$val}" }
	close $fid
}
}

proc {show_error} {emsg} {
   bell ; tk_messageBox -title Error -icon error -message $emsg
}

proc {show_table_information} {tblname} {
global dbc tiw activetab indexlist
set tiw(tablename) $tblname
if {$tiw(tablename)==""} return;
Window show .tiw
.tiw.lb delete 0 end
.tiw.ilb delete 0 end
set tiw(isunique) {}
set tiw(isclustered) {}
set tiw(indexfields) {}
wpg_select $dbc "select attnum,attname,typname,attlen,atttypmod,usename,pg_class.oid from pg_class,pg_user,pg_attribute,pg_type where (pg_class.relname='$tiw(tablename)') and (pg_class.oid=pg_attribute.attrelid) and (pg_class.relowner=pg_user.usesysid) and (pg_attribute.atttypid=pg_type.oid) order by attnum" rec {
	set fsize $rec(attlen)
	set fsize1 $rec(atttypmod)
	set ftype $rec(typname)
	if { $fsize=="-1" && $fsize1!="-1" } {
		set fsize $rec(atttypmod)
		incr fsize -4
	}
	if { $fsize1=="-1" && $fsize=="-1" } {
		set fsize ""
	}
	if {$rec(attnum)>0} {.tiw.lb insert end [format "%-33s %-14s %-4s" $rec(attname) $ftype $fsize]}
	set tiw(owner) $rec(usename)
	set tiw(tableoid) $rec(oid)
	set tiw(f$rec(attnum)) $rec(attname)
}
set tiw(indexlist) {}
wpg_select $dbc "select oid,indexrelid from pg_index where (pg_class.relname='$tiw(tablename)') and (pg_class.oid=pg_index.indrelid)" rec {
	lappend tiw(indexlist) $rec(oid)
	wpg_select $dbc "select relname from pg_class where oid=$rec(indexrelid)" rec1 {
		.tiw.ilb insert end $rec1(relname)
	}
}
}

proc {sql_exec} {how cmd} {
global dbc pgsql
if {[set pgr [wpg_exec $dbc $cmd]]==0} {
	return 0
}
if {($pgsql(status)=="PGRES_COMMAND_OK") || ($pgsql(status)=="PGRES_TUPLES_OK")} {
	pg_result $pgr -clear
	return 1
}	
if {$how != "quiet"} {
	show_error "Error executing query\n\n$cmd\n\nPostgreSQL error message:\n$pgsql(errmsg)\nPostgreSQL status:$pgsql(status)"
}
pg_result $pgr -clear
return 0
}

proc {tab_click} {w} {
global dbc tablist activetab pref
if {$dbc==""} return;
set curtab [$w cget -text]
#if {$activetab==$curtab} return;
.dw.btndesign configure -state disabled
if {$activetab!=""} {
	place .dw.tab$activetab -x 10
	.dw.tab$activetab configure -font $pref(font_normal)
}
$w configure -font $pref(font_bold)
place $w -x 7
place .dw.lmask -x 80 -y [expr 86+25*[lsearch -exact $tablist $curtab]]
set activetab $curtab
# Tabs where button Design is enabled
if {[lsearch {Scripts Queries Reports Forms Users} $activetab]!=-1} {
	.dw.btndesign configure -state normal
}
.dw.lb delete 0 end
cmd_$curtab
}

proc {tiw_show_index} {} {
global tiw dbc
set cs [.tiw.ilb curselection]
if {$cs==""} return
set idxname [.tiw.ilb get $cs]
wpg_select $dbc "select pg_index.*,pg_class.oid from pg_index,pg_class where pg_class.relname='$idxname' and pg_class.oid=pg_index.indexrelid" rec {
	if {$rec(indisunique)=="t"} {
		set tiw(isunique) Yes
	} else {
		set tiw(isunique) No
	}
	if {$rec(indisclustered)=="t"} {
		set tiw(isclustered) Yes
	} else {
		set tiw(isclustered) No
	}
	set tiw(indexfields) {}
	foreach field $rec(indkey) {
		if {$field!=0} {
#            wpg_select $dbc "select attname from pg_attribute where attrelid=$tiw(tableoid) and attnum=$field" rec1 {
#                set tiw(indexfields) "$tiw(indexfields) $rec1(attname)"
#            }
		set tiw(indexfields) "$tiw(indexfields) $tiw(f$field)"
		}

	}
}
set tiw(indexfields) [string trim $tiw(indexfields)]
}

proc {vacuum} {} {
global dbc dbname sdbname pgsql
if {$dbc==""} return;
set sdbname "vacuuming database $dbname ..."
cursor_clock
set pgres [wpg_exec $dbc "vacuum;"]
catch {pg_result $pgres -clear}
cursor_normal
set sdbname $dbname
}

proc {main} {argc argv} {
global pref newdbname newpport newhost newusername newpassword dbc tcl_platform
if {[string toupper $tcl_platform(platform)]=="WINDOWS"} {
	load libpgtcl.dll
} else {
	load libpgtcl.so
}
catch {draw_tabs}
set newusername {}
set newpassword {}
if {$argc>0} {
	set newdbname [lindex $argv 0]
	set newhost localhost
	set newpport 5432
	open_database
} elseif {$pref(autoload) && ($pref(lastdb)!="")} {
	set newdbname $pref(lastdb)
	set newhost $pref(lasthost)
	set newpport $pref(lastport)
	catch {set newusername $pref(lastusername)}
	if {[set openmsg [open_database]]!=""} {
		if {[regexp "no password supplied" $openmsg]} {
			Window show .dbod
			focus .dbod.epassword
			wm transient .dbod .dw
		}
	}
	
}
wm protocol .dw WM_DELETE_WINDOW {
	catch {pg_disconnect $dbc}
	exit }
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

proc vTclWindow.about {base} {
	if {$base == ""} {
		set base .about
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 471x177+168+243
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base "About"
	label $base.l1  -borderwidth 3 -font -Adobe-Helvetica-Bold-R-Normal-*-*-180-*-*-*-*-*  -relief ridge -text PgAccess 
	label $base.l2  -relief groove  -text {A Tcl/Tk interface to
PostgreSQL
by Constantin Teodorescu} 
	label $base.l3  -borderwidth 0 -relief sunken -text {v 0.93}
	label $base.l4  -relief groove  -text {You will always get the latest version at:
http://www.flex.ro/pgaccess

Suggestions : teo@flex.ro} 
	button $base.b1  -borderwidth 1 -command {Window destroy .about} -text Ok 
	place $base.l1  -x 10 -y 10 -width 196 -height 103 -anchor nw -bordermode ignore 
	place $base.l2  -x 10 -y 115 -width 198 -height 55 -anchor nw -bordermode ignore 
	place $base.l3  -x 145 -y 80 -anchor nw -bordermode ignore 
	place $base.l4  -x 215 -y 10 -width 246 -height 103 -anchor nw -bordermode ignore 
	place $base.b1  -x 295 -y 130 -width 105 -height 28 -anchor nw -bordermode ignore
}

proc vTclWindow.dbod {base} {
	if {$base == ""} {
		set base .dbod
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel \
		-cursor left_ptr
	wm focusmodel $base passive
	wm geometry $base 282x180+358+333
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "Open database"
	label $base.lhost \
		-borderwidth 0 -text Host 
	entry $base.ehost \
		-background #fefefe -borderwidth 1 -highlightthickness 1 \
		-selectborderwidth 0 -textvariable newhost 
	bind $base.ehost <Key-Return> {
		focus .dbod.epport
	}
	label $base.lport \
		-borderwidth 0 -text Port 
	entry $base.epport \
		-background #fefefe -borderwidth 1 -highlightthickness 1 \
		-selectborderwidth 0 -textvariable newpport 
	bind $base.epport <Key-Return> {
		focus .dbod.edbname
	}
	label $base.ldbname \
		-borderwidth 0 -text Database 
	entry $base.edbname \
		-background #fefefe -borderwidth 1 -highlightthickness 1 \
		-selectborderwidth 0 -textvariable newdbname 
	bind $base.edbname <Key-Return> {
		focus .dbod.eusername
	.dbod.eusername selection range 0 end
	}
	label $base.lusername \
		-borderwidth 0 -text Username 
	entry $base.eusername \
		-background #fefefe -borderwidth 1 -highlightthickness 1 \
		-selectborderwidth 0 -textvariable newusername 
	bind $base.eusername <Key-Return> {
		focus .dbod.epassword
	}
	label $base.lpassword \
		-borderwidth 0 -text Password 
	entry $base.epassword \
		-background #fefefe -borderwidth 1 -highlightthickness 1 \
		-selectborderwidth 0 -textvariable newpassword -show "*"
	bind $base.epassword <Key-Return> {
		focus .dbod.opbtu
	}
	button $base.opbtu \
		-borderwidth 1 -command open_database -text Open 
	bind $base.opbtu <Key-Return> {
		open_database
	}
	button $base.canbut \
		-borderwidth 1 -command {Window hide .dbod} -text Cancel 
	place $base.lhost \
		-x 35 -y 7 -anchor nw -bordermode ignore 
	place $base.ehost \
		-x 100 -y 5 -anchor nw -bordermode ignore 
	place $base.lport \
		-x 35 -y 32 -anchor nw -bordermode ignore 
	place $base.epport \
		-x 100 -y 30 -anchor nw -bordermode ignore 
	place $base.ldbname \
		-x 35 -y 57 -anchor nw -bordermode ignore 
	place $base.edbname \
		-x 100 -y 55 -anchor nw -bordermode ignore 
	place $base.lusername \
		-x 35 -y 82 -anchor nw -bordermode ignore 
	place $base.eusername \
		-x 100 -y 80 -anchor nw -bordermode ignore 
	place $base.lpassword \
		-x 35 -y 107 -anchor nw -bordermode ignore 
	place $base.epassword \
		-x 100 -y 105 -anchor nw -bordermode ignore 
	place $base.opbtu \
		-x 70 -y 140 -width 60 -height 26 -anchor nw -bordermode ignore 
	place $base.canbut \
		-x 150 -y 140 -width 60 -height 26 -anchor nw -bordermode ignore 
}

proc vTclWindow.dw {base} {
global pref
	if {$base == ""} {
		set base .dw
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel \
		-background #efefef -cursor left_ptr
	wm focusmodel $base passive
	wm geometry $base 322x355+96+172
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "PostgreSQL access"
	label $base.labframe \
		-relief raised 
	listbox $base.lb \
		-background #fefefe \
		-foreground black -highlightthickness 0 -selectborderwidth 0 \
		-yscrollcommand {.dw.sb set} 
	bind $base.lb <Double-Button-1> {
		cmd_Open
	}
	button $base.btnnew \
		-borderwidth 1 -command cmd_New -text New 
	button $base.btnopen \
		-borderwidth 1 -command cmd_Open -text Open 
	button $base.btndesign \
		-borderwidth 1 -command cmd_Design -text Design 
	label $base.lmask \
		-borderwidth 0 \
		-text {  } 
	label $base.label22 \
		-borderwidth 1 \
		-relief raised 
	menubutton $base.menubutton23 \
		-borderwidth 1 -font $pref(font_normal) \
		-menu .dw.menubutton23.01 -padx 4 -pady 3 -text Database 
	menu $base.menubutton23.01 \
		-borderwidth 1 -font $pref(font_normal) \
		-tearoff 0 
	$base.menubutton23.01 add command \
		\
		-command {
Window show .dbod
set newhost $host
set newpport $pport
focus .dbod.edbname
.dbod.edbname selection range 0 end} \
		-label Open -font $pref(font_normal)
	$base.menubutton23.01 add command \
		\
		-command {.dw.lb delete 0 end
set dbc {}
set dbname {}
set sdbname {}} \
		-label Close 
	$base.menubutton23.01 add command \
		-command vacuum -label Vacuum 
	$base.menubutton23.01 add separator
	$base.menubutton23.01 add command \
		-command {cmd_Import_Export Import} -label {Import table} 
	$base.menubutton23.01 add command \
		-command {cmd_Import_Export Export} -label {Export table} 
	$base.menubutton23.01 add separator
	$base.menubutton23.01 add command \
		-command cmd_Preferences -label Preferences 
	$base.menubutton23.01 add command \
		-command "Window show .sqlw" -label "SQL window" 
	$base.menubutton23.01 add separator
	$base.menubutton23.01 add command \
		-command {catch {pg_disconnect $dbc}
save_pref
exit} -label Exit 
	label $base.lshost \
		-relief groove -text localhost -textvariable host 
	label $base.lsdbname \
		-anchor w \
		-relief groove -textvariable sdbname 
	scrollbar $base.sb \
		-borderwidth 1 -command {.dw.lb yview} -orient vert 
	menubutton $base.mnob \
		-borderwidth 1 \
		-menu .dw.mnob.m -font $pref(font_normal) -text Object 
	menu $base.mnob.m \
		-borderwidth 1 -font $pref(font_normal) \
		-tearoff 0 
	$base.mnob.m add command \
		-command cmd_New -font $pref(font_normal) -label New 
	$base.mnob.m add command \
		-command {cmd_Delete } -label Delete 
	$base.mnob.m add command \
		-command {cmd_Rename } -label Rename 
	$base.mnob.m add command \
		-command cmd_Information -label Information 
	menubutton $base.mhelp \
		-borderwidth 1 \
		-menu .dw.mhelp.m -font $pref(font_normal) -text Help 
	menu $base.mhelp.m \
		-borderwidth 1 -font $pref(font_normal) \
		-tearoff 0 
	$base.mhelp.m add command \
		-label Contents 
	$base.mhelp.m add command \
		-label PostgreSQL 
	$base.mhelp.m add separator
	$base.mhelp.m add command \
		-command {Window show .about} -label About 
	place $base.labframe \
		-x 80 -y 30 -width 236 -height 300 -anchor nw -bordermode ignore 
	place $base.lb \
		-x 90 -y 75 -width 205 -height 243 -anchor nw -bordermode ignore 
	place $base.btnnew \
		-x 90 -y 40 -width 60 -height 25 -anchor nw -bordermode ignore 
	place $base.btnopen \
		-x 165 -y 40 -width 60 -height 25 -anchor nw -bordermode ignore 
	place $base.btndesign \
		-x 235 -y 40 -width 60 -height 25 -anchor nw -bordermode ignore 
	place $base.lmask \
		-x 155 -y 45 -width 10 -height 23 -anchor nw -bordermode ignore 
	place $base.label22 \
		-x 0 -y 0 -width 396 -height 23 -anchor nw -bordermode ignore 
	place $base.menubutton23 \
		-x 0 -y 3 -width 63 -height 17 -anchor nw -bordermode ignore 
	place $base.lshost \
		-x 3 -y 335 -width 91 -height 20 -anchor nw -bordermode ignore 
	place $base.lsdbname \
		-x 95 -y 335 -width 223 -height 20 -anchor nw -bordermode ignore 
	place $base.sb \
		-x 295 -y 74 -width 18 -height 245 -anchor nw -bordermode ignore 
	place $base.mnob \
		-x 70 -y 2 -width 44 -height 19 -anchor nw -bordermode ignore 
	place $base.mhelp \
		-x 280 -y 1 -height 20 -anchor nw -bordermode ignore 
}

proc vTclWindow.fw {base} {
	if {$base == ""} {
		set base .fw
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 306x288+233+130
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base "Function"
	label $base.l1  -borderwidth 0 -text Name 
	entry $base.e1  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable funcname 
	label $base.l2  -borderwidth 0 -text Parameters 
	entry $base.e2  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable funcpar 
	label $base.l3  -borderwidth 0 -text Returns 
	entry $base.e3  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable funcret 
	text $base.text1  -background #fefefe -borderwidth 1  -highlightthickness 1 -selectborderwidth 0 -wrap word 
	button $base.okbtn  -borderwidth 1  -command {
			if {$funcname==""} {
				show_error "You must supply a name for this function!"
			} elseif {$funcret==""} {
				show_error "You must supply a return type!"
			} else {
				set funcbody [.fw.text1 get 1.0 end]
				regsub -all "\n" $funcbody " " funcbody
				if {[sql_exec noquiet "create function $funcname ($funcpar) returns $funcret as '$funcbody' language 'sql'"]} {
					Window destroy .fw
					tk_messageBox -title PostgreSQL -message "Function created!"
					tab_click .dw.tabFunctions
				}
								
			}
		}  -state disabled -text Define 
	button $base.cancelbtn  -borderwidth 1 -command {Window destroy .fw} -text Close 
	place $base.l1  -x 15 -y 18 -anchor nw -bordermode ignore 
	place $base.e1  -x 95 -y 15 -width 198 -height 22 -anchor nw -bordermode ignore 
	place $base.l2  -x 15 -y 48 -anchor nw -bordermode ignore 
	place $base.e2  -x 95 -y 45 -width 198 -height 22 -anchor nw -bordermode ignore 
	place $base.l3  -x 15 -y 78 -anchor nw -bordermode ignore 
	place $base.e3  -x 95 -y 75 -width 198 -height 22 -anchor nw -bordermode ignore 
	place $base.text1  -x 15 -y 105 -width 275 -height 141 -anchor nw -bordermode ignore 
	place $base.okbtn  -x 90 -y 400 -anchor nw -bordermode ignore 
	place $base.cancelbtn  -x 160 -y 255 -anchor nw -bordermode ignore
}

proc vTclWindow.iew {base} {
	if {$base == ""} {
		set base .iew
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 287x151+259+304
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base "Import-Export table"
	label $base.l1  -borderwidth 0 -text {Table name} 
	entry $base.e1  -background #fefefe -borderwidth 1 -textvariable ie_tablename 
	label $base.l2  -borderwidth 0 -text {File name} 
	entry $base.e2  -background #fefefe -borderwidth 1 -textvariable ie_filename 
	label $base.l3  -borderwidth 0 -text {Field delimiter} 
	entry $base.e3  -background #fefefe -borderwidth 1 -textvariable ie_delimiter 
	button $base.expbtn  -borderwidth 1  -command {if {$ie_tablename==""} {
	show_error "You have to supply a table name!"
} elseif {$ie_filename==""} {
	show_error "You have to supply a external file name!"
} else {
	if {$ie_delimiter==""} {
		set sup ""
	} else {
		set sup " USING DELIMITERS '$ie_delimiter'"
	}
	if {[.iew.expbtn cget -text]=="Import"} {
		set oper "FROM"
	} else {
		set oper "TO"
	}
		if {$oicb} {
				set sup2 " WITH OIDS "
		} else {
				set sup2 ""
		}
	set sqlcmd "COPY $ie_tablename $sup2 $oper '$ie_filename'$sup"
	cursor_clock
	if {[sql_exec noquiet $sqlcmd]} {
		tk_messageBox -title Information -parent .iew -message "Operation completed!"
		Window destroy .iew
	}
	cursor_normal
}}  -text Export 
	button $base.cancelbtn  -borderwidth 1 -command {Window destroy .iew} -text Cancel 
	checkbutton $base.oicb  -borderwidth 1  -text {with OIDs} -variable oicb 
	place $base.l1  -x 25 -y 15 -anchor nw -bordermode ignore 
	place $base.e1  -x 115 -y 10 -height 22 -anchor nw -bordermode ignore 
	place $base.l2  -x 25 -y 45 -anchor nw -bordermode ignore 
	place $base.e2  -x 115 -y 40 -height 22 -anchor nw -bordermode ignore 
	place $base.l3  -x 25 -y 75 -height 18 -anchor nw -bordermode ignore 
	place $base.e3  -x 115 -y 74 -width 33 -height 22 -anchor nw -bordermode ignore 
	place $base.expbtn  -x 60 -y 110 -height 25 -width 75 -anchor nw -bordermode ignore 
	place $base.cancelbtn  -x 155 -y 110 -height 25 -width 75 -anchor nw -bordermode ignore 
	place $base.oicb  -x 170 -y 75 -anchor nw -bordermode ignore
}

proc {mw_canvas_paste} {wn x y} {
	   global mw
	   $wn.c insert $mw($wn,id_edited) insert [selection get]
	   set mw($wn,dirtyrec) 1
}

proc {mw_create_window} {} {
global mwcount
	set base .mw$mwcount
	set wn .mw$mwcount
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 550x400
	wm maxsize $base 1009 738
	wm minsize $base 550 400
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base "Table browser"
	bind $base <Key-Delete> "mw_delete_record $wn"
	frame $base.f1  -borderwidth 2 -height 75 -relief groove -width 125 
	label $base.f1.l1  -borderwidth 0 -text {Sort field} 
	entry $base.f1.e1  -background #fefefe -borderwidth 1 -width 14  -highlightthickness 1 -textvariable mw($wn,sortfield)
	bind $base.f1.e1 <Key-Return> "mw_reload $wn"	
	bind $base.f1.e1 <Key-KP_Enter> "mw_reload $wn"	
	label $base.f1.lb1  -borderwidth 0 -text {     } 
	label $base.f1.l2  -borderwidth 0 -text {Filter conditions} 
	entry $base.f1.e2  -background #fefefe -borderwidth 1  -highlightthickness 1 -textvariable mw($wn,filter)
	bind $base.f1.e2 <Key-Return> "mw_reload $wn"	
	bind $base.f1.e2 <Key-KP_Enter> "mw_reload $wn"	
	button $base.f1.b1  -borderwidth 1 -text Close -command "
if {\[mw_save_new_record $wn\]} {
	$wn.c delete rows
	$wn.c delete header
	set sortfield {}
	set filter {}
	Window destroy $wn
	mw_free_variables $wn
}
	"
	button $base.f1.b2  -borderwidth 1 -text Reload -command "mw_reload $wn"
	frame $base.frame20  -borderwidth 2 -height 75 -relief groove -width 125 
	button $base.frame20.01  -borderwidth 1 -text < -command "mw_pan_right $wn"
	label $base.frame20.02  -anchor w -borderwidth 1 -height 1  -relief sunken -text {} -textvariable mw($wn,msg) 
	button $base.frame20.03  -borderwidth 1 -text > -command "mw_pan_left $wn"
	canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -highlightthickness 0  -relief ridge -selectborderwidth 0 -takefocus 1 -width 295 
	scrollbar $base.sb  -borderwidth 1 -orient vert -width 12  -command "mw_scroll_window $wn"
	bind $base.c <Button-1> "mw_canvas_click $wn %x %y"
	bind $base.c <Button-2> "mw_canvas_paste $wn %x %y"
	bind $base.c <Button-3> "if {[mw_exit_edit $wn]} \"mw_save_new_record $wn\""
	pack $base.f1  -in $wn -anchor center -expand 0 -fill x -side top 
	pack $base.f1.l1  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.e1  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.lb1  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.l2  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.e2  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.b1  -in $wn.f1 -anchor center -expand 0 -fill none -side right 
	pack $base.f1.b2  -in $wn.f1 -anchor center -expand 0 -fill none -side right 
	pack $base.frame20  -in $wn -anchor s -expand 0 -fill x -side bottom 
	pack $base.frame20.01  -in $wn.frame20 -anchor center -expand 0 -fill none -side left 
	pack $base.frame20.02  -in $wn.frame20 -anchor center -expand 1 -fill x -side left 
	pack $base.frame20.03  -in $wn.frame20 -anchor center -expand 0 -fill none -side right 
	pack $base.c -in $wn -anchor w -expand 1 -fill both -side left 
	pack $base.sb -in $wn -anchor e -expand 0 -fill y -side right
}

proc vTclWindow.nt {base} {
global pref
    if {$base == ""} {
        set base .nt
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 614x392+78+181
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Create new table"
    entry $base.etabn \
        -background #fefefe -borderwidth 1 -selectborderwidth 0 \
        -textvariable ntw(newtablename) 
    bind $base.etabn <Key-Return> {
        focus .nt.einh
    }
    label $base.li \
        -anchor w -borderwidth 0 -text Inherits 
    entry $base.einh \
        -background #fefefe -borderwidth 1 -selectborderwidth 0 \
        -textvariable ntw(fathername) 
    bind $base.einh <Key-Return> {
        focus .nt.e2
    }
    button $base.binh \
        -borderwidth 1 \
        -command {if {[winfo exists .nt.ddf]} {
	destroy .nt.ddf
} else {
	create_drop_down .nt 386 23 220
	focus .nt.ddf.sb
	foreach tbl [get_tables] {.nt.ddf.lb insert end $tbl}
	bind .nt.ddf.lb <ButtonRelease-1> {
		set i [.nt.ddf.lb curselection]
		if {$i!=""} {
			if {$ntw(fathername)==""} {
				set ntw(fathername) "\"[.nt.ddf.lb get $i]\""
			} else {
				set ntw(fathername) "$ntw(fathername),\"[.nt.ddf.lb get $i]\""
			}
		}
		if {$i!=""} {focus .nt.e2}
		destroy .nt.ddf
		break
	}
}} \
        -highlightthickness 0 -takefocus 0 -image dnarw
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -selectborderwidth 0 \
        -textvariable ntw(fldname) 
    bind $base.e2 <Key-Return> {
        focus .nt.e1
    }
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -selectborderwidth 0 \
        -textvariable ntw(fldtype) 
    bind $base.e1 <Key-Return> {
        focus .nt.e5
    }
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -selectborderwidth 0 \
        -textvariable ntw(fldsize) 
    bind $base.e3 <Key-Return> {
        focus .nt.e5
    }
    entry $base.e5 \
        -background #fefefe -borderwidth 1 -selectborderwidth 0 \
        -textvariable ntw(defaultval) 
    bind $base.e5 <Key-Return> {
        focus .nt.cb1
    }
    checkbutton $base.cb1 \
        -borderwidth 1 \
        -offvalue { } -onvalue { NOT NULL} -text {field cannot be null} \
        -variable ntw(notnull) 
    label $base.lab1 \
        -borderwidth 0 -text type 
    label $base.lab2 \
        -borderwidth 0 -anchor w -text {Field name} 
    label $base.lab3 \
        -borderwidth 0 -text size 
    label $base.lab4 \
        -borderwidth 0 -anchor w -text {Default value} 
    button $base.addfld \
        -borderwidth 1 -command add_new_field \
        -text {Add field} 
    button $base.delfld \
        -borderwidth 1 -command {catch {.nt.lb delete [.nt.lb curselection]}} \
        -text {Delete field} 
    button $base.emptb \
        -borderwidth 1 -command {.nt.lb delete 0 [.nt.lb size]} \
        -text {Delete all} 
    button $base.maketbl \
        -borderwidth 1 -command create_table \
        -text Create 
    listbox $base.lb \
        -background #fefefe -borderwidth 1 \
        -font $pref(font_fix) \
        -selectborderwidth 0 -yscrollcommand {.nt.sb set} 
    bind $base.lb <ButtonRelease-1> {
        if {[.nt.lb curselection]!=""} {
	set fldname [string trim [lindex [split [.nt.lb get [.nt.lb curselection]]] 0]]
}
    }
    button $base.exitbtn \
        -borderwidth 1 -command {Window destroy .nt} \
        -text Cancel 
    label $base.l1 \
        -anchor w -borderwidth 1 \
        -relief raised -text {       field name} 
    label $base.l2 \
        -borderwidth 1 \
        -relief raised -text type 
    label $base.l3 \
        -borderwidth 1 \
        -relief raised -text options 
    scrollbar $base.sb \
        -borderwidth 1 -command {.nt.lb yview} -orient vert 
    label $base.l93 \
        -anchor w -borderwidth 0 -text {Table name} 
    button $base.mvup \
        -borderwidth 1 \
        -command {if {[.nt.lb size]>1} {
	set i [.nt.lb curselection]
	if {($i!="")&&($i>0)} {
		.nt.lb insert [expr $i-1] [.nt.lb get $i]
		.nt.lb delete [expr $i+1]
		.nt.lb selection set [expr $i-1]
	}
}} \
        -text {Move up} 
    button $base.mvdn \
        -borderwidth 1 \
        -command {if {[.nt.lb size]>1} {
	set i [.nt.lb curselection]
	if {($i!="")&&($i<[expr [.nt.lb size]-1])} {
		.nt.lb insert [expr $i+2] [.nt.lb get $i]
		.nt.lb delete $i
		.nt.lb selection set [expr $i+1]
	}
}} \
        -text {Move down} 
    button $base.button17 \
        -borderwidth 1 \
        -command {
if {[winfo exists .nt.ddf]} {
	destroy .nt.ddf
} else {
	create_drop_down .nt 291 80 97
	focus .nt.ddf.sb
	.nt.ddf.lb insert end char varchar text int2 int4 serial float4 float8 money abstime date datetime interval reltime time timespan timestamp boolean box circle line lseg path point polygon
	bind .nt.ddf.lb <ButtonRelease-1> {
		set i [.nt.ddf.lb curselection]
		if {$i!=""} {set ntw(fldtype) [.nt.ddf.lb get $i]}
		destroy .nt.ddf
		if {$i!=""} {focus .nt.e3}
		break
	}
}} \
        -highlightthickness 0 -takefocus 0 -image dnarw 
    label $base.lco \
        -borderwidth 0 -anchor w -text Constraint 
    entry $base.eco \
        -background #fefefe -borderwidth 1 -textvariable ntw(constraint) 
    label $base.lch \
        -borderwidth 0 -text check 
    entry $base.ech \
        -background #fefefe -borderwidth 1 -textvariable ntw(check) 
    label $base.ll \
        -borderwidth 1 \
        -relief raised 
    checkbutton $base.pk \
        -borderwidth 1 \
        -offvalue { } -onvalue * -text {primary key} -variable ntw(pk) 
    label $base.lpk \
        -borderwidth 1 \
        -relief raised -text K 
    place $base.etabn \
        -x 85 -y 5 -width 156 -height 20 -anchor nw -bordermode ignore 
    place $base.li \
        -x 245 -y 7 -width 42 -height 16 -anchor nw -bordermode ignore 
    place $base.einh \
        -x 290 -y 5 -width 318 -height 20 -anchor nw -bordermode ignore 
    place $base.binh \
        -x 590 -y 7 -width 16 -height 16 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 85 -y 60 -width 156 -height 20 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 291 -y 60 -width 98 -height 20 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 445 -y 60 -width 46 -height 20 -anchor nw -bordermode ignore 
    place $base.e5 \
        -x 85 -y 82 -width 156 -height 20 -anchor nw -bordermode ignore 
    place $base.cb1 \
        -x 245 -y 83 -width 131 -height 20 -anchor nw -bordermode ignore 
    place $base.lab1 \
        -x 247 -y 62 -width 26 -height 16 -anchor nw -bordermode ignore 
    place $base.lab2 \
        -x 4 -y 62 -width 64 -height 16 -anchor nw -bordermode ignore 
    place $base.lab3 \
        -x 410 -y 62 -width 24 -height 16 -anchor nw -bordermode ignore 
    place $base.lab4 \
        -x 5 -y 83 -width 76 -height 16 -anchor nw -bordermode ignore 
    place $base.addfld \
        -x 534 -y 60 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.delfld \
        -x 534 -y 190 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.emptb \
        -x 534 -y 220 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.maketbl \
        -x 534 -y 365 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.lb \
        -x 4 -y 121 -width 506 -height 269 -anchor nw -bordermode ignore 
    place $base.exitbtn \
        -x 534 -y 335 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.l1 \
        -x 18 -y 105 -width 195 -height 18 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 213 -y 105 -width 88 -height 18 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 301 -y 105 -width 225 -height 18 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 509 -y 121 -width 18 -height 269 -anchor nw -bordermode ignore 
    place $base.l93 \
        -x 4 -y 7 -width 67 -height 16 -anchor nw -bordermode ignore 
    place $base.mvup \
        -x 534 -y 120 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.mvdn \
        -x 534 -y 150 -width 75 -height 26 -anchor nw -bordermode ignore 
    place $base.button17 \
        -x 371 -y 62 -width 16 -height 16 -anchor nw -bordermode ignore 
    place $base.lco \
        -x 5 -y 28 -width 58 -height 16 -anchor nw -bordermode ignore 
    place $base.eco \
        -x 85 -y 27 -width 156 -height 20 -anchor nw -bordermode ignore 
    place $base.lch \
        -x 245 -y 30 -anchor nw -bordermode ignore 
    place $base.ech \
        -x 290 -y 27 -width 318 -height 22 -anchor nw -bordermode ignore 
    place $base.ll \
        -x 5 -y 53 -width 603 -height 2 -anchor nw -bordermode ignore 
    place $base.pk \
        -x 407 -y 83 -width 93 -height 20 -anchor nw -bordermode ignore 
    place $base.lpk \
        -x 4 -y 105 -width 14 -height 18 -anchor nw -bordermode ignore 
}

proc vTclWindow.pw {base} {
global pref
	if {$base == ""} {
		set base .pw
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 322x227+210+219
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base "Preferences"
	label $base.l1  -borderwidth 0 -text {Max rows displayed in table/query view} 
	entry $base.e1  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(rows) 
	label $base.l2  -borderwidth 0 -text "Table viewer font"
	radiobutton $base.tvf  -borderwidth 1 -text {fixed width} -value clean -variable pref(tvfont)
	radiobutton $base.tvfv  -borderwidth 1 -text proportional -value helv -variable pref(tvfont)
	label $base.lfn -borderwidth 0 -anchor w -text "Font normal"
	label $base.lfb -borderwidth 0 -anchor w -text "Font bold"
	label $base.lfi -borderwidth 0 -anchor w -text "Font italic"
	label $base.lff -borderwidth 0 -anchor w -text "Font fixed"
	entry $base.efn -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(font_normal)
	entry $base.efb -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(font_bold)
	entry $base.efi -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(font_italic)
	entry $base.eff -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(font_fix)
	label $base.ll  -borderwidth 1 -relief sunken 
	checkbutton $base.alcb  -borderwidth 1 -text {Auto-load the last opened database at startup}  -variable pref(autoload) 
	button $base.okbtn  -borderwidth 1  -command {
if {$pref(rows)>200} {
	tk_messageBox -title Warning -parent .pw -message "A big number of rows displayed in table view will take a lot of memory!"
}
save_pref
Window destroy .pw
tk_messageBox -title Warning -message "Changed fonts may appear in the next working session!"
} -text Ok 
	place $base.l1  -x 10 -y 10 -anchor nw -bordermode ignore 
	place $base.e1  -x 240 -y 8 -width 65 -height 20 -anchor nw -bordermode ignore 
	place $base.l2  -x 10 -y 38 -anchor nw -bordermode ignore 
	place $base.tvf  -x 115 -y 34 -anchor nw -bordermode ignore 
	place $base.tvfv  -x 205 -y 34 -anchor nw -bordermode ignore 
	place $base.lfn -x 10 -y 65 -anchor nw
	place $base.lfb -x 10 -y 86 -anchor nw
	place $base.lfi -x 10 -y 107 -anchor nw
	place $base.lff -x 10 -y 128 -anchor nw
	place $base.efn -x 80 -y 63 -width 230 -height 20
	place $base.efb -x 80 -y 84 -width 230 -height 20
	place $base.efi -x 80 -y 105 -width 230 -height 20
	place $base.eff -x 80 -y 126 -width 230 -height 20
	place $base.ll  -x 10 -y 150 -width 301 -height 2 -anchor nw -bordermode ignore 
	place $base.alcb  -x 10 -y 155 -anchor nw -bordermode ignore 
	place $base.okbtn  -x 125 -y 195 -width 80 -height 26 -anchor nw -bordermode ignore
}

proc vTclWindow.qb {base} {
global pref
	if {$base == ""} {
		set base .qb
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 442x344+150+150
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "Query builder"
	label $base.lqn  -borderwidth 0 -text {Query name} 
	entry $base.eqn  -background #fefefe -borderwidth 1 -foreground #000000  -highlightthickness 1 -selectborderwidth 0 -textvariable queryname 
	button $base.savebtn  -borderwidth 1  -command {if {$queryname==""} then {
	show_error "You have to supply a name for this query!"
	focus .qb.eqn
} else {
	set qcmd [.qb.text1 get 1.0 end]
	regsub -all "\n" $qcmd " " qcmd
	if {$qcmd==""} then {
	show_error "This query has no commands ?"
	} else {
		if { [lindex [split [string toupper [string trim $qcmd]]] 0] == "SELECT" } {
			set qtype S
		} else {
			set qtype A
		}
		if {$cbv} {
			set pgres [wpg_exec $dbc "create view \"$queryname\" as $qcmd"]
			if {$pgsql(status)!="PGRES_COMMAND_OK"} {
				show_error "Error defining view\n\n$pgsql(errmsg)"
			} else {
				tab_click .dw.tabViews
				Window destroy .qb
			}
			catch {pg_result $pgres -clear}
		} else {
			regsub -all "'" $qcmd "''" qcmd
			cursor_clock
			if {$queryoid==0} then {
				set pgres [wpg_exec $dbc "insert into pga_queries values ('$queryname','$qtype','$qcmd')"]
			} else {
				set pgres [wpg_exec $dbc "update pga_queries set queryname='$queryname',querytype='$qtype',querycommand='$qcmd' where oid=$queryoid"]
			}
			cursor_normal
			if {$pgsql(status)!="PGRES_COMMAND_OK"} then {
				show_error "Error executing query\n$pgres(errmsg)"
			} else {
				tab_click .dw.tabQueries
				if {$queryoid==0} {set queryoid [pg_result $pgres -oid]}
			}
		}
		catch {pg_result $pgres -clear}
	}
}}  -text {Save query definition} 
	button $base.execbtn  -borderwidth 1  -command {
set qcmd [.qb.text1 get 0.0 end]
regsub -all "\n" [string trim $qcmd] " " qcmd
if {[lindex [split [string toupper $qcmd]] 0]!="SELECT"} {
	if {[tk_messageBox -title Warning -parent .qb -message "This is an action query!\n\nExecute it?" -type yesno -default no]=="yes"} {
		sql_exec noquiet $qcmd
	}
} else {
	set wn [mw_get_new_name]
	set mw($wn,query) [subst $qcmd]
	set mw($wn,updatable) 0
	set mw($wn,isaquery) 1
	mw_create_window
	mw_load_layout $wn $queryname
	mw_select_records $wn $mw($wn,query)
}
} -text {Execute query} 
	button $base.termbtn  -borderwidth 1  -command {.qb.cbv configure -state normal
set cbv 0
set queryname {}
.qb.text1 delete 1.0 end
Window destroy .qb} -text Close 
	text $base.text1  -background #fefefe -borderwidth 1  -font $pref(font_normal) -foreground #000000 -highlightthickness 1 -wrap word 
	checkbutton $base.cbv  -borderwidth 1  -text {Save this query as a view} -variable cbv 
	button $base.qlshow  -borderwidth 1  -command {Window show .ql
ql_draw_lizzard
focus .ql.entt} -text {Visual designer} 
	place $base.lqn  -x 5 -y 5 -anchor nw -bordermode ignore 
	place $base.eqn  -x 80 -y 1 -width 355 -height 24 -anchor nw -bordermode ignore 
	place $base.savebtn  -x 5 -y 60 -height 25 -anchor nw -bordermode ignore 
	place $base.execbtn  -x 150 -y 60 -height 25 -anchor nw -bordermode ignore 
	place $base.termbtn  -x 375 -y 60 -width 50 -height 25 -anchor nw -bordermode ignore 
	place $base.text1  -x 5 -y 90 -width 430 -height 246 -anchor nw -bordermode ignore 
	place $base.cbv  -x 5 -y 30 -height 25 -anchor nw -bordermode ignore 
	place $base.qlshow  -x 255 -y 60 -height 25 -anchor nw -bordermode ignore
}

proc vTclWindow.ql {base} {
global pref
	if {$base == ""} {
		set base .ql
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 759x530+10+13
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base "Visual query designer"
	bind $base <B1-Motion> {
		ql_pan %x %y
	}
	bind $base <Button-1> {
		qlc_click %x %y %W
	}
	bind $base <ButtonRelease-1> {
		ql_dragstop %x %y
	}
	bind $base <Key-Delete> {
		ql_delete_object
	}
	canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -relief ridge  -takefocus 0 -width 295 
	button $base.exitbtn  -borderwidth 1 -command {
ql_init
Window destroy .ql} -text Close 
	button $base.showbtn  -borderwidth 1 -command ql_show_sql -text {Show SQL} 
	label $base.l12  -borderwidth 0 -text {Add table} 
	entry $base.entt  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable qlvar(newtablename) 
	bind $base.entt <Key-Return> {
		ql_add_new_table
	}
	button $base.execbtn  -borderwidth 1  -command {
set qcmd [ql_compute_sql]
set wn [mw_get_new_name]
set mw($wn,query) [subst $qcmd]
set mw($wn,updatable) 0
set mw($wn,isaquery) 1
mw_create_window
mw_load_layout $wn nolayoutneeded
mw_select_records $wn $mw($wn,query)} -text {Execute SQL} 
	button $base.stoqb  -borderwidth 1  -command {Window show .qb
.qb.text1 delete 1.0 end
.qb.text1 insert end [ql_compute_sql]
focus .qb} -text {Save to query builder} 
	button $base.bdd  -borderwidth 1  -command {if {[winfo exists .ql.ddf]} {
	destroy .ql.ddf
} else {
	create_drop_down .ql 70 27 200
	focus .ql.ddf.sb
	foreach tbl [get_tables] {.ql.ddf.lb insert end $tbl}
	bind .ql.ddf.lb <ButtonRelease-1> {
		set i [.ql.ddf.lb curselection]
		if {$i!=""} {
			set qlvar(newtablename) [.ql.ddf.lb get $i]
			ql_add_new_table
		}
		destroy .ql.ddf
		break
	}
}}  -image dnarw 
	place $base.c  -x 5 -y 30 -width 748 -height 500 -anchor nw -bordermode ignore 
	place $base.exitbtn  -x 695 -y 5 -height 25 -anchor nw -bordermode ignore 
	place $base.showbtn  -x 367 -y 5 -height 25 -anchor nw -bordermode ignore 
	place $base.l12  -x 10 -y 8 -width 53 -height 16 -anchor nw -bordermode ignore 
	place $base.entt  -x 70 -y 7 -width 126 -height 20 -anchor nw -bordermode ignore 
	place $base.execbtn  -x 452 -y 5 -height 25 -anchor nw -bordermode ignore 
	place $base.stoqb  -x 550 -y 5 -height 25 -anchor nw -bordermode ignore 
	place $base.bdd  -x 200 -y 7 -width 17 -height 20 -anchor nw -bordermode ignore
}


proc vTclWindow.rf {base} {
	if {$base == ""} {
		set base .rf
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 272x105+294+262
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base "Rename"
	label $base.l1  -borderwidth 0 -text {New name} 
	entry $base.e1  -background #fefefe -borderwidth 1 -textvariable newobjname 
	button $base.b1  -borderwidth 1  -command {
			if {$newobjname==""} {
				show_error "You must give object a new name!"
			} elseif {$activetab=="Tables"} {
				set retval [sql_exec noquiet "alter table \"$oldobjname\" rename to \"$newobjname\""]
				if {$retval} {
					sql_exec quiet "update pga_layout set tablename='$newobjname' where tablename='$oldobjname'"
					cmd_Tables
					Window destroy .rf
				}			
			} elseif {$activetab=="Queries"} {
				set pgres [wpg_exec $dbc "select * from pga_queries where queryname='$newobjname'"]
				if {$pgsql(status)!="PGRES_TUPLES_OK"} {
					show_error "Error retrieving from pga_queries\n$pgsql(errmsg)\n$pgsql(status)"
				} elseif {[pg_result $pgres -numTuples]>0} {
					show_error "Query \"$newobjname\" already exists!"
				} else {
					sql_exec noquiet "update pga_queries set queryname='$newobjname' where queryname='$oldobjname'"
					sql_exec noquiet "update pga_layout set tablename='$newobjname' where tablename='$oldobjname'"
					cmd_Queries
					Window destroy .rf
				}
				catch {pg_result $pgres -clear}
			}
	   } -text Rename 
	button $base.b2  -borderwidth 1 -command {Window destroy .rf} -text Cancel 
	place $base.l1  -x 15 -y 28 -anchor nw -bordermode ignore 
	place $base.e1  -x 100 -y 25 -anchor nw -bordermode ignore 
	place $base.b1  -x 65 -y 65 -width 70 -anchor nw -bordermode ignore 
	place $base.b2  -x 145 -y 65 -width 70 -anchor nw -bordermode ignore
}

proc vTclWindow.rb {base} {
global pref
	if {$base == ""} {
		set base .rb
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 652x426+96+120
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "Report builder"
	label $base.l1 \
		-borderwidth 1 \
		-relief raised -text {Report fields} 
	listbox $base.lb \
		-background #fefefe -borderwidth 1 \
		-highlightthickness 1 -selectborderwidth 0 \
		-yscrollcommand {.rb.sb set} 
	bind $base.lb <ButtonRelease-1> {
		rb_add_field
	}
	canvas $base.c \
		-background #fffeff -borderwidth 2 -height 207 -highlightthickness 0 \
		-relief ridge -takefocus 1 -width 295 
	bind $base.c <Button-1> {
		rb_dragstart %W %x %y
	}
	bind $base.c <ButtonRelease-1> {
		rb_dragstop %x %y
	}
	bind $base.c <Key-Delete> {
		rb_delete_object
	}
	bind $base.c <Motion> {
		rb_dragit %W %x %y
	}
	button $base.bt2 \
		-borderwidth 1 \
		-command {if {[tk_messageBox -title Warning -parent .rb -message "All report information will be deleted.\n\nProceed ?" -type yesno -default no]=="yes"} then {
.rb.c delete all
rb_init
rb_draw_regions
}} \
		-text {Clear all} 
	button $base.bt4 \
		-borderwidth 1 -command rb_preview \
		-text Preview 
	button $base.bt5 \
		-borderwidth 1 -command {Window destroy .rb} \
		-text Quit 
	scrollbar $base.sb \
		-borderwidth 1 -command {.rb.lb yview} -orient vert 
	label $base.lmsg \
		-anchor w \
		-relief groove -text {Report header} -textvariable rbvar(msg) 
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable rbvar(tablename) 
	bind $base.e2 <Key-Return> {
		rb_get_report_fields
	}
	entry $base.elab \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable rbvar(labeltext) 
	button $base.badl \
		-borderwidth 1 -command rb_add_label \
		-text {Add label} 
	label $base.lbold \
		-borderwidth 1 -relief raised -text B 
	bind $base.lbold <Button-1> {
		if {[rb_get_bold]=="Bold"} {
   .rb.lbold configure -relief raised
} else {
   .rb.lbold configure -relief sunken
}
rb_change_object_font
	}
	label $base.lita \
		-borderwidth 1 \
		-font $pref(font_italic) \
		-relief raised -text i 
	bind $base.lita <Button-1> {
		if {[rb_get_italic]=="O"} {
   .rb.lita configure -relief raised
} else {
   .rb.lita configure -relief sunken
}
rb_change_object_font
	}
	entry $base.eps \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable rbvar(pointsize) 
	bind $base.eps <Key-Return> {
		rb_change_object_font
	}
	label $base.linfo \
		-anchor w  \
		-relief groove -text {Database field} -textvariable rbvar(info) 
	label $base.llal \
		-borderwidth 0 -text Align 
	button $base.balign \
		-borderwidth 0 -command rb_flip_align \
		-relief groove -text right 
	button $base.savebtn \
		-borderwidth 1 -command rb_save_report \
		-text Save 
	label $base.lfn \
		-borderwidth 0 -text Font 
	button $base.bfont \
		-borderwidth 0 \
		-command {set temp [.rb.bfont cget -text]
if {$temp=="Courier"} then {
  .rb.bfont configure -text Helvetica
} else {
  .rb.bfont configure -text Courier
}
rb_change_object_font} \
		-relief groove -text Courier 
	button $base.bdd \
		-borderwidth 1 \
		-command {if {[winfo exists .rb.ddf]} {
	destroy .rb.ddf
} else {
	create_drop_down .rb 405 22 200
	focus .rb.ddf.sb
	foreach tbl [get_tables] {.rb.ddf.lb insert end $tbl}
	bind .rb.ddf.lb <ButtonRelease-1> {
		set i [.rb.ddf.lb curselection]
		if {$i!=""} {set rbvar(tablename) [.rb.ddf.lb get $i]}
		destroy .rb.ddf
		rb_get_report_fields
		break
	}
}} \
		-highlightthickness 0 -image dnarw 
	label $base.lrn \
		-borderwidth 0 -text {Report name} 
	entry $base.ern \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable rbvar(reportname) 
	bind $base.ern <Key-F5> {
		rb_load_report
	}
	label $base.lrs \
		-borderwidth 0 -text {Report source} 
	label $base.ls \
		-borderwidth 1 -relief raised 
	entry $base.ef \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable rbvar(formula) 
	button $base.baf \
		-borderwidth 1 \
		-text {Add formula} 
	place $base.l1 \
		-x 5 -y 55 -width 131 -height 18 -anchor nw -bordermode ignore 
	place $base.lb \
		-x 5 -y 70 -width 118 -height 121 -anchor nw -bordermode ignore 
	place $base.c \
		-x 140 -y 75 -width 508 -height 345 -anchor nw -bordermode ignore 
	place $base.bt2 \
		-x 5 -y 365 -width 64 -height 26 -anchor nw -bordermode ignore 
	place $base.bt4 \
		-x 70 -y 365 -width 66 -height 26 -anchor nw -bordermode ignore 
	place $base.bt5 \
		-x 70 -y 395 -width 66 -height 26 -anchor nw -bordermode ignore 
	place $base.sb \
		-x 120 -y 70 -width 18 -height 122 -anchor nw -bordermode ignore 
	place $base.lmsg \
		-x 142 -y 55 -width 151 -height 18 -anchor nw -bordermode ignore 
	place $base.e2 \
		-x 405 -y 4 -width 129 -height 18 -anchor nw -bordermode ignore 
	place $base.elab \
		-x 5 -y 225 -width 130 -height 18 -anchor nw -bordermode ignore 
	place $base.badl \
		-x 5 -y 243 -width 132 -height 26 -anchor nw -bordermode ignore 
	place $base.lbold \
		-x 535 -y 55 -width 18 -height 18 -anchor nw -bordermode ignore 
	place $base.lita \
		-x 555 -y 55 -width 18 -height 18 -anchor nw -bordermode ignore 
	place $base.eps \
		-x 500 -y 55 -width 30 -height 18 -anchor nw -bordermode ignore 
	place $base.linfo \
		-x 295 -y 55 -width 91 -height 18 -anchor nw -bordermode ignore 
	place $base.llal \
		-x 575 -y 56 -anchor nw -bordermode ignore 
	place $base.balign \
		-x 610 -y 54 -width 35 -height 21 -anchor nw -bordermode ignore 
	place $base.savebtn \
		-x 5 -y 395 -width 64 -height 26 -anchor nw -bordermode ignore 
	place $base.lfn \
		-x 405 -y 56 -anchor nw -bordermode ignore 
	place $base.bfont \
		-x 435 -y 54 -width 65 -height 21 -anchor nw -bordermode ignore 
	place $base.bdd \
		-x 535 -y 4 -width 15 -height 20 -anchor nw -bordermode ignore 
	place $base.lrn \
		-x 5 -y 5 -anchor nw -bordermode ignore 
	place $base.ern \
		-x 80 -y 4 -width 219 -height 18 -anchor nw -bordermode ignore 
	place $base.lrs \
		-x 320 -y 5 -anchor nw -bordermode ignore 
	place $base.ls \
		-x 5 -y 30 -width 641 -height 2 -anchor nw -bordermode ignore 
	place $base.ef \
		-x 5 -y 280 -width 130 -height 18 -anchor nw -bordermode ignore 
	place $base.baf \
		-x 5 -y 298 -width 132 -height 26 -anchor nw -bordermode ignore 
}

proc vTclWindow.rpv {base} {
	if {$base == ""} {
		set base .rpv
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 495x500+230+50
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base "Report preview"
	frame $base.fr \
		-borderwidth 2 -height 75 -relief groove -width 125 
	canvas $base.fr.c \
		-background #fcfefe -borderwidth 2 -height 207 -relief ridge \
		-scrollregion {0 0 1000 824} -width 295 \
		-yscrollcommand {.rpv.fr.sb set} 
	scrollbar $base.fr.sb \
		-borderwidth 1 -command {.rpv.fr.c yview} -highlightthickness 0 \
		-orient vert -width 12 
	frame $base.f1 \
		-borderwidth 2 -height 75 -width 125 
	button $base.f1.button18 \
		-borderwidth 1 -command {if {$rbvar(justpreview)} then {Window destroy .rb} ; Window destroy .rpv} \
		-text Close 
	button $base.f1.button17 \
		-borderwidth 1 -command rb_print_report \
		-text Print 
	pack $base.fr \
		-in .rpv -anchor center -expand 1 -fill both -side top 
	pack $base.fr.c \
		-in .rpv.fr -anchor center -expand 1 -fill both -side left 
	pack $base.fr.sb \
		-in .rpv.fr -anchor center -expand 0 -fill y -side right 
	pack $base.f1 \
		-in .rpv -anchor center -expand 0 -fill none -side top 
	pack $base.f1.button18 \
		-in .rpv.f1 -anchor center -expand 0 -fill none -side right 
	pack $base.f1.button17 \
		-in .rpv.f1 -anchor center -expand 0 -fill none -side left 
}

proc vTclWindow.sqf {base} {
	if {$base == ""} {
		set base .sqf
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 310x223+245+158
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base "Sequence"
	label $base.l1  -anchor w -borderwidth 0 -text {Sequence name} 
	entry $base.e1  -borderwidth 1 -highlightthickness 1 -textvariable seq_name 
	label $base.l2  -borderwidth 0 -text Increment 
	entry $base.e2  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_inc 
	label $base.l3  -borderwidth 0 -text {Start value} 
	entry $base.e3  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_start 
	label $base.l4  -borderwidth 0 -text Minvalue 
	entry $base.e4  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_minval 
	label $base.l5  -borderwidth 0 -text Maxvalue 
	entry $base.e5  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_maxval 
	button $base.defbtn  -borderwidth 1  -command {
		if {$seq_name==""} {
			show_error "You should supply a name for this sequence"
		} else {
			set s1 {};set s2 {};set s3 {};set s4 {};
			if {$seq_inc!=""} {set s1 "increment $seq_inc"};
			if {$seq_start!=""} {set s2 "start $seq_start"};
			if {$seq_minval!=""} {set s3 "minvalue $seq_minval"};
			if {$seq_maxval!=""} {set s4 "maxvalue $seq_maxval"};
			set sqlcmd "create sequence \"$seq_name\" $s1 $s2 $s3 $s4"
			if {[sql_exec noquiet $sqlcmd]} {
				cmd_Sequences
				tk_messageBox -title Information -parent .sqf -message "Sequence created!"
			}
		}
	} -text {Define sequence} 
	button $base.closebtn  -borderwidth 1  -command {for {set i 1} {$i<6} {incr i} {
	.sqf.e$i configure -state normal
	.sqf.e$i delete 0 end
	.sqf.defbtn configure -state normal
	.sqf.l3 configure -text {Start value}
}
place .sqf.defbtn -x 40 -y 175
Window destroy .sqf
} -text Close 
	place $base.l1  -x 20 -y 20 -width 111 -height 18 -anchor nw -bordermode ignore 
	place $base.e1  -x 135 -y 19 -anchor nw -bordermode ignore 
	place $base.l2  -x 20 -y 50 -anchor nw -bordermode ignore 
	place $base.e2  -x 135 -y 49 -anchor nw -bordermode ignore 
	place $base.l3  -x 20 -y 80 -anchor nw -bordermode ignore 
	place $base.e3  -x 135 -y 79 -anchor nw -bordermode ignore 
	place $base.l4  -x 20 -y 110 -anchor nw -bordermode ignore 
	place $base.e4  -x 135 -y 109 -anchor nw -bordermode ignore 
	place $base.l5  -x 20 -y 140 -anchor nw -bordermode ignore 
	place $base.e5  -x 135 -y 139 -anchor nw -bordermode ignore 
	place $base.defbtn  -x 40 -y 175 -anchor nw -bordermode ignore 
	place $base.closebtn  -x 195 -y 175 -anchor nw -bordermode ignore
}

proc vTclWindow.sw {base} {
global pref
	if {$base == ""} {
		set base .sw
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 594x416+192+152
	wm maxsize $base 1009 738
	wm minsize $base 300 300
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base "Design script"
	frame $base.f1  -height 55 -relief groove -width 125 
	label $base.f1.l1  -borderwidth 0 -text {Script name} 
	entry $base.f1.e1  -background #fefefe -borderwidth 1 -highlightthickness 0  -textvariable scriptname -width 32 
	text $base.src  -background #fefefe  -font $pref(font_normal) -height 2  -highlightthickness 1 -selectborderwidth 0 -width 2 
	frame $base.f2  -height 75 -relief groove -width 125 
	button $base.f2.b1  -borderwidth 1 -command {Window destroy .sw} -text Cancel 
	button $base.f2.b2  -borderwidth 1  -command {if {$scriptname==""} {
	tk_messageBox -title Warning -parent .sw -message "The script must have a name!"
} else {
   sql_exec noquiet "delete from pga_scripts where scriptname='$scriptname'"
   regsub -all {\\} [.sw.src get 1.0 end] {\\\\} scriptsource
   regsub -all ' $scriptsource  \\' scriptsource
   sql_exec noquiet "insert into pga_scripts values ('$scriptname','$scriptsource')"
   cmd_Scripts
}}  -text Save -width 6 
	pack $base.f1  -in .sw -anchor center -expand 0 -fill x -pady 2 -side top 
	pack $base.f1.l1  -in .sw.f1 -anchor center -expand 0 -fill none -ipadx 2 -side left 
	pack $base.f1.e1  -in .sw.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.src  -in .sw -anchor center -expand 1 -fill both -padx 2 -side top 
	pack $base.f2  -in .sw -anchor center -expand 0 -fill none -side top 
	pack $base.f2.b1  -in .sw.f2 -anchor center -expand 0 -fill none -side right 
	pack $base.f2.b2  -in .sw.f2 -anchor center -expand 0 -fill none -side right
}

proc vTclWindow.tiw {base} {
global pref
	if {$base == ""} {
		set base .tiw
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 390x460+243+20
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm title $base "Table information"
	label $base.l1  -borderwidth 0 -text {Table name} 
	label $base.l2  -anchor w -borderwidth 0 -text conturi -textvariable tiw(tablename) 
	label $base.l3  -borderwidth 0 -text Owner 
	label $base.l4  -anchor w -borderwidth 1  -textvariable tiw(owner) 
	listbox $base.lb  -background #fefefe -borderwidth 1  -font $pref(font_fix)  -highlightthickness 1 -selectborderwidth 0  -yscrollcommand {.tiw.sb set} 
	scrollbar $base.sb  -activebackground #d9d9d9 -activerelief sunken -borderwidth 1  -command {.tiw.lb yview} -orient vert 
	button $base.closebtn  -borderwidth 1 -command {Window destroy .tiw}  -pady 3 -text Close
	button $base.renbtn -borderwidth 1 -command {
	if {[set tiw(col_id) [.tiw.lb curselection]]==""} then {bell} else {set tiw(old_cn) [.tiw.lb get [.tiw.lb curselection]] ; set tiw(new_cn) {} ; Window show .rcw ; tkwait visibility .rcw ; wm transient .rcw .tiw ; focus .rcw.e1}} -text {Rename field}
	button $base.addbtn -borderwidth 1 -command "Window show .anfw ; set anfw(name) {} ; set anfw(type) {} ; wm transient .anfw .tiw ; focus .anfw.e1" -text "Add new field"
	label $base.l10  -borderwidth 1  -relief raised -text {field name}
	label $base.l11  -borderwidth 1  -relief raised -text {field type}
	label $base.l12  -borderwidth 1  -relief raised -text size
	label $base.lfi  -borderwidth 0 -text {Field information}
	label $base.lii  -borderwidth 1  -relief raised -text {Indexes defined}
	listbox $base.ilb  -background #fefefe -borderwidth 1  -highlightthickness 1 -selectborderwidth 0 
	bind $base.ilb <ButtonRelease-1> {
		tiw_show_index
	}
	label $base.lip  -borderwidth 1  -relief raised -text {index properties}
	frame $base.fr11  -borderwidth 1 -height 75 -relief sunken -width 125
	label $base.fr11.l9  -borderwidth 0 -text {Is clustered ?} 
	label $base.fr11.l2  -borderwidth 0 -text {Is unique ?} 
	label $base.fr11.liu  -anchor nw -borderwidth 0 -text Yes -textvariable tiw(isunique) 
	label $base.fr11.lic  -anchor nw -borderwidth 0 -text No -textvariable tiw(isclustered) 
	label $base.fr11.l5  -borderwidth 0 -text {Fields :} 
	label $base.fr11.lif  -anchor nw -borderwidth 1  -justify left -relief sunken -text cont  -textvariable tiw(indexfields) -wraplength 170 
	place $base.l1  -x 20 -y 15 -anchor nw -bordermode ignore 
	place $base.l2  -x 100 -y 14 -width 161 -height 18 -anchor nw -bordermode ignore 
	place $base.l3  -x 20 -y 35 -anchor nw -bordermode ignore 
	place $base.l4  -x 100 -y 34 -width 226 -height 18 -anchor nw -bordermode ignore 
	place $base.lb  -x 20 -y 91 -width 338 -height 171 -anchor nw -bordermode ignore 
	place $base.renbtn -x 20 -y 263 -height 25
	place $base.addbtn -x 120 -y 263 -height 25
	place $base.sb  -x 355 -y 90 -width 18 -height 173 -anchor nw -bordermode ignore 
	place $base.closebtn  -x 325 -y 5 -height 25 -anchor nw -bordermode ignore 
	place $base.l10  -x 21 -y 75 -width 204 -height 18 -anchor nw -bordermode ignore 
	place $base.l11  -x 225 -y 75 -width 90 -height 18 -anchor nw -bordermode ignore 
	place $base.l12  -x 315 -y 75 -width 41 -height 18 -anchor nw -bordermode ignore 
	place $base.lfi  -x 20 -y 55 -anchor nw -bordermode ignore 
	place $base.lii  -x 20 -y 290 -width 151 -height 18 -anchor nw -bordermode ignore 
	place $base.ilb  -x 20 -y 306 -width 150 -height 148 -anchor nw -bordermode ignore 
	place $base.lip  -x 171 -y 290 -width 198 -height 18 -anchor nw -bordermode ignore 
	place $base.fr11  -x 170 -y 307 -width 199 -height 147 -anchor nw -bordermode ignore 
	place $base.fr11.l9  -x 10 -y 30 -anchor nw -bordermode ignore 
	place $base.fr11.l2  -x 10 -y 10 -anchor nw -bordermode ignore 
	place $base.fr11.liu  -x 95 -y 10 -width 27 -height 16 -anchor nw -bordermode ignore 
	place $base.fr11.lic  -x 95 -y 30 -width 32 -height 16 -anchor nw -bordermode ignore 
	place $base.fr11.l5  -x 10 -y 55 -anchor nw -bordermode ignore 
	place $base.fr11.lif  -x 10 -y 70 -width 178 -height 68 -anchor nw -bordermode ignore
}

proc vTclWindow.fd {base} {
	if {$base == ""} {
		set base .fd
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 377x315+103+101
	wm maxsize $base 785 570
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base "Form design"
	bind $base <Key-Delete> {
		fd_delete_object
	}
	canvas $base.c \
		-background #828282 -height 207 -highlightthickness 0 -relief ridge \
		-selectborderwidth 0 -width 295 
	bind $base.c <Button-1> {
		fd_mouse_down %x %y
	}
	bind $base.c <ButtonRelease-1> {
		fd_mouse_up %x %y
	}
	bind $base.c <Motion> {
		fd_mouse_move %x %y
	}
	pack $base.c \
		-in .fd -anchor center -expand 1 -fill both -side top 
}

proc vTclWindow.fda {base} {
	if {$base == ""} {
		set base .fda
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 225x197+561+0
	wm maxsize $base 785 570
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base "Attributes"
	label $base.l1 \
		-anchor nw -borderwidth 0 \
		-justify left -text Name -width 8 
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_name) 
	bind $base.e1 <Key-Return> {
		fd_set_name
	}
	label $base.l2 \
		-anchor nw -borderwidth 0 \
		-justify left -text Top -width 8 
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_top) 
	bind $base.e2 <Key-Return> {
		fd_change_coord
	}
	label $base.l3 \
		-anchor w -borderwidth 0 \
		-text Left -width 8 
	entry $base.e3 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_left) 
	bind $base.e3 <Key-Return> {
		fd_change_coord
	}
	label $base.l4 \
		-anchor w -borderwidth 0 \
		-text Width \
		-width 8 
	entry $base.e4 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_width) 
	bind $base.e4 <Key-Return> {
		fd_change_coord
	}
	label $base.l5 \
		-anchor w -borderwidth 0 -padx 0 -text Height -width 8 
	entry $base.e5 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_height) 
	bind $base.e5 <Key-Return> {
		fd_change_coord
	}
	label $base.l6 \
		-borderwidth 0 -text Command 
	entry $base.e6 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_cmd) 
	bind $base.e6 <Key-Return> {
		fd_set_command
	}
	button $base.bcmd \
		-borderwidth 1 \
		-command {Window show .fdcmd
.fdcmd.f.txt delete 1.0 end
.fdcmd.f.txt insert end $fdvar(c_cmd)} \
		-text ... -width 1 
	label $base.l7 \
		-anchor w -borderwidth 0 \
		-text Variable -width 8 
	entry $base.e7 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_var) 
	bind $base.e7 <Key-Return> {
		set fdobj($fdvar(moveitemobj),v) $fdvar(c_var)
	}
	label $base.l8 \
		-anchor w -borderwidth 0 \
		-text Text -width 8 
	entry $base.e8 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(c_text) 
	bind $base.e8 <Key-Return> {
		fd_set_text
	}
	label $base.l0 \
		-borderwidth 1 -relief raised -text {checkbox .udf0.checkbox17} \
		-textvariable fdvar(c_info) -width 28 
	grid $base.l1 \
		-in .fda -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.e1 \
		-in .fda -column 1 -row 1 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.l2 \
		-in .fda -column 0 -row 2 -columnspan 1 -rowspan 1 
	grid $base.e2 \
		-in .fda -column 1 -row 2 -columnspan 1 -rowspan 1 
	grid $base.l3 \
		-in .fda -column 0 -row 3 -columnspan 1 -rowspan 1 
	grid $base.e3 \
		-in .fda -column 1 -row 3 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.l4 \
		-in .fda -column 0 -row 4 -columnspan 1 -rowspan 1 
	grid $base.e4 \
		-in .fda -column 1 -row 4 -columnspan 1 -rowspan 1 
	grid $base.l5 \
		-in .fda -column 0 -row 5 -columnspan 1 -rowspan 1 
	grid $base.e5 \
		-in .fda -column 1 -row 5 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.l6 \
		-in .fda -column 0 -row 6 -columnspan 1 -rowspan 1 
	grid $base.e6 \
		-in .fda -column 1 -row 6 -columnspan 1 -rowspan 1 
	grid $base.bcmd \
		-in .fda -column 2 -row 6 -columnspan 1 -rowspan 1 
	grid $base.l7 \
		-in .fda -column 0 -row 7 -columnspan 1 -rowspan 1 
	grid $base.e7 \
		-in .fda -column 1 -row 7 -columnspan 1 -rowspan 1 
	grid $base.l8 \
		-in .fda -column 0 -row 8 -columnspan 1 -rowspan 1 
	grid $base.e8 \
		-in .fda -column 1 -row 8 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.l0 \
		-in .fda -column 0 -row 0 -columnspan 2 -rowspan 1 
}

proc vTclWindow.fdcmd {base} {
global pref
	if {$base == ""} {
		set base .fdcmd
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 282x274+504+229
	wm maxsize $base 785 570
	wm minsize $base 1 19
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base "Command"
	frame $base.f \
		-borderwidth 2 -height 75 -relief groove -width 125 
	scrollbar $base.f.sb \
		-borderwidth 1 -command {.fdcmd.f.txt yview} -orient vert -width 12 
	text $base.f.txt \
		-font $pref(font_fix) -height 1 \
		-width 115 -yscrollcommand {.fdcmd.f.sb set} 
	frame $base.fb \
		-height 75 -width 125 
	button $base.fb.b1 \
		-borderwidth 1 \
		-command {set fdvar(c_cmd) [.fdcmd.f.txt get 1.0 "end - 1 chars"]
Window hide .fdcmd
fd_set_command} \
		-text Ok -width 5 
	button $base.fb.b2 \
		-borderwidth 1 -command {Window hide .fdcmd} \
		-text Cancel 
	pack $base.f \
		-in .fdcmd -anchor center -expand 1 -fill both -side top 
	pack $base.f.sb \
		-in .fdcmd.f -anchor e -expand 1 -fill y -side right 
	pack $base.f.txt \
		-in .fdcmd.f -anchor center -expand 1 -fill both -side top 
	pack $base.fb \
		-in .fdcmd -anchor center -expand 0 -fill none -side top 
	pack $base.fb.b1 \
		-in .fdcmd.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.b2 \
		-in .fdcmd.fb -anchor center -expand 0 -fill none -side top 
}

proc vTclWindow.fdmenu {base} {
	if {$base == ""} {
		set base .fdmenu
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 288x70+103+0
	wm maxsize $base 785 570
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "Commands"
	button $base.but17 \
		-borderwidth 1 \
		-command {if {[tk_messageBox -title Warning -message "Delete all objects ?" -type yesno -default no]=="no"} return
fd_init} \
		-text {Delete all} 
	button $base.but18 \
		-borderwidth 1 -command {set fdvar(geometry) [wm geometry .fd] ; fd_test } \
		-text {Test form} 
	button $base.but19 \
		-borderwidth 1 -command {destroy .$fdvar(forminame)} \
		-text {Close test form} 
	button $base.bex \
		-borderwidth 1 \
		-command {if {[fd_save_form $fdvar(formname)]==1} {
catch {Window destroy .fd}
catch {Window destroy .fdtb}
catch {Window destroy .fdmenu}
catch {Window destroy .fda}
catch {Window destroy .fdcmd}
catch {Window destroy .$fdvar(forminame)}
}} \
		-text Close 
	button $base.bload \
		-borderwidth 1 -command {fd_load_form nimic design} \
		-text {Load from database} 
	button $base.button17 \
		-borderwidth 1 -command {fd_save_form nimic} \
		-text Save 
	label $base.l1 \
		-borderwidth 0 -text {Form name} 
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(formname) 
	label $base.l2 \
		-borderwidth 0 \
		-text {Form's window internal name} 
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-selectborderwidth 0 -textvariable fdvar(forminame) 
	place $base.but17 \
		-x 5 -y 80 -width 62 -height 24 -anchor nw -bordermode ignore 
	place $base.but18 \
		-x 5 -y 45 -width 62 -height 24 -anchor nw -bordermode ignore 
	place $base.but19 \
		-x 70 -y 45 -width 94 -height 24 -anchor nw -bordermode ignore 
	place $base.bex \
		-x 230 -y 45 -height 24 -anchor nw -bordermode ignore 
	place $base.bload \
		-x 75 -y 80 -width 114 -height 23 -anchor nw -bordermode ignore 
	place $base.button17 \
		-x 165 -y 45 -width 44 -height 24 -anchor nw -bordermode ignore 
	place $base.l1 \
		-x 5 -y 5 -anchor nw -bordermode ignore 
	place $base.e1 \
		-x 75 -y 5 -width 193 -height 17 -anchor nw -bordermode ignore 
	place $base.l2 \
		-x 5 -y 25 -anchor nw -bordermode ignore 
	place $base.e2 \
		-x 175 -y 25 -width 60 -height 17 -anchor nw -bordermode ignore 
}

proc vTclWindow.gpw {base} {
	if {$base == ""} {
		set base .gpw
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
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base "Input parameter"
	label $base.l1 \
		-anchor nw -borderwidth 1 \
		-justify left -relief sunken -textvariable gpw(msg) -wraplength 200 
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable gpw(var) 
	bind $base.e1 <Key-KP_Enter> {
		set gpw(result) 1
destroy .gpw
	}
	bind $base.e1 <Key-Return> {
		set gpw(result) 1
destroy .gpw
	}
	button $base.bok \
		-borderwidth 1 -command {set gpw(result) 1
destroy .gpw} -text Ok 
	button $base.bcanc \
		-borderwidth 1 -command {set gpw(result) 0
destroy .gpw} -text Cancel 
	place $base.l1 \
		-x 10 -y 5 -width 201 -height 53 -anchor nw -bordermode ignore 
	place $base.e1 \
		-x 10 -y 65 -width 200 -height 24 -anchor nw -bordermode ignore 
	place $base.bok \
		-x 225 -y 5 -width 61 -height 26 -anchor nw -bordermode ignore 
	place $base.bcanc \
		-x 225 -y 35 -width 61 -height 26 -anchor nw -bordermode ignore 
}

proc vTclWindow.fdtb {base} {
	if {$base == ""} {
		set base .fdtb
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 90x152+0+0
	wm maxsize $base 785 570
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base "Toolbar"
	radiobutton $base.rb1 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text Point -value point -variable fdvar(tool) \
		-width 9 
	radiobutton $base.rb2 \
		-anchor w -borderwidth 1 \
		-foreground #000000 -highlightthickness 0 \
		-text Label -value label -variable fdvar(tool) -width 9 
	radiobutton $base.rb3 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text Entry -value entry -variable fdvar(tool) \
		-width 9 
	radiobutton $base.rb4 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text Button -value button \
		-variable fdvar(tool) -width 9 
	radiobutton $base.rb5 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text {List box} -value listbox \
		-variable fdvar(tool) -width 9 
	radiobutton $base.rb6 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text {Check box} -value checkbox \
		-variable fdvar(tool) -width 9 
	radiobutton $base.rb7 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text {Radio btn} -value radio \
		-variable fdvar(tool) -width 9 
	radiobutton $base.rb8 \
		-anchor w -borderwidth 1 \
		-highlightthickness 0 -text Query -value query -variable fdvar(tool) \
		-width 9 
	grid $base.rb1 \
		-in .fdtb -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.rb2 \
		-in .fdtb -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.rb3 \
		-in .fdtb -column 0 -row 2 -columnspan 1 -rowspan 1 
	grid $base.rb4 \
		-in .fdtb -column 0 -row 3 -columnspan 1 -rowspan 1 
	grid $base.rb5 \
		-in .fdtb -column 0 -row 4 -columnspan 1 -rowspan 1 
	grid $base.rb6 \
		-in .fdtb -column 0 -row 5 -columnspan 1 -rowspan 1 
	grid $base.rb7 \
		-in .fdtb -column 0 -row 6 -columnspan 1 -rowspan 1 
	grid $base.rb8 \
		-in .fdtb -column 0 -row 7 -columnspan 1 -rowspan 1 
}

proc vTclWindow.sqlw {base} {
	if {$base == ""} {
		set base .sqlw
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 551x408+192+169
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base "SQL commands"
	frame $base.f \
		-borderwidth 1 -height 392 -relief raised -width 396 
	scrollbar $base.f.01 \
		-borderwidth 1 -command {.sqlw.f.t xview} -orient horiz \
		-width 10 
	scrollbar $base.f.02 \
		-borderwidth 1 -command {.sqlw.f.t yview} -orient vert -width 10 
	text $base.f.t \
		-borderwidth 1 \
		-height 200 -width 200 -wrap word \
		-xscrollcommand {.sqlw.f.01 set} \
		-yscrollcommand {.sqlw.f.02 set} 
	button $base.b1 \
		-borderwidth 1 -command {.sqlw.f.t delete 1.0 end} -text Clean 
	button $base.b2 \
		-borderwidth 1 -command {destroy .sqlw} -text Close 
	grid columnconf $base 0 -weight 1
	grid columnconf $base 1 -weight 1
	grid rowconf $base 0 -weight 1
	grid $base.f \
		-in .sqlw -column 0 -row 0 -columnspan 2 -rowspan 1 
	grid columnconf $base.f 0 -weight 1
	grid rowconf $base.f 0 -weight 1
	grid $base.f.01 \
		-in .sqlw.f -column 0 -row 1 -columnspan 1 -rowspan 1 -sticky ew 
	grid $base.f.02 \
		-in .sqlw.f -column 1 -row 0 -columnspan 1 -rowspan 1 -sticky ns 
	grid $base.f.t \
		-in .sqlw.f -column 0 -row 0 -columnspan 1 -rowspan 1 \
		-sticky nesw 
	grid $base.b1 \
		-in .sqlw -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.b2 \
		-in .sqlw -column 1 -row 1 -columnspan 1 -rowspan 1 
}

proc vTclWindow.rcw {base} {
    if {$base == ""} {
        set base .rcw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 215x75+258+213
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Rename field"
    label $base.l1 \
        -borderwidth 0 -text {New name} 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -textvariable tiw(new_cn)
	bind $base.e1 <Key-KP_Enter> "rename_column"
	bind $base.e1 <Key-Return> "rename_column"
    frame $base.f \
        -height 75 -relief groove -width 147 
    button $base.f.b1 \
        -borderwidth 1 -command rename_column -text Rename 
    button $base.f.b2 \
        -borderwidth 1 -command {Window destroy .rcw} -text Cancel 
    label $base.l2 -borderwidth 0 
    grid $base.l1 \
        -in .rcw -column 0 -row 0 -columnspan 1 -rowspan 1 
    grid $base.e1 \
        -in .rcw -column 1 -row 0 -columnspan 1 -rowspan 1 
    grid $base.f \
        -in .rcw -column 0 -row 4 -columnspan 2 -rowspan 1 
    grid $base.f.b1 \
        -in .rcw.f -column 0 -row 0 -columnspan 1 -rowspan 1 
    grid $base.f.b2 \
        -in .rcw.f -column 1 -row 0 -columnspan 1 -rowspan 1 
    grid $base.l2 \
        -in .rcw -column 0 -row 3 -columnspan 1 -rowspan 1 
}

proc vTclWindow.anfw {base} {
    if {$base == ""} {
        set base .anfw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 302x114+195+175
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Add new field"
    label $base.l1 \
        -borderwidth 0 \
        -text {Field name} 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -textvariable anfw(name) 
    bind $base.e1 <Key-KP_Enter> {
        focus .anfw.e2
    }
    bind $base.e1 <Key-Return> {
        focus .anfw.e2
    }
    label $base.l2 \
        -borderwidth 0 \
        -text {Field type} 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -textvariable anfw(type) 
    bind $base.e2 <Key-KP_Enter> {
        anfw:add
    }
    bind $base.e2 <Key-Return> {
        anfw:add
    }
    button $base.b1 \
        -borderwidth 1 -command anfw:add -text {Add field} 
    button $base.b2 \
        -borderwidth 1 -command {Window destroy .anfw} -text Cancel 
    place $base.l1 \
        -x 25 -y 10 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 98 -y 7 -width 178 -height 22 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 25 -y 40 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 98 -y 37 -width 178 -height 22 -anchor nw -bordermode ignore 
    place $base.b1 \
        -x 70 -y 75 -anchor nw -bordermode ignore 
    place $base.b2 \
        -x 160 -y 75 -anchor nw -bordermode ignore 
}

proc vTclWindow.uw {base} {
    if {$base == ""} {
        set base .uw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 263x220+233+165
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Define new user"
    label $base.l1 \
        -borderwidth 0 -anchor w -text "User name"
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -textvariable uw(username) 
	bind $base.e1 <Key-Return> "focus .uw.e2"
	bind $base.e1 <Key-KP_Enter> "focus .uw.e2"
    label $base.l2 \
        -borderwidth 0 -text Password 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -show * -textvariable uw(password) 
	bind $base.e2 <Key-Return> "focus .uw.e3"
	bind $base.e2 <Key-KP_Enter> "focus .uw.e3"
    label $base.l3 \
        -borderwidth 0 -text {verify password} 
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -show * -textvariable uw(verify) 
	bind $base.e3 <Key-Return> "focus .uw.cb1"
	bind $base.e3 <Key-KP_Enter> "focus .uw.cb1"
    checkbutton $base.cb1 \
        -borderwidth 1 -offvalue NOCREATEDB -onvalue CREATEDB \
        -text {Alow user to create databases } -variable uw(createdb) 
    checkbutton $base.cb2 \
        -borderwidth 1 -offvalue NOCREATEUSER -onvalue CREATEUSER \
        -text {Allow users to create other users} -variable uw(createuser) 
    label $base.l4 \
        -borderwidth 0 -anchor w -text {Valid until (date)} 
    entry $base.e4 \
        -background #fefefe -borderwidth 1 -textvariable uw(valid)
	bind $base.e4 <Key-Return> "focus .uw.b1"
	bind $base.e4 <Key-KP_Enter> "focus .uw.b1"
    button $base.b1 \
        -borderwidth 1 -command uw:create_user -text Create 
    button $base.b2 \
        -borderwidth 1 -command {Window destroy .uw} -text Cancel 
    place $base.l1 \
        -x 5 -y 7 -width 62 -height 16 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 109 -y 5 -width 146 -height 20 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 5 -y 35 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 109 -y 32 -width 146 -height 20 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 5 -y 60 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 109 -y 58 -width 146 -height 20 -anchor nw -bordermode ignore 
    place $base.cb1 \
        -x 5 -y 90 -anchor nw -bordermode ignore 
    place $base.cb2 \
        -x 5 -y 115 -anchor nw -bordermode ignore 
    place $base.l4 \
        -x 5 -y 145 -width 100 -height 16 -anchor nw -bordermode ignore 
    place $base.e4 \
        -x 110 -y 143 -width 146 -height 20 -anchor nw -bordermode ignore 
    place $base.b1 \
        -x 45 -y 185 -anchor nw -width 70 -height 25 -bordermode ignore 
    place $base.b2 \
        -x 140 -y 185 -anchor nw -width 70 -height 25 -bordermode ignore 
}

Window show .
Window show .dw

main $argc $argv
