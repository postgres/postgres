package org.postgresql.jdbc1;


import java.util.Vector;
import java.sql.*;
import org.postgresql.core.Field;
import org.postgresql.util.PSQLException;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/Jdbc1Connection.java,v 1.7 2003/03/07 18:39:44 barry Exp $
 * This class implements the java.sql.Connection interface for JDBC1.
 * However most of the implementation is really done in
 * org.postgresql.jdbc1.AbstractJdbc1Connection
 */
public class Jdbc1Connection extends org.postgresql.jdbc1.AbstractJdbc1Connection implements java.sql.Connection
{

	public java.sql.Statement createStatement() throws SQLException
	{
		return new org.postgresql.jdbc1.Jdbc1Statement(this);
	}

	public java.sql.PreparedStatement prepareStatement(String sql) throws SQLException
	{
		return new org.postgresql.jdbc1.Jdbc1PreparedStatement(this, sql);
	}

	public java.sql.CallableStatement prepareCall(String sql) throws SQLException
	{
		return new org.postgresql.jdbc1.Jdbc1CallableStatement(this, sql);
	}

	public java.sql.DatabaseMetaData getMetaData() throws SQLException
	{
		if (metadata == null)
			metadata = new org.postgresql.jdbc1.Jdbc1DatabaseMetaData(this);
		return metadata;
	}

}


