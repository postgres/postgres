# Microsoft Visual C++ Generated NMAKE File, Format Version 2.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

!IF "$(CFG)" == ""
CFG=Win32 Debug
!MESSAGE No configuration specified.  Defaulting to Win32 Debug.
!ENDIF 

!IF "$(CFG)" != "Win32 Release" && "$(CFG)" != "Win32 Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE on this makefile
!MESSAGE by defining the macro CFG on the command line.  For example:
!MESSAGE 
!MESSAGE NMAKE /f "pglite.mak" CFG="Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

################################################################################
# Begin Project
# PROP Target_Last_Scanned "Win32 Debug"
CPP=cl.exe
RSC=rc.exe

!IF  "$(CFG)" == "Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "WinRel"
# PROP BASE Intermediate_Dir "WinRel"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "g:\users\forrest\pglite"
# PROP Intermediate_Dir "g:\users\forrest\pglite"
OUTDIR=g:\users\forrest\pglite
INTDIR=g:\users\forrest\pglite

ALL : $(OUTDIR)/pglite.exe $(OUTDIR)/pglite.bsc

$(OUTDIR) : 
    if not exist $(OUTDIR)/nul mkdir $(OUTDIR)

# ADD BASE CPP /nologo /W3 /GX /YX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /FR /c
# ADD CPP /nologo /W3 /GX /YX /Od /I "g:\pglite\src\backend\include" /I "g:\pglite\src\backend" /I "g:\pglite\src\backend\port\win32" /I "g:\pglite\src\backend\obj" /D "NDEBUG" /D "WIN32" /D "_CONSOLE" /D "__STDC__" /D "_POSIX_" /c
# SUBTRACT CPP /Fr
CPP_PROJ=/nologo /W3 /GX /YX /Od /I "g:\pglite\src\backend\include" /I\
 "g:\pglite\src\backend" /I "g:\pglite\src\backend\port\win32" /I\
 "g:\pglite\src\backend\obj" /D "NDEBUG" /D "WIN32" /D "_CONSOLE" /D "__STDC__"\
 /Fp$(OUTDIR)/"pglite.pch" /Fo$(INTDIR)/ /D "_POSIX_"  /c 
CPP_OBJS=g:\users\forrest\pglite/
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o$(OUTDIR)/"pglite.bsc" 
BSC32_SBRS= \
	

$(OUTDIR)/pglite.bsc : $(OUTDIR)  $(BSC32_SBRS)
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /NOLOGO /SUBSYSTEM:console /MACHINE:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /NOLOGO /SUBSYSTEM:console /MACHINE:I386
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /NOLOGO /SUBSYSTEM:console /INCREMENTAL:no\
 /PDB:$(OUTDIR)/"pglite.pdb" /MACHINE:I386 /OUT:$(OUTDIR)/"pglite.exe"
DEF_FILE=
LINK32_OBJS= \
	$(INTDIR)/scankey.obj \
	$(INTDIR)/printtup.obj \
	$(INTDIR)/indexvalid.obj \
	$(INTDIR)/heaptuple.obj \
	$(INTDIR)/tupdesc.obj \
	$(INTDIR)/indextuple.obj \
	$(INTDIR)/heapvalid.obj \
	$(INTDIR)/hashinsert.obj \
	$(INTDIR)/hashstrat.obj \
	$(INTDIR)/hashutil.obj \
	$(INTDIR)/hashpage.obj \
	$(INTDIR)/hashsearch.obj \
	$(INTDIR)/hashscan.obj \
	$(INTDIR)/hashfunc.obj \
	$(INTDIR)/hash.obj \
	$(INTDIR)/hashovfl.obj \
	$(INTDIR)/bootstrap.obj \
	$(INTDIR)/genam.obj \
	$(INTDIR)/creatinh.obj \
	$(INTDIR)/nodeSeqscan.obj \
	$(INTDIR)/nodeUnique.obj \
	$(INTDIR)/rename.obj \
	$(INTDIR)/transsup.obj \
	$(INTDIR)/transam.obj \
	$(INTDIR)/define.obj \
	$(INTDIR)/execMain.obj \
	$(INTDIR)/xid.obj \
	$(INTDIR)/nodeAgg.obj \
	$(INTDIR)/nbtpage.obj \
	$(INTDIR)/execScan.obj \
	$(INTDIR)/nbtree.obj \
	$(INTDIR)/rtscan.obj \
	$(INTDIR)/indexam.obj \
	$(INTDIR)/execQual.obj \
	$(INTDIR)/nodeHash.obj \
	$(INTDIR)/nbtscan.obj \
	$(INTDIR)/hio.obj \
	$(INTDIR)/pg_proc.obj \
	$(INTDIR)/stats.obj \
	$(INTDIR)/nodeMaterial.obj \
	$(INTDIR)/varsup.obj \
	$(INTDIR)/copy.obj \
	$(INTDIR)/rtproc.obj \
	$(INTDIR)/functions.obj \
	$(INTDIR)/nodeHashjoin.obj \
	$(INTDIR)/catalog.obj \
	$(INTDIR)/nbtinsert.obj \
	$(INTDIR)/rtree.obj \
	$(INTDIR)/version.obj \
	$(INTDIR)/async.obj \
	$(INTDIR)/nbtutils.obj \
	$(INTDIR)/vacuum.obj \
	$(INTDIR)/rtstrat.obj \
	$(INTDIR)/execFlatten.obj \
	$(INTDIR)/nodeTee.obj \
	$(INTDIR)/nodeIndexscan.obj \
	$(INTDIR)/remove.obj \
	$(INTDIR)/indexing.obj \
	$(INTDIR)/command.obj \
	$(INTDIR)/nbtsearch.obj \
	$(INTDIR)/heapam.obj \
	$(INTDIR)/nodeSort.obj \
	$(INTDIR)/execProcnode.obj \
	$(INTDIR)/nodeResult.obj \
	$(INTDIR)/index.obj \
	$(INTDIR)/xact.obj \
	$(INTDIR)/nodeMergejoin.obj \
	$(INTDIR)/pg_operator.obj \
	$(INTDIR)/execJunk.obj \
	$(INTDIR)/pg_aggregate.obj \
	$(INTDIR)/istrat.obj \
	$(INTDIR)/execUtils.obj \
	$(INTDIR)/purge.obj \
	$(INTDIR)/heap.obj \
	$(INTDIR)/nbtstrat.obj \
	$(INTDIR)/execAmi.obj \
	$(INTDIR)/execTuples.obj \
	$(INTDIR)/pg_type.obj \
	$(INTDIR)/view.obj \
	$(INTDIR)/nodeAppend.obj \
	$(INTDIR)/defind.obj \
	$(INTDIR)/nodeNestloop.obj \
	$(INTDIR)/nbtcompare.obj \
	$(INTDIR)/rtget.obj \
	$(INTDIR)/catalog_utils.obj \
	$(INTDIR)/setrefs.obj \
	$(INTDIR)/mergeutils.obj \
	$(INTDIR)/oset.obj \
	$(INTDIR)/arrayutils.obj \
	$(INTDIR)/nodeFuncs.obj \
	$(INTDIR)/rewriteSupport.obj \
	$(INTDIR)/bufpage.obj \
	$(INTDIR)/fd.obj \
	$(INTDIR)/clauseinfo.obj \
	$(INTDIR)/nabstime.obj \
	$(INTDIR)/mcxt.obj \
	$(INTDIR)/ipci.obj \
	$(INTDIR)/qsort.obj \
	$(INTDIR)/outfuncs.obj \
	$(INTDIR)/tqual.obj \
	$(INTDIR)/keys.obj \
	$(INTDIR)/clauses.obj \
	$(INTDIR)/print.obj \
	$(INTDIR)/postinit.obj \
	$(INTDIR)/oidchar16.obj \
	$(INTDIR)/name.obj \
	$(INTDIR)/tid.obj \
	$(INTDIR)/"be-fsstubs.obj" \
	$(INTDIR)/elog.obj \
	$(INTDIR)/bufmgr.obj \
	$(INTDIR)/portalbuf.obj \
	$(INTDIR)/psort.obj \
	$(INTDIR)/syscache.obj \
	$(INTDIR)/exc.obj \
	$(INTDIR)/selfuncs.obj \
	$(INTDIR)/var.obj \
	$(INTDIR)/oid.obj \
	$(INTDIR)/"be-pqexec.obj" \
	$(INTDIR)/ordering.obj \
	$(INTDIR)/inv_api.obj \
	$(INTDIR)/buf_table.obj \
	$(INTDIR)/acl.obj \
	$(INTDIR)/costsize.obj \
	$(INTDIR)/catcache.obj \
	$(INTDIR)/rewriteRemove.obj \
	$(INTDIR)/parse_query.obj \
	$(INTDIR)/excabort.obj \
	$(INTDIR)/lmgr.obj \
	$(INTDIR)/excid.obj \
	$(INTDIR)/int.obj \
	$(INTDIR)/auth.obj \
	$(INTDIR)/regexp.obj \
	$(INTDIR)/proc.obj \
	$(INTDIR)/dbcommands.obj \
	$(INTDIR)/dynahash.obj \
	$(INTDIR)/shmem.obj \
	$(INTDIR)/relnode.obj \
	$(INTDIR)/fstack.obj \
	$(INTDIR)/smgr.obj \
	$(INTDIR)/magic.obj \
	$(INTDIR)/relcache.obj \
	$(INTDIR)/varlena.obj \
	$(INTDIR)/allpaths.obj \
	$(INTDIR)/portalmem.obj \
	$(INTDIR)/bit.obj \
	$(INTDIR)/readfuncs.obj \
	$(INTDIR)/nodes.obj \
	$(INTDIR)/chunk.obj \
	$(INTDIR)/datum.obj \
	$(INTDIR)/analyze.obj \
	$(INTDIR)/oidint4.obj \
	$(INTDIR)/hasht.obj \
	$(INTDIR)/numutils.obj \
	$(INTDIR)/pqcomm.obj \
	$(INTDIR)/indxpath.obj \
	$(INTDIR)/lispsort.obj \
	$(INTDIR)/arrayfuncs.obj \
	$(INTDIR)/copyfuncs.obj \
	$(INTDIR)/planmain.obj \
	$(INTDIR)/makefuncs.obj \
	$(INTDIR)/lsyscache.obj \
	$(INTDIR)/multi.obj \
	$(INTDIR)/freelist.obj \
	$(INTDIR)/aclchk.obj \
	$(INTDIR)/initsplan.obj \
	$(INTDIR)/prune.obj \
	$(INTDIR)/sinvaladt.obj \
	$(INTDIR)/orindxpath.obj \
	$(INTDIR)/joinrels.obj \
	$(INTDIR)/rewriteManip.obj \
	$(INTDIR)/itemptr.obj \
	$(INTDIR)/s_lock.obj \
	$(INTDIR)/miscinit.obj \
	$(INTDIR)/postgres.obj \
	$(INTDIR)/parser.obj \
	$(INTDIR)/tlist.obj \
	$(INTDIR)/dt.obj \
	$(INTDIR)/sinval.obj \
	$(INTDIR)/pqpacket.obj \
	$(INTDIR)/assert.obj \
	$(INTDIR)/utility.obj \
	$(INTDIR)/bool.obj \
	$(INTDIR)/md.obj \
	$(INTDIR)/pqsignal.obj \
	$(INTDIR)/globals.obj \
	$(INTDIR)/postmaster.obj \
	$(INTDIR)/joinpath.obj \
	$(INTDIR)/fastpath.obj \
	$(INTDIR)/archive.obj \
	$(INTDIR)/fcache.obj \
	$(INTDIR)/mm.obj \
	$(INTDIR)/createplan.obj \
	$(INTDIR)/read.obj \
	$(INTDIR)/stringinfo.obj \
	$(INTDIR)/hashfn.obj \
	$(INTDIR)/regproc.obj \
	$(INTDIR)/main.obj \
	$(INTDIR)/enbl.obj \
	$(INTDIR)/prepunion.obj \
	$(INTDIR)/prepqual.obj \
	$(INTDIR)/planner.obj \
	$(INTDIR)/clausesel.obj \
	$(INTDIR)/portal.obj \
	$(INTDIR)/spin.obj \
	$(INTDIR)/lock.obj \
	$(INTDIR)/single.obj \
	$(INTDIR)/io.obj \
	$(INTDIR)/"geo-ops.obj" \
	$(INTDIR)/dest.obj \
	$(INTDIR)/rewriteDefine.obj \
	$(INTDIR)/keywords.obj \
	$(INTDIR)/hashutils.obj \
	$(INTDIR)/format.obj \
	$(INTDIR)/scanner.obj \
	$(INTDIR)/aset.obj \
	$(INTDIR)/"geo-selfuncs.obj" \
	$(INTDIR)/float.obj \
	$(INTDIR)/pquery.obj \
	$(INTDIR)/"be-dumpdata.obj" \
	$(INTDIR)/filename.obj \
	$(INTDIR)/misc.obj \
	$(INTDIR)/pathnode.obj \
	$(INTDIR)/inval.obj \
	$(INTDIR)/smgrtype.obj \
	$(INTDIR)/joininfo.obj \
	$(INTDIR)/lselect.obj \
	$(INTDIR)/rel.obj \
	$(INTDIR)/internal.obj \
	$(INTDIR)/preptlist.obj \
	$(INTDIR)/joinutils.obj \
	$(INTDIR)/shmqueue.obj \
	$(INTDIR)/date.obj \
	$(INTDIR)/locks.obj \
	$(INTDIR)/not_in.obj \
	$(INTDIR)/char.obj \
	$(INTDIR)/rewriteHandler.obj \
	$(INTDIR)/sets.obj \
	$(INTDIR)/palloc.obj \
	$(INTDIR)/indexnode.obj \
	$(INTDIR)/equalfuncs.obj \
	$(INTDIR)/oidint2.obj \
	$(INTDIR)/list.obj \
	$(INTDIR)/plancat.obj \
	$(INTDIR)/fmgr.obj \
	$(INTDIR)/fmgrtab.obj \
	$(INTDIR)/dllist.obj \
	$(INTDIR)/nodeGroup.obj \
	$(INTDIR)/localbuf.obj \
	$(INTDIR)/cluster.obj \
	$(INTDIR)/ipc.obj \
	$(INTDIR)/nt.obj \
	$(INTDIR)/getopt.obj \
	$(INTDIR)/bootscanner.obj \
	$(INTDIR)/scan.obj \
	$(INTDIR)/bootparse.obj \
	$(INTDIR)/gram.obj \
	$(INTDIR)/findbe.obj \
	$(INTDIR)/regerror.obj \
	$(INTDIR)/regfree.obj \
	$(INTDIR)/regcomp.obj \
	$(INTDIR)/regexec.obj \
	$(INTDIR)/nbtsort.obj \
	$(INTDIR)/buf_init.obj \
	$(INTDIR)/dfmgr.obj

$(OUTDIR)/pglite.exe : $(OUTDIR)  $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "WinDebug"
# PROP BASE Intermediate_Dir "WinDebug"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "d:\local\forrest\pglite"
# PROP Intermediate_Dir "d:\local\forrest\pglite"
OUTDIR=d:\local\forrest\pglite
INTDIR=d:\local\forrest\pglite

ALL : $(OUTDIR)/pglite.exe $(OUTDIR)/pglite.bsc

$(OUTDIR) : 
    if not exist $(OUTDIR)/nul mkdir $(OUTDIR)

# ADD BASE CPP /nologo /W3 /GX /Zi /YX /Od /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /FR /c
# ADD CPP /nologo /G5 /W3 /GX /Zi /YX /Od /I "g:\pglite\src\backend" /I "g:\pglite\src\backend\port\win32" /I "g:\pglite\src\backend\obj" /I "g:\pglite\src\backend\include" /I "g:\pglite\src\backend\port/win32/regex" /D "_DEBUG" /D "WIN32" /D "_CONSOLE" /D "__STDC__" /D "_POSIX_" /D "_NTSDK" /D "NO_SECURITY" /D "NEED_RUSAGE" /FR /c
CPP_PROJ=/nologo /G5 /W3 /GX /Zi /YX /Od /I "g:\pglite\src\backend" /I\
 "g:\pglite\src\backend\port\win32" /I "g:\pglite\src\backend\obj" /I\
 "g:\pglite\src\backend\include" /I "g:\pglite\src\backend\port/win32/regex" /D\
 "_DEBUG" /D "WIN32" /D "_CONSOLE" /D "__STDC__" /D "_POSIX_" /D "_NTSDK" /D\
 "NO_SECURITY" /D "NEED_RUSAGE" /FR$(INTDIR)/ /Fp$(OUTDIR)/"pglite.pch"\
 /Fo$(INTDIR)/ /Fd$(OUTDIR)/"pglite.pdb" /c 
CPP_OBJS=d:\local\forrest\pglite/
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o$(OUTDIR)/"pglite.bsc" 
BSC32_SBRS= \
	$(INTDIR)/scankey.sbr \
	$(INTDIR)/printtup.sbr \
	$(INTDIR)/indexvalid.sbr \
	$(INTDIR)/heaptuple.sbr \
	$(INTDIR)/tupdesc.sbr \
	$(INTDIR)/indextuple.sbr \
	$(INTDIR)/heapvalid.sbr \
	$(INTDIR)/hashinsert.sbr \
	$(INTDIR)/hashstrat.sbr \
	$(INTDIR)/hashutil.sbr \
	$(INTDIR)/hashpage.sbr \
	$(INTDIR)/hashsearch.sbr \
	$(INTDIR)/hashscan.sbr \
	$(INTDIR)/hashfunc.sbr \
	$(INTDIR)/hash.sbr \
	$(INTDIR)/hashovfl.sbr \
	$(INTDIR)/bootstrap.sbr \
	$(INTDIR)/genam.sbr \
	$(INTDIR)/creatinh.sbr \
	$(INTDIR)/nodeSeqscan.sbr \
	$(INTDIR)/nodeUnique.sbr \
	$(INTDIR)/rename.sbr \
	$(INTDIR)/transsup.sbr \
	$(INTDIR)/transam.sbr \
	$(INTDIR)/define.sbr \
	$(INTDIR)/execMain.sbr \
	$(INTDIR)/xid.sbr \
	$(INTDIR)/nodeAgg.sbr \
	$(INTDIR)/nbtpage.sbr \
	$(INTDIR)/execScan.sbr \
	$(INTDIR)/nbtree.sbr \
	$(INTDIR)/rtscan.sbr \
	$(INTDIR)/indexam.sbr \
	$(INTDIR)/execQual.sbr \
	$(INTDIR)/nodeHash.sbr \
	$(INTDIR)/nbtscan.sbr \
	$(INTDIR)/hio.sbr \
	$(INTDIR)/pg_proc.sbr \
	$(INTDIR)/stats.sbr \
	$(INTDIR)/nodeMaterial.sbr \
	$(INTDIR)/varsup.sbr \
	$(INTDIR)/copy.sbr \
	$(INTDIR)/rtproc.sbr \
	$(INTDIR)/functions.sbr \
	$(INTDIR)/nodeHashjoin.sbr \
	$(INTDIR)/catalog.sbr \
	$(INTDIR)/nbtinsert.sbr \
	$(INTDIR)/rtree.sbr \
	$(INTDIR)/version.sbr \
	$(INTDIR)/async.sbr \
	$(INTDIR)/nbtutils.sbr \
	$(INTDIR)/vacuum.sbr \
	$(INTDIR)/rtstrat.sbr \
	$(INTDIR)/execFlatten.sbr \
	$(INTDIR)/nodeTee.sbr \
	$(INTDIR)/nodeIndexscan.sbr \
	$(INTDIR)/remove.sbr \
	$(INTDIR)/indexing.sbr \
	$(INTDIR)/command.sbr \
	$(INTDIR)/nbtsearch.sbr \
	$(INTDIR)/heapam.sbr \
	$(INTDIR)/nodeSort.sbr \
	$(INTDIR)/execProcnode.sbr \
	$(INTDIR)/nodeResult.sbr \
	$(INTDIR)/index.sbr \
	$(INTDIR)/xact.sbr \
	$(INTDIR)/nodeMergejoin.sbr \
	$(INTDIR)/pg_operator.sbr \
	$(INTDIR)/execJunk.sbr \
	$(INTDIR)/pg_aggregate.sbr \
	$(INTDIR)/istrat.sbr \
	$(INTDIR)/execUtils.sbr \
	$(INTDIR)/purge.sbr \
	$(INTDIR)/heap.sbr \
	$(INTDIR)/nbtstrat.sbr \
	$(INTDIR)/execAmi.sbr \
	$(INTDIR)/execTuples.sbr \
	$(INTDIR)/pg_type.sbr \
	$(INTDIR)/view.sbr \
	$(INTDIR)/nodeAppend.sbr \
	$(INTDIR)/defind.sbr \
	$(INTDIR)/nodeNestloop.sbr \
	$(INTDIR)/nbtcompare.sbr \
	$(INTDIR)/rtget.sbr \
	$(INTDIR)/catalog_utils.sbr \
	$(INTDIR)/setrefs.sbr \
	$(INTDIR)/mergeutils.sbr \
	$(INTDIR)/oset.sbr \
	$(INTDIR)/arrayutils.sbr \
	$(INTDIR)/nodeFuncs.sbr \
	$(INTDIR)/rewriteSupport.sbr \
	$(INTDIR)/bufpage.sbr \
	$(INTDIR)/fd.sbr \
	$(INTDIR)/clauseinfo.sbr \
	$(INTDIR)/nabstime.sbr \
	$(INTDIR)/mcxt.sbr \
	$(INTDIR)/ipci.sbr \
	$(INTDIR)/qsort.sbr \
	$(INTDIR)/outfuncs.sbr \
	$(INTDIR)/tqual.sbr \
	$(INTDIR)/keys.sbr \
	$(INTDIR)/clauses.sbr \
	$(INTDIR)/print.sbr \
	$(INTDIR)/postinit.sbr \
	$(INTDIR)/oidchar16.sbr \
	$(INTDIR)/name.sbr \
	$(INTDIR)/tid.sbr \
	$(INTDIR)/"be-fsstubs.sbr" \
	$(INTDIR)/elog.sbr \
	$(INTDIR)/bufmgr.sbr \
	$(INTDIR)/portalbuf.sbr \
	$(INTDIR)/psort.sbr \
	$(INTDIR)/syscache.sbr \
	$(INTDIR)/exc.sbr \
	$(INTDIR)/selfuncs.sbr \
	$(INTDIR)/var.sbr \
	$(INTDIR)/oid.sbr \
	$(INTDIR)/"be-pqexec.sbr" \
	$(INTDIR)/ordering.sbr \
	$(INTDIR)/inv_api.sbr \
	$(INTDIR)/buf_table.sbr \
	$(INTDIR)/acl.sbr \
	$(INTDIR)/costsize.sbr \
	$(INTDIR)/catcache.sbr \
	$(INTDIR)/rewriteRemove.sbr \
	$(INTDIR)/parse_query.sbr \
	$(INTDIR)/excabort.sbr \
	$(INTDIR)/lmgr.sbr \
	$(INTDIR)/excid.sbr \
	$(INTDIR)/int.sbr \
	$(INTDIR)/auth.sbr \
	$(INTDIR)/regexp.sbr \
	$(INTDIR)/proc.sbr \
	$(INTDIR)/dbcommands.sbr \
	$(INTDIR)/dynahash.sbr \
	$(INTDIR)/shmem.sbr \
	$(INTDIR)/relnode.sbr \
	$(INTDIR)/fstack.sbr \
	$(INTDIR)/smgr.sbr \
	$(INTDIR)/magic.sbr \
	$(INTDIR)/relcache.sbr \
	$(INTDIR)/varlena.sbr \
	$(INTDIR)/allpaths.sbr \
	$(INTDIR)/portalmem.sbr \
	$(INTDIR)/bit.sbr \
	$(INTDIR)/readfuncs.sbr \
	$(INTDIR)/nodes.sbr \
	$(INTDIR)/chunk.sbr \
	$(INTDIR)/datum.sbr \
	$(INTDIR)/analyze.sbr \
	$(INTDIR)/oidint4.sbr \
	$(INTDIR)/hasht.sbr \
	$(INTDIR)/numutils.sbr \
	$(INTDIR)/pqcomm.sbr \
	$(INTDIR)/indxpath.sbr \
	$(INTDIR)/lispsort.sbr \
	$(INTDIR)/arrayfuncs.sbr \
	$(INTDIR)/copyfuncs.sbr \
	$(INTDIR)/planmain.sbr \
	$(INTDIR)/makefuncs.sbr \
	$(INTDIR)/lsyscache.sbr \
	$(INTDIR)/multi.sbr \
	$(INTDIR)/freelist.sbr \
	$(INTDIR)/aclchk.sbr \
	$(INTDIR)/initsplan.sbr \
	$(INTDIR)/prune.sbr \
	$(INTDIR)/sinvaladt.sbr \
	$(INTDIR)/orindxpath.sbr \
	$(INTDIR)/joinrels.sbr \
	$(INTDIR)/rewriteManip.sbr \
	$(INTDIR)/itemptr.sbr \
	$(INTDIR)/s_lock.sbr \
	$(INTDIR)/miscinit.sbr \
	$(INTDIR)/postgres.sbr \
	$(INTDIR)/parser.sbr \
	$(INTDIR)/tlist.sbr \
	$(INTDIR)/dt.sbr \
	$(INTDIR)/sinval.sbr \
	$(INTDIR)/pqpacket.sbr \
	$(INTDIR)/assert.sbr \
	$(INTDIR)/utility.sbr \
	$(INTDIR)/bool.sbr \
	$(INTDIR)/md.sbr \
	$(INTDIR)/pqsignal.sbr \
	$(INTDIR)/globals.sbr \
	$(INTDIR)/postmaster.sbr \
	$(INTDIR)/joinpath.sbr \
	$(INTDIR)/fastpath.sbr \
	$(INTDIR)/archive.sbr \
	$(INTDIR)/fcache.sbr \
	$(INTDIR)/mm.sbr \
	$(INTDIR)/createplan.sbr \
	$(INTDIR)/read.sbr \
	$(INTDIR)/stringinfo.sbr \
	$(INTDIR)/hashfn.sbr \
	$(INTDIR)/regproc.sbr \
	$(INTDIR)/main.sbr \
	$(INTDIR)/enbl.sbr \
	$(INTDIR)/prepunion.sbr \
	$(INTDIR)/prepqual.sbr \
	$(INTDIR)/planner.sbr \
	$(INTDIR)/clausesel.sbr \
	$(INTDIR)/portal.sbr \
	$(INTDIR)/spin.sbr \
	$(INTDIR)/lock.sbr \
	$(INTDIR)/single.sbr \
	$(INTDIR)/io.sbr \
	$(INTDIR)/"geo-ops.sbr" \
	$(INTDIR)/dest.sbr \
	$(INTDIR)/rewriteDefine.sbr \
	$(INTDIR)/keywords.sbr \
	$(INTDIR)/hashutils.sbr \
	$(INTDIR)/format.sbr \
	$(INTDIR)/scanner.sbr \
	$(INTDIR)/aset.sbr \
	$(INTDIR)/"geo-selfuncs.sbr" \
	$(INTDIR)/float.sbr \
	$(INTDIR)/pquery.sbr \
	$(INTDIR)/"be-dumpdata.sbr" \
	$(INTDIR)/filename.sbr \
	$(INTDIR)/misc.sbr \
	$(INTDIR)/pathnode.sbr \
	$(INTDIR)/inval.sbr \
	$(INTDIR)/smgrtype.sbr \
	$(INTDIR)/joininfo.sbr \
	$(INTDIR)/lselect.sbr \
	$(INTDIR)/rel.sbr \
	$(INTDIR)/internal.sbr \
	$(INTDIR)/preptlist.sbr \
	$(INTDIR)/joinutils.sbr \
	$(INTDIR)/shmqueue.sbr \
	$(INTDIR)/date.sbr \
	$(INTDIR)/locks.sbr \
	$(INTDIR)/not_in.sbr \
	$(INTDIR)/char.sbr \
	$(INTDIR)/rewriteHandler.sbr \
	$(INTDIR)/sets.sbr \
	$(INTDIR)/palloc.sbr \
	$(INTDIR)/indexnode.sbr \
	$(INTDIR)/equalfuncs.sbr \
	$(INTDIR)/oidint2.sbr \
	$(INTDIR)/list.sbr \
	$(INTDIR)/plancat.sbr \
	$(INTDIR)/fmgr.sbr \
	$(INTDIR)/fmgrtab.sbr \
	$(INTDIR)/dllist.sbr \
	$(INTDIR)/nodeGroup.sbr \
	$(INTDIR)/localbuf.sbr \
	$(INTDIR)/cluster.sbr \
	$(INTDIR)/ipc.sbr \
	$(INTDIR)/nt.sbr \
	$(INTDIR)/getopt.sbr \
	$(INTDIR)/bootscanner.sbr \
	$(INTDIR)/scan.sbr \
	$(INTDIR)/bootparse.sbr \
	$(INTDIR)/gram.sbr \
	$(INTDIR)/findbe.sbr \
	$(INTDIR)/regerror.sbr \
	$(INTDIR)/regfree.sbr \
	$(INTDIR)/regcomp.sbr \
	$(INTDIR)/regexec.sbr \
	$(INTDIR)/nbtsort.sbr \
	$(INTDIR)/buf_init.sbr \
	$(INTDIR)/dfmgr.sbr

$(OUTDIR)/pglite.bsc : $(OUTDIR)  $(BSC32_SBRS)
    $(BSC32) @<<
  $(BSC32_FLAGS) $(BSC32_SBRS)
<<

LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /NOLOGO /SUBSYSTEM:console /DEBUG /MACHINE:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib wsock32.lib /NOLOGO /SUBSYSTEM:console /DEBUG /MACHINE:I386
# SUBTRACT LINK32 /PDB:none
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib wsock32.lib /NOLOGO /SUBSYSTEM:console /INCREMENTAL:yes\
 /PDB:$(OUTDIR)/"pglite.pdb" /DEBUG /MACHINE:I386 /OUT:$(OUTDIR)/"pglite.exe"
DEF_FILE=
LINK32_OBJS= \
	$(INTDIR)/scankey.obj \
	$(INTDIR)/printtup.obj \
	$(INTDIR)/indexvalid.obj \
	$(INTDIR)/heaptuple.obj \
	$(INTDIR)/tupdesc.obj \
	$(INTDIR)/indextuple.obj \
	$(INTDIR)/heapvalid.obj \
	$(INTDIR)/hashinsert.obj \
	$(INTDIR)/hashstrat.obj \
	$(INTDIR)/hashutil.obj \
	$(INTDIR)/hashpage.obj \
	$(INTDIR)/hashsearch.obj \
	$(INTDIR)/hashscan.obj \
	$(INTDIR)/hashfunc.obj \
	$(INTDIR)/hash.obj \
	$(INTDIR)/hashovfl.obj \
	$(INTDIR)/bootstrap.obj \
	$(INTDIR)/genam.obj \
	$(INTDIR)/creatinh.obj \
	$(INTDIR)/nodeSeqscan.obj \
	$(INTDIR)/nodeUnique.obj \
	$(INTDIR)/rename.obj \
	$(INTDIR)/transsup.obj \
	$(INTDIR)/transam.obj \
	$(INTDIR)/define.obj \
	$(INTDIR)/execMain.obj \
	$(INTDIR)/xid.obj \
	$(INTDIR)/nodeAgg.obj \
	$(INTDIR)/nbtpage.obj \
	$(INTDIR)/execScan.obj \
	$(INTDIR)/nbtree.obj \
	$(INTDIR)/rtscan.obj \
	$(INTDIR)/indexam.obj \
	$(INTDIR)/execQual.obj \
	$(INTDIR)/nodeHash.obj \
	$(INTDIR)/nbtscan.obj \
	$(INTDIR)/hio.obj \
	$(INTDIR)/pg_proc.obj \
	$(INTDIR)/stats.obj \
	$(INTDIR)/nodeMaterial.obj \
	$(INTDIR)/varsup.obj \
	$(INTDIR)/copy.obj \
	$(INTDIR)/rtproc.obj \
	$(INTDIR)/functions.obj \
	$(INTDIR)/nodeHashjoin.obj \
	$(INTDIR)/catalog.obj \
	$(INTDIR)/nbtinsert.obj \
	$(INTDIR)/rtree.obj \
	$(INTDIR)/version.obj \
	$(INTDIR)/async.obj \
	$(INTDIR)/nbtutils.obj \
	$(INTDIR)/vacuum.obj \
	$(INTDIR)/rtstrat.obj \
	$(INTDIR)/execFlatten.obj \
	$(INTDIR)/nodeTee.obj \
	$(INTDIR)/nodeIndexscan.obj \
	$(INTDIR)/remove.obj \
	$(INTDIR)/indexing.obj \
	$(INTDIR)/command.obj \
	$(INTDIR)/nbtsearch.obj \
	$(INTDIR)/heapam.obj \
	$(INTDIR)/nodeSort.obj \
	$(INTDIR)/execProcnode.obj \
	$(INTDIR)/nodeResult.obj \
	$(INTDIR)/index.obj \
	$(INTDIR)/xact.obj \
	$(INTDIR)/nodeMergejoin.obj \
	$(INTDIR)/pg_operator.obj \
	$(INTDIR)/execJunk.obj \
	$(INTDIR)/pg_aggregate.obj \
	$(INTDIR)/istrat.obj \
	$(INTDIR)/execUtils.obj \
	$(INTDIR)/purge.obj \
	$(INTDIR)/heap.obj \
	$(INTDIR)/nbtstrat.obj \
	$(INTDIR)/execAmi.obj \
	$(INTDIR)/execTuples.obj \
	$(INTDIR)/pg_type.obj \
	$(INTDIR)/view.obj \
	$(INTDIR)/nodeAppend.obj \
	$(INTDIR)/defind.obj \
	$(INTDIR)/nodeNestloop.obj \
	$(INTDIR)/nbtcompare.obj \
	$(INTDIR)/rtget.obj \
	$(INTDIR)/catalog_utils.obj \
	$(INTDIR)/setrefs.obj \
	$(INTDIR)/mergeutils.obj \
	$(INTDIR)/oset.obj \
	$(INTDIR)/arrayutils.obj \
	$(INTDIR)/nodeFuncs.obj \
	$(INTDIR)/rewriteSupport.obj \
	$(INTDIR)/bufpage.obj \
	$(INTDIR)/fd.obj \
	$(INTDIR)/clauseinfo.obj \
	$(INTDIR)/nabstime.obj \
	$(INTDIR)/mcxt.obj \
	$(INTDIR)/ipci.obj \
	$(INTDIR)/qsort.obj \
	$(INTDIR)/outfuncs.obj \
	$(INTDIR)/tqual.obj \
	$(INTDIR)/keys.obj \
	$(INTDIR)/clauses.obj \
	$(INTDIR)/print.obj \
	$(INTDIR)/postinit.obj \
	$(INTDIR)/oidchar16.obj \
	$(INTDIR)/name.obj \
	$(INTDIR)/tid.obj \
	$(INTDIR)/"be-fsstubs.obj" \
	$(INTDIR)/elog.obj \
	$(INTDIR)/bufmgr.obj \
	$(INTDIR)/portalbuf.obj \
	$(INTDIR)/psort.obj \
	$(INTDIR)/syscache.obj \
	$(INTDIR)/exc.obj \
	$(INTDIR)/selfuncs.obj \
	$(INTDIR)/var.obj \
	$(INTDIR)/oid.obj \
	$(INTDIR)/"be-pqexec.obj" \
	$(INTDIR)/ordering.obj \
	$(INTDIR)/inv_api.obj \
	$(INTDIR)/buf_table.obj \
	$(INTDIR)/acl.obj \
	$(INTDIR)/costsize.obj \
	$(INTDIR)/catcache.obj \
	$(INTDIR)/rewriteRemove.obj \
	$(INTDIR)/parse_query.obj \
	$(INTDIR)/excabort.obj \
	$(INTDIR)/lmgr.obj \
	$(INTDIR)/excid.obj \
	$(INTDIR)/int.obj \
	$(INTDIR)/auth.obj \
	$(INTDIR)/regexp.obj \
	$(INTDIR)/proc.obj \
	$(INTDIR)/dbcommands.obj \
	$(INTDIR)/dynahash.obj \
	$(INTDIR)/shmem.obj \
	$(INTDIR)/relnode.obj \
	$(INTDIR)/fstack.obj \
	$(INTDIR)/smgr.obj \
	$(INTDIR)/magic.obj \
	$(INTDIR)/relcache.obj \
	$(INTDIR)/varlena.obj \
	$(INTDIR)/allpaths.obj \
	$(INTDIR)/portalmem.obj \
	$(INTDIR)/bit.obj \
	$(INTDIR)/readfuncs.obj \
	$(INTDIR)/nodes.obj \
	$(INTDIR)/chunk.obj \
	$(INTDIR)/datum.obj \
	$(INTDIR)/analyze.obj \
	$(INTDIR)/oidint4.obj \
	$(INTDIR)/hasht.obj \
	$(INTDIR)/numutils.obj \
	$(INTDIR)/pqcomm.obj \
	$(INTDIR)/indxpath.obj \
	$(INTDIR)/lispsort.obj \
	$(INTDIR)/arrayfuncs.obj \
	$(INTDIR)/copyfuncs.obj \
	$(INTDIR)/planmain.obj \
	$(INTDIR)/makefuncs.obj \
	$(INTDIR)/lsyscache.obj \
	$(INTDIR)/multi.obj \
	$(INTDIR)/freelist.obj \
	$(INTDIR)/aclchk.obj \
	$(INTDIR)/initsplan.obj \
	$(INTDIR)/prune.obj \
	$(INTDIR)/sinvaladt.obj \
	$(INTDIR)/orindxpath.obj \
	$(INTDIR)/joinrels.obj \
	$(INTDIR)/rewriteManip.obj \
	$(INTDIR)/itemptr.obj \
	$(INTDIR)/s_lock.obj \
	$(INTDIR)/miscinit.obj \
	$(INTDIR)/postgres.obj \
	$(INTDIR)/parser.obj \
	$(INTDIR)/tlist.obj \
	$(INTDIR)/dt.obj \
	$(INTDIR)/sinval.obj \
	$(INTDIR)/pqpacket.obj \
	$(INTDIR)/assert.obj \
	$(INTDIR)/utility.obj \
	$(INTDIR)/bool.obj \
	$(INTDIR)/md.obj \
	$(INTDIR)/pqsignal.obj \
	$(INTDIR)/globals.obj \
	$(INTDIR)/postmaster.obj \
	$(INTDIR)/joinpath.obj \
	$(INTDIR)/fastpath.obj \
	$(INTDIR)/archive.obj \
	$(INTDIR)/fcache.obj \
	$(INTDIR)/mm.obj \
	$(INTDIR)/createplan.obj \
	$(INTDIR)/read.obj \
	$(INTDIR)/stringinfo.obj \
	$(INTDIR)/hashfn.obj \
	$(INTDIR)/regproc.obj \
	$(INTDIR)/main.obj \
	$(INTDIR)/enbl.obj \
	$(INTDIR)/prepunion.obj \
	$(INTDIR)/prepqual.obj \
	$(INTDIR)/planner.obj \
	$(INTDIR)/clausesel.obj \
	$(INTDIR)/portal.obj \
	$(INTDIR)/spin.obj \
	$(INTDIR)/lock.obj \
	$(INTDIR)/single.obj \
	$(INTDIR)/io.obj \
	$(INTDIR)/"geo-ops.obj" \
	$(INTDIR)/dest.obj \
	$(INTDIR)/rewriteDefine.obj \
	$(INTDIR)/keywords.obj \
	$(INTDIR)/hashutils.obj \
	$(INTDIR)/format.obj \
	$(INTDIR)/scanner.obj \
	$(INTDIR)/aset.obj \
	$(INTDIR)/"geo-selfuncs.obj" \
	$(INTDIR)/float.obj \
	$(INTDIR)/pquery.obj \
	$(INTDIR)/"be-dumpdata.obj" \
	$(INTDIR)/filename.obj \
	$(INTDIR)/misc.obj \
	$(INTDIR)/pathnode.obj \
	$(INTDIR)/inval.obj \
	$(INTDIR)/smgrtype.obj \
	$(INTDIR)/joininfo.obj \
	$(INTDIR)/lselect.obj \
	$(INTDIR)/rel.obj \
	$(INTDIR)/internal.obj \
	$(INTDIR)/preptlist.obj \
	$(INTDIR)/joinutils.obj \
	$(INTDIR)/shmqueue.obj \
	$(INTDIR)/date.obj \
	$(INTDIR)/locks.obj \
	$(INTDIR)/not_in.obj \
	$(INTDIR)/char.obj \
	$(INTDIR)/rewriteHandler.obj \
	$(INTDIR)/sets.obj \
	$(INTDIR)/palloc.obj \
	$(INTDIR)/indexnode.obj \
	$(INTDIR)/equalfuncs.obj \
	$(INTDIR)/oidint2.obj \
	$(INTDIR)/list.obj \
	$(INTDIR)/plancat.obj \
	$(INTDIR)/fmgr.obj \
	$(INTDIR)/fmgrtab.obj \
	$(INTDIR)/dllist.obj \
	$(INTDIR)/nodeGroup.obj \
	$(INTDIR)/localbuf.obj \
	$(INTDIR)/cluster.obj \
	$(INTDIR)/ipc.obj \
	$(INTDIR)/nt.obj \
	$(INTDIR)/getopt.obj \
	$(INTDIR)/bootscanner.obj \
	$(INTDIR)/scan.obj \
	$(INTDIR)/bootparse.obj \
	$(INTDIR)/gram.obj \
	$(INTDIR)/findbe.obj \
	$(INTDIR)/regerror.obj \
	$(INTDIR)/regfree.obj \
	$(INTDIR)/regcomp.obj \
	$(INTDIR)/regexec.obj \
	$(INTDIR)/nbtsort.obj \
	$(INTDIR)/buf_init.obj \
	$(INTDIR)/dfmgr.obj

$(OUTDIR)/pglite.exe : $(OUTDIR)  $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ENDIF 

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

################################################################################
# Begin Group "Source Files"

################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\scankey.c

$(INTDIR)/scankey.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\printtup.c

$(INTDIR)/printtup.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\indexvalid.c

$(INTDIR)/indexvalid.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\heaptuple.c

$(INTDIR)/heaptuple.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\tupdesc.c

$(INTDIR)/tupdesc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\indextuple.c

$(INTDIR)/indextuple.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\common\heapvalid.c

$(INTDIR)/heapvalid.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashinsert.c

$(INTDIR)/hashinsert.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashstrat.c

$(INTDIR)/hashstrat.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashutil.c

$(INTDIR)/hashutil.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashpage.c

$(INTDIR)/hashpage.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashsearch.c

$(INTDIR)/hashsearch.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashscan.c

$(INTDIR)/hashscan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashfunc.c

$(INTDIR)/hashfunc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hash.c

$(INTDIR)/hash.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\hash\hashovfl.c

$(INTDIR)/hashovfl.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\bootstrap\bootstrap.c

$(INTDIR)/bootstrap.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\index\genam.c

$(INTDIR)/genam.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\creatinh.c

$(INTDIR)/creatinh.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeSeqscan.c

$(INTDIR)/nodeSeqscan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeUnique.c

$(INTDIR)/nodeUnique.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\rename.c

$(INTDIR)/rename.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\transam\transsup.c

$(INTDIR)/transsup.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\transam\transam.c

$(INTDIR)/transam.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\define.c

$(INTDIR)/define.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execMain.c

$(INTDIR)/execMain.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\transam\xid.c

$(INTDIR)/xid.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeAgg.c

$(INTDIR)/nodeAgg.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtpage.c

$(INTDIR)/nbtpage.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execScan.c

$(INTDIR)/execScan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtree.c

$(INTDIR)/nbtree.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\rtree\rtscan.c

$(INTDIR)/rtscan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\index\indexam.c

$(INTDIR)/indexam.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execQual.c

$(INTDIR)/execQual.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeHash.c

$(INTDIR)/nodeHash.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtscan.c

$(INTDIR)/nbtscan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\heap\hio.c

$(INTDIR)/hio.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\pg_proc.c

$(INTDIR)/pg_proc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\heap\stats.c

$(INTDIR)/stats.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeMaterial.c

$(INTDIR)/nodeMaterial.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\transam\varsup.c

$(INTDIR)/varsup.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\copy.c

$(INTDIR)/copy.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\rtree\rtproc.c

$(INTDIR)/rtproc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\functions.c

$(INTDIR)/functions.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeHashjoin.c

$(INTDIR)/nodeHashjoin.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\catalog.c

$(INTDIR)/catalog.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtinsert.c

$(INTDIR)/nbtinsert.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\rtree\rtree.c

$(INTDIR)/rtree.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\version.c

$(INTDIR)/version.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\async.c

$(INTDIR)/async.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtutils.c

$(INTDIR)/nbtutils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\vacuum.c

$(INTDIR)/vacuum.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\rtree\rtstrat.c

$(INTDIR)/rtstrat.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execFlatten.c

$(INTDIR)/execFlatten.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeTee.c

$(INTDIR)/nodeTee.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeIndexscan.c

$(INTDIR)/nodeIndexscan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\remove.c

$(INTDIR)/remove.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\indexing.c

$(INTDIR)/indexing.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\command.c

$(INTDIR)/command.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtsearch.c

$(INTDIR)/nbtsearch.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\heap\heapam.c

$(INTDIR)/heapam.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeSort.c

$(INTDIR)/nodeSort.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execProcnode.c

$(INTDIR)/execProcnode.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeResult.c

$(INTDIR)/nodeResult.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\index.c

$(INTDIR)/index.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\transam\xact.c

$(INTDIR)/xact.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeMergejoin.c

$(INTDIR)/nodeMergejoin.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\pg_operator.c

$(INTDIR)/pg_operator.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execJunk.c

$(INTDIR)/execJunk.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\pg_aggregate.c

$(INTDIR)/pg_aggregate.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\index\istrat.c

$(INTDIR)/istrat.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execUtils.c

$(INTDIR)/execUtils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\purge.c

$(INTDIR)/purge.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\heap.c

$(INTDIR)/heap.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtstrat.c

$(INTDIR)/nbtstrat.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execAmi.c

$(INTDIR)/execAmi.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\execTuples.c

$(INTDIR)/execTuples.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\catalog\pg_type.c

$(INTDIR)/pg_type.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\view.c

$(INTDIR)/view.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeAppend.c

$(INTDIR)/nodeAppend.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\defind.c

$(INTDIR)/defind.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeNestloop.c

$(INTDIR)/nodeNestloop.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtcompare.c

$(INTDIR)/nbtcompare.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\rtree\rtget.c

$(INTDIR)/rtget.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\catalog_utils.c

$(INTDIR)/catalog_utils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\plan\setrefs.c

$(INTDIR)/setrefs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\mergeutils.c

$(INTDIR)/mergeutils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\mmgr\oset.c

$(INTDIR)/oset.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\arrayutils.c

$(INTDIR)/arrayutils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\nodeFuncs.c

$(INTDIR)/nodeFuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\rewrite\rewriteSupport.c

$(INTDIR)/rewriteSupport.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\page\bufpage.c

$(INTDIR)/bufpage.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\file\fd.c

$(INTDIR)/fd.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\clauseinfo.c

$(INTDIR)/clauseinfo.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\nabstime.c

$(INTDIR)/nabstime.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\mmgr\mcxt.c

$(INTDIR)/mcxt.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\ipci.c

$(INTDIR)/ipci.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\qsort.c

$(INTDIR)/qsort.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\outfuncs.c

$(INTDIR)/outfuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\time\tqual.c

$(INTDIR)/tqual.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\keys.c

$(INTDIR)/keys.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\clauses.c

$(INTDIR)/clauses.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\print.c

$(INTDIR)/print.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\init\postinit.c

$(INTDIR)/postinit.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\oidchar16.c

$(INTDIR)/oidchar16.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\name.c

$(INTDIR)/name.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\tid.c

$(INTDIR)/tid.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE="G:\pglite\src\backend\libpq\be-fsstubs.c"

$(INTDIR)/"be-fsstubs.obj" :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\error\elog.c

$(INTDIR)/elog.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\buffer\bufmgr.c

$(INTDIR)/bufmgr.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\libpq\portalbuf.c

$(INTDIR)/portalbuf.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\sort\psort.c

$(INTDIR)/psort.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\syscache.c

$(INTDIR)/syscache.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\error\exc.c

$(INTDIR)/exc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\selfuncs.c

$(INTDIR)/selfuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\var.c

$(INTDIR)/var.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\oid.c

$(INTDIR)/oid.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE="G:\pglite\src\backend\libpq\be-pqexec.c"

$(INTDIR)/"be-pqexec.obj" :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\ordering.c

$(INTDIR)/ordering.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\large_object\inv_api.c

$(INTDIR)/inv_api.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\buffer\buf_table.c

$(INTDIR)/buf_table.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\acl.c

$(INTDIR)/acl.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\costsize.c

$(INTDIR)/costsize.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\catcache.c

$(INTDIR)/catcache.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\rewrite\rewriteRemove.c

$(INTDIR)/rewriteRemove.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\parse_query.c

$(INTDIR)/parse_query.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\error\excabort.c

$(INTDIR)/excabort.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\lmgr\lmgr.c

$(INTDIR)/lmgr.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\error\excid.c

$(INTDIR)/excid.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\int.c

$(INTDIR)/int.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\libpq\auth.c

$(INTDIR)/auth.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\regexp.c

$(INTDIR)/regexp.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\lmgr\proc.c

$(INTDIR)/proc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\dbcommands.c

$(INTDIR)/dbcommands.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\hash\dynahash.c

$(INTDIR)/dynahash.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\shmem.c

$(INTDIR)/shmem.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\relnode.c

$(INTDIR)/relnode.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\fstack.c

$(INTDIR)/fstack.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\smgr\smgr.c

$(INTDIR)/smgr.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\init\magic.c

$(INTDIR)/magic.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\relcache.c

$(INTDIR)/relcache.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\varlena.c

$(INTDIR)/varlena.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\allpaths.c

$(INTDIR)/allpaths.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\mmgr\portalmem.c

$(INTDIR)/portalmem.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\bit.c

$(INTDIR)/bit.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\readfuncs.c

$(INTDIR)/readfuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\nodes.c

$(INTDIR)/nodes.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\chunk.c

$(INTDIR)/chunk.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\datum.c

$(INTDIR)/datum.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\analyze.c

$(INTDIR)/analyze.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\oidint4.c

$(INTDIR)/oidint4.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\hasht.c

$(INTDIR)/hasht.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\numutils.c

$(INTDIR)/numutils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\libpq\pqcomm.c

$(INTDIR)/pqcomm.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\indxpath.c

$(INTDIR)/indxpath.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\lispsort.c

$(INTDIR)/lispsort.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\arrayfuncs.c

$(INTDIR)/arrayfuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\copyfuncs.c

$(INTDIR)/copyfuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\plan\planmain.c

$(INTDIR)/planmain.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\makefuncs.c

$(INTDIR)/makefuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\lsyscache.c

$(INTDIR)/lsyscache.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\lmgr\multi.c

$(INTDIR)/multi.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\buffer\freelist.c

$(INTDIR)/freelist.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\tcop\aclchk.c

$(INTDIR)/aclchk.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\plan\initsplan.c

$(INTDIR)/initsplan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\prune.c

$(INTDIR)/prune.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\sinvaladt.c

$(INTDIR)/sinvaladt.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\orindxpath.c

$(INTDIR)/orindxpath.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\joinrels.c

$(INTDIR)/joinrels.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\rewrite\rewriteManip.c

$(INTDIR)/rewriteManip.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\page\itemptr.c

$(INTDIR)/itemptr.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\s_lock.c

$(INTDIR)/s_lock.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\init\miscinit.c

$(INTDIR)/miscinit.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\tcop\postgres.c

$(INTDIR)/postgres.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\parser.c

$(INTDIR)/parser.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\tlist.c

$(INTDIR)/tlist.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\dt.c

$(INTDIR)/dt.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\sinval.c

$(INTDIR)/sinval.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\libpq\pqpacket.c

$(INTDIR)/pqpacket.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\error\assert.c

$(INTDIR)/assert.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\tcop\utility.c

$(INTDIR)/utility.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\bool.c

$(INTDIR)/bool.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\smgr\md.c

$(INTDIR)/md.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\libpq\pqsignal.c

$(INTDIR)/pqsignal.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\init\globals.c

$(INTDIR)/globals.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\postmaster\postmaster.c

$(INTDIR)/postmaster.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\joinpath.c

$(INTDIR)/joinpath.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\tcop\fastpath.c

$(INTDIR)/fastpath.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\prep\archive.c

$(INTDIR)/archive.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\fcache.c

$(INTDIR)/fcache.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\smgr\mm.c

$(INTDIR)/mm.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\plan\createplan.c

$(INTDIR)/createplan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\read.c

$(INTDIR)/read.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\stringinfo.c

$(INTDIR)/stringinfo.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\hash\hashfn.c

$(INTDIR)/hashfn.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\regproc.c

$(INTDIR)/regproc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\main\main.c

$(INTDIR)/main.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\init\enbl.c

$(INTDIR)/enbl.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\prep\prepunion.c

$(INTDIR)/prepunion.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\prep\prepqual.c

$(INTDIR)/prepqual.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\plan\planner.c

$(INTDIR)/planner.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\clausesel.c

$(INTDIR)/clausesel.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\libpq\portal.c

$(INTDIR)/portal.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\spin.c

$(INTDIR)/spin.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\lmgr\lock.c

$(INTDIR)/lock.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\lmgr\single.c

$(INTDIR)/single.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\io.c

$(INTDIR)/io.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE="G:\pglite\src\backend\utils\adt\geo-ops.c"

$(INTDIR)/"geo-ops.obj" :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\tcop\dest.c

$(INTDIR)/dest.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\rewrite\rewriteDefine.c

$(INTDIR)/rewriteDefine.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\keywords.c

$(INTDIR)/keywords.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\hashutils.c

$(INTDIR)/hashutils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\error\format.c

$(INTDIR)/format.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\parser\scanner.c

$(INTDIR)/scanner.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\mmgr\aset.c

$(INTDIR)/aset.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE="G:\pglite\src\backend\utils\adt\geo-selfuncs.c"

$(INTDIR)/"geo-selfuncs.obj" :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\float.c

$(INTDIR)/float.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\tcop\pquery.c

$(INTDIR)/pquery.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE="G:\pglite\src\backend\libpq\be-dumpdata.c"

$(INTDIR)/"be-dumpdata.obj" :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\filename.c

$(INTDIR)/filename.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\misc.c

$(INTDIR)/misc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\pathnode.c

$(INTDIR)/pathnode.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\inval.c

$(INTDIR)/inval.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\smgr\smgrtype.c

$(INTDIR)/smgrtype.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\joininfo.c

$(INTDIR)/joininfo.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\sort\lselect.c

$(INTDIR)/lselect.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\cache\rel.c

$(INTDIR)/rel.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\internal.c

$(INTDIR)/internal.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\prep\preptlist.c

$(INTDIR)/preptlist.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\path\joinutils.c

$(INTDIR)/joinutils.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\shmqueue.c

$(INTDIR)/shmqueue.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\date.c

$(INTDIR)/date.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\rewrite\locks.c

$(INTDIR)/locks.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\not_in.c

$(INTDIR)/not_in.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\char.c

$(INTDIR)/char.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\rewrite\rewriteHandler.c

$(INTDIR)/rewriteHandler.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\sets.c

$(INTDIR)/sets.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\mmgr\palloc.c

$(INTDIR)/palloc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\indexnode.c

$(INTDIR)/indexnode.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\equalfuncs.c

$(INTDIR)/equalfuncs.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\adt\oidint2.c

$(INTDIR)/oidint2.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\nodes\list.c

$(INTDIR)/list.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\optimizer\util\plancat.c

$(INTDIR)/plancat.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\fmgr\fmgr.c

$(INTDIR)/fmgr.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\obj\fmgrtab.c

$(INTDIR)/fmgrtab.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\lib\dllist.c

$(INTDIR)/dllist.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\executor\nodeGroup.c

$(INTDIR)/nodeGroup.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\buffer\localbuf.c

$(INTDIR)/localbuf.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\commands\cluster.c

$(INTDIR)/cluster.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\ipc\ipc.c

$(INTDIR)/ipc.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\port\win32\nt.c

$(INTDIR)/nt.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\port\win32\getopt.c

$(INTDIR)/getopt.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\obj\bootscanner.c

$(INTDIR)/bootscanner.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\obj\scan.c

$(INTDIR)/scan.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\obj\bootparse.c

$(INTDIR)/bootparse.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\obj\gram.c

$(INTDIR)/gram.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\init\findbe.c

$(INTDIR)/findbe.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\port\win32\regex\regerror.c

$(INTDIR)/regerror.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\port\win32\regex\regfree.c

$(INTDIR)/regfree.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\port\win32\regex\regcomp.c

$(INTDIR)/regcomp.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\port\win32\regex\regexec.c

$(INTDIR)/regexec.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\access\nbtree\nbtsort.c

$(INTDIR)/nbtsort.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\storage\buffer\buf_init.c

$(INTDIR)/buf_init.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
################################################################################
# Begin Source File

SOURCE=G:\pglite\src\backend\utils\fmgr\dfmgr.c

$(INTDIR)/dfmgr.obj :  $(SOURCE)  $(INTDIR)
   $(CPP) $(CPP_PROJ)  $(SOURCE) 

# End Source File
# End Group
# End Project
################################################################################
