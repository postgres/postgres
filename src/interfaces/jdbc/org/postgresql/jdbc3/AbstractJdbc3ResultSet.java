package org.postgresql.jdbc3;


import java.sql.*;
import java.util.Vector;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;


/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc3/Attic/AbstractJdbc3ResultSet.java,v 1.4.4.1 2004/03/29 17:47:47 barry Exp $
 * This class defines methods of the jdbc3 specification.  This class extends
 * org.postgresql.jdbc2.AbstractJdbc2ResultSet which provides the jdbc2
 * methods.  The real Statement class (for jdbc3) is org.postgresql.jdbc3.Jdbc3ResultSet
 */
public abstract class AbstractJdbc3ResultSet extends org.postgresql.jdbc2.AbstractJdbc2ResultSet
{

	public AbstractJdbc3ResultSet(BaseStatement statement, Field[] fields, Vector tuples, String status, int updateCount, long insertOID)
	{
		super (statement, fields, tuples, status, updateCount, insertOID);
	}

	/**
	 * Retrieves the value of the designated column in the current row
	 * of this <code>ResultSet</code> object as a <code>java.net.URL</code>
	 * object in the Java programming language.
	 *
	 * @param columnIndex the index of the column 1 is the first, 2 is the second,...
	 * @return the column value as a <code>java.net.URL</code> object;
	 * if the value is SQL <code>NULL</code>,
	 * the value returned is <code>null</code> in the Java programming language
	 * @exception SQLException if a database access error occurs,
	 *			  or if a URL is malformed
	 * @since 1.4
	 */
	public java.net.URL getURL(int columnIndex) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves the value of the designated column in the current row
	 * of this <code>ResultSet</code> object as a <code>java.net.URL</code>
	 * object in the Java programming language.
	 *
	 * @param columnName the SQL name of the column
	 * @return the column value as a <code>java.net.URL</code> object;
	 * if the value is SQL <code>NULL</code>,
	 * the value returned is <code>null</code> in the Java programming language
	 * @exception SQLException if a database access error occurs
	 *			  or if a URL is malformed
	 * @since 1.4
	 */
	public java.net.URL getURL(String columnName) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Ref</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnIndex the first column is 1, the second is 2, ...
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateRef(int columnIndex, java.sql.Ref x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Ref</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateRef(String columnName, java.sql.Ref x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Blob</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnIndex the first column is 1, the second is 2, ...
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateBlob(int columnIndex, java.sql.Blob x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Blob</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateBlob(String columnName, java.sql.Blob x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Clob</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnIndex the first column is 1, the second is 2, ...
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateClob(int columnIndex, java.sql.Clob x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Clob</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateClob(String columnName, java.sql.Clob x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Array</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnIndex the first column is 1, the second is 2, ...
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateArray(int columnIndex, java.sql.Array x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Updates the designated column with a <code>java.sql.Array</code> value.
	 * The updater methods are used to update column values in the
	 * current row or the insert row.  The updater methods do not
	 * update the underlying database; instead the <code>updateRow</code> or
	 * <code>insertRow</code> methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public void updateArray(String columnName, java.sql.Array x) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

}

