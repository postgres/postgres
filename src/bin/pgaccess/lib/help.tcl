namespace eval Help {

proc {findLink} {} {
	foreach tagname [.pgaw:Help.f.t tag names current] {
		if {$tagname!="link"} {
			load $tagname
			return
		}
	}
}


proc {load} {topic args} {
global PgAcVar
	if {![winfo exists .pgaw:Help]} {
		Window show .pgaw:Help
		tkwait visibility .pgaw:Help
	}
	wm deiconify .pgaw:Help
	if {![info exists PgAcVar(help,history)]} {
		set PgAcVar(help,history) {}
	}
	if {[llength $args]==1} {
		set PgAcVar(help,current_topic) [lindex $args 0]
		set PgAcVar(help,history) [lrange $PgAcVar(help,history) 0 [lindex $args 0]]
	} else {
		lappend PgAcVar(help,history) $topic
		set PgAcVar(help,current_topic) [expr {[llength $PgAcVar(help,history)]-1}]
	}
	# Limit the history length to 100 topics
	if {[llength $PgAcVar(help,history)]>100} {
		set PgAcVar(help,history) [lrange $PgAcVar(help,history) 1 end]
	}

	.pgaw:Help.f.t configure -state normal
	.pgaw:Help.f.t delete 1.0 end
	.pgaw:Help.f.t tag configure bold -font $PgAcVar(pref,font_bold)
	.pgaw:Help.f.t tag configure italic -font $PgAcVar(pref,font_italic)
	.pgaw:Help.f.t tag configure large -font {Helvetica -14 bold}
	.pgaw:Help.f.t tag configure title -font $PgAcVar(pref,font_bold) -justify center
	.pgaw:Help.f.t tag configure link -font {Helvetica -12 underline} -foreground #000080
	.pgaw:Help.f.t tag configure code -font $PgAcVar(pref,font_fix)
	.pgaw:Help.f.t tag configure warning -font $PgAcVar(pref,font_bold) -foreground #800000
	.pgaw:Help.f.t tag bind link <Button-1> {Help::findLink}
	set errmsg {}
	.pgaw:Help.f.t configure -tabs {30 60 90 120 150 180 210 240 270 300 330 360 390}
	catch { source [file join $PgAcVar(PGACCESS_HOME) lib help $topic.hlp] } errmsg
	if {$errmsg!=""} {
		.pgaw:Help.f.t insert end "Error loading help file [file join $PgAcVar(PGACCESS_HOME) $topic.hlp]\n\n$errmsg" bold
	}
	.pgaw:Help.f.t configure -state disabled
	focus .pgaw:Help.f.sb
}

proc {back} {} {
global PgAcVar
	if {![info exists PgAcVar(help,history)]} {return}
	if {[llength $PgAcVar(help,history)]==0} {return}
	set i $PgAcVar(help,current_topic)
	if {$i<1} {return}
	incr i -1
	load [lindex $PgAcVar(help,history) $i] $i
}


}

proc vTclWindow.pgaw:Help {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:Help
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	set sw [winfo screenwidth .]
	set sh [winfo screenheight .]
	set x [expr {($sw - 640)/2}]
	set y [expr {($sh - 480)/2}] 
	wm geometry $base 640x480+$x+$y
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base [intlmsg "Help"]
	bind $base <Key-Escape> "Window destroy .pgaw:Help"
	frame $base.fb \
		-borderwidth 2 -height 75 -relief groove -width 125 
	button $base.fb.bback \
		-command Help::back -padx 9 -pady 3 -text [intlmsg Back]
	button $base.fb.bi \
		-command {Help::load index} -padx 9 -pady 3 -text [intlmsg Index]
	button $base.fb.bp \
		-command {Help::load postgresql} -padx 9 -pady 3 -text PostgreSQL 
	button $base.fb.btnclose \
		-command {Window destroy .pgaw:Help} -padx 9 -pady 3 -text [intlmsg Close]
	frame $base.f \
		-borderwidth 2 -height 75 -relief groove -width 125 
	text $base.f.t \
		-borderwidth 1 -cursor {} -font $PgAcVar(pref,font_normal) -height 2 \
		-highlightthickness 0 -state disabled \
		-tabs {30 60 90 120 150 180 210 240 270 300 330 360 390} -width 8 \
		-wrap word -yscrollcommand {.pgaw:Help.f.sb set} 
	scrollbar $base.f.sb \
		-borderwidth 1 -command {.pgaw:Help.f.t yview} -highlightthickness 0 \
		-orient vert 
	pack $base.fb \
		-in .pgaw:Help -anchor center -expand 0 -fill x -side top 
	pack $base.fb.bback \
		-in .pgaw:Help.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.bi \
		-in .pgaw:Help.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.bp \
		-in .pgaw:Help.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.btnclose \
		-in .pgaw:Help.fb -anchor center -expand 0 -fill none -side right 
	pack $base.f \
		-in .pgaw:Help -anchor center -expand 1 -fill both -side top 
	pack $base.f.t \
		-in .pgaw:Help.f -anchor center -expand 1 -fill both -side left 
	pack $base.f.sb \
		-in .pgaw:Help.f -anchor center -expand 0 -fill y -side right 
}

