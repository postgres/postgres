package org.postgresql.largeobject;

// IMPORTANT NOTE: This file implements the JDBC 2 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 1 class in the
// org.postgresql.jdbc1 package.


import java.lang.*;
import java.io.*;
import java.math.*;
import java.text.*;
import java.util.*;
import java.sql.*;
import org.postgresql.Field;
import org.postgresql.largeobject.*;
import org.postgresql.largeobject.*;

/**
 * This implements the Blob interface, which is basically another way to
 * access a LargeObject.
 *
 * $Id: PGclob.java,v 1.1 2001/02/16 16:45:01 peter Exp $
 *
 */
public class PGclob implements java.sql.Clob
{
    private org.postgresql.Connection conn;
    private int oid;
    private LargeObject lo;

    public PGclob(org.postgresql.Connection conn,int oid) throws SQLException {
	this.conn=conn;
	this.oid=oid;
	LargeObjectManager lom = conn.getLargeObjectAPI();
	this.lo = lom.open(oid);
    }

    public long length() throws SQLException {
	return lo.size();
    }

    public InputStream getAsciiStream() throws SQLException {
	return lo.getInputStream();
    }

    public Reader getCharacterStream() throws SQLException {
	return new InputStreamReader(lo.getInputStream());
    }

    public String getSubString(long i,int j) throws SQLException {
      lo.seek((int)i-1);
      return new String(lo.read(j));
    }

    /*
     * For now, this is not implemented.
     */
    public long position(String pattern,long start) throws SQLException {
	throw org.postgresql.Driver.notImplemented();
    }

    /*
     * This should be simply passing the byte value of the pattern Blob
     */
    public long position(Clob pattern,long start) throws SQLException {
	throw org.postgresql.Driver.notImplemented();
    }

}
