package org.postgresql.jdbc2;


import java.lang.*;
import java.io.*;
import java.math.*;
import java.text.*;
import java.util.*;
import java.sql.*;
import org.postgresql.PGConnection;
import org.postgresql.largeobject.*;

public class AbstractJdbc2Clob
{
	private int oid;
	private LargeObject lo;

	public AbstractJdbc2Clob(PGConnection conn, int oid) throws SQLException
	{
		this.oid = oid;
		LargeObjectManager lom = conn.getLargeObjectAPI();
		this.lo = lom.open(oid);
	}

	public long length() throws SQLException
	{
		return lo.size();
	}

	public InputStream getAsciiStream() throws SQLException
	{
		return lo.getInputStream();
	}

	public Reader getCharacterStream() throws SQLException
	{
		return new InputStreamReader(lo.getInputStream());
	}

	public String getSubString(long i, int j) throws SQLException
	{
		lo.seek((int)i - 1);
		return new String(lo.read(j));
	}

	/*
	 * For now, this is not implemented.
	 */
	public long position(String pattern, long start) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/*
	 * This should be simply passing the byte value of the pattern Blob
	 */
	public long position(Clob pattern, long start) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

}
