namespace eval Forms {

proc {new} {} {
global PgAcVar
	Window show .pgaw:FormDesign:menu
	tkwait visibility .pgaw:FormDesign:menu
	Window show .pgaw:FormDesign:toolbar
	tkwait visibility .pgaw:FormDesign:toolbar
	Window show .pgaw:FormDesign:attributes
	tkwait visibility .pgaw:FormDesign:attributes
	Window show .pgaw:FormDesign:draft
	design:init
}


proc {open} {formname} {
	 forms:load $formname run
	 design:run
}

proc {design} {formname} {
	forms:load $formname design
}


proc {design:change_coords} {} {
global PgAcVar
	set PgAcVar(fdvar,dirty) 1
	set i $PgAcVar(fdvar,attributeFrame)
	if {$i == 0} {
		# it's the form
		set errmsg ""
		if {[catch {wm geometry .pgaw:FormDesign:draft $PgAcVar(fdvar,c_width)x$PgAcVar(fdvar,c_height)+$PgAcVar(fdvar,c_left)+$PgAcVar(fdvar,c_top)} errmsg] != 0} {
			showError $errmsg
		}
		return
	}		
	set c [list $PgAcVar(fdvar,c_left) $PgAcVar(fdvar,c_top) [expr $PgAcVar(fdvar,c_left)+$PgAcVar(fdvar,c_width)] [expr $PgAcVar(fdvar,c_top)+$PgAcVar(fdvar,c_height)]]
	set PgAcVar(fdobj,$i,coord) $c
	.pgaw:FormDesign:draft.c delete o$i
	design:draw_object $i
	design:draw_hookers $i
}


proc {design:delete_object} {} {
global PgAcVar
	set i $PgAcVar(fdvar,moveitemobj)
	.pgaw:FormDesign:draft.c delete o$i
	.pgaw:FormDesign:draft.c delete hook
	set j [lsearch $PgAcVar(fdvar,objlist) $i]
	set PgAcVar(fdvar,objlist) [lreplace $PgAcVar(fdvar,objlist) $j $j]
	set PgAcVar(fdvar,dirty) 1
}


proc {design:draw_hook} {x y} {
	.pgaw:FormDesign:draft.c create rectangle [expr $x-2] [expr $y-2] [expr $x+2] [expr $y+2] -fill black -tags hook
}


proc {design:draw_hookers} {i} {
global PgAcVar
	foreach {x1 y1 x2 y2} $PgAcVar(fdobj,$i,coord) {}
	.pgaw:FormDesign:draft.c delete hook
	design:draw_hook $x1 $y1
	design:draw_hook $x1 $y2
	design:draw_hook $x2 $y1
	design:draw_hook $x2 $y2
}


proc {design:draw_grid} {} {
	for {set i 0} {$i<100} {incr i} {
		.pgaw:FormDesign:draft.c create line 0 [expr {$i*6}] 1000 [expr {$i*6}] -fill #afafaf -tags grid
		.pgaw:FormDesign:draft.c create line [expr {$i*6}] 0 [expr {$i*6}] 1000 -fill #afafaf -tags grid
	}
}


proc {design:draw_object} {i} {
global PgAcVar
set c $PgAcVar(fdobj,$i,coord)
foreach {x1 y1 x2 y2} $c {}
.pgaw:FormDesign:draft.c delete o$i
set wfont $PgAcVar(fdobj,$i,font)
switch $wfont {
	{} {set wfont $PgAcVar(pref,font_normal) ; set PgAcVar(fdobj,$i,font) normal}
	normal  {set wfont $PgAcVar(pref,font_normal)}
	bold  {set wfont $PgAcVar(pref,font_bold)}
	italic  {set wfont $PgAcVar(pref,font_italic)}
	fixed  {set wfont $PgAcVar(pref,font_fix)}
}
switch $PgAcVar(fdobj,$i,class) {
	button {
		design:draw_rectangle $x1 $y1 $x2 $y2 $PgAcVar(fdobj,$i,relief) $PgAcVar(fdobj,$i,bcolor) o$i
		.pgaw:FormDesign:draft.c create text [expr ($x1+$x2)/2] [expr ($y1+$y2)/2] -fill $PgAcVar(fdobj,$i,fcolor) -text $PgAcVar(fdobj,$i,label) -font $wfont -tags o$i
	}
	text {
		design:draw_rectangle $x1 $y1 $x2 $y2 $PgAcVar(fdobj,$i,relief) $PgAcVar(fdobj,$i,bcolor) o$i
	}
	entry {
		design:draw_rectangle $x1 $y1 $x2 $y2 $PgAcVar(fdobj,$i,relief) $PgAcVar(fdobj,$i,bcolor) o$i
	}
	label {
		set temp $PgAcVar(fdobj,$i,label)
		if {$temp==""} {set temp "____"}
		design:draw_rectangle $x1 $y1 $x2 $y2 $PgAcVar(fdobj,$i,relief) $PgAcVar(fdobj,$i,bcolor) o$i
		.pgaw:FormDesign:draft.c create text [expr {$x1+1}] [expr {$y1+1}] -text $temp -fill $PgAcVar(fdobj,$i,fcolor) -font $wfont -anchor nw -tags o$i
	}
	checkbox {
		design:draw_rectangle [expr $x1+2] [expr $y1+5] [expr $x1+12] [expr $y1+15] raised #a0a0a0 o$i
		.pgaw:FormDesign:draft.c create text [expr $x1+20] [expr $y1+3] -text $PgAcVar(fdobj,$i,label) -anchor nw \
		 -fill $PgAcVar(fdobj,$i,fcolor) -font $wfont -tags o$i
	}
	radio {
		.pgaw:FormDesign:draft.c create oval [expr $x1+4] [expr $y1+5] [expr $x1+14] [expr $y1+15] -fill white -tags o$i
		.pgaw:FormDesign:draft.c create text [expr $x1+24] [expr $y1+3] -text $PgAcVar(fdobj,$i,label) -anchor nw \
		 -fill $PgAcVar(fdobj,$i,fcolor) -font $wfont -tags o$i
	}
	query {
		.pgaw:FormDesign:draft.c create oval $x1 $y1 [expr $x1+20] [expr $y1+20] -fill white -tags o$i
		.pgaw:FormDesign:draft.c create text [expr $x1+5] [expr $y1+4] -text Q  -anchor nw -font $PgAcVar(pref,font_normal) -tags o$i
	}
	listbox {
		design:draw_rectangle $x1 $y1 [expr $x2-12] $y2 sunken $PgAcVar(fdobj,$i,bcolor) o$i
		design:draw_rectangle [expr $x2-11] $y1 $x2 $y2 sunken gray o$i
		.pgaw:FormDesign:draft.c create line [expr $x2-5] $y1 $x2 [expr $y1+10] -fill #808080 -tags o$i
		.pgaw:FormDesign:draft.c create line [expr $x2-10] [expr $y1+9] $x2 [expr $y1+9] -fill #808080 -tags o$i
		.pgaw:FormDesign:draft.c create line [expr $x2-10] [expr $y1+9] [expr $x2-5] $y1 -fill white -tags o$i
		.pgaw:FormDesign:draft.c create line [expr $x2-5] $y2 $x2 [expr $y2-10] -fill #808080 -tags o$i
		.pgaw:FormDesign:draft.c create line [expr $x2-10] [expr $y2-9] $x2 [expr $y2-9] -fill white -tags o$i
		.pgaw:FormDesign:draft.c create line [expr $x2-10] [expr $y2-9] [expr $x2-5] $y2 -fill white -tags o$i
	}
}
.pgaw:FormDesign:draft.c raise hook
}

proc {design:draw_rectangle} {x1 y1 x2 y2 relief color tag} {
	if {$relief=="raised"} {
		set c1 white
		set c2 #606060
	}
	if {$relief=="sunken"} {
		set c1 #606060
		set c2 white
	}
	if {$relief=="ridge"} {
		design:draw_rectangle $x1 $y1 $x2 $y2 raised none $tag
		design:draw_rectangle [expr {$x1+1}] [expr {$y1+1}] [expr {$x2+1}] [expr {$y2+1}] sunken none $tag
		design:draw_rectangle [expr {$x1+2}] [expr {$y1+2}] $x2 $y2 flat $color $tag
		return
	}
	if {$relief=="groove"} {
		design:draw_rectangle $x1 $y1 $x2 $y2 sunken none $tag
		design:draw_rectangle [expr {$x1+1}] [expr {$y1+1}] [expr {$x2+1}] [expr {$y2+1}] raised none $tag
		design:draw_rectangle [expr {$x1+2}] [expr {$y1+2}] $x2 $y2 flat $color $tag
		return
	}
	if {$color != "none"} {
		.pgaw:FormDesign:draft.c create rectangle $x1 $y1 $x2 $y2 -outline "" -fill $color -tags $tag
	}
	if {$relief=="flat"} {
		return
	}
	.pgaw:FormDesign:draft.c create line $x1 $y1 $x2 $y1 -fill $c1 -tags $tag
	.pgaw:FormDesign:draft.c create line $x1 $y1 $x1 $y2 -fill $c1 -tags $tag
	.pgaw:FormDesign:draft.c create line $x1 $y2 $x2 $y2 -fill $c2 -tags $tag
	.pgaw:FormDesign:draft.c create line $x2 $y1 $x2 [expr 1+$y2] -fill $c2 -tags $tag
}


proc {design:init} {} {
global PgAcVar
	PgAcVar:clean fdvar,*
	PgAcVar:clean fdobj,*
	catch {.pgaw:FormDesign:draft.c delete all}
	# design:draw_grid
	set PgAcVar(fdobj,0,name) {f1}
	set PgAcVar(fdobj,0,class) form
	set PgAcVar(fdobj,0,command) {}
	set PgAcVar(fdvar,formtitle) "New form"
	set PgAcVar(fdvar,objnum) 0
	set PgAcVar(fdvar,objlist) {}
	set PgAcVar(fdvar,oper) none
	set PgAcVar(fdvar,tool) point
	set PgAcVar(fdvar,resizable) 1
	set PgAcVar(fdvar,dirty) 0
}


proc {design:item_click} {x y} {
global PgAcVar
	set PgAcVar(fdvar,oper) none
	set PgAcVar(fdvar,moveitemobj) {}
	set il [.pgaw:FormDesign:draft.c find overlapping $x $y $x $y]
	.pgaw:FormDesign:draft.c delete hook
	if {[llength $il] == 0} {
		design:show_attributes 0
		return
	}
	set tl [.pgaw:FormDesign:draft.c gettags [lindex $il 0]]
	set i [lsearch -glob $tl o*]
	if {$i == -1} return
	set objnum [string range [lindex $tl $i] 1 end]
	set PgAcVar(fdvar,moveitemobj) $objnum
	set PgAcVar(fdvar,moveitemx) $x
	set PgAcVar(fdvar,moveitemy) $y
	set PgAcVar(fdvar,oper) move
	design:show_attributes $objnum
	design:draw_hookers $objnum
}


proc {forms:load} {name mode} {
global PgAcVar CurrentDB
	design:init
	set PgAcVar(fdvar,formtitle) $name
	if {$mode=="design"} {
		Window show .pgaw:FormDesign:draft
		Window show .pgaw:FormDesign:menu
		Window show .pgaw:FormDesign:attributes
		Window show .pgaw:FormDesign:toolbar
	}
	set res [wpg_exec $CurrentDB "select * from pga_forms where formname='$PgAcVar(fdvar,formtitle)'"]
	set info [lindex [pg_result $res -getTuple 0] 1]
	pg_result $res -clear
	set PgAcVar(fdobj,0,name) [lindex $info 0]
	set PgAcVar(fdvar,objnum) [lindex $info 1]
	# check for old format , prior to 0.97 that
	# save here the objlist (deprecated)
	set temp [lindex $info 2]
	if {[lindex $temp 0] == "FS"} {
		set PgAcVar(fdobj,0,command) [lindex $temp 1]
	} else {
		set PgAcVar(fdobj,0,command) {}
	}
	set PgAcVar(fdvar,objlist) {}
	set PgAcVar(fdvar,geometry) [lindex $info 3]
	set i 1
	foreach objinfo [lrange $info 4 end] {
		lappend PgAcVar(fdvar,objlist) $i
		set PgAcVar(fdobj,$i,class)    [lindex $objinfo 0]
		set PgAcVar(fdobj,$i,name)     [lindex $objinfo 1]
		set PgAcVar(fdobj,$i,coord)    [lindex $objinfo 2]
		set PgAcVar(fdobj,$i,command)  [lindex $objinfo 3]
		set PgAcVar(fdobj,$i,label)    [lindex $objinfo 4]
		set PgAcVar(fdobj,$i,variable) [lindex $objinfo 5]
		design:setDefaultReliefAndColor $i
		set PgAcVar(fdobj,$i,value) $PgAcVar(fdobj,$i,name)
		if {[llength $objinfo] >  6 } {
			set PgAcVar(fdobj,$i,value)       [lindex $objinfo 6]
			set PgAcVar(fdobj,$i,relief)      [lindex $objinfo 7]
			set PgAcVar(fdobj,$i,fcolor)      [lindex $objinfo 8]
			set PgAcVar(fdobj,$i,bcolor)      [lindex $objinfo 9]
			set PgAcVar(fdobj,$i,borderwidth) [lindex $objinfo 10]
			set PgAcVar(fdobj,$i,font)        [lindex $objinfo 11]
			# for space saving purposes we have saved onbly the first letter
			switch $PgAcVar(fdobj,$i,font) {
				n {set PgAcVar(fdobj,$i,font) normal}
				i {set PgAcVar(fdobj,$i,font) italic}
				b {set PgAcVar(fdobj,$i,font) bold}
				f {set PgAcVar(fdobj,$i,font) fixed}
			}
		}
		if {$mode=="design"} {design:draw_object $i}
		incr i
	}
	if {$mode=="design"} {wm geometry .pgaw:FormDesign:draft $PgAcVar(fdvar,geometry)}
}


proc {design:mouse_down} {x y} {
global PgAcVar
	set x [expr 3*int($x/3)]
	set y [expr 3*int($y/3)]
	set PgAcVar(fdvar,xstart) $x
	set PgAcVar(fdvar,ystart) $y
	if {$PgAcVar(fdvar,tool)=="point"} {
		design:item_click $x $y
		return
	}
	set PgAcVar(fdvar,oper) draw
}


proc {design:mouse_move} {x y} {
global PgAcVar
	#set PgAcVar(fdvar,msg) "x=$x y=$y"
	set x [expr 3*int($x/3)]
	set y [expr 3*int($y/3)]
	set oper ""
	catch {set oper $PgAcVar(fdvar,oper)}
	if {$oper=="draw"} {
		catch {.pgaw:FormDesign:draft.c delete curdraw}
		.pgaw:FormDesign:draft.c create rectangle $PgAcVar(fdvar,xstart) $PgAcVar(fdvar,ystart) $x $y -tags curdraw
		return
	}
	if {$oper=="move"} {
		set dx [expr $x-$PgAcVar(fdvar,moveitemx)]
		set dy [expr $y-$PgAcVar(fdvar,moveitemy)]
		.pgaw:FormDesign:draft.c move o$PgAcVar(fdvar,moveitemobj) $dx $dy
		.pgaw:FormDesign:draft.c move hook $dx $dy
		set PgAcVar(fdvar,moveitemx) $x
		set PgAcVar(fdvar,moveitemy) $y
		set PgAcVar(fdvar,dirty) 1
	}
}

proc {design:setDefaultReliefAndColor} {i} {
global PgAcVar
	set PgAcVar(fdobj,$i,borderwidth) 1
	set PgAcVar(fdobj,$i,relief) flat
	set PgAcVar(fdobj,$i,fcolor) {}
	set PgAcVar(fdobj,$i,bcolor) {}
	set PgAcVar(fdobj,$i,font) normal
	switch $PgAcVar(fdobj,$i,class) {
		button {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #d9d9d9
			set PgAcVar(fdobj,$i,relief) raised
		}
		text {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #fefefe
			set PgAcVar(fdobj,$i,relief) sunken
		}
		entry {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #fefefe
			set PgAcVar(fdobj,$i,relief) sunken
		}
		label {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #d9d9d9
			set PgAcVar(fdobj,$i,relief) flat
		}
		checkbox {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #d9d9d9
			set PgAcVar(fdobj,$i,relief) flat
		}
		radio {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #d9d9d9
			set PgAcVar(fdobj,$i,relief) flat
		}
		listbox {
			set PgAcVar(fdobj,$i,fcolor) #000000
			set PgAcVar(fdobj,$i,bcolor) #fefefe
			set PgAcVar(fdobj,$i,relief) sunken
		}
	}
}

proc {design:mouse_up} {x y} {
global PgAcVar
	set x [expr 3*int($x/3)]
	set y [expr 3*int($y/3)]
	if {$PgAcVar(fdvar,oper)=="move"} {
		set PgAcVar(fdvar,moveitem) {}
		set PgAcVar(fdvar,oper) none
		set oc $PgAcVar(fdobj,$PgAcVar(fdvar,moveitemobj),coord)
		set dx [expr $x - $PgAcVar(fdvar,xstart)]
		set dy [expr $y - $PgAcVar(fdvar,ystart)]
		set newcoord [list [expr $dx+[lindex $oc 0]] [expr $dy+[lindex $oc 1]] [expr $dx+[lindex $oc 2]] [expr $dy+[lindex $oc 3]]]
		set PgAcVar(fdobj,$PgAcVar(fdvar,moveitemobj),coord) $newcoord
		design:show_attributes $PgAcVar(fdvar,moveitemobj)
		design:draw_hookers $PgAcVar(fdvar,moveitemobj)
		return
	}
	if {$PgAcVar(fdvar,oper)!="draw"} return
	set PgAcVar(fdvar,oper) none
	.pgaw:FormDesign:draft.c delete curdraw
	# Check for x2<x1 or y2<y1
	if {$x<$PgAcVar(fdvar,xstart)} {set temp $x ; set x $PgAcVar(fdvar,xstart) ; set PgAcVar(fdvar,xstart) $temp}
	if {$y<$PgAcVar(fdvar,ystart)} {set temp $y ; set y $PgAcVar(fdvar,ystart) ; set PgAcVar(fdvar,ystart) $temp}
	# Check for too small sizes
	if {[expr $x-$PgAcVar(fdvar,xstart)]<20} {set x [expr $PgAcVar(fdvar,xstart)+20]}
	if {[expr $y-$PgAcVar(fdvar,ystart)]<10} {set y [expr $PgAcVar(fdvar,ystart)+10]}
	incr PgAcVar(fdvar,objnum)
	set i $PgAcVar(fdvar,objnum)
	lappend PgAcVar(fdvar,objlist) $i

	set PgAcVar(fdobj,$i,class) $PgAcVar(fdvar,tool)
	set PgAcVar(fdobj,$i,coord) [list $PgAcVar(fdvar,xstart) $PgAcVar(fdvar,ystart) $x $y]
	set PgAcVar(fdobj,$i,name) $PgAcVar(fdvar,tool)$i
	set PgAcVar(fdobj,$i,label) $PgAcVar(fdvar,tool)$i
	set PgAcVar(fdobj,$i,command) {}
	set PgAcVar(fdobj,$i,variable) {}
	set PgAcVar(fdobj,$i,value) {}

	design:setDefaultReliefAndColor $i
	
	design:draw_object $i
	design:show_attributes $i
	set PgAcVar(fdvar,moveitemobj) $i
	design:draw_hookers $i
	set PgAcVar(fdvar,tool) point
	set PgAcVar(fdvar,dirty) 1
}


proc {design:save} {name} {
global PgAcVar CurrentDB
	if {[string length $PgAcVar(fdobj,0,name)]==0} {
		tk_messageBox -title [intlmsg Warning] -message [intlmsg "Forms need an internal name, only literals, low case"]
		return 0
	}
	if {[string length $PgAcVar(fdvar,formtitle)]==0} {
		tk_messageBox -title [intlmsg Warning] -message [intlmsg "Form must have a name"]
		return 0
	}
	set info [list $PgAcVar(fdobj,0,name) $PgAcVar(fdvar,objnum) [list FS $PgAcVar(fdobj,0,command)] [wm geometry .pgaw:FormDesign:draft]]
	foreach i $PgAcVar(fdvar,objlist) {
		set wfont $PgAcVar(fdobj,$i,font)
		if {[lsearch {normal bold italic fixed} $wfont] != -1} {
			set wfont [string range $wfont 0 0]
		}
		lappend info [list $PgAcVar(fdobj,$i,class) $PgAcVar(fdobj,$i,name) $PgAcVar(fdobj,$i,coord) $PgAcVar(fdobj,$i,command) $PgAcVar(fdobj,$i,label) $PgAcVar(fdobj,$i,variable) $PgAcVar(fdobj,$i,value) $PgAcVar(fdobj,$i,relief) $PgAcVar(fdobj,$i,fcolor) $PgAcVar(fdobj,$i,bcolor) $PgAcVar(fdobj,$i,borderwidth) $wfont]
	}
	sql_exec noquiet "delete from pga_forms where formname='$PgAcVar(fdvar,formtitle)'"
	regsub -all "'" $info "''" info
	sql_exec noquiet "insert into pga_forms values ('$PgAcVar(fdvar,formtitle)','$info')"
	Mainlib::cmd_Forms
	set PgAcVar(fdvar,dirty) 0
	return 1
}


proc {design:set_name} {} {
global PgAcVar
	set i $PgAcVar(fdvar,moveitemobj)
	foreach k $PgAcVar(fdvar,objlist) {
		if {($PgAcVar(fdobj,$k,name)==$PgAcVar(fdvar,c_name)) && ($i!=$k)} {
			tk_messageBox -title [intlmsg Warning] -message [format [intlmsg "There is another object (a %s) with the same name.\nPlease change it!"] $PgAcVar(fdobj,$k,class)]
			return
		}
	}
	set PgAcVar(fdobj,$i,name) $PgAcVar(fdvar,c_name)
	design:show_attributes $i
	set PgAcVar(fdvar,dirty) 1
}


proc {design:set_text} {} {
global PgAcVar
	design:draw_object $PgAcVar(fdvar,moveitemobj)
	set PgAcVar(fdvar,dirty) 1
}


proc {design:createAttributesFrame} {i} {
global PgAcVar
	# Check if attributes frame is already created for that item
	
	if {[info exists PgAcVar(fdvar,attributeFrame)]} {
		if {$PgAcVar(fdvar,attributeFrame) == $i} return
	}
	set PgAcVar(fdvar,attributeFrame) $i
	
	# Delete old widgets from the frame
	foreach wid [winfo children .pgaw:FormDesign:attributes.f] {
		destroy $wid
	}

	set row 0
	set base .pgaw:FormDesign:attributes.f
	grid columnconf $base 1 -weight 1

	set objclass $PgAcVar(fdobj,$i,class)

	# if i is zero, then the object is the form
	
	if {$i == 0} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg {Startup script}]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,command) \
	        -background #fefefe -borderwidth 1 -width 200 
		button $base.b$row \
	        -borderwidth 1 -padx 1 -pady 0 -text ... -command "
				Window show .pgaw:FormDesign:commands
				set PgAcVar(fdvar,commandFor) $i
				.pgaw:FormDesign:commands.f.txt delete 1.0 end
				.pgaw:FormDesign:commands.f.txt insert end \$PgAcVar(fdobj,$i,command)"
		grid $base.l$row \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 				
		grid $base.b$row \
	        -in $base -column 2 -row $row -columnspan 1 -rowspan 1 
		incr row
	}

	# does it have a text attribute ?
	if {[lsearch {button label radio checkbox} $objclass] > -1} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg Text]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,label) \
	        -background #fefefe -borderwidth 1 -width 200 
		bind $base.e$row <Key-Return> "Forms::design:set_text"
		grid $base.l$row \
			-in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 -sticky w 				
		incr row
	}

	# does it have a variable attribute ?
	if {[lsearch {button label radio checkbox entry} $objclass] > -1} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg Variable]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,variable) \
	        -background #fefefe -borderwidth 1 -width 200 
		grid $base.l$row \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 				
		incr row
	}

	# does it have a Command attribute ?
	if {[lsearch {button checkbox} $objclass] > -1} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg Command]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,command) \
	        -background #fefefe -borderwidth 1 -width 200 
		button $base.b$row \
	        -borderwidth 1 -padx 1 -pady 0 -text ... -command "
				Window show .pgaw:FormDesign:commands
				set PgAcVar(fdvar,commandFor) $i
				.pgaw:FormDesign:commands.f.txt delete 1.0 end
				.pgaw:FormDesign:commands.f.txt insert end \$PgAcVar(fdobj,$i,command)"
		grid $base.l$row \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 				
		grid $base.b$row \
	        -in $base -column 2 -row $row -columnspan 1 -rowspan 1 
		incr row
	}

	# does it have a value attribute ?
	if {[lsearch {radio checkbox} $objclass] > -1} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg Value]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,value) \
	        -background #fefefe -borderwidth 1 -width 200 
		grid $base.l$row \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 				
		incr row
	}

	# does it have fonts ?
	if {[lsearch {label button entry listbox text checkbox radio} $objclass] > -1} {
		label $base.lfont \
			-borderwidth 0 -text [intlmsg Font]
		grid $base.lfont \
			-in $base -column 0 -row $row -columnspan 1 -rowspan 1 -pady 2 -sticky w 
		entry $base.efont -textvariable PgAcVar(fdobj,$i,font) \
	        -background #fefefe -borderwidth 1 -width 200 
		bind $base.efont <Key-Return> "Forms::design:draw_object $i ; set PgAcVar(fdvar,dirty) 1"
		grid $base.efont \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 -sticky w 				
		menubutton $base.mbf \
    	    -borderwidth 1 -menu $base.mbf.m -padx 2 -pady 0 \
        	-text {...}  -font $PgAcVar(pref,font_normal) -relief raised
		menu $base.mbf.m \
			-borderwidth 1 -cursor {} -tearoff 0 -font $PgAcVar(pref,font_normal)
		foreach font {normal bold italic fixed} {
			$base.mbf.m add command \
    	    	-command "
    	    		set PgAcVar(fdobj,$i,font) $font
    	    		Forms::design:draw_object $i
    	    		set PgAcVar(fdvar,dirty) 1
    	    	" -label $font
		}
		grid $base.mbf \
			-in $base -column 2 -row $row -columnspan 1 -rowspan 1 -pady 2 -padx 2 -sticky w 
		incr row
	}

	# does it have colors ?
	if {[lsearch {label button radio checkbox entry listbox text} $objclass] > -1} {
		label $base.lcf \
	        -borderwidth 0 -text [intlmsg Foreground]
		label $base.scf \
	        -background $PgAcVar(fdobj,$i,fcolor) -borderwidth 1 -relief sunken -width 200 
		button $base.bcf \
			-command "set tempcolor \[tk_chooseColor -initialcolor $PgAcVar(fdobj,$i,fcolor) -title {Choose color}\] 
				if {\$tempcolor != {}} {
					set PgAcVar(fdobj,$i,fcolor) \$tempcolor
					$base.scf configure -background \$PgAcVar(fdobj,$i,fcolor)
					set PgAcVar(fdvar,dirty) 1
					Forms::design:draw_object $i
				}" \
	        -borderwidth 1 -padx 1 -pady 0 -text ... 
		grid $base.lcf \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.scf \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 
		grid $base.bcf \
	        -in $base -column 2 -row $row -columnspan 1 -rowspan 1 
		incr row
		label $base.lcb \
			-borderwidth 0 -text Background 
		label $base.scb \
			-background $PgAcVar(fdobj,$i,bcolor) -borderwidth 1 -relief sunken -width 200 
		button $base.bcb \
			-command "set tempcolor \[tk_chooseColor -initialcolor $PgAcVar(fdobj,$i,bcolor) -title {Choose color}\]
				if {\$tempcolor != {}} {
					set PgAcVar(fdobj,$i,bcolor) \$tempcolor
					$base.scb configure -background \$PgAcVar(fdobj,$i,bcolor)
					set PgAcVar(fdvar,dirty) 1
					Forms::design:draw_object $i
				}" \
			-borderwidth 1 -padx 1 -pady 0 -text ... 
		grid $base.lcb \
			-in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.scb \
			-in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 -sticky w 
		grid $base.bcb \
			-in $base -column 2 -row $row -columnspan 1 -rowspan 1
		incr row
	}

	# does it have border types ?
	if {[lsearch {label button entry listbox text} $objclass] > -1} {
		label $base.lrelief \
			-borderwidth 0 -text [intlmsg Relief]
		grid $base.lrelief \
			-in $base -column 0 -row $row -columnspan 1 -rowspan 1 -pady 2 -sticky w 
		menubutton $base.mb \
    	    -borderwidth 2 -menu $base.mb.m -padx 4 -pady 3 -width 100 -relief $PgAcVar(fdobj,$i,relief) \
        	-text groove -textvariable PgAcVar(fdobj,$i,relief) \
        	-font $PgAcVar(pref,font_normal)
		menu $base.mb.m \
			-borderwidth 1 -cursor {} -tearoff 0 -font $PgAcVar(pref,font_normal)
		foreach brdtype {raised sunken ridge groove flat} {
			$base.mb.m add command \
    	    	-command "
    	    		set PgAcVar(fdobj,$i,relief) $brdtype
    	    		$base.mb configure -relief \$PgAcVar(fdobj,$i,relief)
    	    		Forms::design:draw_object $i
    	    	" -label $brdtype
		}
		grid $base.mb \
			-in $base -column 1 -row $row -columnspan 1 -rowspan 1 -pady 2 -padx 2 -sticky w 
		incr row

	}

	# is it a DataControl ?
	if {$objclass == "query"} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg SQL]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,command) \
	        -background #fefefe -borderwidth 1 -width 200 
		grid $base.l$row \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 				
		incr row
	}

	# does it have a borderwidth attribute ?
	if {[lsearch {button label radio checkbox entry listbox text} $objclass] > -1} {
		label $base.l$row \
	        -borderwidth 0 -text [intlmsg {Border width}]
		entry $base.e$row -textvariable PgAcVar(fdobj,$i,borderwidth) \
	        -background #fefefe -borderwidth 1 -width 200 
		grid $base.l$row \
	        -in $base -column 0 -row $row -columnspan 1 -rowspan 1 -sticky w 
		grid $base.e$row \
	        -in $base -column 1 -row $row -columnspan 1 -rowspan 1 -padx 2 \
	        -sticky w 				
		incr row
	}


	# The last dummy label
	
	label $base.ldummy -text {} -borderwidth 0
	grid $base.ldummy -in $base -column 0 -row 100
	grid rowconf $base 100 -weight 1

}


proc {design:show_attributes} {i} {
global PgAcVar
	set objclass $PgAcVar(fdobj,$i,class)
	set PgAcVar(fdvar,c_class) $objclass
	design:createAttributesFrame $i
	set PgAcVar(fdvar,c_name) $PgAcVar(fdobj,$i,name)
	if {$i == 0} {
		# Object 0 is the form
		set c [split [winfo geometry .pgaw:FormDesign:draft] x+]
		set PgAcVar(fdvar,c_top) [lindex $c 3]
		set PgAcVar(fdvar,c_left) [lindex $c 2]
		set PgAcVar(fdvar,c_width) [lindex $c 0]
		set PgAcVar(fdvar,c_height) [lindex $c 1]
		return
	}
	set c $PgAcVar(fdobj,$i,coord)
	set PgAcVar(fdvar,c_top) [lindex $c 1]
	set PgAcVar(fdvar,c_left) [lindex $c 0]
	set PgAcVar(fdvar,c_width) [expr [lindex $c 2]-[lindex $c 0]]
	set PgAcVar(fdvar,c_height) [expr [lindex $c 3]-[lindex $c 1]]
}


proc {design:run} {} {
global PgAcVar CurrentDB DataControlVar
set base .$PgAcVar(fdobj,0,name)
if {[winfo exists $base]} {
   wm deiconify $base; return
}
toplevel $base -class Toplevel
wm focusmodel $base passive
wm geometry $base $PgAcVar(fdvar,geometry)
wm maxsize $base 785 570
wm minsize $base 1 1
wm overrideredirect $base 0
wm resizable $base 1 1
wm deiconify $base
wm title $base $PgAcVar(fdvar,formtitle)
foreach item $PgAcVar(fdvar,objlist) {
set coord $PgAcVar(fdobj,$item,coord)
set name $PgAcVar(fdobj,$item,name)
set wh "-width [expr 3+[lindex $coord 2]-[lindex $coord 0]]  -height [expr 3+[lindex $coord 3]-[lindex $coord 1]]"
set visual 1

set wfont $PgAcVar(fdobj,$item,font)
switch $wfont {
	{} {set wfont $PgAcVar(pref,font_normal)}
	normal  {set wfont $PgAcVar(pref,font_normal)}
	bold  {set wfont $PgAcVar(pref,font_bold)}
	italic  {set wfont $PgAcVar(pref,font_italic)}
	fixed  {set wfont $PgAcVar(pref,font_fix)}
}

namespace forget ::DataControl($base.$name)

# Checking if relief ridge or groove has borderwidth 2
if {[lsearch {ridge groove} $PgAcVar(fdobj,$item,relief)] != -1} {
	if {$PgAcVar(fdobj,$item,borderwidth) < 2} {
		set PgAcVar(fdobj,$item,borderwidth) 2
	}
}

# Checking if borderwidth is okay
if {[lsearch {0 1 2 3 4 5} $PgAcVar(fdobj,$item,borderwidth)] == -1} {
	set PgAcVar(fdobj,$item,borderwidth) 1
}

set cmd {}
catch {set cmd $PgAcVar(fdobj,$item,command)}

switch $PgAcVar(fdobj,$item,class) {
	button {
		button $base.$name  -borderwidth 1 -padx 0 -pady 0 -text "$PgAcVar(fdobj,$item,label)" \
		-fg $PgAcVar(fdobj,$item,fcolor) -bg $PgAcVar(fdobj,$item,bcolor) \
		-borderwidth $PgAcVar(fdobj,$item,borderwidth) \
		-relief $PgAcVar(fdobj,$item,relief) -font $wfont -command [subst {$cmd}]
		if {$PgAcVar(fdobj,$item,variable) != ""} {
			$base.$name configure -textvariable $PgAcVar(fdobj,$item,variable)
		}
	}
	checkbox {
		checkbutton  $base.$name -onvalue t -offvalue f -font $wfont \
		-fg $PgAcVar(fdobj,$item,fcolor) \
		-borderwidth $PgAcVar(fdobj,$item,borderwidth) \
		-command [subst {$cmd}] \
		-text "$PgAcVar(fdobj,$item,label)" -variable "$PgAcVar(fdobj,$item,variable)" -borderwidth 1
		set wh {}
	}
	query {
		set visual 0
		set DataControlVar($base.$name,sql) $PgAcVar(fdobj,$item,command)
		namespace eval ::DataControl($base.$name) "proc open {} {
			global CurrentDB DataControlVar
			variable tuples
			catch {unset tuples}
			set wn \[focus\] ; setCursor CLOCK
			set res \[wpg_exec \$CurrentDB \"\$DataControlVar($base.$name,sql)\"\]
			pg_result \$res -assign tuples
			set fl {}
			foreach fd \[pg_result \$res -lAttributes\] {lappend fl \[lindex \$fd 0\]}
			set DataControlVar($base.$name,fields) \$fl
			set DataControlVar($base.$name,recno) 0
			set DataControlVar($base.$name,nrecs) \[pg_result \$res -numTuples\]
			setCursor NORMAL
		}"
		namespace eval ::DataControl($base.$name) "proc setSQL {sqlcmd} {
			global DataControlVar
			set DataControlVar($base.$name,sql) \$sqlcmd
		}"
		namespace eval ::DataControl($base.$name) "proc getRowCount {} {
			global DataControlVar
			return \$DataControlVar($base.$name,nrecs)
		}"
		namespace eval ::DataControl($base.$name)  "proc getRowIndex {} {
			global DataControlVar
			return \$DataControlVar($base.$name,recno)
		}"
		namespace eval ::DataControl($base.$name)  "proc moveTo {newrecno} {
			global DataControlVar
			set DataControlVar($base.$name,recno) \$newrecno
		}"
		namespace eval ::DataControl($base.$name) "proc close {} {
			variable tuples
			catch {unset tuples}
		}"
		namespace eval ::DataControl($base.$name)  "proc getFieldList {} {
			global DataControlVar
			return \$DataControlVar($base.$name,fields)
		}"
		namespace eval ::DataControl($base.$name)  "proc fill {lb fld} {
			global DataControlVar
			variable tuples
			\$lb delete 0 end
			for {set i 0} {\$i<\$DataControlVar($base.$name,nrecs)} {incr i} {
				\$lb insert end \$tuples\(\$i,\$fld\)
			}
		}"
		namespace eval ::DataControl($base.$name)  "proc moveFirst {} {global DataControlVar ; set DataControlVar($base.$name,recno) 0}"
		namespace eval ::DataControl($base.$name)  "proc moveNext {} {global DataControlVar ; incr DataControlVar($base.$name,recno) ; if {\$DataControlVar($base.$name,recno)==\[getRowCount\]} {moveLast}}"
		namespace eval ::DataControl($base.$name)  "proc movePrevious {} {global DataControlVar ; incr DataControlVar($base.$name,recno) -1 ; if {\$DataControlVar($base.$name,recno)==-1} {moveFirst}}"
		namespace eval ::DataControl($base.$name)  "proc moveLast {} {global DataControlVar ; set DataControlVar($base.$name,recno) \[expr \[getRowCount\] -1\]}"
		namespace eval ::DataControl($base.$name)  "proc updateDataSet {} {\
			global DataControlVar
			global DataSet
			variable tuples
			set i \$DataControlVar($base.$name,recno)
			foreach fld \$DataControlVar($base.$name,fields) {
				catch {
					upvar DataSet\($base.$name,\$fld\) dbvar
					set dbvar \$tuples\(\$i,\$fld\)
				}
			}
		}"
		namespace eval ::DataControl($base.$name)  "proc clearDataSet {} {
			global DataControlVar
			global DataSet
			catch { foreach fld \$DataControlVar($base.$name,fields) {
				catch {
					upvar DataSet\($base.$name,\$fld\) dbvar
					set dbvar {}
				}
			}}
		}"
	}
	radio {
		radiobutton  $base.$name -font $wfont -text "$PgAcVar(fdobj,$item,label)" \
		-fg $PgAcVar(fdobj,$item,fcolor) -bg $PgAcVar(fdobj,$item,bcolor) -variable $PgAcVar(fdobj,$item,variable) \
		-value $PgAcVar(fdobj,$item,value) -borderwidth 1
		set wh {}
	}
	entry {
		set var {} ; catch {set var $PgAcVar(fdobj,$item,variable)}
		entry $base.$name -bg $PgAcVar(fdobj,$item,bcolor) -fg $PgAcVar(fdobj,$item,fcolor) \
		-borderwidth $PgAcVar(fdobj,$item,borderwidth) -font $wfont \
		-relief $PgAcVar(fdobj,$item,relief) -selectborderwidth 0  -highlightthickness 0 
		if {$var!=""} {$base.$name configure -textvar $var}
	}
	text {
		text $base.$name -fg $PgAcVar(fdobj,$item,fcolor) -bg $PgAcVar(fdobj,$item,bcolor) \
		-relief $PgAcVar(fdobj,$item,relief) -borderwidth $PgAcVar(fdobj,$item,borderwidth) \
		-font $wfont
	}
	label {
		# set wh {}
		label $base.$name -font $wfont -anchor nw -padx 0 -pady 0 -text $PgAcVar(fdobj,$item,label) \
		-borderwidth $PgAcVar(fdobj,$item,borderwidth) \
		-relief $PgAcVar(fdobj,$item,relief)  -fg $PgAcVar(fdobj,$item,fcolor) -bg $PgAcVar(fdobj,$item,bcolor) 
		set var {} ; catch {set var $PgAcVar(fdobj,$item,variable)}
		if {$var!=""} {$base.$name configure -textvar $var}
	}
	listbox {
		listbox $base.$name -bg $PgAcVar(fdobj,$item,bcolor)  -highlightthickness 0 -selectborderwidth 0 \
		-borderwidth $PgAcVar(fdobj,$item,borderwidth) -relief $PgAcVar(fdobj,$item,relief) \
		-fg $PgAcVar(fdobj,$item,fcolor) -bg $PgAcVar(fdobj,$item,bcolor) -font $wfont -yscrollcommand [subst {$base.sb$name set}]
		scrollbar $base.sb$name -borderwidth 1 -command [subst {$base.$name yview}] -orient vert  -highlightthickness 0
		eval [subst "place $base.sb$name -x [expr [lindex $coord 2]-14] -y [expr [lindex $coord 1]-1] -width 16 -height [expr 3+[lindex $coord 3]-[lindex $coord 1]] -anchor nw -bordermode ignore"]
	}
}
if $visual {eval [subst "place $base.$name  -x [expr [lindex $coord 0]-1] -y [expr [lindex $coord 1]-1] -anchor nw $wh -bordermode ignore"]}
}
if {$PgAcVar(fdobj,0,command) != ""} {
	uplevel #0 $PgAcVar(fdobj,0,command)
}
}

proc {design:close} {} {
global PgAcVar
	if {$PgAcVar(fdvar,dirty)} {
		if {[tk_messageBox -title [intlmsg Warning] -message [intlmsg "Do you want to save the form into the database?"] -type yesno -default yes]=="yes"} {
			if {[design:save $PgAcVar(fdvar,formtitle)]==0} {return}
		}
	}
	catch {Window destroy .pgaw:FormDesign:draft}
	catch {Window destroy .pgaw:FormDesign:toolbar}
	catch {Window destroy .pgaw:FormDesign:menu}
	catch {Window destroy .pgaw:FormDesign:attributes}
	catch {Window destroy .pgaw:FormDesign:commands}
	catch {Window destroy .$PgAcVar(fdobj,0,name)}
}

}

proc vTclWindow.pgaw:FormDesign:draft {base} {
	if {$base == ""} {
		set base .pgaw:FormDesign:draft
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 377x315+50+130
	wm maxsize $base 785 570
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base [intlmsg "Form design"]
	bind $base <Key-Delete> {
		Forms::design:delete_object
	}
	bind $base <Key-F1> "Help::load form_design"
	canvas $base.c \
		-background #a0a0a0 -height 207 -highlightthickness 0 -relief ridge \
		-selectborderwidth 0 -width 295 
	bind $base.c <Button-1> {
		Forms::design:mouse_down %x %y
	}
	bind $base.c <ButtonRelease-1> {
		Forms::design:mouse_up %x %y
	}
	bind $base.c <Motion> {
		Forms::design:mouse_move %x %y
	}
	pack $base.c \
		-in .pgaw:FormDesign:draft -anchor center -expand 1 -fill both -side top
}

proc vTclWindow.pgaw:FormDesign:attributes {base} {
	if {$base == ""} {
		set base .pgaw:FormDesign:attributes
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 237x300+461+221
	wm maxsize $base 785 570
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Attributes"]

	# The identification frame

	frame $base.fi \
        -borderwidth 2 -height 75 -relief groove -width 125 
	label $base.fi.lclass \
        -borderwidth 0 -text [intlmsg Class]
	entry $base.fi.eclass -textvariable PgAcVar(fdvar,c_class) \
        -borderwidth 1 -width 200 
	label $base.fi.lname \
        -borderwidth 0 -text [intlmsg Name]
	entry $base.fi.ename -textvariable PgAcVar(fdvar,c_name) \
        -background #fefefe -borderwidth 1 -width 200 
	bind $base.fi.ename <Key-Return> {
		Forms::design:set_name
	}


	# The geometry frame

	frame $base.fg \
        -borderwidth 2 -height 75 -relief groove -width 125 
	entry $base.fg.e1 -textvariable PgAcVar(fdvar,c_width) \
        -background #fefefe -borderwidth 1 -width 5 
	entry $base.fg.e2 -textvariable PgAcVar(fdvar,c_height) \
        -background #fefefe -borderwidth 1 -width 5 
	entry $base.fg.e3 -textvariable PgAcVar(fdvar,c_left) \
        -background #fefefe -borderwidth 1 -width 5 
	entry $base.fg.e4 -textvariable PgAcVar(fdvar,c_top) \
        -background #fefefe -borderwidth 1 -width 5 
	bind $base.fg.e1 <Key-Return> {
		Forms::design:change_coords
	}
	bind $base.fg.e2 <Key-Return> {
		Forms::design:change_coords
	}
	bind $base.fg.e3 <Key-Return> {
		Forms::design:change_coords
	}
	bind $base.fg.e4 <Key-Return> {
		Forms::design:change_coords
	}
	label $base.fg.l1 \
        -borderwidth 0 -text Width 
	label $base.fg.l2 \
        -borderwidth 0 -text Height 
	label $base.fg.l3 \
        -borderwidth 0 -text Left 
	label $base.fg.l4 \
        -borderwidth 0 -text Top 
	label $base.fg.lx1 \
        -borderwidth 0 -text x 
	label $base.fg.lp1 \
        -borderwidth 0 -text + 
	label $base.fg.lp2 \
        -borderwidth 0 -text + 

	# The frame for the rest of the attributes (dynamically generated)

	
	frame $base.f \
        -borderwidth 2 -height 75 -relief groove -width 125 


	# Geometry for "identification frame"


	place $base.fi \
        -x 5 -y 5 -width 230 -height 55 -anchor nw -bordermode ignore 
	grid columnconf $base.fi 1 -weight 1
	grid $base.fi.lclass \
        -in $base.fi -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.fi.eclass \
        -in $base.fi -column 1 -row 0 -columnspan 1 -rowspan 1 -padx 2 \
        -sticky w 
	grid $base.fi.lname \
        -in $base.fi -column 0 -row 1 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.fi.ename \
        -in $base.fi -column 1 -row 1 -columnspan 1 -rowspan 1 -padx 2 \
        -sticky w 



	# Geometry for "geometry frame"

	place $base.fg \
        -x 5 -y 60 -width 230 -height 45 -anchor nw -bordermode ignore 
	grid $base.fg.e1 \
        -in $base.fg -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fg.e2 \
        -in $base.fg -column 2 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fg.e3 \
        -in $base.fg -column 4 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fg.e4 \
        -in $base.fg -column 6 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fg.l1 \
        -in $base.fg -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.fg.l2 \
        -in $base.fg -column 2 -row 1 -columnspan 1 -rowspan 1 
	grid $base.fg.l3 \
        -in $base.fg -column 4 -row 1 -columnspan 1 -rowspan 1 
	grid $base.fg.l4 \
        -in $base.fg -column 6 -row 1 -columnspan 1 -rowspan 1 
	grid $base.fg.lx1 \
        -in $base.fg -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fg.lp1 \
        -in $base.fg -column 5 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fg.lp2 \
        -in $base.fg -column 3 -row 0 -columnspan 1 -rowspan 1 

	place $base.f -x 5 -y 105 -width 230 -height 190 -anchor nw

}


proc vTclWindow.pgaw:FormDesign:commands {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:FormDesign:commands
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 640x480+120+100
	wm maxsize $base 785 570
	wm minsize $base 1 19
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base [intlmsg "Command"]
	frame $base.f \
		-borderwidth 2 -height 75 -relief groove -width 125 
	scrollbar $base.f.sb \
		-borderwidth 1 -command {.pgaw:FormDesign:commands.f.txt yview} -orient vert -width 12 
	text $base.f.txt \
		-font $PgAcVar(pref,font_fix) -height 1 -tabs {20 40 60 80 100 120 140 160 180 200} \
		-width 200 -yscrollcommand {.pgaw:FormDesign:commands.f.sb set} 
	frame $base.fb \
		-height 75 -width 125 
	button $base.fb.b1 \
		-borderwidth 1 \
		-command {
			set PgAcVar(fdobj,$PgAcVar(fdvar,commandFor),command) [.pgaw:FormDesign:commands.f.txt get 1.0 "end - 1 chars"]
			Window hide .pgaw:FormDesign:commands
			set PgAcVar(fdvar,dirty) 1
		} -text [intlmsg Save] -width 5 
	button $base.fb.b2 \
		-borderwidth 1 -command {Window hide .pgaw:FormDesign:commands} \
		-text [intlmsg Cancel]
	pack $base.f \
		-in .pgaw:FormDesign:commands -anchor center -expand 1 -fill both -side top 
	pack $base.f.sb \
		-in .pgaw:FormDesign:commands.f -anchor e -expand 1 -fill y -side right 
	pack $base.f.txt \
		-in .pgaw:FormDesign:commands.f -anchor center -expand 1 -fill both -side top 
	pack $base.fb \
		-in .pgaw:FormDesign:commands -anchor center -expand 0 -fill none -side top 
	pack $base.fb.b1 \
		-in .pgaw:FormDesign:commands.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.b2 \
		-in .pgaw:FormDesign:commands.fb -anchor center -expand 0 -fill none -side top 
}

proc vTclWindow.pgaw:FormDesign:menu {base} {
	if {$base == ""} {
		set base .pgaw:FormDesign:menu
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 432x74+0+0
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Form designer"]
	frame $base.f1 \
		-height 75 -relief groove -width 125 
	label $base.f1.l1 \
		-borderwidth 0 -text "[intlmsg {Form name}] "
	entry $base.f1.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(fdvar,formtitle) 
	frame $base.f2 \
		-height 75 -relief groove -width 125 
	label $base.f2.l \
		-borderwidth 0 -text "[intlmsg {Form's window internal name}] "
	entry $base.f2.e \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(fdobj,0,name) 
	frame $base.f3 \
		-height 1 -width 125 
	button $base.f3.b1 \
		-command {set PgAcVar(fdvar,geometry) [wm geometry .pgaw:FormDesign:draft] ; Forms::design:run} -padx 1 \
		-text [intlmsg {Test form}]
	button $base.f3.b2 \
		-command {destroy .$PgAcVar(fdobj,0,name)} -padx 1 \
		-text [intlmsg {Close test form}]
	button $base.f3.b3 \
		-command {Forms::design:save nimic} -padx 1 -text [intlmsg Save]
	button $base.f3.b4 \
		-command {Forms::design:close} \
		-padx 1 -text [intlmsg Close]
	pack $base.f1 \
		-in .pgaw:FormDesign:menu -anchor center -expand 0 -fill x -pady 2 -side top 
	pack $base.f1.l1 \
		-in .pgaw:FormDesign:menu.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.e1 \
		-in .pgaw:FormDesign:menu.f1 -anchor center -expand 1 -fill x -side left 
	pack $base.f2 \
		-in .pgaw:FormDesign:menu -anchor center -expand 0 -fill x -pady 1 -side top 
	pack $base.f2.l \
		-in .pgaw:FormDesign:menu.f2 -anchor center -expand 0 -fill none -side left 
	pack $base.f2.e \
		-in .pgaw:FormDesign:menu.f2 -anchor center -expand 1 -fill x -side left 
	pack $base.f3 \
		-in .pgaw:FormDesign:menu -anchor center -expand 0 -fill x -pady 2 -side bottom 
	pack $base.f3.b1 \
		-in .pgaw:FormDesign:menu.f3 -anchor center -expand 0 -fill none -side left 
	pack $base.f3.b2 \
		-in .pgaw:FormDesign:menu.f3 -anchor center -expand 0 -fill none -side left 
	pack $base.f3.b3 \
		-in .pgaw:FormDesign:menu.f3 -anchor center -expand 0 -fill none -side left 
	pack $base.f3.b4 \
		-in .pgaw:FormDesign:menu.f3 -anchor center -expand 0 -fill none -side right 
}


proc vTclWindow.pgaw:FormDesign:toolbar {base} {
global PgAcVar
	foreach wid {button frame radiobutton checkbutton label text entry listbox query} {
		image create photo "icon_$wid"  -file [file join $PgAcVar(PGACCESS_HOME) images icon_$wid.gif] 
	}
	if {$base == ""} {
		set base .pgaw:FormDesign:toolbar
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel -menu .pgaw:FormDesign:toolbar.m17 
	wm focusmodel $base passive
	wm geometry $base 29x235+1+130
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Toolbar"]
	button $base.b1 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) button} -image icon_button \
		-padx 9 -pady 3 
	button $base.b3 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) radio} \
		-image icon_radiobutton -padx 9 -pady 3 
	button $base.b4 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) checkbox} \
		-image icon_checkbutton -padx 9 -pady 3 
	button $base.b5 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) label} -image icon_label \
		-padx 9 -pady 3 
	button $base.b6 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) text} -image icon_text \
		-padx 9 -pady 3 
	button $base.b7 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) entry} -image icon_entry \
		-padx 9 -pady 3 
	button $base.b8 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) listbox} -image icon_listbox \
		-padx 9 -pady 3 
	button $base.b9 \
		-borderwidth 1 -command {set PgAcVar(fdvar,tool) query} -height 21 \
		-image icon_query -padx 9 -pady 3 -width 20 
	grid $base.b1 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 2 -columnspan 1 -rowspan 1 
	grid $base.b3 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 4 -columnspan 1 -rowspan 1 
	grid $base.b4 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 5 -columnspan 1 -rowspan 1 
	grid $base.b5 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.b6 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 6 -columnspan 1 -rowspan 1 
	grid $base.b7 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.b8 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 7 -columnspan 1 -rowspan 1 
	grid $base.b9 \
		-in .pgaw:FormDesign:toolbar -column 0 -row 8 -columnspan 2 -rowspan 3 
}

