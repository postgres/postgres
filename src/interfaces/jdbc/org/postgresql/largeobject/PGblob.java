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

/*
 * This implements the Blob interface, which is basically another way to
 * access a LargeObject.
 *
 * $Id: PGblob.java,v 1.4 2002/07/23 03:59:55 barry Exp $
 *
 */
public class PGblob implements java.sql.Blob
{
	private int oid;
	private LargeObject lo;

	public PGblob(org.postgresql.PGConnection conn, int oid) throws SQLException
	{
		this.oid = oid;
		LargeObjectManager lom = conn.getLargeObjectAPI();
		this.lo = lom.open(oid);
	}

	public long length() throws SQLException
	{
		return lo.size();
	}

	public InputStream getBinaryStream() throws SQLException
	{
		return lo.getInputStream();
	}

	public byte[] getBytes(long pos, int length) throws SQLException
	{
		lo.seek((int)pos, LargeObject.SEEK_SET);
		return lo.read(length);
	}

	/*
	 * For now, this is not implemented.
	 */
	public long position(byte[] pattern, long start) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/*
	 * This should be simply passing the byte value of the pattern Blob
	 */
	public long position(Blob pattern, long start) throws SQLException
	{
		return position(pattern.getBytes(0, (int)pattern.length()), start);
	}

}
