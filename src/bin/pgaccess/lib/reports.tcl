namespace eval Reports {


proc {new} {} {
global PgAcVar
	Window show .pgaw:ReportBuilder
	tkwait visibility .pgaw:ReportBuilder
	init
	set PgAcVar(report,reportname) {}
	set PgAcVar(report,justpreview) 0
	focus .pgaw:ReportBuilder.e2
}


proc {open} {reportname} {
global PgAcVar CurrentDB
	Window show .pgaw:ReportBuilder
	#tkwait visibility .pgaw:ReportBuilder
	Window hide .pgaw:ReportBuilder
	Window show .pgaw:ReportPreview
	init
	set PgAcVar(report,reportname) $reportname
	loadReport
	tkwait visibility .pgaw:ReportPreview
	set PgAcVar(report,justpreview) 1
	preview
}


proc {design} {reportname} {
global PgAcVar
	Window show .pgaw:ReportBuilder
	tkwait visibility .pgaw:ReportBuilder
	init
	set PgAcVar(report,reportname) $reportname
	loadReport
	set PgAcVar(report,justpreview) 0
}


proc {drawReportAreas} {} {
global PgAcVar
foreach rg $PgAcVar(report,regions) {
	.pgaw:ReportBuilder.c delete bg_$rg
	.pgaw:ReportBuilder.c create line 0 $PgAcVar(report,y_$rg) 5000 $PgAcVar(report,y_$rg) -tags [subst {bg_$rg}]
	.pgaw:ReportBuilder.c create rectangle 6 [expr $PgAcVar(report,y_$rg)-3] 12 [expr $PgAcVar(report,y_$rg)+3] -fill black -tags [subst {bg_$rg mov reg}]
	.pgaw:ReportBuilder.c lower bg_$rg
}
}

proc {toggleAlignMode} {} {
set bb [.pgaw:ReportBuilder.c bbox hili]
if {[.pgaw:ReportBuilder.balign cget -text]=="left"} then {
	.pgaw:ReportBuilder.balign configure -text right
	.pgaw:ReportBuilder.c itemconfigure hili -anchor ne
	.pgaw:ReportBuilder.c move hili [expr [lindex $bb 2]-[lindex $bb 0]-3] 0
} else {
	.pgaw:ReportBuilder.balign configure -text left
	.pgaw:ReportBuilder.c itemconfigure hili -anchor nw
	.pgaw:ReportBuilder.c move hili [expr [lindex $bb 0]-[lindex $bb 2]+3] 0
}
}

proc {getBoldStatus} {} {
	if {[.pgaw:ReportBuilder.lbold cget -relief]=="raised"} then {return Medium} else {return Bold}
}

proc {getItalicStatus} {} {
	if {[.pgaw:ReportBuilder.lita cget -relief]=="raised"} then {return R} else {return O}
}

proc {toggleBold} {} {
	if {[getBoldStatus]=="Bold"} {
	   .pgaw:ReportBuilder.lbold configure -relief raised
	} else {
	   .pgaw:ReportBuilder.lbold configure -relief sunken
	}
	setObjectFont
}


proc {toggleItalic} {} {
	if {[getItalicStatus]=="O"} {
	   .pgaw:ReportBuilder.lita configure -relief raised
	} else {
	   .pgaw:ReportBuilder.lita configure -relief sunken
	}
	setObjectFont
}


proc {setFont} {} {
	set temp [.pgaw:ReportBuilder.bfont cget -text]
	if {$temp=="Courier"} then {
	  .pgaw:ReportBuilder.bfont configure -text Helvetica
	} else {
	  .pgaw:ReportBuilder.bfont configure -text Courier
	}
	setObjectFont
}


proc {getSourceFields} {} {
global PgAcVar CurrentDB
	.pgaw:ReportBuilder.lb delete 0 end
	if {$PgAcVar(report,tablename)==""} return ;
	#setCursor CLOCK
	wpg_select $CurrentDB "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$PgAcVar(report,tablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
		.pgaw:ReportBuilder.lb insert end $rec(attname)
	}
	#setCursor DEFAULT
}


proc {hasTag} {id tg} {
	if {[lsearch [.pgaw:ReportBuilder.c itemcget $id -tags] $tg]==-1} then {return 0 } else {return 1}
}


proc {init} {} {
global PgAcVar
	set PgAcVar(report,xl_auto) 10
	set PgAcVar(report,xf_auto) 10
	set PgAcVar(report,regions) {rpthdr pghdr detail pgfoo rptfoo}
	set PgAcVar(report,y_rpthdr) 30
	set PgAcVar(report,y_pghdr) 60
	set PgAcVar(report,y_detail) 90
	set PgAcVar(report,y_pgfoo) 120
	set PgAcVar(report,y_rptfoo) 150
	set PgAcVar(report,e_rpthdr) [intlmsg {Report header}]
	set PgAcVar(report,e_pghdr) [intlmsg {Page header}]
	set PgAcVar(report,e_detail) [intlmsg {Detail record}]
	set PgAcVar(report,e_pgfoo) [intlmsg {Page footer}]
	set PgAcVar(report,e_rptfoo) [intlmsg {Report footer}]
	drawReportAreas
}

proc {loadReport} {} {
global PgAcVar CurrentDB
	.pgaw:ReportBuilder.c delete all
	wpg_select $CurrentDB "select * from pga_reports where reportname='$PgAcVar(report,reportname)'" rcd {
		eval $rcd(reportbody)
	}
	getSourceFields
	drawReportAreas
}


proc {preview} {} {
global PgAcVar CurrentDB
Window show .pgaw:ReportPreview
.pgaw:ReportPreview.fr.c delete all
set ol [.pgaw:ReportBuilder.c find withtag ro]
set fields {}
foreach objid $ol {
	set tags [.pgaw:ReportBuilder.c itemcget $objid -tags]
	lappend fields [string range [lindex $tags [lsearch -glob $tags f-*]] 2 64]
	lappend fields [lindex [.pgaw:ReportBuilder.c coords $objid] 0]
	lappend fields [lindex [.pgaw:ReportBuilder.c coords $objid] 1]
	lappend fields $objid
	lappend fields [lindex $tags [lsearch -glob $tags t_*]]
}
# Parsing page header
set py 10
foreach {field x y objid objtype} $fields {
	if {$objtype=="t_l"} {
		.pgaw:ReportPreview.fr.c create text $x [expr $py+$y] -text [.pgaw:ReportBuilder.c itemcget $objid -text]  -font [.pgaw:ReportBuilder.c itemcget $objid -font] -anchor nw
	}
}
incr py [expr $PgAcVar(report,y_pghdr)-$PgAcVar(report,y_rpthdr)]
# Parsing detail group
set di [lsearch $PgAcVar(report,regions) detail]
set y_hi $PgAcVar(report,y_detail)
set y_lo $PgAcVar(report,y_[lindex $PgAcVar(report,regions) [expr $di-1]])
wpg_select $CurrentDB "select * from \"$PgAcVar(report,tablename)\"" rec {
	foreach {field x y objid objtype} $fields {
		if {($y>=$y_lo) && ($y<=$y_hi)} then {
		if {$objtype=="t_f"} {
			.pgaw:ReportPreview.fr.c create text $x [expr $py+$y] -text $rec($field) -font [.pgaw:ReportBuilder.c itemcget $objid -font] -anchor [.pgaw:ReportBuilder.c itemcget $objid -anchor]
		}
		if {$objtype=="t_l"} {
			.pgaw:ReportPreview.fr.c create text $x [expr $py+$y] -text [.pgaw:ReportBuilder.c itemcget $objid -text]  -font [.pgaw:ReportBuilder.c itemcget $objid -font] -anchor nw
		}
		}
	}
	incr py [expr $PgAcVar(report,y_detail)-$PgAcVar(report,y_pghdr)]
}
.pgaw:ReportPreview.fr.c configure -scrollregion [subst {0 0 1000 $py}]
}


proc {print} {} {
	set bb [.pgaw:ReportPreview.fr.c bbox all]
	.pgaw:ReportPreview.fr.c postscript -file "pgaccess-report.ps" -width [expr 10+[lindex $bb 2]-[lindex $bb 0]] -height [expr 10+[lindex $bb 3]-[lindex $bb 1]]
	tk_messageBox -title Information -parent .pgaw:ReportBuilder -message "The printed image in Postscript is in the file pgaccess-report.ps"
}


proc {save} {} {
global PgAcVar
set prog "set PgAcVar(report,tablename) \"$PgAcVar(report,tablename)\""
foreach region $PgAcVar(report,regions) {
	set prog "$prog ; set PgAcVar(report,y_$region) $PgAcVar(report,y_$region)"
}
foreach obj [.pgaw:ReportBuilder.c find all] {
	if {[.pgaw:ReportBuilder.c type $obj]=="text"} {
		set bb [.pgaw:ReportBuilder.c bbox $obj]
		if {[.pgaw:ReportBuilder.c itemcget $obj -anchor]=="nw"} then {set x [expr [lindex $bb 0]+1]} else {set x [expr [lindex $bb 2]-2]}
		set prog "$prog ; .pgaw:ReportBuilder.c create text $x [lindex $bb 1] -font [.pgaw:ReportBuilder.c itemcget $obj -font] -anchor [.pgaw:ReportBuilder.c itemcget $obj -anchor] -text {[.pgaw:ReportBuilder.c itemcget $obj -text]} -tags {[.pgaw:ReportBuilder.c itemcget $obj -tags]}"
	}
}
sql_exec noquiet "delete from pga_reports where reportname='$PgAcVar(report,reportname)'"
sql_exec noquiet "insert into pga_reports (reportname,reportsource,reportbody) values ('$PgAcVar(report,reportname)','$PgAcVar(report,tablename)','$prog')"
}


proc {addField} {} {
global PgAcVar
	set fldname [.pgaw:ReportBuilder.lb get [.pgaw:ReportBuilder.lb curselection]]
	set newid [.pgaw:ReportBuilder.c create text $PgAcVar(report,xf_auto) [expr $PgAcVar(report,y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)]
	.pgaw:ReportBuilder.c create text $PgAcVar(report,xf_auto) [expr $PgAcVar(report,y_pghdr)+5] -text $fldname -tags [subst {f-$fldname t_f rg_detail mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)
	set bb [.pgaw:ReportBuilder.c bbox $newid]
	incr PgAcVar(report,xf_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}


proc {addLabel} {} {
global PgAcVar
	set fldname $PgAcVar(report,labeltext)
	set newid [.pgaw:ReportBuilder.c create text $PgAcVar(report,xl_auto) [expr $PgAcVar(report,y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)]
	set bb [.pgaw:ReportBuilder.c bbox $newid]
	incr PgAcVar(report,xl_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}


proc {setObjectFont} {} {
global PgAcVar
	.pgaw:ReportBuilder.c itemconfigure hili -font -Adobe-[.pgaw:ReportBuilder.bfont cget -text]-[getBoldStatus]-[getItalicStatus]-Normal--*-$PgAcVar(report,pointsize)-*-*-*-*-*-*
}


proc {deleteObject} {} {
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:ReportBuilder -message "Delete current report object?" -type yesno -default no]=="no"} return;
	.pgaw:ReportBuilder.c delete hili
}


proc {dragMove} {w x y} {
global PgAcVar
	# Showing current region
	foreach rg $PgAcVar(report,regions) {
		set PgAcVar(report,msg) $PgAcVar(report,e_$rg)
		if {$PgAcVar(report,y_$rg)>$y} break;
	}
	set temp {}
	catch {set temp $PgAcVar(draginfo,obj)}
	if {"$temp" != ""} {
		set dx [expr $x - $PgAcVar(draginfo,x)]
		set dy [expr $y - $PgAcVar(draginfo,y)]
		if {$PgAcVar(draginfo,region)!=""} {
			set x $PgAcVar(draginfo,x) ; $w move bg_$PgAcVar(draginfo,region) 0 $dy
		} else {
			$w move $PgAcVar(draginfo,obj) $dx $dy
		}
		set PgAcVar(draginfo,x) $x
		set PgAcVar(draginfo,y) $y
	}
}


proc {dragStart} {w x y} {
global PgAcVar
focus .pgaw:ReportBuilder.c
catch {unset draginfo}
set obj {}
# Only movable objects start dragging
foreach id [$w find overlapping $x $y $x $y] {
	if {[hasTag $id mov]} {
		set obj $id
		break
	}
}
if {$obj==""} return;
set PgAcVar(draginfo,obj) $obj
set taglist [.pgaw:ReportBuilder.c itemcget $obj -tags]
set i [lsearch -glob $taglist bg_*]
if {$i==-1} {
	set PgAcVar(draginfo,region) {}
} else {
	set PgAcVar(draginfo,region) [string range [lindex $taglist $i] 3 64]
} 
.pgaw:ReportBuilder configure -cursor hand1
.pgaw:ReportBuilder.c itemconfigure [.pgaw:ReportBuilder.c find withtag hili] -fill black
.pgaw:ReportBuilder.c dtag [.pgaw:ReportBuilder.c find withtag hili] hili
.pgaw:ReportBuilder.c addtag hili withtag $PgAcVar(draginfo,obj)
.pgaw:ReportBuilder.c itemconfigure hili -fill blue
set PgAcVar(draginfo,x) $x
set PgAcVar(draginfo,y) $y
set PgAcVar(draginfo,sx) $x
set PgAcVar(draginfo,sy) $y
# Setting font information
if {[.pgaw:ReportBuilder.c type hili]=="text"} {
	set fnta [split [.pgaw:ReportBuilder.c itemcget hili -font] -]
	.pgaw:ReportBuilder.bfont configure -text [lindex $fnta 2]
	if {[lindex $fnta 3]=="Medium"} then {.pgaw:ReportBuilder.lbold configure -relief raised} else {.pgaw:ReportBuilder.lbold configure -relief sunken}
	if {[lindex $fnta 4]=="R"} then {.pgaw:ReportBuilder.lita configure -relief raised} else {.pgaw:ReportBuilder.lita configure -relief sunken}
	set PgAcVar(report,pointsize) [lindex $fnta 8]
	if {[hasTag $obj t_f]} {set PgAcVar(report,info) "Database field"}
	if {[hasTag $obj t_l]} {set PgAcVar(report,info) "Label"}
	if {[.pgaw:ReportBuilder.c itemcget $obj -anchor]=="nw"} then {.pgaw:ReportBuilder.balign configure -text left} else {.pgaw:ReportBuilder.balign configure -text right}
}
}

proc {dragStop} {x y} {
global PgAcVar
# when click Close, ql window is destroyed but event ButtonRelease-1 is fired
if {![winfo exists .pgaw:ReportBuilder]} return;
.pgaw:ReportBuilder configure -cursor left_ptr
set este {}
catch {set este $PgAcVar(draginfo,obj)}
if {$este==""} return
# Erase information about object beeing dragged
if {$PgAcVar(draginfo,region)!=""} {
	set dy 0
	foreach rg $PgAcVar(report,regions) {
		.pgaw:ReportBuilder.c move rg_$rg 0 $dy
		if {$rg==$PgAcVar(draginfo,region)} {
			set dy [expr $y-$PgAcVar(report,y_$PgAcVar(draginfo,region))]
		}
		incr PgAcVar(report,y_$rg) $dy
	}
#    .pgaw:ReportBuilder.c move det 0 [expr $y-$PgAcVar(report,y_$PgAcVar(draginfo,region))]
	set PgAcVar(report,y_$PgAcVar(draginfo,region)) $y
	drawReportAreas
} else {
	# Check if object beeing dragged is inside the canvas
	set bb [.pgaw:ReportBuilder.c bbox $PgAcVar(draginfo,obj)]
	if {[lindex $bb 0] < 5} {
		.pgaw:ReportBuilder.c move $PgAcVar(draginfo,obj) [expr 5-[lindex $bb 0]] 0
	}
}
set PgAcVar(draginfo,obj) {}
PgAcVar:clean draginfo,*
}


proc {deleteAllObjects} {} {
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:ReportBuilder -message [intlmsg "All report information will be deleted.\n\nProceed ?"] -type yesno -default no]=="yes"} then {
		.pgaw:ReportBuilder.c delete all
		init
		drawReportAreas
	}
}

}

################################################################


proc vTclWindow.pgaw:ReportBuilder {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:ReportBuilder
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
	wm title $base [intlmsg "Report builder"]
	label $base.l1 \
		-borderwidth 1 \
		-relief raised -text [intlmsg {Report fields}]
	listbox $base.lb \
		-background #fefefe -foreground #000000 -borderwidth 1 \
		-selectbackground #c3c3c3 \
		-highlightthickness 1 -selectborderwidth 0 \
		-yscrollcommand {.pgaw:ReportBuilder.sb set} 
	bind $base.lb <ButtonRelease-1> {
		Reports::addField
	}
	canvas $base.c \
		-background #fffeff -borderwidth 2 -height 207 -highlightthickness 0 \
		-relief ridge -takefocus 1 -width 295 
	bind $base.c <Button-1> {
		Reports::dragStart %W %x %y
	}
	bind $base.c <ButtonRelease-1> {
		Reports::dragStop %x %y
	}
	bind $base.c <Key-Delete> {
		Reports::deleteObject
	}
	bind $base.c <Motion> {
		Reports::dragMove %W %x %y
	}
	button $base.bt2 \
		-command Reports::deleteAllObjects \
		-text [intlmsg {Delete all}]
	button $base.bt4 \
		-command Reports::preview \
		-text [intlmsg Preview]
	button $base.bt5 \
		-borderwidth 1 -command {Window destroy .pgaw:ReportBuilder} \
		-text [intlmsg Close]
	scrollbar $base.sb \
		-borderwidth 1 -command {.pgaw:ReportBuilder.lb yview} -orient vert 
	label $base.lmsg \
		-anchor w \
		-relief groove -text [intlmsg {Report header}] -textvariable PgAcVar(report,msg) 
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,tablename) 
	bind $base.e2 <Key-Return> {
		Reports::getSourceFields
	}
	entry $base.elab \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,labeltext) 
	button $base.badl \
		-borderwidth 1 -command Reports::addLabel \
		-text [intlmsg {Add label}]
	label $base.lbold \
		-borderwidth 1 -relief raised -text B 
	bind $base.lbold <Button-1> {
		Reports::toggleBold
	}
	label $base.lita \
		-borderwidth 1 \
		-font $PgAcVar(pref,font_italic) \
		-relief raised -text i 
	bind $base.lita <Button-1> {
		Reports::toggleItalic
	}
	entry $base.eps \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable PgAcVar(report,pointsize) 
	bind $base.eps <Key-Return> {
		Reports::setObjectFont
	}
	label $base.linfo \
		-anchor w  \
		-relief groove -text {Database field} -textvariable PgAcVar(report,info) 
	label $base.llal \
		-borderwidth 0 -text Align 
	button $base.balign \
		-borderwidth 0 -command Reports::toggleAlignMode \
		-relief groove -text right 
	button $base.savebtn \
		-borderwidth 1 -command Reports::save \
		-text [intlmsg Save]
	label $base.lfn \
		-borderwidth 0 -text Font 
	button $base.bfont \
		-borderwidth 0 \
		-command Reports::setFont \
		-relief groove -text Courier 
	button $base.bdd \
		-borderwidth 1 \
		-command {if {[winfo exists .pgaw:ReportBuilder.ddf]} {
	destroy .pgaw:ReportBuilder.ddf
} else {
	create_drop_down .pgaw:ReportBuilder 405 22 200
	focus .pgaw:ReportBuilder.ddf.sb
	foreach tbl [Database::getTablesList] {.pgaw:ReportBuilder.ddf.lb insert end $tbl}
	bind .pgaw:ReportBuilder.ddf.lb <ButtonRelease-1> {
		set i [.pgaw:ReportBuilder.ddf.lb curselection]
		if {$i!=""} {set PgAcVar(report,tablename) [.pgaw:ReportBuilder.ddf.lb get $i]}
		destroy .pgaw:ReportBuilder.ddf
		Reports::getSourceFields
		break
	}
}} \
		-highlightthickness 0 -image dnarw 
	label $base.lrn \
		-borderwidth 0 -text [intlmsg {Report name}]
	entry $base.ern \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,reportname) 
	bind $base.ern <Key-F5> {
		loadReport
	}
	label $base.lrs \
		-borderwidth 0 -text [intlmsg {Report source}]
	label $base.ls \
		-borderwidth 1 -relief raised 
	entry $base.ef \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,formula) 
	button $base.baf \
		-borderwidth 1 \
		-text [intlmsg {Add formula}]
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

proc vTclWindow.pgaw:ReportPreview {base} {
	if {$base == ""} {
		set base .pgaw:ReportPreview
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
		-yscrollcommand {.pgaw:ReportPreview.fr.sb set} 
	scrollbar $base.fr.sb \
		-borderwidth 1 -command {.pgaw:ReportPreview.fr.c yview} -highlightthickness 0 \
		-orient vert -width 12 
	frame $base.f1 \
		-borderwidth 2 -height 75 -width 125 
	button $base.f1.button18 \
		-borderwidth 1 -command {if {$PgAcVar(report,justpreview)} then {Window destroy .pgaw:ReportBuilder} ; Window destroy .pgaw:ReportPreview} \
		-text [intlmsg Close] 
	button $base.f1.button17 \
		-borderwidth 1 -command Reports::print \
		-text Print 
	pack $base.fr \
		-in .pgaw:ReportPreview -anchor center -expand 1 -fill both -side top 
	pack $base.fr.c \
		-in .pgaw:ReportPreview.fr -anchor center -expand 1 -fill both -side left 
	pack $base.fr.sb \
		-in .pgaw:ReportPreview.fr -anchor center -expand 0 -fill y -side right 
	pack $base.f1 \
		-in .pgaw:ReportPreview -anchor center -expand 0 -fill none -side top 
	pack $base.f1.button18 \
		-in .pgaw:ReportPreview.f1 -anchor center -expand 0 -fill none -side right 
	pack $base.f1.button17 \
		-in .pgaw:ReportPreview.f1 -anchor center -expand 0 -fill none -side left 
}
