package org.postgresql.jdbc2;


import java.sql.*;
import java.util.Vector;
import java.util.Hashtable;
import org.postgresql.core.Field;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/Jdbc2Connection.java,v 1.7 2003/03/07 18:39:45 barry Exp $
 * This class implements the java.sql.Connection interface for JDBC2.
 * However most of the implementation is really done in
 * org.postgresql.jdbc2.AbstractJdbc2Connection or one of it's parents
 */
public class Jdbc2Connection extends org.postgresql.jdbc2.AbstractJdbc2Connection implements java.sql.Connection
{

	public java.sql.Statement createStatement(int resultSetType, int resultSetConcurrency) throws SQLException
	{
		Jdbc2Statement s = new Jdbc2Statement(this);
		s.setResultSetType(resultSetType);
		s.setResultSetConcurrency(resultSetConcurrency);
		return s;
	}


	public java.sql.PreparedStatement prepareStatement(String sql, int resultSetType, int resultSetConcurrency) throws SQLException
	{
		Jdbc2PreparedStatement s = new Jdbc2PreparedStatement(this, sql);
		s.setResultSetType(resultSetType);
		s.setResultSetConcurrency(resultSetConcurrency);
		return s;
	}

	public java.sql.CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency) throws SQLException
	{
		Jdbc2CallableStatement s = new org.postgresql.jdbc2.Jdbc2CallableStatement(this, sql);
		s.setResultSetType(resultSetType);
		s.setResultSetConcurrency(resultSetConcurrency);
		return s;
	}

	public java.sql.DatabaseMetaData getMetaData() throws SQLException
	{
		if (metadata == null)
			metadata = new org.postgresql.jdbc2.Jdbc2DatabaseMetaData(this);
		return metadata;
	}

}


