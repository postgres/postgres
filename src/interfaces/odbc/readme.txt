
Readme for psqlodbc.dll                                                  04/04/2001
-----------------------------------------------------------------------------------
Precompiled binaries for Win32 are available from ftp://ftp.postgresql.org/pub/odbc


I.  Building the Driver from the source code

This section describes how to build the PostgreSQL ODBC Driver (psqlodbc.dll).
Microsoft Visual C++ version 4.0 or higher is required. Other compilers may work 
but have not been formally tested. The psqlodbc.dll may be built either in the 
VC++ IDE or from the command line:

IDE Method
----------

1.  Create a new project workspace with the type DLL.  For the name, type in the
    name "psqlodbc".

2.  The above step creates the directory "psqlodbc" under the 
    "\<Visual C++ top level directory>\projects" path to hold the source files.
    (example, \msdev\projects\psqlodbc).  Now, either unzip the source code release
    into this directory or just copy all the files into this directory.

3.  Insert all of the source files (*.c, *.h, *.rc, *.def) into the Visual project
    using the "Insert files into project" command.  You may have to do 2 inserts --
    the first to get the 'c' and header files, and the second to get the def file.
    Don't forget the .def file since it is an important part of the release.
    You can even insert ".txt" files into the projects -- they will do nothing.
	
4.  Add the "wsock32.lib" library to the end of the list of libraries for linking
    using the Build settings menu.
	
5.  Select the type of build on the toolbar (i.e., Release or Debug).  This is
    one of the useful features of the visual c++ environment in that you can
    browse the entire project if you build the "Debug" release.  For release
    purposes however, select "Release" build.

6.  Build the dll by selecting Build from the build menu.

7.  When complete, the "psqlodbc.dll" file is under the "Release" subdirectory.
    (i.e., "\msdev\projects\psqlodbc\release\psqlodbc.dll")

Command Line Method
-------------------

1.  From a command prompt, CD to the directory containing the source code.

2.  Use NMAKE to build the dll eg:

    C:\psqlodbc\> nmake /f win32.mak CFG=Release ALL

    Possible configurations are Release, Debug, MultiByteRelease or MultiByteDebug
    Possible build types are ALL or CLEAN


II.  Using Large Objects for handling LongVarBinary (OLE Objects in Access)

Large objects are mapped to LONGVARBINARY in the driver to allow storing things like
OLE objects in Microsoft Access.  Multiple SQLPutData and SQLGetData calls are usually
used to send and retrieve these objects.  The driver creates a new large object and simply
inserts its 'identifier' into the respective table.  However, since Postgres uses an 'Oid'
to identify a Large Object, it is necessary to create a new Postgres type to be able
to discriminate between an ordinary Oid and a Large Object Oid.  Until this new type
becomes an official part of Postgres, it must be added into the desired database and
looked up for each connection.  The type used in the driver is simply called "lo" and
here is the command used to create it:

create type lo (internallength=4,externallength=10,input=int4in,output=int4out,
                default='',passedbyvalue);

Once this is done, simply use the new 'lo' type to define columns in that database.  Note
that this must be done for each database you want to use large objects in with the driver.
When the driver sees an 'lo' type, it will handle it as LONGVARBINARY.

Another important note is that this new type is lacking in functionality.  It will not
cleanup after itself on updates and deletes, thus leaving orphans around and using up
extra disk space.  And currently, Postgres does not support the vacuuming of large
objects.  Hopefully in the future, a real large object data type will be available.

But for now, it sure is fun to stick a Word document, Visio document, or avi of a dancing
baby into a database column, even if you will fill up your server's hard disk after a while!


