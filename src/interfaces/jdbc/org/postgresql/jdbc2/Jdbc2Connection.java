package org.postgresql.jdbc2;


import java.sql.*;
import java.util.Vector;
import org.postgresql.Field;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/Jdbc2Connection.java,v 1.1 2002/07/23 03:59:55 barry Exp $
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
                org.postgresql.jdbc2.PreparedStatement s = new org.postgresql.jdbc2.PreparedStatement(this, sql);
                s.setResultSetType(resultSetType);
                s.setResultSetConcurrency(resultSetConcurrency);
                return s;
        }

        public java.sql.CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency) throws SQLException
        {
		org.postgresql.jdbc2.CallableStatement s = new org.postgresql.jdbc2.CallableStatement(this,sql);
		s.setResultSetType(resultSetType);
	      	s.setResultSetConcurrency(resultSetConcurrency);
	       	return s;
        }

        public java.sql.DatabaseMetaData getMetaData() throws SQLException
        {
                if (metadata == null)
                        metadata = new org.postgresql.jdbc2.DatabaseMetaData(this);
                return metadata;
        }

        public java.sql.ResultSet getResultSet(java.sql.Statement stat, Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
        {
                if (stat != null)
                {
                        if (stat.getResultSetConcurrency() == java.sql.ResultSet.CONCUR_UPDATABLE)
                          return new org.postgresql.jdbc2.UpdateableResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
                }

                return new Jdbc2ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
        }


}


