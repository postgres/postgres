package org.postgresql.jdbc1;

// IMPORTANT NOTE: This file implements the JDBC 1 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 2 class in the
// org.postgresql.jdbc2 package.

import java.io.*;
import java.lang.*;
import java.lang.reflect.*;
import java.net.*;
import java.util.*;
import java.sql.*;
import org.postgresql.Field;
import org.postgresql.fastpath.*;
import org.postgresql.largeobject.*;
import org.postgresql.util.*;

/*
 * $Id: Connection.java,v 1.15 2002/01/15 06:55:13 barry Exp $
 *
 * A Connection represents a session with a specific database.	Within the
 * context of a Connection, SQL statements are executed and results are
 * returned.
 *
 * <P>A Connection's database is able to provide information describing
 * its tables, its supported SQL grammar, its stored procedures, the
 * capabilities of this connection, etc.  This information is obtained
 * with the getMetaData method.
 *
 * <p><B>Note:</B> By default, the Connection automatically commits changes
 * after executing each statement.	If auto-commit has been disabled, an
 * explicit commit must be done or database changes will not be saved.
 *
 * @see java.sql.Connection
 */
public class Connection extends org.postgresql.Connection implements java.sql.Connection
{
	// This is a cache of the DatabaseMetaData instance for this connection
	protected DatabaseMetaData metadata;

	/*
	 * SQL statements without parameters are normally executed using
	 * Statement objects.  If the same SQL statement is executed many
	 * times, it is more efficient to use a PreparedStatement
	 *
	 * @return a new Statement object
	 * @exception SQLException passed through from the constructor
	 */
	public java.sql.Statement createStatement() throws SQLException
	{
		return new Statement(this);
	}

	/*
	 * A SQL statement with or without IN parameters can be pre-compiled
	 * and stored in a PreparedStatement object.  This object can then
	 * be used to efficiently execute this statement multiple times.
	 *
	 * <B>Note:</B> This method is optimized for handling parametric
	 * SQL statements that benefit from precompilation if the drivers
	 * supports precompilation.  PostgreSQL does not support precompilation.
	 * In this case, the statement is not sent to the database until the
	 * PreparedStatement is executed.  This has no direct effect on users;
	 * however it does affect which method throws certain SQLExceptions
	 *
	 * @param sql a SQL statement that may contain one or more '?' IN
	 *	parameter placeholders
	 * @return a new PreparedStatement object containing the pre-compiled
	 *	statement.
	 * @exception SQLException if a database access error occurs.
	 */
	public java.sql.PreparedStatement prepareStatement(String sql) throws SQLException
	{
		return new PreparedStatement(this, sql);
	}

	/*
	 * A SQL stored procedure call statement is handled by creating a
	 * CallableStatement for it.  The CallableStatement provides methods
	 * for setting up its IN and OUT parameters and methods for executing
	 * it.
	 *
	 * <B>Note:</B> This method is optimised for handling stored procedure
	 * call statements.  Some drivers may send the call statement to the
	 * database when the prepareCall is done; others may wait until the
	 * CallableStatement is executed.  This has no direct effect on users;
	 * however, it does affect which method throws certain SQLExceptions
	 *
	 * @param sql a SQL statement that may contain one or more '?' parameter
	 *	placeholders.  Typically this statement is a JDBC function call
	 *	escape string.
	 * @return a new CallableStatement object containing the pre-compiled
	 *	SQL statement
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.CallableStatement prepareCall(String sql) throws SQLException
	{
		throw new PSQLException("postgresql.con.call");
		//		return new CallableStatement(this, sql);
	}

	/*
	 * Tests to see if a Connection is closed
	 *
	 * @return the status of the connection
	 * @exception SQLException (why?)
	 */
	public boolean isClosed() throws SQLException
	{
		return (pg_stream == null);
	}

	/*
	 * A connection's database is able to provide information describing
	 * its tables, its supported SQL grammar, its stored procedures, the
	 * capabilities of this connection, etc.  This information is made
	 * available through a DatabaseMetaData object.
	 *
	 * @return a DatabaseMetaData object for this connection
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.DatabaseMetaData getMetaData() throws SQLException
	{
		if (metadata == null)
			metadata = new DatabaseMetaData(this);
		return metadata;
	}

	/*
	 * This overides the method in org.postgresql.Connection and returns a
	 * ResultSet.
	 */
	public java.sql.ResultSet getResultSet(org.postgresql.Connection conn, java.sql.Statement stat, Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		// in jdbc1 stat is ignored.
		return new org.postgresql.jdbc1.ResultSet((org.postgresql.jdbc1.Connection)conn, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}


	/* An implementation of the abstract method in the parent class.
	 * This implemetation uses the jdbc1Types array to support the jdbc1
	 * datatypes.  Basically jdbc1 and jdbc2 are the same, except that
	 * jdbc2 adds the Array types.
	 */
	public int getSQLType(String pgTypeName)
	{
		int sqlType = Types.OTHER; // default value
		for (int i = 0;i < jdbc1Types.length;i++)
		{
			if (pgTypeName.equals(jdbc1Types[i]))
			{
				sqlType = jdbc1Typei[i];
				break;
			}
		}
		return sqlType;
	}

	/*
	 * This table holds the org.postgresql names for the types supported.
	 * Any types that map to Types.OTHER (eg POINT) don't go into this table.
	 * They default automatically to Types.OTHER
	 *
	 * Note: This must be in the same order as below.
	 *
	 * Tip: keep these grouped together by the Types. value
	 */
	private static final String jdbc1Types[] = {
				"int2",
				"int4", "oid",
				"int8",
				"cash", "money",
				"numeric",
				"float4",
				"float8",
				"bpchar", "char", "char2", "char4", "char8", "char16",
				"varchar", "text", "name", "filename",
				"bytea",
				"bool",
				"date",
				"time",
				"abstime", "timestamp", "timestamptz"
			};

	/*
	 * This table holds the JDBC type for each entry above.
	 *
	 * Note: This must be in the same order as above
	 *
	 * Tip: keep these grouped together by the Types. value
	 */
	private static final int jdbc1Typei[] = {
												Types.SMALLINT,
												Types.INTEGER, Types.INTEGER,
												Types.BIGINT,
												Types.DOUBLE, Types.DOUBLE,
												Types.NUMERIC,
												Types.REAL,
												Types.DOUBLE,
												Types.CHAR, Types.CHAR, Types.CHAR, Types.CHAR, Types.CHAR, Types.CHAR,
												Types.VARCHAR, Types.VARCHAR, Types.VARCHAR, Types.VARCHAR,
												Types.BINARY,
												Types.BIT,
												Types.DATE,
												Types.TIME,
												Types.TIMESTAMP, Types.TIMESTAMP, Types.TIMESTAMP
											};


}

// ***********************************************************************

