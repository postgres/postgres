#!/usr/bin/wish
#############################################################################
# Visual Tcl v1.11 Project
#

#################################
# GLOBAL VARIABLES
#
global activetab; 
global dbc; 
global dbname; 
global host; 
global mw; 
global newdbname; 
global newhost; 
global newpport; 
global pport; 
global pref; 
global qlvar; 
global sdbname; 
global tablist; 
global widget; 

#################################
# USER DEFINED PROCEDURES
#
proc init {argc argv} {
global dbc host pport tablist mw fldval activetab qlvar
foreach wid {Label Text Button Listbox Checkbutton Radiobutton} {
	option add *$wid.font  -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
}
set host localhost
set pport 5432
set dbc {}
set tablist [list Tables Queries Views Sequences Functions Reports Forms Scripts]
set activetab {}
set mw(dirtyrec) 0
set mw(id_edited) {}
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

init $argc $argv


proc {MsgBox} {mesaj} {
tk_messageBox -title Mesaj -message $mesaj
}

proc {add_new_field} {} {
global fldname fldtype fldsize defaultval notnull
if {$fldname==""} {
    show_error "Enter a field name"
    focus .nt.e2
	return
}
if {$fldtype==""} {
    show_error "The field type is not specified!"
	return
}
if {(($fldtype=="varchar")||($fldtype=="char"))&&($fldsize=="")} {
    focus .nt.e3
    show_error "You must specify field size!"
	return
}
if {$fldsize==""} then {set sup ""} else {set sup "($fldsize)"}
if {[regexp $fldtype "varchar2char4char8char16textdatetime"]} {set supc "'"} else {set supc ""}
if {$defaultval==""} then {set sup2 ""} else {set sup2 " DEFAULT $supc$defaultval$supc"}
# Checking for field name collision
set inspos end
for {set i 0} {$i<[.nt.lb size]} {incr i} {
	set linie [.nt.lb get $i]
	if {$fldname==[lindex [split $linie] 0]} {
		if {[tk_messageBox -title Warning -message "There is another field with the same name!\n\nReplace it ?" -type yesno -default yes]=="no"} return
		.nt.lb delete $i
		set inspos $i
	}	 
  }
.nt.lb insert $inspos [format "%-17s%-14s%-16s" $fldname $fldtype$sup $sup2$notnull]
focus .nt.e2
set fldname {}
set fldsize {}
set defaultval {}
}

proc {cmd_Delete} {} {
global dbc activetab
if {$dbc==""} return;
set objtodelete [get_dwlb_Selection]
if {$objtodelete==""} return;
set temp {}
switch $activetab {
	Tables {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete table:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop table $objtodelete"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Tables
		}
	}
	Views {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete view:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop view $objtodelete"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Views
		}
	}
	Queries {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete query:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_queries where queryname='$objtodelete'"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Queries
		}
	}
	Scripts {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete script:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_scripts where scriptname='$objtodelete'"
			cmd_Scripts
		}
	}
	Forms {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete form:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_forms where formname='$objtodelete'"
			cmd_Forms
		}
	}
	Sequences {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete sequence:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "drop sequence $objtodelete"
			cmd_Sequences
		}
	}
	Functions {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete function:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			delete_function $objtodelete
			cmd_Functions
		}
	}
	Reports {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete report:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "delete from pga_reports where reportname='$objtodelete'"
			cmd_Reports
		}
	}
}
if {$temp==""} return;
}

proc {cmd_Design} {} {
global dbc activetab tablename rbvar
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
}
}

proc {cmd_Forms} {} {
global dbc
cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pga_forms order by formname" rec {
        .dw.lb insert end $rec(formname)
    }
}
cursor_arrow .dw
}

proc {cmd_Functions} {} {
global dbc
set maxim 0
set pgid 0
cursor_watch .dw
catch {
	pg_select $dbc "select proowner,count(*) from pg_proc group by proowner" rec {
		if {$rec(count)>$maxim} {
			set maxim $rec(count)
			set pgid $rec(proowner)
		}
	}
.dw.lb delete 0 end
catch {
	pg_select $dbc "select proname from pg_proc where prolang=14 and proowner<>$pgid order by proname" rec {
		.dw.lb insert end $rec(proname)
	}	
}
cursor_arrow .dw
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
global dbc activetab queryname queryoid cbv funcpar funcname funcret rbvar
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
    pg_select $dbc "select * from pga_queries order by queryname" rec {
        .dw.lb insert end $rec(queryname)
    }
}
}

proc {cmd_Rename} {} {
global dbc oldobjname activetab
if {$dbc==""} return;
if {$activetab=="Views"} return;
if {$activetab=="Sequences"} return;
if {$activetab=="Functions"} return;
set temp [get_dwlb_Selection]
if {$temp==""} {
	tk_messageBox -title Warning -message "Please select an object first !"
	return;
}
set oldobjname $temp
Window show .rf
}

proc {cmd_Reports} {} {
global dbc
cursor_watch .dw
catch {
    pg_select $dbc "select * from pga_reports order by reportname" rec {
	.dw.lb insert end "$rec(reportname)"
    }
}
cursor_arrow .dw
}

proc {cmd_Scripts} {} {
global dbc
cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pga_scripts order by scriptname" rec {
	.dw.lb insert end $rec(scriptname)
    }
}
cursor_arrow .dw
}

proc {cmd_Sequences} {} {
global dbc

cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pg_class where (relname not like 'pg_%') and (relkind='S') order by relname" rec {
        .dw.lb insert end $rec(relname)
    }
}
cursor_arrow .dw
}

proc {cmd_Tables} {} {
global dbc
cursor_watch .dw
.dw.lb delete 0 end
foreach tbl [get_tables] {.dw.lb insert end $tbl}
cursor_arrow .dw
}

proc {cmd_Views} {} {
global dbc

cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pg_class where (relname !~ '^pg_') and (relkind='r') and (relhasrules) order by relname" rec {
        .dw.lb insert end $rec(relname)
    }
}
cursor_arrow .dw
}

proc {create_drop_down} {base x y} {
frame $base.ddf -borderwidth 1 -height 75 -relief raised -width 55 
listbox $base.ddf.lb -background #fefefe -borderwidth 1  -font -Adobe-Helvetica-medium-R-Normal--*-120-*-*-*-*-*-*  -highlightthickness 0 -selectborderwidth 0 -yscrollcommand [subst {$base.ddf.sb set}]
scrollbar $base.ddf.sb -borderwidth 1 -command [subst {$base.ddf.lb yview}] -highlightthickness 0 -orient vert 
place $base.ddf -x $x -y $y -width 220 -height 185 -anchor nw -bordermode ignore 
place $base.ddf.lb -x 1 -y 1 -width 202 -height 182 -anchor nw -bordermode ignore 
place $base.ddf.sb -x 205 -y 1 -width 14 -height 183 -anchor nw -bordermode ignore
}

proc {cursor_arrow} {w} {
$w configure -cursor top_left_arrow
update idletasks
}

proc {cursor_watch} {w} {
$w configure -cursor watch
update idletasks
}

proc {delete_function} {objname} {
global dbc
pg_select $dbc "select * from pg_proc where proname='$objname'" rec {
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
pg_select $dbc "select * from pga_scripts where scriptname='$sname'" rec {
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

proc {drag_start} {w x y} {
global draglocation
catch {unset draglocation}
set object [$w find closest $x $y]
if {[lsearch [.mw.c gettags $object] movable]==-1} return;
.mw.c bind movable <Leave> {}
set draglocation(obj) $object
set draglocation(x) $x
set draglocation(y) $y
set draglocation(start) $x
}

proc {drag_stop} {w x y} {
global draglocation mw dbc
	set dlo ""
	catch { set dlo $draglocation(obj) }
    if {$dlo != ""} {
		.mw.c bind movable <Leave> {.mw configure -cursor top_left_arrow}
		.mw configure -cursor top_left_arrow
        set ctr [get_tag_info $draglocation(obj) v]
        set diff [expr $x-$draglocation(start)]
        if {$diff==0} return;
        set newcw {}
        for {set i 0} {$i<$mw(colcount)} {incr i} {
            if {$i==$ctr} {
                lappend newcw [expr [lindex $mw(colwidth) $i]+$diff]
            } else {
                lappend newcw [lindex $mw(colwidth) $i]
            }
        }
        set mw(colwidth) $newcw
		.mw.c itemconfigure c$ctr -width [expr [lindex $mw(colwidth) $ctr]-5]
        mw_draw_headers
		mw_draw_hgrid
		if {$mw(crtrow)!=""} {mw_show_record $mw(crtrow)}
        for {set i [expr $ctr+1]} {$i<$mw(colcount)} {incr i} {
            .mw.c move c$i $diff 0
        }
		cursor_watch .mw
        sql_exec quiet "update pga_layout set colwidth='$mw(colwidth)' where tablename='$mw(layout_name)'"
		cursor_arrow .mw
    }
}

proc {draw_tabs} {} {
global tablist activetab
set ypos 85
foreach tab $tablist {
    label .dw.tab$tab -borderwidth 1  -anchor w -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief raised -text $tab
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
    pg_select $dbc "select * from pga_scripts where scriptname='$scriptname'" rec {
	set ss $rec(scriptsource)
    }
#    if {[string length $ss] > 0} {
	eval $ss
#    }
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
global fdvar fdobj
set c $fdobj($i,c)
foreach {x1 y1 x2 y2} $c {}
.fd.c delete o$i
switch $fdobj($i,t) {
    button {
        fd_draw_rectangle $x1 $y1 $x2 $y2 raised #a0a0a0 o$i
        .fd.c create text [expr ($x1+$x2)/2] [expr ($y1+$y2)/2] -text $fdobj($i,l) -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags o$i
    }
    entry {
        fd_draw_rectangle $x1 $y1 $x2 $y2 sunken white o$i
    }
    label {
        .fd.c create text $x1 $y1 -text $fdobj($i,l) -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -anchor nw -tags o$i
    }
    checkbox {
        fd_draw_rectangle [expr $x1+2] [expr $y1+5] [expr $x1+12] [expr $y1+15] raised #a0a0a0 o$i
        .fd.c create text [expr $x1+20] [expr $y1+3] -text $fdobj($i,l) -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags o$i
    }
    radio {
        .fd.c create oval [expr $x1+4] [expr $y1+5] [expr $x1+14] [expr $y1+15] -fill white -tags o$i
        .fd.c create text [expr $x1+24] [expr $y1+3] -text $fdobj($i,l) -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags o$i
    }
    query {
        .fd.c create oval $x1 $y1 [expr $x1+20] [expr $y1+20] -fill white -tags o$i
        .fd.c create text [expr $x1+5] [expr $y1+4] -text Q  -anchor nw -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-* -tags o$i
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
set res [pg_exec $dbc "select * from pga_forms where formname='$fdvar(formname)'"]
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
#set fid [open "$name.form" w]
set info [list $fdvar(forminame) $fdvar(objnum) $fdvar(objlist) [wm geometry .fd]]
foreach i $fdvar(objlist) {
    lappend info [list $fdobj($i,t) $fdobj($i,n) $fdobj($i,c) $fdobj($i,x) $fdobj($i,l) $fdobj($i,v)]
}
#puts $fid $info
#close $fid
set res [pg_exec $dbc "delete from pga_forms where formname='$fdvar(formname)'"]
pg_result $res -clear
set res [pg_exec $dbc "insert into pga_forms values ('$fdvar(formname)','$info')"]
pg_result $res -clear
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
global fdvar fdobj dbc datasets
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
        button $base.$name  -borderwidth 1 -padx 0 -pady 0 -text "$fdobj($item,l)" -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -command [subst {$cmd}]
    }
    checkbox {
        checkbutton  $base.$name -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -text "$fdobj($item,l)" -variable "$fdobj($item,v)" -borderwidth 1
        set wh {}
    }
    query { set visual 0
        set procbody "proc $base.$name:execute {} {global dbc datasets ; set datasets($base.$name) \[pg_exec \$dbc \"$fdobj($item,x)\"\] ; set ceva \[$base.$name:fields\]}"
        eval $procbody
#        tk_messageBox -message $procbody
        set procbody "proc $base.$name:nrecords {} {global datasets ; return \[pg_result \$datasets($base.$name) -numTuples\]}"
        eval $procbody
#        tk_messageBox -message $procbody
        set procbody "proc $base.$name:close {} {global datasets ; pg_result \$datasets($base.$name) -clear}"
        eval $procbody
#        tk_messageBox -message $procbody
        set procbody "proc $base.$name:fields {} {global datasets ; set fl {} ; foreach fd \[pg_result \$datasets($base.$name) -lAttributes\] {lappend fl \[lindex \$fd 0\]} ; set datasets($base.$name,fields) \$fl ; return \$fl}"
#        tk_messageBox -message $procbody
        eval $procbody
        eval "proc $base.$name:movefirst {} {global datasets ; set datasets($base.$name,recno) 0}"
        eval "proc $base.$name:movenext {} {global datasets ; incr datasets($base.$name,recno)}"
        eval "proc $base.$name:moveprevious {} {global datasets ; incr datasets($base.$name,recno) -1 ; if {\$datasets($base.$name,recno)==-1} {$base.$name:movefirst}}"
        eval "proc $base.$name:movelast {} {global datasets ; set datasets($base.$name,recno) \[expr \[$base.$name:nrecords\] -1\]}"
        eval "proc $base.$name:updatecontrols {} {global datasets ; set i 0 ; foreach fld \$datasets($base.$name,fields) {catch {upvar $base.$name.\$fld dbvar ; set dbvar \[lindex \[pg_result \$datasets($base.$name) -getTuple \$datasets($base.$name,recno)\] \$i\]} ; incr i}}"
    }
    radio {
        radiobutton  $base.$name -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -text "$fdobj($item,l)" -variable "$fdobj($item,v)" -borderwidth 1
        set wh {}
    }
    entry {
        set var {} ; catch {set var $fdobj($item,v)}
        entry $base.$name -bo 1 -ba white -selectborderwidth 0  -highlightthickness 0 
        if {$var!=""} {$base.$name configure -textvar $var}
    }
    label {set wh {} ; label $base.$name -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -anchor nw -padx 0 -pady 0 -text $fdobj($item,l)}
    listbox {listbox $base.$name -borderwidth 1 -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*}
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
pg_select $dbc "select typname from pg_type where oid=$oid" rec {
	set temp $rec(typname)
}
return $temp
}

proc {get_tables} {} {
global dbc
set tbl {}
catch {
    pg_select $dbc "select * from pg_class where (relname !~ '^pg_') and (relkind='r') and (not relhasrules) order by relname" rec {
        if {![regexp "^pga_" $rec(relname)]} then {lappend tbl $rec(relname)}
    }
}
return $tbl
}

proc {get_tag_info} {itemid prefix} {
set taglist [.mw.c itemcget $itemid -tags]
set i [lsearch -glob $taglist $prefix*]
set thetag [lindex $taglist $i]
return [string range $thetag 1 end]
}

proc {load_pref} {} {
global pref
set retval [catch {set fid [open "~/.pgaccessrc" r]}]
if {$retval} {
    set pref(rows) 200
    set pref(tvfont) clean
    set pref(autoload) 1
    set pref(lastdb) {}
    set pref(lasthost) localhost
    set pref(lastport) 5432
} else {
    while {![eof $fid]} {
        set pair [gets $fid]
        set pref([lindex $pair 0]) [lindex $pair 1]
    }
    close $fid
}
}




proc {mw_canvas_click} {x y} {
global mw msg
if {![mw_exit_edit]} return
# Determining row
for {set row 0} {$row<$mw(nrecs)} {incr row} {
	if {[lindex $mw(rowy) $row]>$y} break
}
incr row -1
if {$y>[lindex $mw(rowy) $mw(last_rownum)]} {set row $mw(last_rownum)}
if {$row<0} return
set mw(row_edited) $row
set mw(crtrow) $row
mw_show_record $row
if {$mw(errorsavingnew)} return
# Determining column
set posx [expr -$mw(leftoffset)]
set col 0
foreach cw $mw(colwidth) {
    incr posx [expr $cw+2]
    if {$x<$posx} break
    incr col
}
set itlist [.mw.c find withtag r$row]
foreach item $itlist {
    if {[get_tag_info $item c]==$col} {
        mw_start_edit $item $x $y
        break
    }
}
}

proc {mw_delete_record} {} {
global dbc mw tablename
if {!$mw(updatable)} return;
if {![mw_exit_edit]} return;
set taglist [.mw.c gettags hili]
if {[llength $taglist]==0} return;
set rowtag [lindex $taglist [lsearch -regexp $taglist "^r"]]
set row [string range $rowtag 1 end]
set oid [lindex $mw(keylist) $row]
if {[tk_messageBox -title "FINAL WARNING" -icon question -message "Delete current record ?" -type yesno -default no]=="no"} return
if {[sql_exec noquiet "delete from $tablename where oid=$oid"]} {
	.mw.c delete hili
}
}

proc {mw_draw_headers} {} {
global mw
.mw.c delete header
set posx [expr 5-$mw(leftoffset)]
for {set i 0} {$i<$mw(colcount)} {incr i} {
    set xf [expr $posx+[lindex $mw(colwidth) $i]]
    .mw.c create rectangle $posx 1 $xf 22 -fill #CCCCCC -outline "" -width 0 -tags header
    .mw.c create text [expr $posx+[lindex $mw(colwidth) $i]*1.0/2] 14 -text [lindex $mw(colnames) $i] -tags header -fill navy -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .mw.c create line $posx 22 [expr $xf-1] 22 -fill #AAAAAA -tags header
    .mw.c create line [expr $xf-1] 5 [expr $xf-1] 22 -fill #AAAAAA -tags header
    .mw.c create line [expr $xf+1] 5 [expr $xf+1] 22 -fill white -tags header
    .mw.c create line $xf -15000 $xf 15000 -fill #CCCCCC -tags [subst {header movable v$i}]
    set posx [expr $xf+2]
}
set mw(r_edge) $posx
.mw.c bind movable <Button-1> {drag_start %W %x %y}
.mw.c bind movable <B1-Motion> {drag_it %W %x %y}
.mw.c bind movable <ButtonRelease-1> {drag_stop %W %x %y}
.mw.c bind movable <Enter> {.mw configure -cursor left_side}
.mw.c bind movable <Leave> {.mw configure -cursor top_left_arrow}
}

proc {mw_draw_hgrid} {} {
global mw
.mw.c delete hgrid
set posx 10
for {set j 0} {$j<$mw(colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr [lindex $mw(colwidth) $j]+2]
	set textwidth($j) [expr [lindex $mw(colwidth) $j]-5]
}
incr posx -6
for {set i 0} {$i<$mw(nrecs)} {incr i} {
	.mw.c create line [expr -$mw(leftoffset)] [lindex $mw(rowy) [expr $i+1]] [expr $posx-$mw(leftoffset)] [lindex $mw(rowy) [expr $i+1]] -fill gray -tags [subst {hgrid g$i}]
}
if {$mw(updatable)} {
	set i $mw(nrecs)
	set posy [expr 14+[lindex $mw(rowy) $mw(nrecs)]]
	.mw.c create line [expr -$mw(leftoffset)] $posy [expr $posx-$mw(leftoffset)] $posy -fill gray -tags [subst {hgrid g$i}]
}
}

proc {mw_draw_new_record} {} {
global mw pref msg
set posx 10
set posy [lindex $mw(rowy) $mw(last_rownum)]
if {$pref(tvfont)=="helv"} {
    set tvfont -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
} else {
    set tvfont -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
}
if {$mw(updatable)} {
  for {set j 0} {$j<$mw(colcount)} {incr j} {
    .mw.c create text $posx $posy -text * -tags [subst {r$mw(nrecs) c$j q new unt}]  -anchor nw -font $tvfont -width [expr [lindex $mw(colwidth) $j]-5]
    incr posx [expr [lindex $mw(colwidth) $j]+2]
  }
  incr posy 14
  .mw.c create line [expr -$mw(leftoffset)] $posy [expr $mw(r_edge)-$mw(leftoffset)] $posy -fill gray -tags [subst {hgrid g$mw(nrecs)}]
}
}

proc {mw_edit_text} {c k} {
global mw msg
set bbin [.mw.c bbox r$mw(row_edited)]
switch $k {
    BackSpace { set dp [expr [.mw.c index $mw(id_edited) insert]-1];if {$dp>=0} {.mw.c dchars $mw(id_edited) $dp $dp; set mw(dirtyrec) 1}}
    Home {.mw.c icursor $mw(id_edited) 0}
    End {.mw.c icursor $mw(id_edited) end}
    Left {.mw.c icursor $mw(id_edited) [expr [.mw.c index $mw(id_edited) insert]-1]}
    Delete {}
    Right {.mw.c icursor $mw(id_edited) [expr [.mw.c index $mw(id_edited) insert]+1]}
    Return {if {[mw_exit_edit]} {.mw.c focus {}}}
    Escape {set mw(dirtyrec) 0; .mw.c itemconfigure $mw(id_edited) -text $mw(text_initial_value); .mw.c focus {}}
    default {if {[string compare $c " "]>-1} {.mw.c insert $mw(id_edited) insert $c;set mw(dirtyrec) 1}}
}
set bbout [.mw.c bbox r$mw(row_edited)]
set dy [expr [lindex $bbout 3]-[lindex $bbin 3]]
if {$dy==0} return
set re $mw(row_edited)
.mw.c move g$re 0 $dy
for {set i [expr 1+$re]} {$i<=$mw(nrecs)} {incr i} {
	.mw.c move r$i 0 $dy
	.mw.c move g$i 0 $dy
	set rh [lindex $mw(rowy) $i]
	incr rh $dy
	set mw(rowy) [lreplace $mw(rowy) $i $i $rh]
}
mw_show_record $mw(row_edited)
# Delete is trapped by window interpreted as record delete
#    Delete {.mw.c dchars $mw(id_edited) insert insert; set mw(dirtyrec) 1}
}

proc {mw_exit_edit} {} {
global mw dbc msg tablename
# User has edited the text ?
if {!$mw(dirtyrec)} {
	# No, unfocus text
    .mw.c focus {}
    # For restoring * to the new record position
	if {$mw(id_edited)!=""} {
	    if {[lsearch [.mw.c gettags $mw(id_edited)] new]!=-1} {
	        .mw.c itemconfigure $mw(id_edited) -text $mw(text_initial_value)
	    }
	}
    set mw(id_edited) {};set mw(text_initial_value) {}
    return 1
}
# Trimming the spaces
set fldval [string trim [.mw.c itemcget $mw(id_edited) -text]]
.mw.c itemconfigure $mw(id_edited) -text $fldval
if {[string compare $mw(text_initial_value) $fldval]==0} {
    set mw(dirtyrec) 0
    .mw.c focus {}
    set mw(id_edited) {};set mw(text_initial_value) {}
    return 1
}
cursor_watch .mw
set oid [lindex $mw(keylist) $mw(row_edited)]
set fld [lindex $mw(colnames) [get_tag_info $mw(id_edited) c]]
set fillcolor black
if {$mw(row_edited)==$mw(last_rownum)} {
	set fillcolor red
	set sfp [lsearch $mw(newrec_fields) $fld]
	if {$sfp>-1} {
		set mw(newrec_fields) [lreplace $mw(newrec_fields) $sfp $sfp]
		set mw(newrec_values) [lreplace $mw(newrec_values) $sfp $sfp]
	}			
	lappend mw(newrec_fields) $fld
	lappend mw(newrec_values) '$fldval'
	# Remove the untouched tag from the object
	.mw.c dtag $mw(id_edited) unt
        .mw.c itemconfigure $mw(id_edited) -fill red
	set retval 1
} else {
    set msg "Updating record ..."
    after 1000 {set msg ""}
    regsub -all ' $fldval  \\' sqlfldval
    set retval [sql_exec noquiet "update $tablename set $fld='$sqlfldval' where oid=$oid"]
}
cursor_arrow .mw
if {!$retval} {
    set msg ""
	focus .mw.c
    return 0
}
set mw(dirtyrec) 0
.mw.c focus {}
set mw(id_edited) {};set mw(text_initial_value) {}
return 1
}

proc {mw_load_layout} {tablename} {
global dbc msg mw
cursor_watch .mw
set mw(layout_name) $tablename
catch {unset mw(colcount) mw(colnames) mw(colwidth)}
set mw(layout_found) 0
set retval [catch {set pgres [pg_exec $dbc "select *,oid from pga_layout where tablename='$tablename' order by oid desc"]}]
if {$retval} {
    # Probably table pga_layout isn't yet defined
    sql_exec noquiet "create table pga_layout (tablename varchar(64),nrcols int2,colnames text,colwidth text)"
	sql_exec quiet "grant ALL on pga_layout to PUBLIC"
} else {
	set nrlay [pg_result $pgres -numTuples]
    if {$nrlay>=1} {
        set layoutinfo [pg_result $pgres -getTuple 0]
        set mw(colcount) [lindex $layoutinfo 1]
        set mw(colnames)  [lindex $layoutinfo 2]
        set mw(colwidth) [lindex $layoutinfo 3]
		set goodoid [lindex $layoutinfo 4]
        set mw(layout_found) 1
    }
    if {$nrlay>1} {
		show_error "Multiple ([pg_result $pgres -numTuples]) layout info found\n\nPlease report the bug!"
		sql_exec quiet "delete from pga_layout where (tablename='$tablename') and (oid<>$goodoid)"
    }
}
catch {pg_result $pgres -clear}
}

proc {mw_pan_left} {} {
global mw
if {![mw_exit_edit]} return;
if {$mw(leftcol)==[expr $mw(colcount)-1]} return;
set diff [expr 2+[lindex $mw(colwidth) $mw(leftcol)]]
incr mw(leftcol)
incr mw(leftoffset) $diff
.mw.c move header -$diff 0
.mw.c move q -$diff 0
.mw.c move hgrid -$diff 0
}

proc {mw_pan_right} {} {
global mw
if {![mw_exit_edit]} return;
if {$mw(leftcol)==0} return;
incr mw(leftcol) -1
set diff [expr 2+[lindex $mw(colwidth) $mw(leftcol)]]
incr mw(leftoffset) -$diff
.mw.c move header $diff 0
.mw.c move q $diff 0
.mw.c move hgrid $diff 0
}

proc {mw_save_new_record} {} {
global dbc mw tablename msg
if {![mw_exit_edit]} {return 0}
if {$mw(newrec_fields)==""} {return 1}
set msg "Saving new record ..."
after 1000 {set msg ""}
set retval [catch {
	set sqlcmd "insert into $tablename ([join $mw(newrec_fields) ,]) values ([join $mw(newrec_values) ,])"
	set pgres [pg_exec $dbc $sqlcmd]
	} errmsg]
if {$retval} {
	show_error "Error inserting new record\n\n$errmsg"
	return 0
}
set oid [pg_result $pgres -oid]
lappend mw(keylist) $oid
pg_result $pgres -clear
# Get bounds of the last record
set lrbb [.mw.c bbox new]
lappend mw(rowy) [lindex $lrbb 3]
.mw.c itemconfigure new -fill black
.mw.c dtag q new
# Replace * from untouched new row elements with "  "
foreach item [.mw.c find withtag unt] {
	.mw.c itemconfigure $item -text "  "
}
.mw.c dtag q unt
incr mw(last_rownum)
incr mw(nrecs)
mw_draw_new_record
set mw(newrec_fields) {}
set mw(newrec_values) {}
return 1
}

proc {mw_scroll_window} {par1 par2 args} {
global mw
if {![mw_exit_edit]} return;
if {$par1=="scroll"} {
    set newtop $mw(toprec)
    if {[lindex $args 0]=="units"} {
        incr newtop $par2
    } else {
        incr newtop [expr $par2*25]
        if {$newtop<0} {set newtop 0}
        if {$newtop>=[expr $mw(nrecs)-1]} {set newtop [expr $mw(nrecs)-1]}
    }
} else {
    set newtop [expr int($par2*$mw(nrecs))]
}
if {$newtop<0} return;
if {$newtop>=[expr $mw(nrecs)-1]} return;
set dy [expr [lindex $mw(rowy) $mw(toprec)]-[lindex $mw(rowy) $newtop]]
.mw.c move q 0 $dy
.mw.c move hgrid 0 $dy
set newrowy {}
foreach y $mw(rowy) {lappend newrowy [expr $y+$dy]}
set mw(rowy) $newrowy
set mw(toprec) $newtop
mw_set_scrollbar
}

proc {mw_select_records} {sql} {
global dbc field mw
global tablename msg pref
set mw(newrec_fields) {}
set mw(newrec_values) {}
if {![mw_exit_edit]} return;
.mw.c delete q
.mw.c delete header
.mw.c delete hgrid
.mw.c delete new
set mw(leftcol) 0
set mw(leftoffset) 0
set mw(crtrow) {}
set msg {}
set msg "Accessing data. Please wait ..."
cursor_watch .mw
set retval [catch {set pgres [pg_exec $dbc "BEGIN"]} errmsg]
if {!$retval} {
	pg_result $pgres -clear
	set retval [catch {set pgres [pg_exec $dbc "declare mycursor cursor for $sql"]} errmsg]
	if {!$retval} {
		pg_result $pgres -clear
		set retval [catch {set pgres [pg_exec $dbc "fetch $pref(rows) in mycursor"]} errmsg]
	}
}
#set retval [catch {set pgres [pg_exec $dbc $sql]} errmsg]
if {$retval} {
	sql_exec quiet "END"
	set msg {}
    cursor_arrow .mw
    show_error "Error executing SQL command\n\n$sql\n\nError message:$errmsg"
    set msg "Error executing : $sql"
    return
}
if {$mw(updatable)} then {set shift 1} else {set shift 0}
#
# checking at least the numer of fields
set attrlist [pg_result $pgres -lAttributes]
if {$mw(layout_found)} then {
    if {  ($mw(colcount) != [expr [llength $attrlist]-$shift]) ||
          ($mw(colcount) != [llength $mw(colnames)]) ||
          ($mw(colcount) != [llength $mw(colwidth)]) } then {
        # No. of columns don't match, something is wrong
		# tk_messageBox -title Information -message "Layout info changed !\nRescanning..."
        set mw(layout_found) 0
        sql_exec quiet "delete from pga_layout where tablename='$mw(layout_name)'"
    }
}
# Always take the col. names from the result
set mw(colcount) [llength $attrlist]
if {$mw(updatable)} then {incr mw(colcount) -1}
set mw(colnames) {}
# In defmw(colwidth) prepare mw(colwidth) (in case that not layout_found)
set defmw(colwidth) {}
for {set i 0} {$i<$mw(colcount)} {incr i} {
    lappend mw(colnames) [lindex [lindex $attrlist [expr $i+$shift]] 0]
    lappend defmw(colwidth) 150
}
if {!$mw(layout_found)} {
    set mw(colwidth) $defmw(colwidth)
    sql_exec quiet "insert into pga_layout values ('$mw(layout_name)',$mw(colcount),'$mw(colnames)','$mw(colwidth)')"
    set mw(layout_found) 1
}
set mw(nrecs) [pg_result $pgres -numTuples]
if {$mw(nrecs)>$pref(rows)} {
	set msg "Only first $pref(rows) records from $mw(nrecs) have been loaded"
	set mw(nrecs) $pref(rows)
}
set tagoid {}
if {$pref(tvfont)=="helv"} {
    set tvfont -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
} else {
    set tvfont -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
}
# Computing column's left edge
set posx 10
for {set j 0} {$j<$mw(colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr [lindex $mw(colwidth) $j]+2]
	set textwidth($j) [expr [lindex $mw(colwidth) $j]-5]
}
incr posx -6
set posy 24
mw_draw_headers
set mw(updatekey) oid
set mw(keylist) {}
set mw(rowy) {24}
set msg "Loading maximum $pref(rows) records ..."
for {set i 0} {$i<$mw(nrecs)} {incr i} {
    set curtup [pg_result $pgres -getTuple $i]
    if {$mw(updatable)} then {lappend mw(keylist) [lindex $curtup 0]}
    for {set j 0} {$j<$mw(colcount)} {incr j} {
        .mw.c create text $ledge($j) $posy -text [lindex $curtup [expr $j+$shift]] -tags [subst {r$i c$j q}] -anchor nw -font $tvfont -width $textwidth($j) -fill black
    }
    set bb [.mw.c bbox r$i]
    incr posy [expr [lindex $bb 3]-[lindex $bb 1]]
    lappend mw(rowy) $posy
    .mw.c create line 0 [lindex $bb 3] $posx [lindex $bb 3] -fill gray -tags [subst {hgrid g$i}]
    if {$i==25} {update; update idletasks}
}
after 3000 {set msg {} }
set mw(last_rownum) $i
# Defining position for input data
mw_draw_new_record
pg_result $pgres -clear
sql_exec quiet "END"
set mw(toprec) 0
mw_set_scrollbar
if {$mw(updatable)} then {
    .mw.c bind q <Key> {mw_edit_text %A %K}
} else {
	.mw.c bind q <Key> {}
}
set mw(dirtyrec) 0
#mw_draw_headers
.mw.c raise header
cursor_arrow .mw
}

proc {mw_set_scrollbar} {} {
global mw
if {$mw(nrecs)==0} return;
.mw.sb set [expr $mw(toprec)*1.0/$mw(nrecs)] [expr ($mw(toprec)+27.0)/$mw(nrecs)]
}

proc {mw_show_record} {row} {
global mw msg
set mw(errorsavingnew) 0
if {$mw(newrec_fields)!=""} {
	if {$row!=$mw(last_rownum)} {
		if {![mw_save_new_record]} {
                    set mw(errorsavingnew) 1
                    return
                }
	}
}
set y1 [lindex $mw(rowy) $row]
set y2 [lindex $mw(rowy) [expr $row+1]]
if {$y2==""} {set y2 [expr $y1+14]}
.mw.c dtag hili hili
.mw.c addtag hili withtag r$row
# Making a rectangle arround the record
set x 3
foreach wi $mw(colwidth) {incr x [expr $wi+2]}
.mw.c delete crtrec
.mw.c create rectangle [expr -1-$mw(leftoffset)] $y1 [expr $x-$mw(leftoffset)] $y2 -fill #EEEEEE -outline {} -tags {q crtrec}
.mw.c lower crtrec
}

proc {mw_start_edit} {id x y} {
global mw msg
if {!$mw(updatable)} return
set mw(id_edited) $id
set mw(dirtyrec) 0
set mw(text_initial_value) [.mw.c itemcget $id -text]
focus .mw.c
.mw.c focus $id
.mw.c icursor $id @$x,$y
if {$mw(row_edited)==$mw(nrecs)} {
    if {[.mw.c itemcget $id -text]=="*"} {
        .mw.c itemconfigure $id -text ""
        .mw.c icursor $id 0
    }
}
}

proc {open_database} {} {
global dbc host pport dbname sdbname newdbname newhost newpport pref
catch {cursor_watch .dbod}
if {[catch {set newdbc [pg_connect $newdbname -host $newhost -port $newpport]} msg]} {
    catch {cursor_arrow .dbod}
    show_error "Error connecting database\n$msg"
} else {
    catch {pg_disconnect $dbc}
    set dbc $newdbc
    set host $newhost
    set pport $newpport
    set dbname $newdbname
    set sdbname $dbname
    set pref(lastdb) $dbname
    set pref(lasthost) $host
    set pref(lastport) $pport
    save_pref
    catch {cursor_arrow .dbod; Window hide .dbod}
    tab_click .dw.tabTables
    # Check for pga_ tables
    foreach {table structure} { pga_queries {queryname varchar(64),querytype char(1),querycommand text} pga_forms {formname varchar(64),formsource text} pga_scripts {scriptname varchar(64),scriptsource text} pga_reports {reportname varchar(64),reportsource text,reportbody text,reportprocs text,reportoptions text}} {
        set pgres [pg_exec $dbc "select relname from pg_class where relname='$table'"]
        if {[pg_result $pgres -numTuples]==0} {
            pg_result $pgres -clear
            sql_exec quiet "create table $table ($structure)"
	    sql_exec quiet "grant ALL on $table to PUBLIC"
        }
        catch { pg_result $pgres -clear }
    }
    # searching for autoexec script
    pg_select $dbc "select * from pga_scripts where scriptname ~* '^autoexec$'" recd {
	eval $recd(scriptsource)
    }    
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
pg_select $dbc "select * from pg_proc where proname='$objname'" rec {
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
global dbc queryname mw queryoid sortfield filter

if {[.dw.lb curselection]==""} return;
set queryname [.dw.lb get [.dw.lb curselection]]
if {[catch {set pgres [pg_exec $dbc "select querycommand,querytype,oid from pga_queries where queryname='$queryname'"]}]} then {
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
        Window show .mw
		wm title .mw "Query result: $queryname"
        mw_load_layout $queryname
        set mw(query) $qcmd
        set mw(updatable) 0
        set mw(isaquery) 1
        mw_select_records $qcmd
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

proc {open_sequence} {objname} {
global dbc seq_name seq_inc seq_start seq_minval seq_maxval
Window show .sqf
set flag 1
pg_select $dbc "select * from $objname" rec {
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
global mw sortfield filter tablename
Window show .mw
set tablename $objname
mw_load_layout $objname
set mw(query) "select oid,$tablename.* from $objname"
set mw(updatable) 1
set mw(isaquery) 0
mw_select_records $mw(query)
wm title .mw "Table viewer : $objname"
}

proc {open_view} {} {
global mw
set vn [get_dwlb_Selection]
if {$vn==""} return;
Window show .mw
set mw(query) "select * from $vn"
set mw(isaquery) 0
set mw(updatable) 0
mw_load_layout $vn
mw_select_records $mw(query)
}

proc {ql_add_new_table} {} {
global qlvar dbc

if {$qlvar(newtablename)==""} return
set fldlist {}
cursor_watch .ql
pg_select $dbc "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$qlvar(newtablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
        lappend fldlist $rec(attname)
}
cursor_arrow .ql
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
    if {$thename!=""} {lappend tables "$qlvar(tablename$i) $qlvar(tablealias$i)"}
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
    if {[tk_messageBox -title WARNING -icon question -message "Remove link ?" -type yesno -default no]=="no"} return
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
    if {[tk_messageBox -title WARNING -icon question -message "Remove field from result ?" -type yesno -default no]=="no"} return
    set qlvar(resfields) [lreplace $qlvar(resfields) $col $col]
    set qlvar(restables) [lreplace $qlvar(restables) $col $col]
    set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $col $col]
    ql_draw_res_panel
    return
}
# Is object a table ?
set tablealias [ql_get_tag_info $obj tab]
set tablename $qlvar(ali_$tablealias)
if {$tablename==""} return
if {[tk_messageBox -title WARNING -icon question -message "Remove table $tablename from query ?" -type yesno -default no]=="no"} return
for {set i [expr [llength $qlvar(restables)]-1]} {$i>=0} {incr i -1} {
    if {$tablename==[lindex $qlvar(restables) $i]} {
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
.ql configure -cursor top_left_arrow
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
global qlvar
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
.ql.c create text 5 [expr 1+$qlvar(yoffs)] -text Field: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c create text 5 [expr 16+$qlvar(yoffs)] -text Table: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c create text 5 [expr 31+$qlvar(yoffs)] -text Sort: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c create text 5 [expr 46+$qlvar(yoffs)] -text Criteria: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c bind mov <Button-1> {ql_dragstart %W %x %y}
.ql.c bind mov <B1-Motion> {ql_dragit %W %x %y}
bind .ql <ButtonRelease-1> {ql_dragstop %x %y}
bind .ql <Button-1> {qlc_click %x %y %W}
bind .ql <B1-Motion> {ql_pan %x %y}
bind .ql <Key-Delete> {ql_delete_object}
}

proc {ql_draw_res_panel} {} {
global qlvar
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
.ql.c delete resp
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 1+$qlvar(yoffs)] -text [lindex $qlvar(resfields) $i] -anchor nw -tags [subst {resf resp col$i}] -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 16+$qlvar(yoffs)] -text $qlvar(ali_[lindex $qlvar(restables) $i]) -anchor nw -tags {resp rest} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 31+$qlvar(yoffs)] -text [lindex $qlvar(ressort) $i] -anchor nw -tags {resp sort} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    if {[lindex $qlvar(rescriteria) $i]!=""} {
        .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)]  [expr $qlvar(yoffs)+46+15*0] -anchor nw -text [lindex $qlvar(rescriteria) $i]  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -tags [subst {resp cr-c$i-r0}]
    }
}
.ql.c raise reshdr
.ql.c bind resf <Button-1> {ql_resfield_click %x %y}
.ql.c bind sort <Button-1> {ql_swap_sort %W %x %y}
}

proc {ql_draw_table} {it} {
global qlvar

set posy 10
set allbox [.ql.c bbox rect]
if {$allbox==""} {set posx 10} else {set posx [expr 20+[lindex $allbox 2]]}
set tablename $qlvar(tablename$it)
set tablealias $qlvar(tablealias$it)
.ql.c create text $posx $posy -text $tablename -anchor nw -tags [subst {tab$tablealias f-oid mov tableheader}] -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
incr posy 16
foreach fld $qlvar(tablestruct$it) {
   .ql.c create text $posx $posy -text $fld -fill #010101 -anchor nw -tags [subst {f-$fld tab$tablealias mov}] -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
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
global qlvar

set sqlcmd [ql_compute_sql]
.ql.c delete sqlpage
.ql.c create rectangle 0 0 2000 [expr $qlvar(yoffs)-1] -fill #ffffff -tags {sqlpage}
.ql.c create text 10 10 -text $sqlcmd -anchor nw -width 550 -tags {sqlpage} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
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
global qlvar
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
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$qlvar(critcol)*$qlvar(reswidth)] [expr $qlvar(yoffs)+46+15*$qlvar(critrow)] -anchor nw -text $qlvar(critval) -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags [subst {resp cr-c$qlvar(critcol)-r$qlvar(critrow)}]
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
entry .ql.entc -textvar qlvar(critval) -borderwidth 0 -background #FFFFFF -highlightthickness 0 -selectborderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
place .ql.entc -x $nx -y $ny -height 14
focus .ql.entc
bind .ql.entc <Button-1> {set qlvar(panstarted) 0}
set qlvar(critcol) $col
set qlvar(critrow) 0
set qlvar(critedit) 1
}

proc {rb_add_field} {} {
global rbvar
set fldname [.rb.lb get [.rb.lb curselection]]
set newid [.rb.c create text $rbvar(xf_auto) [expr $rbvar(y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font -Adobe-Helvetica-Bold-R-Normal--*-120-*-*-*-*-*-*]
.rb.c create text $rbvar(xf_auto) [expr $rbvar(y_pghdr)+5] -text $fldname -tags [subst {f-$fldname t_f rg_detail mov ro}] -anchor nw -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*
set bb [.rb.c bbox $newid]
incr rbvar(xf_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}

proc {rb_add_label} {} {
global rbvar
set fldname $rbvar(labeltext)
set newid [.rb.c create text $rbvar(xl_auto) [expr $rbvar(y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*]
set bb [.rb.c bbox $newid]
incr rbvar(xl_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}

proc {rb_change_object_font} {} {
global rbvar
.rb.c itemconfigure hili -font -Adobe-[.rb.bfont cget -text]-[rb_get_bold]-[rb_get_italic]-Normal--*-$rbvar(pointsize)-*-*-*-*-*-*
}

proc {rb_delete_object} {} {
if {[tk_messageBox -title Warning -message "Delete current report object?" -type yesno -default no]=="no"} return;
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
.rb configure -cursor top_left_arrow
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
#cursor_watch .ql
pg_select $dbc "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$rbvar(tablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
    .rb.lb insert end $rec(attname)
}
#cursor_arrow .ql
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
pg_select $dbc "select * from pga_reports where reportname='$rbvar(reportname)'" rcd {
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
#msgbox $fields
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
pg_select $dbc "select * from $rbvar(tablename)" rec {
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
tk_messageBox -title Information -message "The printed image in Postscript is in the file pgaccess-report.ps"
}

proc {rb_save_report} {} {
global rbvar
set prog "set rbvar(tablename) $rbvar(tablename)"
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
    foreach {opt val} [array get pref] { puts $fid "$opt $val" }
    close $fid
}
}

proc {show_error} {emsg} {
tk_messageBox -title Error -icon error -message $emsg
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
pg_select $dbc "select attnum,attname,typname,attlen,usename,pg_class.oid from pg_class,pg_user,pg_attribute,pg_type where (pg_class.relname='$tiw(tablename)') and (pg_class.oid=pg_attribute.attrelid) and (pg_class.relowner=pg_user.usesysid) and (pg_attribute.atttypid=pg_type.oid) order by attnum" rec {
    set fsize $rec(attlen)
    set ftype $rec(typname)
    if {$ftype=="varchar"} {
        incr fsize -4
    }
    if {$ftype=="bpchar"} {
    	incr fsize -4
    }
    if {$ftype=="text"} {
        set fsize ""
    }
    if {$rec(attnum)>0} {.tiw.lb insert end [format "%-33s %-14s %-4s" $rec(attname) $ftype $fsize]}
    set tiw(owner) $rec(usename)
    set tiw(tableoid) $rec(oid)
    set tiw(f$rec(attnum)) $rec(attname)
}
set tiw(indexlist) {}
pg_select $dbc "select oid,indexrelid from pg_index where (pg_class.relname='$tiw(tablename)') and (pg_class.oid=pg_index.indrelid)" rec {
    lappend tiw(indexlist) $rec(oid)
    pg_select $dbc "select relname from pg_class where oid=$rec(indexrelid)" rec1 {
        .tiw.ilb insert end $rec1(relname)
    }
}
}

proc {sql_exec} {how cmd} {
global dbc
set retval [catch {set pgr [pg_exec $dbc $cmd]} errmsg]
if { $retval } {
    if {$how != "quiet"} {
        show_error "Error executing query\n\n$cmd\n\nPostgreSQL error message:\n$errmsg"
    }
    return 0
}
pg_result $pgr -clear
return 1
}

proc {tab_click} {w} {
global dbc tablist activetab
if {$dbc==""} return;
set curtab [$w cget -text]
#if {$activetab==$curtab} return;
.dw.btndesign configure -state disabled
if {$activetab!=""} {
    place .dw.tab$activetab -x 10
    .dw.tab$activetab configure -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
}
$w configure -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
place $w -x 7
place .dw.lmask -x 80 -y [expr 86+25*[lsearch -exact $tablist $curtab]]
set activetab $curtab
# Tabs where button Design is enabled
if {[lsearch {Scripts Queries Reports Forms} $activetab]!=-1} {
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
pg_select $dbc "select pg_index.*,pg_class.oid from pg_index,pg_class where pg_class.relname='$idxname' and pg_class.oid=pg_index.indexrelid" rec {
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
#            pg_select $dbc "select attname from pg_attribute where attrelid=$tiw(tableoid) and attnum=$field" rec1 {
#                set tiw(indexfields) "$tiw(indexfields) $rec1(attname)"
#            }
        set tiw(indexfields) "$tiw(indexfields) $tiw(f$field)"
        }

    }
}
set tiw(indexfields) [string trim $tiw(indexfields)]
}

proc {vacuum} {} {
global dbc dbname sdbname

if {$dbc==""} return;
cursor_watch .dw
set sdbname "vacuuming database $dbname ..."
update; update idletasks
set retval [catch {
        set pgres [pg_exec $dbc "vacuum;"]
        pg_result $pgres -clear
    } msg]
cursor_arrow .dw
set sdbname $dbname
if {$retval} {
    show_error $msg
}
}

proc {main} {argc argv} {
global pref newdbname newpport newhost dbc
load libpgtcl.so
catch {draw_tabs}
load_pref
if {$pref(autoload) && ($pref(lastdb)!="")} {
    set newdbname $pref(lastdb)
    set newhost $pref(lasthost)
    set newpport $pref(lastport)
    open_database
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

#################################
# VTCL GENERATED GUI PROCEDURES
#

proc vTclWindow. {base} {
    if {$base == ""} {
        set base .
    }
    ###################
    # CREATING WIDGETS
    ###################
    wm focusmodel $base passive
    wm geometry $base 1x1+0+0
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm withdraw $base
    wm title $base "vt.tcl"
    ###################
    # SETTING GEOMETRY
    ###################
}

proc vTclWindow.about {base} {
    if {$base == ""} {
        set base .about
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 471x177+168+243
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "About"
    label $base.l1  -borderwidth 3 -font -Adobe-Helvetica-Bold-R-Normal-*-*-180-*-*-*-*-*  -relief ridge -text PgAccess 
    label $base.l2  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief groove  -text {A Tcl/Tk interface to
PostgreSQL
by Constantin Teodorescu} 
    label $base.l3  -borderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief sunken -text {vers 0.81}
    label $base.l4  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief groove  -text {You will always get the latest version at:
http://www.flex.ro/pgaccess

Suggestions : teo@flex.ro} 
    button $base.b1  -borderwidth 1 -command {Window destroy .about}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text Ok 
    ###################
    # SETTING GEOMETRY
    ###################
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel  -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 282x128+353+310
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Open database"
    label $base.lhost  -borderwidth 0  -relief raised -text Host 
    entry $base.ehost  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable newhost 
    label $base.lport  -borderwidth 0  -relief raised -text Port 
    entry $base.epport  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable newpport 
    label $base.ldbname  -borderwidth 0  -relief raised -text Database 
    entry $base.edbname  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable newdbname 
    button $base.opbtu  -borderwidth 1 -command open_database  -padx 9 -pady 3 -text Open 
    button $base.canbut  -borderwidth 1 -command {Window hide .dbod}  -padx 9  -pady 3 -text Cancel 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.lhost  -x 35 -y 7 -anchor nw -bordermode ignore 
    place $base.ehost  -x 100 -y 5 -anchor nw -bordermode ignore 
    place $base.lport  -x 35 -y 32 -anchor nw -bordermode ignore 
    place $base.epport  -x 100 -y 30 -anchor nw -bordermode ignore 
    place $base.ldbname  -x 35 -y 57 -anchor nw -bordermode ignore 
    place $base.edbname  -x 100 -y 55 -anchor nw -bordermode ignore 
    place $base.opbtu  -x 70 -y 90 -width 60 -height 26 -anchor nw -bordermode ignore 
    place $base.canbut  -x 150 -y 90 -width 60 -height 26 -anchor nw -bordermode ignore
}

proc vTclWindow.dw {base} {
    if {$base == ""} {
        set base .dw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel \
        -background #efefef -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 322x355+96+172
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "PostgreSQL access"
    label $base.labframe \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised 
    listbox $base.lb \
        -background #fefefe \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -foreground black -highlightthickness 0 -selectborderwidth 0 \
        -yscrollcommand {.dw.sb set} 
    bind $base.lb <Double-Button-1> {
        cmd_Open
    }
    button $base.btnnew \
        -borderwidth 1 -command cmd_New \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text New 
    button $base.btnopen \
        -borderwidth 1 -command cmd_Open \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Open 
    button $base.btndesign \
        -borderwidth 1 -command cmd_Design \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Design 
    label $base.lmask \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {  } 
    label $base.label22 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised 
    menubutton $base.menubutton23 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -menu .dw.menubutton23.01 -padx 4 -pady 3 -text Database 
    menu $base.menubutton23.01 \
        -borderwidth 1 -cursor {} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -tearoff 0 
    $base.menubutton23.01 add command \
        \
        -command {
Window show .dbod
set newhost $host
set newpport $pport
focus .dbod.edbname} \
        -label Open 
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
    $base.menubutton23.01 add separator
    $base.menubutton23.01 add command \
        -command {catch {pg_disconnect $dbc}
save_pref
exit} -label Exit 
    label $base.lshost \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief groove -text localhost -textvariable host 
    label $base.lsdbname \
        -anchor w -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief groove -textvariable sdbname 
    scrollbar $base.sb \
        -borderwidth 1 -command {.dw.lb yview} -orient vert 
    menubutton $base.mnob \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -menu .dw.mnob.m -padx 4 -pady 3 -text Object 
    menu $base.mnob.m \
        -borderwidth 1 -cursor {} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -tearoff 0 
    $base.mnob.m add command \
        -command cmd_New -label New 
    $base.mnob.m add command \
        -command {cmd_Delete } -label Delete 
    $base.mnob.m add command \
        -command {cmd_Rename } -label Rename 
    $base.mnob.m add command \
        -command cmd_Information -label Information 
    menubutton $base.mhelp \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -menu .dw.mhelp.m -padx 4 -pady 3 -text Help 
    menu $base.mhelp.m \
        -borderwidth 1 -cursor {} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -tearoff 0 
    $base.mhelp.m add command \
        -label Contents 
    $base.mhelp.m add command \
        -label PostgreSQL 
    $base.mhelp.m add separator
    $base.mhelp.m add command \
        -command {Window show .about} -label About 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.labframe \
        -x 80 -y 30 -width 236 -height 300 -anchor nw -bordermode ignore 
    place $base.lb \
        -x 90 -y 75 -width 205 -height 248 -anchor nw -bordermode ignore 
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
        -x 295 -y 73 -width 18 -height 252 -anchor nw -bordermode ignore 
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 306x288+298+290
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Function"
    label $base.l1  -borderwidth 0  -relief raised -text Name 
    entry $base.e1  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable funcname 
    label $base.l2  -borderwidth 0  -relief raised -text Parameters 
    entry $base.e2  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable funcpar 
    label $base.l3  -borderwidth 0  -relief raised -text Returns 
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
        }  -padx 9  -pady 3 -state disabled -text Define 
    button $base.cancelbtn  -borderwidth 1 -command {Window destroy .fw}  -padx 9  -pady 3 -text Close 
    ###################
    # SETTING GEOMETRY
    ###################
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 287x151+259+304
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Import-Export table"
    label $base.l1  -borderwidth 0  -relief raised -text {Table name} 
    entry $base.e1  -background #fefefe -borderwidth 1 -textvariable ie_tablename 
    label $base.l2  -borderwidth 0  -relief raised -text {File name} 
    entry $base.e2  -background #fefefe -borderwidth 1 -textvariable ie_filename 
    label $base.l3  -borderwidth 0  -relief raised -text {Field delimiter} 
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
        cursor_watch .iew
	if {[sql_exec noquiet $sqlcmd]} {
                cursor_arrow .iew
		tk_messageBox -title Information -message "Operation completed!"
		Window destroy .iew
	}
        cursor_arrow .iew
}}  -padx 9  -pady 3 -text Export 
    button $base.cancelbtn  -borderwidth 1 -command {Window destroy .iew}  -padx 9  -pady 3 -text Cancel 
    checkbutton $base.oicb  -borderwidth 1  -text {with OIDs} -variable oicb 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 25 -y 15 -anchor nw -bordermode ignore 
    place $base.e1  -x 115 -y 10 -anchor nw -bordermode ignore 
    place $base.l2  -x 25 -y 45 -anchor nw -bordermode ignore 
    place $base.e2  -x 115 -y 40 -anchor nw -bordermode ignore 
    place $base.l3  -x 25 -y 75 -height 18 -anchor nw -bordermode ignore 
    place $base.e3  -x 115 -y 74 -width 33 -height 22 -anchor nw -bordermode ignore 
    place $base.expbtn  -x 60 -y 110 -anchor nw -bordermode ignore 
    place $base.cancelbtn  -x 155 -y 110 -anchor nw -bordermode ignore 
    place $base.oicb  -x 170 -y 75 -anchor nw -bordermode ignore
}

proc vTclWindow.mw {base} {
    if {$base == ""} {
        set base .mw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 550x400+189+228
    wm maxsize $base 1009 738
    wm minsize $base 550 400
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm deiconify $base
    wm title $base "Table browser"
    bind $base <Key-Delete> {
        mw_delete_record
    }
    frame $base.f1  -borderwidth 2 -height 75 -relief groove -width 125 
    label $base.f1.l1  -borderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -relief raised -text {Sort field} 
    entry $base.f1.e1  -background #fefefe -borderwidth 1 -width 14  -highlightthickness 1 -textvariable sortfield
    label $base.f1.lb1  -borderwidth 0 -relief raised -text {     } 
    label $base.f1.l2  -background #dfdfdf -borderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -relief raised -text {Filter conditions} 
    entry $base.f1.e2  -background #fefefe -borderwidth 1  -highlightthickness 1 -textvariable filter
    button $base.f1.b1  -borderwidth 1  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9  -pady 3 -text Close -command {
if {[mw_save_new_record]} {
	.mw.c delete rows
	.mw.c delete header
	set sortfield {}
	set filter {}
	Window destroy .mw
}
	}
    button $base.f1.b2  -borderwidth 1  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9  -pady 3 -text Reload -command {
set nq $mw(query)
if {($mw(isaquery)) && ("$filter$sortfield"!="")} {
    show_error "Sorting and filtering not (yet) available from queries!\n\nPlease enter them in the query definition!"
	set sortfield {}
	set filter {}
} else {
    if {$filter!=""} {
        set nq "$mw(query) where ($filter)"
    } else {
        set nq $mw(query)
    }
    if {$sortfield!=""} {
        set nq "$nq order by $sortfield"
    }
}
if {[mw_save_new_record]} {mw_select_records $nq}
	}
    frame $base.frame20  -borderwidth 2 -height 75 -relief groove -width 125 
    button $base.frame20.01  -borderwidth 1 -padx 9 -pady 3 -text < -command {mw_pan_right}
    label $base.frame20.02  -anchor w -borderwidth 1  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -height 1  -relief sunken -text {} -textvariable msg 
    button $base.frame20.03  -borderwidth 1 -padx 9 -pady 3 -text > -command {mw_pan_left}
    canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -highlightthickness 0  -relief ridge -selectborderwidth 0 -takefocus 1 -width 295 
    scrollbar $base.sb  -borderwidth 1 -orient vert -width 12  -command mw_scroll_window
    bind $base.c <Button-1> {
        mw_canvas_click %x %y
    }
    bind $base.c <Button-3> {
        if {[mw_exit_edit]} {mw_save_new_record}
    }
    ###################
    # SETTING GEOMETRY
    ###################
    pack $base.f1  -in .mw -anchor center -expand 0 -fill x -side top 
    pack $base.f1.l1  -in .mw.f1 -anchor center -expand 0 -fill none -side left 
    pack $base.f1.e1  -in .mw.f1 -anchor center -expand 0 -fill none -side left 
    pack $base.f1.lb1  -in .mw.f1 -anchor center -expand 0 -fill none -side left 
    pack $base.f1.l2  -in .mw.f1 -anchor center -expand 0 -fill none -side left 
    pack $base.f1.e2  -in .mw.f1 -anchor center -expand 0 -fill none -side left 
    pack $base.f1.b1  -in .mw.f1 -anchor center -expand 0 -fill none -side right 
    pack $base.f1.b2  -in .mw.f1 -anchor center -expand 0 -fill none -side right 
    pack $base.frame20  -in .mw -anchor s -expand 0 -fill x -side bottom 
    pack $base.frame20.01  -in .mw.frame20 -anchor center -expand 0 -fill none -side left 
    pack $base.frame20.02  -in .mw.frame20 -anchor center -expand 1 -fill x -side left 
    pack $base.frame20.03  -in .mw.frame20 -anchor center -expand 0 -fill none -side right 
    pack $base.c  -in .mw -anchor w -expand 1 -fill both -side left 
    pack $base.sb  -in .mw -anchor e -expand 0 -fill y -side right
}

proc vTclWindow.nt {base} {
    if {$base == ""} {
        set base .nt
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 630x312+148+315
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Create table"
    entry $base.etabn \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newtablename 
    bind $base.etabn <Key-Return> {
        focus .nt.einh
    }
    label $base.li \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Inherits 
    entry $base.einh \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable fathername 
    bind $base.einh <Key-Return> {
        focus .nt.e2
    }
    button $base.binh \
        -borderwidth 1 \
        -command {if {[winfo exists .nt.ddf]} {
    destroy .nt.ddf
} else {
    create_drop_down .nt 95 52
    focus .nt.ddf.sb
    foreach tbl [get_tables] {.nt.ddf.lb insert end $tbl}
    bind .nt.ddf.lb <ButtonRelease-1> {
        set i [.nt.ddf.lb curselection]
        if {$i!=""} {set fathername [.nt.ddf.lb get $i]}
        after 50 {destroy .nt.ddf}
        if {$i!=""} {focus .nt.e2}
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 0 -padx 9 -pady 3 -takefocus 0 -text v 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable fldname 
    bind $base.e2 <Key-Return> {
        focus .nt.e1
    }
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -cursor {} -highlightthickness 1 \
        -selectborderwidth 0 -textvariable fldtype 
    bind $base.e1 <Key-Return> {
        focus .nt.e5
    }
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable fldsize 
    bind $base.e3 <Key-Return> {
        focus .nt.e5
    }
    entry $base.e5 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable defaultval 
    bind $base.e5 <Key-Return> {
        focus .nt.cb1
    }
    checkbutton $base.cb1 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -offvalue { } -onvalue { NOT NULL} -text {field cannot be null} \
        -variable notnull 
    label $base.lab1 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Field type} 
    label $base.lab2 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Field name} 
    label $base.lab3 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Field size} 
    label $base.lab4 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Default value} 
    button $base.addfld \
        -borderwidth 1 -command add_new_field \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Add field} 
    button $base.delfld \
        -borderwidth 1 -command {catch {.nt.lb delete [.nt.lb curselection]}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Delete field} 
    button $base.emptb \
        -borderwidth 1 -command {.nt.lb delete 0 [.nt.lb size]} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Delete all} 
    button $base.maketbl \
        -borderwidth 1 \
        -command {if {$newtablename==""} then {
    show_error "You must supply a name for your table!"
    focus .nt.etabn
} elseif {[.nt.lb size]==0} then {
    show_error "Your table has no fields!"
    focus .nt.e2
} else {
    set temp "create table $newtablename ([join [.nt.lb get 0 end] ,])"
    if {$fathername!=""} then {set temp "$temp inherits ($fathername)"}
    cursor_watch .nt
    set retval [catch {
            set pgres [pg_exec $dbc $temp]
            pg_result $pgres -clear
        } errmsg ]
    cursor_arrow .nt
    if {$retval} {
        show_error "Error creating table\n$errmsg"
    } else {
        .nt.lb delete 0 end
        Window destroy .nt
        cmd_Tables
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Create table} 
    listbox $base.lb \
        -background #fefefe -borderwidth 1 \
        -font -*-Clean-Medium-R-Normal--*-130-*-*-*-*-*-* \
        -highlightthickness 1 -selectborderwidth 0 \
        -yscrollcommand {.nt.sb set} 
    bind $base.lb <ButtonRelease-1> {
        if {[.nt.lb curselection]!=""} {
    set fldname [string trim [lindex [split [.nt.lb get [.nt.lb curselection]]] 0]]
}
    }
    button $base.exitbtn \
        -borderwidth 1 -command {Window destroy .nt} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Cancel 
    label $base.l1 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {field name} 
    label $base.l2 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text type 
    label $base.l3 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text options 
    scrollbar $base.sb \
        -borderwidth 1 -command {.nt.lb yview} -orient vert 
    label $base.l93 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Table name} 
    button $base.mvup \
        -borderwidth 1 \
        -command {if {[.nt.lb size]>2} {
    set i [.nt.lb curselection]
    if {($i!="")&&($i>0)} {
        .nt.lb insert [expr $i-1] [.nt.lb get $i]
        .nt.lb delete [expr $i+1]
        .nt.lb selection set [expr $i-1]
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Move field up} 
    button $base.mvdn \
        -borderwidth 1 \
        -command {if {[.nt.lb size]>2} {
    set i [.nt.lb curselection]
    if {($i!="")&&($i<[expr [.nt.lb size]-1])} {
        .nt.lb insert [expr $i+2] [.nt.lb get $i]
        .nt.lb delete $i
        .nt.lb selection set [expr $i+1]
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Move field down} 
    label $base.ll \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief sunken 
    button $base.button17 \
        -borderwidth 1 \
        -command {if {[winfo exists .nt.ddf]} {
    destroy .nt.ddf
} else {
    create_drop_down .nt 95 125
    focus .nt.ddf.sb
    .nt.ddf.lb insert end char char2 char4 char8 char16 varchar text int2 int4 float4 float8 date datetime
    bind .nt.ddf.lb <ButtonRelease-1> {
        set i [.nt.ddf.lb curselection]
        if {$i!=""} {set fldtype [.nt.ddf.lb get $i]}
        after 50 {destroy .nt.ddf}
        if {$i!=""} {focus .nt.e3}
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 0 -padx 9 -pady 3 -takefocus 0 -text v 
    label $base.label18 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief sunken 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.etabn \
        -x 95 -y 7 -anchor nw -bordermode ignore 
    place $base.li \
        -x 10 -y 35 -anchor nw -bordermode ignore 
    place $base.einh \
        -x 95 -y 32 -anchor nw -bordermode ignore 
    place $base.binh \
        -x 242 -y 33 -width 16 -height 19 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 95 -y 80 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 95 -y 105 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 95 -y 130 -anchor nw -bordermode ignore 
    place $base.e5 \
        -x 95 -y 155 -anchor nw -bordermode ignore 
    place $base.cb1 \
        -x 95 -y 180 -anchor nw -bordermode ignore 
    place $base.lab1 \
        -x 10 -y 107 -anchor nw -bordermode ignore 
    place $base.lab2 \
        -x 10 -y 82 -anchor nw -bordermode ignore 
    place $base.lab3 \
        -x 10 -y 132 -anchor nw -bordermode ignore 
    place $base.lab4 \
        -x 10 -y 157 -anchor nw -bordermode ignore 
    place $base.addfld \
        -x 10 -y 220 -anchor nw -bordermode ignore 
    place $base.delfld \
        -x 85 -y 220 -width 82 -anchor nw -bordermode ignore 
    place $base.emptb \
        -x 170 -y 220 -anchor nw -bordermode ignore 
    place $base.maketbl \
        -x 10 -y 280 -width 156 -height 26 -anchor nw -bordermode ignore 
    place $base.lb \
        -x 260 -y 25 -width 353 -height 281 -anchor nw -bordermode ignore 
    place $base.exitbtn \
        -x 170 -y 280 -width 77 -height 26 -anchor nw -bordermode ignore 
    place $base.l1 \
        -x 261 -y 9 -width 98 -height 18 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 360 -y 9 -width 86 -height 18 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 446 -y 9 -width 166 -height 18 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 610 -y 25 -width 18 -height 282 -anchor nw -bordermode ignore 
    place $base.l93 \
        -x 10 -y 10 -anchor nw -bordermode ignore 
    place $base.mvup \
        -x 10 -y 250 -width 118 -height 26 -anchor nw -bordermode ignore 
    place $base.mvdn \
        -x 130 -y 250 -height 26 -anchor nw -bordermode ignore 
    place $base.ll \
        -x 10 -y 210 -width 233 -height 2 -anchor nw -bordermode ignore 
    place $base.button17 \
        -x 242 -y 106 -width 16 -height 19 -anchor nw -bordermode ignore 
    place $base.label18 \
        -x 10 -y 65 -width 233 -height 2 -anchor nw -bordermode ignore 
}

proc vTclWindow.pw {base} {
    if {$base == ""} {
        set base .pw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 322x167+210+219
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Preferences"
    label $base.l1  -borderwidth 0 -relief raised -text {Max rows displayed in table/query view} 
    entry $base.e1  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(rows) 
    label $base.l2  -borderwidth 0 -relief raised -text Font 
    radiobutton $base.tvf  -borderwidth 1 -text {fixed (clean)} -value clean -variable pref(tvfont) 
    radiobutton $base.tvfv  -borderwidth 1 -text {proportional (helvetica)} -value helv -variable pref(tvfont) 
    label $base.ll  -borderwidth 1 -relief sunken 
    checkbutton $base.alcb  -borderwidth 1 -text {Auto-load the last opened database at startup}  -variable pref(autoload) 
    button $base.okbtn  -borderwidth 1  -command {if {$pref(rows)>200} {
tk_messageBox -title Warning -message "A big number of rows displayed in table view will take a lot of memory!"
}
save_pref
Window destroy .pw} -padx 9  -pady 3 -text Ok 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 10 -y 20 -anchor nw -bordermode ignore 
    place $base.e1  -x 245 -y 17 -width 65 -height 24 -anchor nw -bordermode ignore 
    place $base.l2  -x 10 -y 53 -anchor nw -bordermode ignore 
    place $base.tvf  -x 50 -y 50 -anchor nw -bordermode ignore 
    place $base.tvfv  -x 155 -y 50 -anchor nw -bordermode ignore 
    place $base.ll  -x 10 -y 85 -width 301 -height 2 -anchor nw -bordermode ignore 
    place $base.alcb  -x 10 -y 95 -anchor nw -bordermode ignore 
    place $base.okbtn  -x 125 -y 135 -width 80 -height 26 -anchor nw -bordermode ignore
}

proc vTclWindow.qb {base} {
    if {$base == ""} {
        set base .qb
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel  -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 442x344+282+299
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Query builder"
    label $base.lqn  -borderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief raised -text {Query name} 
    entry $base.eqn  -background #fefefe -borderwidth 1 -foreground #000000  -highlightthickness 1 -selectborderwidth 0 -textvariable queryname 
    button $base.savebtn  -borderwidth 1  -command {if {$queryname==""} then {
    show_error "You have to supply a name for this query!"
    focus .qb.eqn
} else {
    set qcmd [.qb.text1 get 1.0 end]
    regsub -all "\n" $qcmd " " qcmd
    regsub -all "'" $qcmd "''" qcmd
    if {$qcmd==""} then {
        show_error "This query has no commands ?"
    } else {
        if { [lindex [split [string toupper [string trim $qcmd]]] 0] == "SELECT" } {
            set qtype S
        } else {
            set qtype A
        }
        if {$cbv} {
            set retval [catch {set pgres [pg_exec $dbc "create view $queryname as $qcmd"]} errmsg]
            if {$retval} {
                show_error "Error defining view\n\n$errmsg"
            } else {
                tab_click .dw.tabViews
                Window destroy .qb
            }
        } else {
			cursor_watch .qb
            set retval [catch {
                if {$queryoid==0} then {
                    set pgres [pg_exec $dbc "insert into pga_queries values ('$queryname','$qtype','$qcmd')"]
                } else {
                    set pgres [pg_exec $dbc "update pga_queries set queryname='$queryname',querytype='$qtype',querycommand='$qcmd' where oid=$queryoid"]
                }
            } errmsg]
			cursor_arrow .qb
            if {$retval} then {
                show_error "Error executing query\n$errmsg"
            } else {
                cmd_Queries
                if {$queryoid==0} {set queryoid [pg_result $pgres -oid]}
            }
        }
        catch {pg_result $pgres -clear}
    }
}}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text {Save query definition} 
    button $base.execbtn  -borderwidth 1  -command {Window show .mw
set qcmd [.qb.text1 get 0.0 end]
regsub -all "\n" $qcmd " " qcmd
set mw(layout_name) $queryname
mw_load_layout $queryname
set mw(query) $qcmd
set mw(updatable) 0
set mw(isaquery) 1
mw_select_records $qcmd}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text {Execute query} 
    button $base.termbtn  -borderwidth 1  -command {.qb.cbv configure -state normal
set cbv 0
set queryname {}
.qb.text1 delete 1.0 end
Window destroy .qb}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text Close 
    text $base.text1  -background #fefefe -borderwidth 1  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -foreground #000000 -highlightthickness 1 -wrap word 
    checkbutton $base.cbv  -borderwidth 1  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -text {Save this query as a view} -variable cbv 
    button $base.qlshow  -borderwidth 1  -command {Window show .ql
ql_draw_lizzard
focus .ql.entt}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text {Visual designer} 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.lqn  -x 5 -y 5 -anchor nw -bordermode ignore 
    place $base.eqn  -x 80 -y 1 -width 355 -height 24 -anchor nw -bordermode ignore 
    place $base.savebtn  -x 5 -y 60 -anchor nw -bordermode ignore 
    place $base.execbtn  -x 150 -y 60 -anchor nw -bordermode ignore 
    place $base.termbtn  -x 375 -y 60 -anchor nw -bordermode ignore 
    place $base.text1  -x 5 -y 90 -width 430 -height 246 -anchor nw -bordermode ignore 
    place $base.cbv  -x 5 -y 30 -anchor nw -bordermode ignore 
    place $base.qlshow  -x 255 -y 60 -anchor nw -bordermode ignore
}

proc vTclWindow.ql {base} {
    if {$base == ""} {
        set base .ql
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel  -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 759x530+233+177
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
Window destroy .ql}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text Close 
    button $base.showbtn  -borderwidth 1 -command ql_show_sql  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text {Show SQL} 
    label $base.l12  -borderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief raised -text {Add table} 
    entry $base.entt  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable qlvar(newtablename) 
    bind $base.entt <Key-Return> {
        ql_add_new_table
    }
    button $base.execbtn  -borderwidth 1  -command {Window show .mw
set qcmd [ql_compute_sql]
set mw(layout_name) nolayoutneeded
mw_load_layout $mw(layout_name)
set mw(query) $qcmd
set mw(updatable) 0
set mw(isaquery) 1
mw_select_records $qcmd}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text {Execute SQL} 
    button $base.stoqb  -borderwidth 1  -command {Window show .qb
.qb.text1 delete 1.0 end
.qb.text1 insert end [ql_compute_sql]
focus .qb}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text {Save to query builder} 
    button $base.bdd  -borderwidth 1  -command {if {[winfo exists .ql.ddf]} {
    destroy .ql.ddf
} else {
    create_drop_down .ql 70 27
    focus .ql.ddf.sb
    foreach tbl [get_tables] {.ql.ddf.lb insert end $tbl}
    bind .ql.ddf.lb <ButtonRelease-1> {
        set i [.ql.ddf.lb curselection]
        if {$i!=""} {set qlvar(newtablename) [.ql.ddf.lb get $i]}
        after 50 {destroy .ql.ddf}
        if {$i!=""} {ql_add_new_table}
    }
}}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -highlightthickness 0 -padx 9 -pady 3 -text v 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.c  -x 5 -y 30 -width 748 -height 500 -anchor nw -bordermode ignore 
    place $base.exitbtn  -x 695 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.showbtn  -x 367 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.l12  -x 10 -y 8 -width 53 -height 16 -anchor nw -bordermode ignore 
    place $base.entt  -x 70 -y 7 -width 126 -height 20 -anchor nw -bordermode ignore 
    place $base.execbtn  -x 452 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.stoqb  -x 550 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.bdd  -x 200 -y 7 -width 17 -height 20 -anchor nw -bordermode ignore
}


proc vTclWindow.rf {base} {
    if {$base == ""} {
        set base .rf
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 272x105+294+262
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Rename"
    label $base.l1  -borderwidth 0 -relief raised -text {New name} 
    entry $base.e1  -background #fefefe -borderwidth 1 -textvariable newobjname 
    button $base.b1  -borderwidth 1  -command {
			if {$newobjname==""} {
				show_error "You must give object a new name!"
			} elseif {$activetab=="Tables"} {
				set retval [sql_exec noquiet "alter table $oldobjname rename to $newobjname"]
				if {$retval} {
					sql_exec quiet "update pga_layout set tablename='$newobjname' where tablename='$oldobjname'"
					cmd_Tables
					Window destroy .rf
				}			
			} elseif {$activetab=="Queries"} {
				set retval [catch {set pgres [pg_exec $dbc "select * from pga_queries where queryname='$newobjname'"]} errmsg]
				if {$retval} {
					show_error $errmsg
				} elseif {[pg_result $pgres -numTuples]>0} {
					show_error "Query $newobjname already exists!"
					pg_result $pgres -clear
				} else {
					pg_result $pgres -clear
					sql_exec noquiet "update pga_queries set queryname='$newobjname' where queryname='$oldobjname'"
					sql_exec noquiet "update pga_layout set tablename='$newobjname' where tablename='$oldobjname'"
					cmd_Queries
					Window destroy .rf
				}
			}
       } -padx 9  -pady 3 -text Rename 
    button $base.b2  -borderwidth 1 -command {Window destroy .rf} -padx 9  -pady 3 -text Cancel 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 15 -y 28 -anchor nw -bordermode ignore 
    place $base.e1  -x 100 -y 25 -anchor nw -bordermode ignore 
    place $base.b1  -x 65 -y 65 -width 70 -anchor nw -bordermode ignore 
    place $base.b2  -x 145 -y 65 -width 70 -anchor nw -bordermode ignore
}

proc vTclWindow.rb {base} {
    if {$base == ""} {
        set base .rb
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 652x426+96+160
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "Report builder"
    label $base.l1 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -relief raised -text {Report fields} 
    listbox $base.lb \
        -background #fefefe -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
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
        -command {if {[tk_messageBox -title Warning -message "All report information will be deleted.\n\nProceed ?" -type yesno -default no]=="yes"} then {
.rb.c delete all
rb_init
rb_draw_regions
}} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text {Clear all} 
    button $base.bt4 \
        -borderwidth 1 -command rb_preview \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text Preview 
    button $base.bt5 \
        -borderwidth 1 -command {Window destroy .rb} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text Quit 
    scrollbar $base.sb \
        -borderwidth 1 -command {.rb.lb yview} -orient vert 
    label $base.lmsg \
        -anchor w -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
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
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text {Add label} 
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
        -font -Adobe-Helvetica-Medium-O-Normal--*-120-*-*-*-*-*-* \
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
        -anchor w -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -relief groove -text {Database field} -textvariable rbvar(info) 
    label $base.llal \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -relief raised -text Align 
    button $base.balign \
        -borderwidth 0 -command rb_flip_align \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -relief groove -text right 
    button $base.savebtn \
        -borderwidth 1 -command rb_save_report \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text Save 
    label $base.lfn \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -relief raised -text Font 
    button $base.bfont \
        -borderwidth 0 \
        -command {set temp [.rb.bfont cget -text]
if {$temp=="Courier"} then {
  .rb.bfont configure -text Helvetica
} else {
  .rb.bfont configure -text Courier
}
rb_change_object_font} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -relief groove -text Courier 
    button $base.bdd \
        -borderwidth 1 \
        -command {if {[winfo exists .rb.ddf]} {
    destroy .rb.ddf
} else {
    create_drop_down .rb 405 24
    focus .rb.ddf.sb
    foreach tbl [get_tables] {.rb.ddf.lb insert end $tbl}
    bind .rb.ddf.lb <ButtonRelease-1> {
        set i [.rb.ddf.lb curselection]
        if {$i!=""} {set rbvar(tablename) [.rb.ddf.lb get $i]}
        after 50 {destroy .rb.ddf}
        rb_get_report_fields
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -highlightthickness 0 -padx 9 -pady 2 -text v 
    label $base.lrn \
        -borderwidth 0 \
        -font -Adobe-Helvetica-medium-R-Normal--*-120-*-*-*-*-*-* \
        -relief raised -text {Report name} 
    entry $base.ern \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -textvariable rbvar(reportname) 
    bind $base.ern <Key-F5> {
        rb_load_report
    }
    label $base.lrs \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -relief raised -text {Report source} 
    label $base.ls \
        -borderwidth 1 -relief raised 
    entry $base.ef \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -textvariable rbvar(formula) 
    button $base.baf \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text {Add formula} 
    ###################
    # SETTING GEOMETRY
    ###################
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 495x500+239+165
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
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text Close 
    button $base.f1.button17 \
        -borderwidth 1 -command rb_print_report \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text Print 
    ###################
    # SETTING GEOMETRY
    ###################
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 310x223+245+158
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Sequence"
    label $base.l1  -anchor w -borderwidth 0 -relief raised -text {Sequence name} 
    entry $base.e1  -borderwidth 1 -highlightthickness 1 -textvariable seq_name 
    label $base.l2  -borderwidth 0 -relief raised -text Increment 
    entry $base.e2  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_inc 
    label $base.l3  -borderwidth 0 -relief raised -text {Start value} 
    entry $base.e3  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_start 
    label $base.l4  -borderwidth 0 -relief raised -text Minvalue 
    entry $base.e4  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_minval 
    label $base.l5  -borderwidth 0 -relief raised -text Maxvalue 
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
			set sqlcmd "create sequence $seq_name $s1 $s2 $s3 $s4"
			if {[sql_exec noquiet $sqlcmd]} {
				cmd_Sequences
				tk_messageBox -title Information -message "Sequence created!"
			}
        }
    }  -padx 9  -pady 3 -text {Define sequence} 
    button $base.closebtn  -borderwidth 1  -command {for {set i 1} {$i<6} {incr i} {
    .sqf.e$i configure -state normal
    .sqf.e$i delete 0 end
    .sqf.defbtn configure -state normal
    .sqf.l3 configure -text {Start value}
}
place .sqf.defbtn -x 40 -y 175
Window destroy .sqf
} -padx 9  -pady 3 -text Close 
    ###################
    # SETTING GEOMETRY
    ###################
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
    if {$base == ""} {
        set base .sw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 594x416+248+217
    wm maxsize $base 1009 738
    wm minsize $base 300 300
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Design script"
    frame $base.f1  -height 55 -relief groove -width 125 
    label $base.f1.l1  -borderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief raised -text {Script name} 
    entry $base.f1.e1  -background #fefefe -borderwidth 1 -highlightthickness 0  -textvariable scriptname -width 32 
    text $base.src  -background #fefefe  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -height 2  -highlightthickness 1 -selectborderwidth 0 -width 2 
    frame $base.f2  -height 75 -relief groove -width 125 
    button $base.f2.b1  -borderwidth 1 -command {Window destroy .sw}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text Cancel 
    button $base.f2.b2  -borderwidth 1  -command {if {$scriptname==""} {
    tk_messageBox -title Warning -message "The script must have a name!"
} else {
   sql_exec noquiet "delete from pga_scripts where scriptname='$scriptname'"
   regsub -all {\\} [.sw.src get 1.0 end] {\\\\} scriptsource
   regsub -all ' $scriptsource  \\' scriptsource
   sql_exec noquiet "insert into pga_scripts values ('$scriptname','$scriptsource')"
   cmd_Scripts
}}  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9  -pady 3 -text Save -width 6 
    ###################
    # SETTING GEOMETRY
    ###################
    pack $base.f1  -in .sw -anchor center -expand 0 -fill x -pady 2 -side top 
    pack $base.f1.l1  -in .sw.f1 -anchor center -expand 0 -fill none -ipadx 2 -side left 
    pack $base.f1.e1  -in .sw.f1 -anchor center -expand 0 -fill none -side left 
    pack $base.src  -in .sw -anchor center -expand 1 -fill both -padx 2 -side top 
    pack $base.f2  -in .sw -anchor center -expand 0 -fill none -side top 
    pack $base.f2.b1  -in .sw.f2 -anchor center -expand 0 -fill none -side right 
    pack $base.f2.b2  -in .sw.f2 -anchor center -expand 0 -fill none -side right
}

proc vTclWindow.tiw {base} {
    if {$base == ""} {
        set base .tiw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 390x460+243+120
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Table information"
    label $base.l1  -borderwidth 0  -relief raised -text {Table name} 
    label $base.l2  -anchor w -borderwidth 0  -relief raised -text conturi -textvariable tiw(tablename) 
    label $base.l3  -borderwidth 0  -relief raised -text Owner 
    label $base.l4  -anchor w -borderwidth 1  -textvariable tiw(owner) 
    listbox $base.lb  -background #fefefe -borderwidth 1  -font -*-Clean-Medium-R-Normal--*-130-*-*-*-*-*-*  -highlightthickness 1 -selectborderwidth 0  -yscrollcommand {.tiw.sb set} 
    scrollbar $base.sb  -activebackground #d9d9d9 -activerelief sunken -borderwidth 1  -command {.tiw.lb yview} -orient vert 
    button $base.closebtn  -borderwidth 1 -command {Window destroy .tiw}  -pady 3 -text Close 
    label $base.l10  -borderwidth 1  -relief raised -text {field name} 
    label $base.l11  -borderwidth 1  -relief raised -text {field type} 
    label $base.l12  -borderwidth 1  -relief raised -text size 
    label $base.lfi  -borderwidth 0  -relief raised -text {Field information} 
    label $base.lii  -borderwidth 1  -relief raised -text {Indexes defined} 
    listbox $base.ilb  -background #fefefe -borderwidth 1  -highlightthickness 1 -selectborderwidth 0 
    bind $base.ilb <ButtonRelease-1> {
        tiw_show_index
    }
    label $base.lip  -borderwidth 1  -relief raised -text {index properties} 
    frame $base.fr11  -borderwidth 1 -height 75 -relief sunken -width 125 
    label $base.fr11.l9  -borderwidth 0  -relief raised -text {Is clustered ?} 
    label $base.fr11.l2  -borderwidth 0  -relief raised -text {Is unique ?} 
    label $base.fr11.liu  -anchor nw -borderwidth 0  -relief raised -text Yes -textvariable tiw(isunique) 
    label $base.fr11.lic  -anchor nw -borderwidth 0  -relief raised -text No -textvariable tiw(isclustered) 
    label $base.fr11.l5  -borderwidth 0  -relief raised -text {Fields :} 
    label $base.fr11.lif  -anchor nw -borderwidth 1  -justify left -relief sunken -text cont  -textvariable tiw(indexfields) -wraplength 170 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 20 -y 15 -anchor nw -bordermode ignore 
    place $base.l2  -x 100 -y 14 -width 161 -height 18 -anchor nw -bordermode ignore 
    place $base.l3  -x 20 -y 35 -anchor nw -bordermode ignore 
    place $base.l4  -x 100 -y 34 -width 226 -height 18 -anchor nw -bordermode ignore 
    place $base.lb  -x 20 -y 91 -width 338 -height 171 -anchor nw -bordermode ignore 
    place $base.sb  -x 355 -y 90 -width 18 -height 173 -anchor nw -bordermode ignore 
    place $base.closebtn  -x 325 -y 5 -anchor nw -bordermode ignore 
    place $base.l10  -x 21 -y 75 -width 204 -height 18 -anchor nw -bordermode ignore 
    place $base.l11  -x 225 -y 75 -width 90 -height 18 -anchor nw -bordermode ignore 
    place $base.l12  -x 315 -y 75 -width 41 -height 18 -anchor nw -bordermode ignore 
    place $base.lfi  -x 20 -y 55 -anchor nw -bordermode ignore 
    place $base.lii  -x 20 -y 280 -width 151 -height 18 -anchor nw -bordermode ignore 
    place $base.ilb  -x 20 -y 296 -width 150 -height 148 -anchor nw -bordermode ignore 
    place $base.lip  -x 171 -y 280 -width 198 -height 18 -anchor nw -bordermode ignore 
    place $base.fr11  -x 170 -y 297 -width 199 -height 147 -anchor nw -bordermode ignore 
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 377x315+185+234
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
    ###################
    # SETTING GEOMETRY
    ###################
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 225x197+589+29
    wm maxsize $base 785 570
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm deiconify $base
    wm title $base "Attributes"
    label $base.l1 \
        -anchor nw -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -justify left -text Name -width 8 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_name) 
    bind $base.e1 <Key-Return> {
        fd_set_name
    }
    label $base.l2 \
        -anchor nw -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -justify left -text Top -width 8 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_top) 
    bind $base.e2 <Key-Return> {
        fd_change_coord
    }
    label $base.l3 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -text Left \
        -width 8 
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_left) 
    bind $base.e3 <Key-Return> {
        fd_change_coord
    }
    label $base.l4 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -text Width \
        -width 8 
    entry $base.e4 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_width) 
    bind $base.e4 <Key-Return> {
        fd_change_coord
    }
    label $base.l5 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 0 \
        -text Height -width 8 
    entry $base.e5 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_height) 
    bind $base.e5 <Key-Return> {
        fd_change_coord
    }
    label $base.l6 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 0 \
        -text Command 
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
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 3 \
        -pady 3 -text ... -width 1 
    label $base.l7 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -text Variable -width 8 
    entry $base.e7 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_var) 
    bind $base.e7 <Key-Return> {
        set fdobj($fdvar(moveitemobj),v) $fdvar(c_var)
    }
    label $base.l8 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -text Text \
        -width 8 
    entry $base.e8 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(c_text) 
    bind $base.e8 <Key-Return> {
        fd_set_text
    }
    label $base.l0 \
        -borderwidth 1 -relief raised -text {checkbox .udf0.checkbox17} \
        -textvariable fdvar(c_info) -width 28 
    ###################
    # SETTING GEOMETRY
    ###################
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
    if {$base == ""} {
        set base .fdcmd
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 282x274+616+367
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
        -font -*-Clean-Medium-R-Normal--*-130-*-*-*-*-*-* -height 1 \
        -width 115 -yscrollcommand {.fdcmd.f.sb set} 
    frame $base.fb \
        -height 75 -width 125 
    button $base.fb.b1 \
        -borderwidth 1 \
        -command {set fdvar(c_cmd) [.fdcmd.f.txt get 1.0 "end - 1 chars"]
Window hide .fdcmd
fd_set_command} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Ok -width 5 
    button $base.fb.b2 \
        -borderwidth 1 -command {Window hide .fdcmd} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Cancel 
    ###################
    # SETTING GEOMETRY
    ###################
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
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 288x70+193+129
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
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Delete all} 
    button $base.but18 \
        -borderwidth 1 -command {set fdvar(geometry) [wm geometry .fd] ; fd_test } \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Test form} 
    button $base.but19 \
        -borderwidth 1 -command {destroy .$fdvar(forminame)} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Close test form} 
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
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Close 
    button $base.bload \
        -borderwidth 1 -command {fd_load_form nimic design} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text {Load from database} 
    button $base.button17 \
        -borderwidth 1 -command {fd_save_form nimic} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -padx 9 \
        -pady 3 -text Save 
    label $base.l1 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -text {Form name} 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(formname) 
    label $base.l2 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -text {Form's window internal name} 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -highlightthickness 0 \
        -selectborderwidth 0 -textvariable fdvar(forminame) 
    ###################
    # SETTING GEOMETRY
    ###################
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

proc vTclWindow.fdtb {base} {
    if {$base == ""} {
        set base .fdtb
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 90x152+65+180
    wm maxsize $base 785 570
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm deiconify $base
    wm title $base "Toolbar"
    radiobutton $base.rb1 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 0 -text Point -value point -variable fdvar(tool) \
        -width 9 
    radiobutton $base.rb2 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -foreground #000000 -highlightthickness 0 -selectcolor #0000ee \
        -text Label -value label -variable fdvar(tool) -width 9 
    radiobutton $base.rb3 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 0 -text Entry -value entry -variable fdvar(tool) \
        -width 9 
    radiobutton $base.rb4 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 0 -text Button -value button \
        -variable fdvar(tool) -width 9 
    radiobutton $base.rb5 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 0 -text {List box} -value listbox \
        -variable fdvar(tool) -width 9 
    radiobutton $base.rb6 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -highlightthickness 0 -text {Check box} -value checkbox \
        -variable fdvar(tool) -width 9 
    radiobutton $base.rb7 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -highlightthickness 0 -text {Radio btn} -value radio \
        -variable fdvar(tool) -width 9 
    radiobutton $base.rb8 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -highlightthickness 0 -text Query -value query -variable fdvar(tool) \
        -width 9 
    ###################
    # SETTING GEOMETRY
    ###################
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

Window show .
Window show .dw

main $argc $argv
