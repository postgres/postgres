/*-------------------------------------------------------------------------
 *
 * BaseResultSet.java
 *	  The internal interface definition for a jdbc result set
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/jdbc/org/postgresql/core/BaseResultSet.java,v 1.3 2003/11/29 19:52:09 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.core;


import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.text.SimpleDateFormat;
import java.util.Vector;

public interface BaseResultSet
{

    public BaseStatement getPGStatement();

	public void append(BaseResultSet r);
	public void close() throws SQLException;
	public int getColumnCount();
	public String getCursorName() throws SQLException;
	public SimpleDateFormat getDateFormat();
	public String getFixedString(int col) throws SQLException;
	public long getLastOID();
	public ResultSetMetaData getMetaData() throws SQLException;
	public ResultSet getNext();
	public Object getObject(int columnIndex) throws SQLException;
	public int getResultCount();
	public String getStatusString();
	public String getString(int columnIndex) throws SQLException;
	public StringBuffer getStringBuffer();
	public SimpleDateFormat getTimestampFormat();
	public SimpleDateFormat getTimestampTZFormat();
	public int getTupleCount();
	public boolean next() throws SQLException;
	public boolean reallyResultSet();
	public void reInit (Field[] fields, Vector tuples, String status,
						int updateCount, long insertOID, boolean binaryCursor);
	public void setStatement(BaseStatement statement);

}
