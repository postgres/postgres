namespace eval Reports {

proc {new} {} {
global PgAcVar
	Window show .pgaw:ReportBuilder:draft
	tkwait visibility .pgaw:ReportBuilder:draft
	Window show .pgaw:ReportBuilder:menu
	tkwait visibility .pgaw:ReportBuilder:menu
	design:init
	set PgAcVar(report,reportname) {}
	set PgAcVar(report,justpreview) 0
	focus .pgaw:ReportBuilder:menu.e2
}


proc {open} {reportname} {
global PgAcVar CurrentDB
	Window show .pgaw:ReportBuilder:draft
	#tkwait visibility .pgaw:ReportBuilder:draft
	Window hide .pgaw:ReportBuilder:draft
	Window show .pgaw:ReportBuilder:menu
	Window hide .pgaw:ReportBuilder:menu
	Window show .pgaw:ReportPreview
	design:init
	set PgAcVar(report,reportname) $reportname
	design:loadReport
	tkwait visibility .pgaw:ReportPreview
	set PgAcVar(report,justpreview) 1
	design:preview
}


proc {design} {reportname} {
global PgAcVar
	Window show .pgaw:ReportBuilder:draft
	tkwait visibility .pgaw:ReportBuilder:draft
	Window show .pgaw:ReportBuilder:menu
	tkwait visibility .pgaw:ReportBuilder:menu
	design:init
	set PgAcVar(report,reportname) $reportname
	design:loadReport
	set PgAcVar(report,justpreview) 0
}


proc {design:close} {} {
global PgAcVar
	catch {Window destroy .pgaw:ReportBuilder:draft}
	catch {Window destroy .pgaw:ReportBuilder:menu}
}


proc {design:drawReportAreas} {} {
global PgAcVar
foreach rg $PgAcVar(report,regions) {
	.pgaw:ReportBuilder:draft.c delete bg_$rg
	.pgaw:ReportBuilder:draft.c create line 0 $PgAcVar(report,y_$rg) 5000 $PgAcVar(report,y_$rg) -tags [subst {bg_$rg}]
	.pgaw:ReportBuilder:draft.c create rectangle 6 [expr $PgAcVar(report,y_$rg)-3] 12 [expr $PgAcVar(report,y_$rg)+3] -fill black -tags [subst {bg_$rg mov reg}]
	.pgaw:ReportBuilder:draft.c lower bg_$rg
}
}

proc {design:toggleAlignMode} {} {
set bb [.pgaw:ReportBuilder:draft.c bbox hili]
if {[.pgaw:ReportBuilder:menu.balign cget -text]=="left"} then {
	.pgaw:ReportBuilder:menu.balign configure -text right
	.pgaw:ReportBuilder:draft.c itemconfigure hili -anchor ne
	.pgaw:ReportBuilder:draft.c move hili [expr [lindex $bb 2]-[lindex $bb 0]-3] 0
} else {
	.pgaw:ReportBuilder:menu.balign configure -text left
	.pgaw:ReportBuilder:draft.c itemconfigure hili -anchor nw
	.pgaw:ReportBuilder:draft.c move hili [expr [lindex $bb 0]-[lindex $bb 2]+3] 0
}
}

proc {design:getBoldStatus} {} {
	if {[.pgaw:ReportBuilder:menu.lbold cget -relief]=="raised"} then {return Medium} else {return Bold}
}

proc {design:getItalicStatus} {} {
	if {[.pgaw:ReportBuilder:menu.lita cget -relief]=="raised"} then {return R} else {return O}
}

proc {design:toggleBold} {} {
	if {[design:getBoldStatus]=="Bold"} {
	   .pgaw:ReportBuilder:menu.lbold configure -relief raised
	} else {
	   .pgaw:ReportBuilder:menu.lbold configure -relief sunken
	}
	design:setObjectFont
}


proc {design:toggleItalic} {} {
	if {[design:getItalicStatus]=="O"} {
	   .pgaw:ReportBuilder:menu.lita configure -relief raised
	} else {
	   .pgaw:ReportBuilder:menu.lita configure -relief sunken
	}
	design:setObjectFont
}


# fonts remain an issue to be dealt with
proc {design:setFont} {} {
	set temp [.pgaw:ReportBuilder:menu.bfont cget -text]
	switch $temp {

		Courier
		{.pgaw:ReportBuilder:menu.bfont configure -text Helvetica}

		Helvetica	
		{.pgaw:ReportBuilder:menu.bfont configure -text Times}

		Times	
		{.pgaw:ReportBuilder:menu.bfont configure -text Newcenturyschlbk}

		#Newcenturyschlbk	
		#{.pgaw:ReportBuilder:menu.bfont configure -text Palatino}

		#Palatino	
		#{.pgaw:ReportBuilder:menu.bfont configure -text Utopia}

		default
		{.pgaw:ReportBuilder:menu.bfont configure -text Courier}

	}

	design:setObjectFont
}

# fills in an array with columns so formulas can access them
proc {design:getSourceFieldsForPreview} {} {
global PgAcVar CurrentDB
	set PgAcVar(report,source_fields) {}
	wpg_select $CurrentDB "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$PgAcVar(report,tablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
		lappend PgAcVar(report,source_fields) $rec(attname)
	}
}

# fills in the drop box with column names
proc {design:getSourceFields} {} {
global PgAcVar CurrentDB
	.pgaw:ReportBuilder:menu.lb delete 0 end
	if {$PgAcVar(report,tablename)==""} return ;
	#setCursor CLOCK
	wpg_select $CurrentDB "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$PgAcVar(report,tablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
		.pgaw:ReportBuilder:menu.lb insert end $rec(attname)
	}
	#setCursor DEFAULT
}


proc {design:hasTag} {id tg} {
	if {[lsearch [.pgaw:ReportBuilder:draft.c itemcget $id -tags] $tg]==-1} then {return 0 } else {return 1}
}


proc {design:init} {} {
global PgAcVar
	set PgAcVar(report,xl_auto) 10
	set PgAcVar(report,xf_auto) 10
	set PgAcVar(report,xp_auto) 10
	set PgAcVar(report,xo_auto) 10
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
	design:drawReportAreas
}

proc {design:loadReport} {} {
global PgAcVar CurrentDB
	.pgaw:ReportBuilder:draft.c delete all
	wpg_select $CurrentDB "select * from pga_reports where reportname='$PgAcVar(report,reportname)'" rcd {
		eval $rcd(reportbody)
	}
	design:changeDraftCoords
	design:getSourceFields
	design:drawReportAreas
}

# get the preview cranking
proc {design:preview} {} {
global PgAcVar
	design:previewInit
	set PgAcVar(report,curr_page) 1
	if {$PgAcVar(report,last_page)>0} {
		design:previewPage
	}
}

# finds the record and page counts
proc {design:previewInit} {} {
global PgAcVar CurrentDB
	Window show .pgaw:ReportPreview
	set ol [.pgaw:ReportBuilder:draft.c find withtag ro]
	set PgAcVar(report,prev_fields) {}
	# set up the fields we need to fill with data
	foreach objid $ol {
		set tags [.pgaw:ReportBuilder:draft.c itemcget $objid -tags]
		lappend PgAcVar(report,prev_fields) [string range [lindex $tags [lsearch -glob $tags f-*]] 2 64]
		lappend PgAcVar(report,prev_fields) [lindex [.pgaw:ReportBuilder:draft.c coords $objid] 0]
		lappend PgAcVar(report,prev_fields) [lindex [.pgaw:ReportBuilder:draft.c coords $objid] 1]
		lappend PgAcVar(report,prev_fields) $objid
		lappend PgAcVar(report,prev_fields) [lindex $tags [lsearch -glob $tags t_*]]
	}
	# set up all the source fields - needed for formulas
	design:getSourceFieldsForPreview
	# get number of records (thus the number of detail sections)
	set res [pg_exec $CurrentDB "select * from \"$PgAcVar(report,tablename)\""]
	set PgAcVar(report,prev_num_recs) [pg_result $res -numTuples]
	# get number of detail sections per page (screw report head/foot for now)
	# first: page height - (page header height + page footer height)
	set pgdiff [expr {$PgAcVar(report,ph)-(($PgAcVar(report,y_pghdr)-$PgAcVar(report,y_rpthdr))+($PgAcVar(report,y_pgfoo)-$PgAcVar(report,y_detail)))}]
	# second: result of first / detail height
	set PgAcVar(report,prev_recs_page) [expr {round(double($pgdiff)/double($PgAcVar(report,y_detail)-$PgAcVar(report,y_pghdr)))}]
	# get number of pages
	set PgAcVar(report,last_page) [expr {int(ceil(double($PgAcVar(report,prev_num_recs))/double($PgAcVar(report,prev_recs_page))))}]
	set PgAcVar(report,total_page) $PgAcVar(report,last_page)
}


# displays one section
proc {design:previewSection} {x y objid objtype py recfield} {
global PgAcVar CurrentDB

# for fields
if {$objtype=="t_f"} {
	.pgaw:ReportPreview.fr.c create text $x [expr $py+$y] -text $recfield -font [.pgaw:ReportBuilder:draft.c itemcget $objid -font] -anchor [.pgaw:ReportBuilder:draft.c itemcget $objid -anchor]
}

# for labels
if {$objtype=="t_l"} {
	.pgaw:ReportPreview.fr.c create text $x [expr $py+$y] -text [.pgaw:ReportBuilder:draft.c itemcget $objid -text]  -font [.pgaw:ReportBuilder:draft.c itemcget $objid -font] -anchor nw
}

# for pictures
if {$objtype=="t_p"} {
	.pgaw:ReportPreview.fr.c create image $x [expr $py+$y] -image [image create photo -file [.pgaw:ReportBuilder:draft.c itemcget $objid -image]] -anchor nw
}

# for formulas
if {$objtype=="t_o"} {
	wpg_select $CurrentDB "select * from \"$PgAcVar(report,tablename)\" limit $PgAcVar(report,prev_recs_page) offset [expr {(($PgAcVar(report,curr_page)-1)*$PgAcVar(report,prev_recs_page))}]" frec {
		# assign each source field to a variable
		foreach {ffield} $PgAcVar(report,source_fields) {
			variable $ffield $frec($ffield)
		}
	}
	# now flesh out and evaluate the formula
	.pgaw:ReportPreview.fr.c create text $x [expr $py+$y] -text [eval [.pgaw:ReportBuilder:draft.c itemcget $objid -text]] -font [.pgaw:ReportBuilder:draft.c itemcget $objid -font] -anchor nw
}

}


# displays the current page
# for now we worry about the page head/foot and detail, not report head/foot
proc {design:previewPage} {} {
global PgAcVar CurrentDB

set sql ""
set recfield ""

.pgaw:ReportPreview.fr.c delete all

# parse the page header
set py $PgAcVar(report,y_rpthdr)
# now get the data for the section, which is the first record on the page
wpg_select $CurrentDB "select * from \"$PgAcVar(report,tablename)\" limit 1 offset [expr {(($PgAcVar(report,curr_page)-1)*$PgAcVar(report,prev_recs_page))}]" rec {
	foreach {field x y objid objtype} $PgAcVar(report,prev_fields) {
		if {$y < $PgAcVar(report,y_pghdr)} {
			# make sure we line up the section where it was designed to go
			set y [expr $y-$PgAcVar(report,y_rpthdr)]
			# looking for formulas
			if {$objtype=="t_f"} {
				set recfield $rec($field)
			} else {
				set recfield ""
			}
			design:previewSection $x $y $objid $objtype $py $recfield
		}
	}
}

# Parsing detail group
set shownrecs 1
set py $PgAcVar(report,y_pghdr)
# keep the records on the page
# and do not repeat the last record on the last page
while {($py < [expr ($PgAcVar(report,rh)-($PgAcVar(report,y_detail)-$PgAcVar(report,y_pghdr)))]) && ([expr $shownrecs*$PgAcVar(report,last_page)]<=$PgAcVar(report,prev_num_recs))} {
	# now lets get some data for a record
	wpg_select $CurrentDB "select * from \"$PgAcVar(report,tablename)\" limit $PgAcVar(report,prev_recs_page) offset [expr {(($PgAcVar(report,curr_page)-1)*$PgAcVar(report,prev_recs_page))}]" rec {
		foreach {field x y objid objtype} $PgAcVar(report,prev_fields) {
			if {$y > $PgAcVar(report,y_pghdr) && $y < $PgAcVar(report,y_detail)} {
				set y [expr $y-$PgAcVar(report,y_pghdr)]
				if {$objtype=="t_f"} {
					set recfield $rec($field)
				} else {
					set recfield ""
				}
				design:previewSection $x $y $objid $objtype $py $recfield
			}
		}
		incr py [expr $PgAcVar(report,y_detail)-$PgAcVar(report,y_pghdr)]
		incr shownrecs
	}	
}

# parse the page footer
# put it in the same place on each page
set py [expr {(($PgAcVar(report,y_detail)-$PgAcVar(report,y_pghdr))*$PgAcVar(report,prev_recs_page))+$PgAcVar(report,y_pghdr)}]
# get the data for the section, which is the last record on the page
# pay attention to the case when we are looking at the last page
if {$PgAcVar(report,curr_page)==$PgAcVar(report,last_page)} {
	set sql "select * from \"$PgAcVar(report,tablename)\" limit 1 offset [expr {$PgAcVar(report,prev_num_recs)-1}]"
} else {
	set sql "select * from \"$PgAcVar(report,tablename)\" limit 1 offset [expr {(($PgAcVar(report,curr_page)-1)*$PgAcVar(report,prev_recs_page))+($PgAcVar(report,prev_recs_page)-1)}]"
}
wpg_select $CurrentDB $sql rec {
	foreach {field x y objid objtype} $PgAcVar(report,prev_fields) {
			if {$y > $PgAcVar(report,y_detail) && $y < $PgAcVar(report,y_pgfoo)} {
			set y [expr $y-$PgAcVar(report,y_detail)]
			if {$objtype=="t_f"} {
				set recfield $rec($field)
			} else {
				set recfield ""
			}
			design:previewSection $x $y $objid $objtype $py $recfield
		}
	}
}

design:changePreviewCoords $py

}

# this postscript stuff needs some work but it sort of works
# since the tk canvas widget produces encapsulated postscript
# we need to wrap it inside of regular postscript
proc {design:printPostscriptStart} {c} {
global PgAcVar
	puts $c "%!PS-Adobe-3.0"
	puts $c "%%Creator: PgAccess"
	puts $c "%%LanguageLevel: 2"
	puts $c "%%Title: Report"
	puts $c "%%CreationDate: [clock format [clock seconds]]"
	puts -nonewline $c "%%Pages: "
	puts $c "[expr $PgAcVar(report,last_page)-$PgAcVar(report,curr_page)+1]"
	puts $c "%%PageOrder: Ascend"
	puts $c "%%BoundingBox: 0 0 $PgAcVar(report,pw) $PgAcVar(report,ph)"
	puts $c "%%EndComments"
	puts $c "%%BeginProlog"
	puts $c "%%EndProlog"
	puts $c "%%BeginSetup"
	puts $c "%%EndSetup"
	puts $c ""
}

proc {design:printPostscriptStop} {c} {
global PgAcVar
	puts $c "%%EOF"
}

proc {design:printPostscriptStartPage} {c} {
global PgAcVar
	puts $c "%%Page: $PgAcVar(report,curr_page) $PgAcVar(report,curr_page)"
	puts $c "%%BeginPageSetup"
	puts $c "/pagesave save def"
	puts $c "%%EndPageSetup"
	puts $c "%%BeginDocument"
}

proc {design:printPostscriptStopPage} {c} {
global PgAcVar
	puts $c "%%EndDocument"
	puts $c "pagesave restore"
}

# prints all pages between and including those in the entry boxes
# opens a stream and just starts feeding it postscript from the canvas
# there must be a cleaner way to do this
proc {design:print} {} {
global PgAcVar
	set rpt [parameter "Enter file name or pipe for Postscript output:"]
	set fid [::open $rpt w]
	design:printPostscriptStart $fid
	set start_page $PgAcVar(report,curr_page)
	for {} {$PgAcVar(report,curr_page)<=$PgAcVar(report,last_page)} {incr PgAcVar(report,curr_page)} {
		design:previewPage
		design:printPostscriptStartPage $fid
		.pgaw:ReportPreview.fr.c postscript -channel $fid -width $PgAcVar(report,pw) -height $PgAcVar(report,ph) -pagex 0 -pagey 0 -pageanchor sw
		design:printPostscriptStopPage $fid
	}
	design:printPostscriptStop $fid
	::close $fid
	# reset current page to the page we started printing on
	set PgAcVar(report,curr_page) $start_page
	design:previewPage
	tk_messageBox -title Information -parent .pgaw:ReportBuilder:draft -message "Done printing $rpt"
}


proc {design:save} {} {
global PgAcVar
set prog "set PgAcVar(report,tablename) \"$PgAcVar(report,tablename)\""
set prog "$prog ; set PgAcVar(report,rw) $PgAcVar(report,rw)"
set prog "$prog ; set PgAcVar(report,rh) $PgAcVar(report,rh)"
set prog "$prog ; set PgAcVar(report,pw) $PgAcVar(report,pw)"
set prog "$prog ; set PgAcVar(report,ph) $PgAcVar(report,ph)"
foreach region $PgAcVar(report,regions) {
	set prog "$prog ; set PgAcVar(report,y_$region) $PgAcVar(report,y_$region)"
}
foreach obj [.pgaw:ReportBuilder:draft.c find all] {
	if {[.pgaw:ReportBuilder:draft.c type $obj]=="text"} {
		set bb [.pgaw:ReportBuilder:draft.c bbox $obj]
		if {[.pgaw:ReportBuilder:draft.c itemcget $obj -anchor]=="nw"} then {set x [expr [lindex $bb 0]+1]} else {set x [expr [lindex $bb 2]-2]}
		set prog "$prog ; .pgaw:ReportBuilder:draft.c create text $x [lindex $bb 1] -font [.pgaw:ReportBuilder:draft.c itemcget $obj -font] -anchor [.pgaw:ReportBuilder:draft.c itemcget $obj -anchor] -text {[.pgaw:ReportBuilder:draft.c itemcget $obj -text]} -tags {[.pgaw:ReportBuilder:draft.c itemcget $obj -tags]}"
	}
	if {[.pgaw:ReportBuilder:draft.c type $obj]=="image"} {
		set bb [.pgaw:ReportBuilder:draft.c bbox $obj]
		if {[.pgaw:ReportBuilder:draft.c itemcget $obj -anchor]=="nw"} then {set x [expr [lindex $bb 0]+1]} else {set x [expr [lindex $bb 2]-2]}
		set prog "$prog ; image create photo [.pgaw:ReportBuilder:draft.c itemcget $obj -image] -file [.pgaw:ReportBuilder:draft.c itemcget $obj -image] ; .pgaw:ReportBuilder:draft.c create image $x [lindex $bb 1] -anchor [.pgaw:ReportBuilder:draft.c itemcget $obj -anchor] -image {[.pgaw:ReportBuilder:draft.c itemcget $obj -image]} -tags {[.pgaw:ReportBuilder:draft.c itemcget $obj -tags]}"
	}	
}
sql_exec noquiet "delete from pga_reports where reportname='$PgAcVar(report,reportname)'"
sql_exec noquiet "insert into pga_reports (reportname,reportsource,reportbody) values ('$PgAcVar(report,reportname)','$PgAcVar(report,tablename)','$prog')"
}


proc {design:addField} {} {
global PgAcVar
	set fldname [.pgaw:ReportBuilder:menu.lb get [.pgaw:ReportBuilder:menu.lb curselection]]
	set newid [.pgaw:ReportBuilder:draft.c create text $PgAcVar(report,xf_auto) [expr $PgAcVar(report,y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)]
	.pgaw:ReportBuilder:draft.c create text $PgAcVar(report,xf_auto) [expr $PgAcVar(report,y_pghdr)+5] -text $fldname -tags [subst {f-$fldname t_f rg_detail mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)
	set bb [.pgaw:ReportBuilder:draft.c bbox $newid]
	incr PgAcVar(report,xf_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}


proc {design:addLabel} {} {
global PgAcVar
	set fldname $PgAcVar(report,labeltext)
	set newid [.pgaw:ReportBuilder:draft.c create text $PgAcVar(report,xl_auto) [expr $PgAcVar(report,y_rpthdr)+5] -text $fldname -tags [subst {t_l mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)]
	set bb [.pgaw:ReportBuilder:draft.c bbox $newid]
	incr PgAcVar(report,xl_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}

# pictures are from files and not the database, maybe this should be different
proc {design:addPicture} {} {
global PgAcVar
	set fldname $PgAcVar(report,picture)
	set newid [.pgaw:ReportBuilder:draft.c create image $PgAcVar(report,xp_auto) [expr $PgAcVar(report,y_rpthdr)+5] -image [image create photo $fldname -file $fldname] -tags [subst {t_p mov ro}] -anchor nw]
	set bb [.pgaw:ReportBuilder:draft.c bbox $newid]
	incr PgAcVar(report,xp_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}

# formulas are tcl snippets right now, but should allow scripts in the future
proc {design:addFormula} {} {
global PgAcVar
	set fldname $PgAcVar(report,formula)
	set newid [.pgaw:ReportBuilder:draft.c create text $PgAcVar(report,xo_auto) [expr $PgAcVar(report,y_rpthdr)+5] -text $fldname -tags [subst {t_o mov ro}] -anchor nw -font $PgAcVar(pref,font_normal)]
	set bb [.pgaw:ReportBuilder:draft.c bbox $newid]
	incr PgAcVar(report,xo_auto) [expr 5+[lindex $bb 2]-[lindex $bb 0]]
}


proc {design:setObjectFont} {} {
global PgAcVar
	.pgaw:ReportBuilder:draft.c itemconfigure hili -font -Adobe-[.pgaw:ReportBuilder:menu.bfont cget -text]-[design:getBoldStatus]-[design:getItalicStatus]-Normal--*-$PgAcVar(report,pointsize)-*-*-*-*-*-*
}


proc {design:deleteObject} {} {
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:ReportBuilder:draft -message "Delete current report object?" -type yesno -default no]=="no"} return;
	.pgaw:ReportBuilder:draft.c delete hili
}


proc {design:dragMove} {w x y} {
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


proc {design:dragStart} {w x y} {
global PgAcVar
focus .pgaw:ReportBuilder:draft.c
catch {unset draginfo}
set obj {}
# Only movable objects start dragging
foreach id [$w find overlapping $x $y $x $y] {
	if {[design:hasTag $id mov]} {
		set obj $id
		break
	}
}
# mouse resize does update after a click
if {$obj==""} {
	.pgaw:ReportBuilder:draft configure -cursor watch
	set c [split [winfo geometry .pgaw:ReportBuilder:draft] x+]
	set PgAcVar(report,rw) [lindex $c 0]
	set PgAcVar(report,rh) [lindex $c 1]
	Reports::design:changeDraftCoords
	return
}
set PgAcVar(draginfo,obj) $obj
set taglist [.pgaw:ReportBuilder:draft.c itemcget $obj -tags]
set i [lsearch -glob $taglist bg_*]
if {$i==-1} {
	set PgAcVar(draginfo,region) {}
} else {
	set PgAcVar(draginfo,region) [string range [lindex $taglist $i] 3 64]
} 
.pgaw:ReportBuilder:draft configure -cursor hand1
# dont highlight pictures when moving them, it just wont work
if {![design:hasTag [.pgaw:ReportBuilder:draft.c find withtag hili] t_p]} {
	.pgaw:ReportBuilder:draft.c itemconfigure [.pgaw:ReportBuilder:draft.c find withtag hili] -fill black
}
	.pgaw:ReportBuilder:draft.c dtag [.pgaw:ReportBuilder:draft.c find withtag hili] hili
	.pgaw:ReportBuilder:draft.c addtag hili withtag $PgAcVar(draginfo,obj)
if {![design:hasTag $obj t_p]} {
	.pgaw:ReportBuilder:draft.c itemconfigure hili -fill blue
}
set PgAcVar(draginfo,x) $x
set PgAcVar(draginfo,y) $y
set PgAcVar(draginfo,sx) $x
set PgAcVar(draginfo,sy) $y
# Setting font information
if {[.pgaw:ReportBuilder:draft.c type hili]=="text"} {
	set fnta [split [.pgaw:ReportBuilder:draft.c itemcget hili -font] -]
	.pgaw:ReportBuilder:menu.bfont configure -text [lindex $fnta 2]
	if {[lindex $fnta 3]=="Medium"} then {.pgaw:ReportBuilder:menu.lbold configure -relief raised} else {.pgaw:ReportBuilder:menu.lbold configure -relief sunken}
	if {[lindex $fnta 4]=="R"} then {.pgaw:ReportBuilder:menu.lita configure -relief raised} else {.pgaw:ReportBuilder:menu.lita configure -relief sunken}
	set PgAcVar(report,pointsize) [lindex $fnta 8]
	if {[design:hasTag $obj t_f]} {set PgAcVar(report,info) "Database field"}
	if {[design:hasTag $obj t_l]} {set PgAcVar(report,info) "Label"}
	if {[design:hasTag $obj t_o]} {set PgAcVar(report,info) "Formula"}
	if {[.pgaw:ReportBuilder:draft.c itemcget $obj -anchor]=="nw"} then {.pgaw:ReportBuilder:menu.balign configure -text left} else {.pgaw:ReportBuilder:menu.balign configure -text right}
}
}

proc {design:dragStop} {x y} {
global PgAcVar
# when click Close, ql window is destroyed but event ButtonRelease-1 is fired
if {![winfo exists .pgaw:ReportBuilder:draft]} return;
.pgaw:ReportBuilder:draft configure -cursor left_ptr
set este {}
catch {set este $PgAcVar(draginfo,obj)}
if {$este==""} return
# Erase information about object beeing dragged
if {$PgAcVar(draginfo,region)!=""} {
	set dy 0
	foreach rg $PgAcVar(report,regions) {
		.pgaw:ReportBuilder:draft.c move rg_$rg 0 $dy
		if {$rg==$PgAcVar(draginfo,region)} {
			set dy [expr $y-$PgAcVar(report,y_$PgAcVar(draginfo,region))]
		}
		incr PgAcVar(report,y_$rg) $dy
	}
#    .pgaw:ReportBuilder:menu.c move det 0 [expr $y-$PgAcVar(report,y_$PgAcVar(draginfo,region))]
	set PgAcVar(report,y_$PgAcVar(draginfo,region)) $y
	design:drawReportAreas
} else {
	# Check if object beeing dragged is inside the canvas
	set bb [.pgaw:ReportBuilder:draft.c bbox $PgAcVar(draginfo,obj)]
	if {[lindex $bb 0] < 5} {
		.pgaw:ReportBuilder:draft.c move $PgAcVar(draginfo,obj) [expr 5-[lindex $bb 0]] 0
	}
}
set PgAcVar(draginfo,obj) {}
PgAcVar:clean draginfo,*
}


proc {design:deleteAllObjects} {} {
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:ReportBuilder:draft -message [intlmsg "All report information will be deleted.\n\nProceed ?"] -type yesno -default no]=="yes"} then {
		.pgaw:ReportBuilder:draft.c delete all
		design:init
		design:drawReportAreas
	}
}

proc {design:changeDraftCoords} {} {
global PgAcVar
	wm geometry .pgaw:ReportBuilder:draft $PgAcVar(report,rw)x$PgAcVar(report,rh)
	place .pgaw:ReportBuilder:draft.c -x 0 -y 0 -width $PgAcVar(report,rw) -height $PgAcVar(report,rh) -anchor nw -bordermode ignore	
}
		
proc {design:changePreviewCoords} {scroller} {
global PgAcVar
	wm geometry .pgaw:ReportPreview $PgAcVar(report,rw)x$PgAcVar(report,rh)
	place .pgaw:ReportPreview.fr.c -x 0 -y 0 -width $PgAcVar(report,rw) -height $PgAcVar(report,rh) -anchor nw -bordermode ignore	
}

}

################################################################

# handmade but call it vTcl for continuity, someday use visualtcl again
proc vTclWindow.pgaw:ReportBuilder:draft {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:ReportBuilder:draft
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 508x345+406+120
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base [intlmsg "Report draft"]
	canvas $base.c \
		-background #fffeff -borderwidth 2 -height 207 -highlightthickness 0 \
		-relief ridge -takefocus 1 -width 295 
	place $base.c \
		-x 0 -y 0 -width 508 -height 345 -anchor nw -bordermode ignore 
	bind $base.c <Button-1> {
		Reports::design:dragStart %W %x %y
	}
	bind $base.c <ButtonRelease-1> {
		Reports::design:dragStop %x %y
	}
	bind $base.c <Key-Delete> {
		Reports::design:deleteObject
	}
	bind $base.c <Motion> {
		Reports::design:dragMove %W %x %y
	}
}


proc vTclWindow.pgaw:ReportBuilder:menu {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:ReportBuilder:menu
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 307x426+96+120
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Report menu"]

	# report size
	label $base.lrsize \
		-borderwidth 0 \
		-text [intlmsg {Report size}]
	entry $base.erw \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable PgAcVar(report,rw)
	label $base.lrwbyh \
		-borderwidth 0 \
		-text x
	entry $base.erh \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable PgAcVar(report,rh)
	bind $base.erw <Key-Return> { 
		Reports::design:changeDraftCoords
	}
	bind $base.erh <Key-Return> {
		Reports::design:changeDraftCoords
	}

	# page size
	label $base.lpsize \
		-borderwidth 0 \
		-text [intlmsg {Page size}]
	entry $base.epw \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable PgAcVar(report,pw)
	label $base.lpwbyh \
		-borderwidth 0 \
		-text x
	entry $base.eph \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable PgAcVar(report,ph)

	label $base.l1 \
		-borderwidth 1 \
		-relief raised -text [intlmsg {Report fields}]
	scrollbar $base.sb \
		-borderwidth 1 -command {.pgaw:ReportBuilder:menu.lb yview} -orient vert 
	listbox $base.lb \
		-background #fefefe -foreground #000000 -borderwidth 1 \
		-selectbackground #c3c3c3 \
		-highlightthickness 1 -selectborderwidth 0 \
		-yscrollcommand {.pgaw:ReportBuilder:menu.sb set} 
	bind $base.lb <ButtonRelease-1> {
		Reports::design:addField
	}
	button $base.bt2 \
		-command Reports::design:deleteAllObjects \
		-text [intlmsg {Delete all}]
	button $base.bt4 \
		-command Reports::design:preview \
		-text [intlmsg Preview]
	button $base.bt5 \
		-borderwidth 1 \
		-command Reports::design:close \
		-text [intlmsg Close]
	label $base.lmsg \
		-anchor w \
		-relief groove -text [intlmsg {Report header}] -textvariable PgAcVar(report,msg) 
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,tablename) 
	bind $base.e2 <Key-Return> {
		Reports::design:getSourceFields
	}
	entry $base.elab \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,labeltext) 
	button $base.badl \
		-borderwidth 1 -command Reports::design:addLabel \
		-text [intlmsg {Add label}]
	label $base.lbold \
		-borderwidth 1 -relief raised -text B 
	bind $base.lbold <Button-1> {
		Reports::design:toggleBold
	}
	label $base.lita \
		-borderwidth 1 \
		-font $PgAcVar(pref,font_italic) \
		-relief raised -text i 
	bind $base.lita <Button-1> {
		Reports::design:toggleItalic
	}
	entry $base.eps \
		-background #fefefe -highlightthickness 0 -relief groove \
		-textvariable PgAcVar(report,pointsize) 
	bind $base.eps <Key-Return> {
		Reports::design:setObjectFont
	}
	label $base.linfo \
		-anchor w  \
		-relief groove -text {Database field} -textvariable PgAcVar(report,info) 
	label $base.llal \
		-borderwidth 0 -text Align 
	button $base.balign \
		-borderwidth 0 -command Reports::design:toggleAlignMode \
		-relief groove -text right 
	button $base.savebtn \
		-borderwidth 1 -command Reports::design:save \
		-text [intlmsg Save]
	label $base.lfn \
		-borderwidth 0 -text Font 
	button $base.bfont \
		-borderwidth 0 \
		-command Reports::design:setFont \
		-relief groove -text Courier 
	button $base.bdd \
		-borderwidth 1 \
		-command {if {[winfo exists .pgaw:ReportBuilder:menu.ddf]} {
	destroy .pgaw:ReportBuilder:menu.ddf
} else {
	create_drop_down .pgaw:ReportBuilder:menu 100 45 200
	focus .pgaw:ReportBuilder:menu.ddf.sb
	foreach tbl [Database::getTablesList] {.pgaw:ReportBuilder:menu.ddf.lb insert end $tbl}
	bind .pgaw:ReportBuilder:menu.ddf.lb <ButtonRelease-1> {
		set i [.pgaw:ReportBuilder:menu.ddf.lb curselection]
		if {$i!=""} {set PgAcVar(report,tablename) [.pgaw:ReportBuilder:menu.ddf.lb get $i]}
		destroy .pgaw:ReportBuilder:menu.ddf
		Reports::design:getSourceFields
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
		-borderwidth 1 -command Reports::design:addFormula \
		-text [intlmsg {Add formula}]
	entry $base.ep \
		-background #fefefe -borderwidth 1 -highlightthickness 0 \
		-textvariable PgAcVar(report,picture) 
	button $base.bap \
		-borderwidth 1 -command Reports::design:addPicture \
		-text [intlmsg {Add picture}]

	place $base.lrsize \
		-x 142 -y 58 -anchor nw -bordermode ignore 
	place $base.erw \
		-x 142 -y 75 -width 35 -height 18 -anchor nw -bordermode ignore 
	place $base.lrwbyh \
		-x 177 -y 75 -anchor nw -bordermode ignore 
	place $base.erh \
		-x 186 -y 75 -width 35 -height 18 -anchor nw -bordermode ignore 

	place $base.lpsize \
		-x 225 -y 58 -anchor nw -bordermode ignore 
	place $base.epw \
		-x 225 -y 75 -width 35 -height 18 -anchor nw -bordermode ignore 
	place $base.lpwbyh \
		-x 260 -y 75 -anchor nw -bordermode ignore 
	place $base.eph \
		-x 269 -y 75 -width 35 -height 18 -anchor nw -bordermode ignore 

	place $base.l1 \
		-x 5 -y 55 -width 131 -height 18 -anchor nw -bordermode ignore 
	place $base.lb \
		-x 5 -y 70 -width 118 -height 121 -anchor nw -bordermode ignore 
	place $base.bt2 \
		-x 5 -y 365 -width 64 -height 26 -anchor nw -bordermode ignore 
	place $base.bt4 \
		-x 70 -y 365 -width 66 -height 26 -anchor nw -bordermode ignore 
	place $base.bt5 \
		-x 70 -y 395 -width 66 -height 26 -anchor nw -bordermode ignore 
	place $base.sb \
		-x 120 -y 70 -width 18 -height 122 -anchor nw -bordermode ignore 
	place $base.lmsg \
		-x 142 -y 95 -width 151 -height 18 -anchor nw -bordermode ignore 
	place $base.e2 \
		-x 120 -y 25 -width 159 -height 18 -anchor nw -bordermode ignore 
	place $base.lbold \
		-x 252 -y 165 -width 18 -height 18 -anchor nw -bordermode ignore 
	place $base.lita \
		-x 272 -y 165 -width 18 -height 18 -anchor nw -bordermode ignore 
	place $base.eps \
		-x 252 -y 140 -width 40 -height 18 -anchor nw -bordermode ignore 
	place $base.linfo \
		-x 142 -y 115 -width 151 -height 18 -anchor nw -bordermode ignore 
	place $base.llal \
		-x 142 -y 165 -anchor nw -bordermode ignore 
	place $base.balign \
		-x 182 -y 165 -width 35 -height 21 -anchor nw -bordermode ignore 
	place $base.savebtn \
		-x 5 -y 395 -width 64 -height 26 -anchor nw -bordermode ignore 
	place $base.lfn \
		-x 142 -y 140 -anchor nw -bordermode ignore 
	place $base.bfont \
		-x 182 -y 140 -width 65 -height 21 -anchor nw -bordermode ignore 
	place $base.bdd \
		-x 280 -y 25 -width 15 -height 20 -anchor nw -bordermode ignore 
	place $base.lrn \
		-x 5 -y 5 -anchor nw -bordermode ignore 
	place $base.ern \
		-x 80 -y 4 -width 219 -height 18 -anchor nw -bordermode ignore 
	place $base.lrs \
		-x 5 -y 25 -anchor nw -bordermode ignore 
	place $base.elab \
		-x 5 -y 200 -width 297 -height 18 -anchor nw -bordermode ignore 
	place $base.badl \
		-x 5 -y 218 -width 132 -height 26 -anchor nw -bordermode ignore 
	place $base.ef \
		-x 5 -y 255 -width 297 -height 18 -anchor nw -bordermode ignore 
	place $base.baf \
		-x 5 -y 273 -width 132 -height 26 -anchor nw -bordermode ignore 
	place $base.ep \
		-x 5 -y 310 -width 297 -height 18 -anchor nw -bordermode ignore 
	place $base.bap \
		-x 5 -y 328 -width 132 -height 26 -anchor nw -bordermode ignore 
}

proc vTclWindow.pgaw:ReportPreview {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:ReportPreview
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 495x500+230+50
	wm maxsize $base 1280 1024
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
	frame $base.fp \
		-borderwidth 2 -height 75 -width 125 
	button $base.fp.bclose \
		-borderwidth 1 -command {if {$PgAcVar(report,justpreview)} then {Window destroy .pgaw:ReportBuilder:draft} ; Window destroy .pgaw:ReportPreview} \
		-text [intlmsg Close] 
	button $base.fp.bprint \
		-borderwidth 1 -command Reports::design:print \
		-text Print 

	label $base.fp.ltexttotal -text "pages "
	label $base.fp.ltotal -textvariable PgAcVar(report,total_page)
	button $base.fp.bprev -text <
	bind $base.fp.bprev <Button-1> { 
		if {$PgAcVar(report,curr_page)>1} {
			set PgAcVar(report,curr_page) [expr $PgAcVar(report,curr_page)-1]
		}
		Reports::design:previewPage
	}
	button $base.fp.bnext -text >
	bind $base.fp.bnext <Button-1> { 
		if {$PgAcVar(report,curr_page)<$PgAcVar(report,last_page)} {
			set PgAcVar(report,curr_page) [expr $PgAcVar(report,curr_page)+1]
		}
		Reports::design:previewPage
	}
	entry $base.fp.estart -width 5 -textvariable PgAcVar(report,curr_page)
	bind $base.fp.estart <Key-Return> { 
		Reports::design:previewPage
	}
	label $base.fp.lthru -text -
	entry $base.fp.estop -width 5 -textvariable PgAcVar(report,last_page)
	bind $base.fp.estop <Key-Return> { 
		Reports::design:previewPage
	}
	
	pack $base.fr \
		-in .pgaw:ReportPreview -anchor center -expand 1 -fill both -side top 
	pack $base.fr.c \
		-in .pgaw:ReportPreview.fr -anchor center -expand 1 -fill both -side left 
	pack $base.fr.sb \
		-in .pgaw:ReportPreview.fr -anchor center -expand 0 -fill y -side right 
	pack $base.fp \
		-in .pgaw:ReportPreview -anchor center -expand 0 -fill none -side bottom

	pack $base.fp.ltotal \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.ltexttotal \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.bprev \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.bnext \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.estart \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.lthru \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.estop \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.bprint \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
	pack $base.fp.bclose \
		-in .pgaw:ReportPreview.fp -expand 0 -fill none -side left
}
