package org.postgresql.jdbc3;

import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc3/Attic/AbstractJdbc3Connection.java,v 1.4 2003/06/30 21:10:55 davec Exp $
 * This class defines methods of the jdbc3 specification.  This class extends
 * org.postgresql.jdbc2.AbstractJdbc2Connection which provides the jdbc2
 * methods.  The real Connection class (for jdbc3) is org.postgresql.jdbc3.Jdbc3Connection
 */
public abstract class AbstractJdbc3Connection extends org.postgresql.jdbc2.AbstractJdbc2Connection
{

	/**
	 * Changes the holdability of <code>ResultSet</code> objects
	 * created using this <code>Connection</code> object to the given
	 * holdability.
	 *
	 * @param holdability a <code>ResultSet</code> holdability constant; one of
	 *		  <code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *		  <code>ResultSet.CLOSE_CURSORS_AT_COMMIT</code>
	 * @throws SQLException if a database access occurs, the given parameter
	 *		   is not a <code>ResultSet</code> constant indicating holdability,
	 *		   or the given holdability is not supported
	 * @see #getHoldability
	 * @see ResultSet
	 * @since 1.4
	 */
	public void setHoldability(int holdability) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves the current holdability of <code>ResultSet</code> objects
	 * created using this <code>Connection</code> object.
	 *
	 * @return the holdability, one of
	 *		  <code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *		  <code>ResultSet.CLOSE_CURSORS_AT_COMMIT</code>
	 * @throws SQLException if a database access occurs
	 * @see #setHoldability
	 * @see ResultSet
	 * @since 1.4
	 */
	public int getHoldability() throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Creates an unnamed savepoint in the current transaction and
	 * returns the new <code>Savepoint</code> object that represents it.
	 *
	 * @return the new <code>Savepoint</code> object
	 * @exception SQLException if a database access error occurs
	 *			  or this <code>Connection</code> object is currently in
	 *			  auto-commit mode
	 * @see Savepoint
	 * @since 1.4
	 */
	public Savepoint setSavepoint() throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Creates a savepoint with the given name in the current transaction
	 * and returns the new <code>Savepoint</code> object that represents it.
	 *
	 * @param name a <code>String</code> containing the name of the savepoint
	 * @return the new <code>Savepoint</code> object
	 * @exception SQLException if a database access error occurs
	 *			  or this <code>Connection</code> object is currently in
	 *			  auto-commit mode
	 * @see Savepoint
	 * @since 1.4
	 */
	public Savepoint setSavepoint(String name) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Undoes all changes made after the given <code>Savepoint</code> object
	 * was set.
	 * <P>
	 * This method should be used only when auto-commit has been disabled.
	 *
	 * @param savepoint the <code>Savepoint</code> object to roll back to
	 * @exception SQLException if a database access error occurs,
	 *			  the <code>Savepoint</code> object is no longer valid,
	 *			  or this <code>Connection</code> object is currently in
	 *			  auto-commit mode
	 * @see Savepoint
	 * @see #rollback
	 * @since 1.4
	 */
	public void rollback(Savepoint savepoint) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}


	/**
	 * Removes the given <code>Savepoint</code> object from the current
	 * transaction. Any reference to the savepoint after it have been removed
	 * will cause an <code>SQLException</code> to be thrown.
	 *
	 * @param savepoint the <code>Savepoint</code> object to be removed
	 * @exception SQLException if a database access error occurs or
	 *			  the given <code>Savepoint</code> object is not a valid
	 *			  savepoint in the current transaction
	 * @since 1.4
	 */
	public void releaseSavepoint(Savepoint savepoint) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}


	/**
	 * Creates a <code>Statement</code> object that will generate
	 * <code>ResultSet</code> objects with the given type, concurrency,
	 * and holdability.
	 * This method is the same as the <code>createStatement</code> method
	 * above, but it allows the default result set
	 * type, concurrency, and holdability to be overridden.
	 *
	 * @param resultSetType one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.TYPE_FORWARD_ONLY</code>,
	 *		   <code>ResultSet.TYPE_SCROLL_INSENSITIVE</code>, or
	 *		   <code>ResultSet.TYPE_SCROLL_SENSITIVE</code>
	 * @param resultSetConcurrency one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.CONCUR_READ_ONLY</code> or
	 *		   <code>ResultSet.CONCUR_UPDATABLE</code>
	 * @param resultSetHoldability one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *		   <code>ResultSet.CLOSE_CURSORS_AT_COMMIT</code>
	 * @return a new <code>Statement</code> object that will generate
	 *		   <code>ResultSet</code> objects with the given type,
	 *		   concurrency, and holdability
	 * @exception SQLException if a database access error occurs
	 *			  or the given parameters are not <code>ResultSet</code>
	 *			  constants indicating type, concurrency, and holdability
	 * @see ResultSet
	 * @since 1.4
	 */
	public Statement createStatement(int resultSetType, int resultSetConcurrency,
									 int resultSetHoldability) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}


	/**
	 * Creates a <code>PreparedStatement</code> object that will generate
	 * <code>ResultSet</code> objects with the given type, concurrency,
	 * and holdability.
	 * <P>
	 * This method is the same as the <code>prepareStatement</code> method
	 * above, but it allows the default result set
	 * type, concurrency, and holdability to be overridden.
	 *
	 * @param sql a <code>String</code> object that is the SQL statement to
	 *			  be sent to the database; may contain one or more ? IN
	 *			  parameters
	 * @param resultSetType one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.TYPE_FORWARD_ONLY</code>,
	 *		   <code>ResultSet.TYPE_SCROLL_INSENSITIVE</code>, or
	 *		   <code>ResultSet.TYPE_SCROLL_SENSITIVE</code>
	 * @param resultSetConcurrency one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.CONCUR_READ_ONLY</code> or
	 *		   <code>ResultSet.CONCUR_UPDATABLE</code>
	 * @param resultSetHoldability one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *		   <code>ResultSet.CLOSE_CURSORS_AT_COMMIT</code>
	 * @return a new <code>PreparedStatement</code> object, containing the
	 *		   pre-compiled SQL statement, that will generate
	 *		   <code>ResultSet</code> objects with the given type,
	 *		   concurrency, and holdability
	 * @exception SQLException if a database access error occurs
	 *			  or the given parameters are not <code>ResultSet</code>
	 *			  constants indicating type, concurrency, and holdability
	 * @see ResultSet
	 * @since 1.4
	 */
	public PreparedStatement prepareStatement(String sql, int resultSetType,
			int resultSetConcurrency, int resultSetHoldability)
	throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}


	/**
	 * Creates a <code>CallableStatement</code> object that will generate
	 * <code>ResultSet</code> objects with the given type and concurrency.
	 * This method is the same as the <code>prepareCall</code> method
	 * above, but it allows the default result set
	 * type, result set concurrency type and holdability to be overridden.
	 *
	 * @param sql a <code>String</code> object that is the SQL statement to
	 *			  be sent to the database; may contain on or more ? parameters
	 * @param resultSetType one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.TYPE_FORWARD_ONLY</code>,
	 *		   <code>ResultSet.TYPE_SCROLL_INSENSITIVE</code>, or
	 *		   <code>ResultSet.TYPE_SCROLL_SENSITIVE</code>
	 * @param resultSetConcurrency one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.CONCUR_READ_ONLY</code> or
	 *		   <code>ResultSet.CONCUR_UPDATABLE</code>
	 * @param resultSetHoldability one of the following <code>ResultSet</code>
	 *		  constants:
	 *		   <code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *		   <code>ResultSet.CLOSE_CURSORS_AT_COMMIT</code>
	 * @return a new <code>CallableStatement</code> object, containing the
	 *		   pre-compiled SQL statement, that will generate
	 *		   <code>ResultSet</code> objects with the given type,
	 *		   concurrency, and holdability
	 * @exception SQLException if a database access error occurs
	 *			  or the given parameters are not <code>ResultSet</code>
	 *			  constants indicating type, concurrency, and holdability
	 * @see ResultSet
	 * @since 1.4
	 */
	public CallableStatement prepareCall(String sql, int resultSetType,
										 int resultSetConcurrency,
										 int resultSetHoldability) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}



	/**
	 * Creates a default <code>PreparedStatement</code> object that has
	 * the capability to retrieve auto-generated keys. The given constant
	 * tells the driver whether it should make auto-generated keys
	 * available for retrieval.  This parameter is ignored if the SQL
	 * statement is not an <code>INSERT</code> statement.
	 * <P>
	 * <B>Note:</B> This method is optimized for handling
	 * parametric SQL statements that benefit from precompilation. If
	 * the driver supports precompilation,
	 * the method <code>prepareStatement</code> will send
	 * the statement to the database for precompilation. Some drivers
	 * may not support precompilation. In this case, the statement may
	 * not be sent to the database until the <code>PreparedStatement</code>
	 * object is executed.	This has no direct effect on users; however, it does
	 * affect which methods throw certain SQLExceptions.
	 * <P>
	 * Result sets created using the returned <code>PreparedStatement</code>
	 * object will by default be type <code>TYPE_FORWARD_ONLY</code>
	 * and have a concurrency level of <code>CONCUR_READ_ONLY</code>.
	 *
	 * @param sql an SQL statement that may contain one or more '?' IN
	 *		  parameter placeholders
	 * @param autoGeneratedKeys a flag indicating whether auto-generated keys
	 *		  should be returned; one of the following <code>Statement</code>
	 *		  constants:
	 * @param autoGeneratedKeys a flag indicating that auto-generated keys should be returned, one of
	 *		  <code>Statement.RETURN_GENERATED_KEYS</code> or
	 *		  <code>Statement.NO_GENERATED_KEYS</code>.
	 * @return a new <code>PreparedStatement</code> object, containing the
	 *		   pre-compiled SQL statement, that will have the capability of
	 *		   returning auto-generated keys
	 * @exception SQLException if a database access error occurs
	 *		   or the given parameter is not a <code>Statement</code>
	 *		   constant indicating whether auto-generated keys should be
	 *		   returned
	 * @since 1.4
	 */
	public PreparedStatement prepareStatement(String sql, int autoGeneratedKeys)
	throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}


	/**
	 * Creates a default <code>PreparedStatement</code> object capable
	 * of returning the auto-generated keys designated by the given array.
	 * This array contains the indexes of the columns in the target
	 * table that contain the auto-generated keys that should be made
	 * available. This array is ignored if the SQL
	 * statement is not an <code>INSERT</code> statement.
	 * <P>
	 * An SQL statement with or without IN parameters can be
	 * pre-compiled and stored in a <code>PreparedStatement</code> object. This
	 * object can then be used to efficiently execute this statement
	 * multiple times.
	 * <P>
	 * <B>Note:</B> This method is optimized for handling
	 * parametric SQL statements that benefit from precompilation. If
	 * the driver supports precompilation,
	 * the method <code>prepareStatement</code> will send
	 * the statement to the database for precompilation. Some drivers
	 * may not support precompilation. In this case, the statement may
	 * not be sent to the database until the <code>PreparedStatement</code>
	 * object is executed.	This has no direct effect on users; however, it does
	 * affect which methods throw certain SQLExceptions.
	 * <P>
	 * Result sets created using the returned <code>PreparedStatement</code>
	 * object will by default be type <code>TYPE_FORWARD_ONLY</code>
	 * and have a concurrency level of <code>CONCUR_READ_ONLY</code>.
	 *
	 * @param sql an SQL statement that may contain one or more '?' IN
	 *		  parameter placeholders
	 * @param columnIndexes an array of column indexes indicating the columns
	 *		  that should be returned from the inserted row or rows
	 * @return a new <code>PreparedStatement</code> object, containing the
	 *		   pre-compiled statement, that is capable of returning the
	 *		   auto-generated keys designated by the given array of column
	 *		   indexes
	 * @exception SQLException if a database access error occurs
	 *
	 * @since 1.4
	 */
	public PreparedStatement prepareStatement(String sql, int columnIndexes[])
	throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}


	/**
	 * Creates a default <code>PreparedStatement</code> object capable
	 * of returning the auto-generated keys designated by the given array.
	 * This array contains the names of the columns in the target
	 * table that contain the auto-generated keys that should be returned.
	 * This array is ignored if the SQL
	 * statement is not an <code>INSERT</code> statement.
	 * <P>
	 * An SQL statement with or without IN parameters can be
	 * pre-compiled and stored in a <code>PreparedStatement</code> object. This
	 * object can then be used to efficiently execute this statement
	 * multiple times.
	 * <P>
	 * <B>Note:</B> This method is optimized for handling
	 * parametric SQL statements that benefit from precompilation. If
	 * the driver supports precompilation,
	 * the method <code>prepareStatement</code> will send
	 * the statement to the database for precompilation. Some drivers
	 * may not support precompilation. In this case, the statement may
	 * not be sent to the database until the <code>PreparedStatement</code>
	 * object is executed.	This has no direct effect on users; however, it does
	 * affect which methods throw certain SQLExceptions.
	 * <P>
	 * Result sets created using the returned <code>PreparedStatement</code>
	 * object will by default be type <code>TYPE_FORWARD_ONLY</code>
	 * and have a concurrency level of <code>CONCUR_READ_ONLY</code>.
	 *
	 * @param sql an SQL statement that may contain one or more '?' IN
	 *		  parameter placeholders
	 * @param columnNames an array of column names indicating the columns
	 *		  that should be returned from the inserted row or rows
	 * @return a new <code>PreparedStatement</code> object, containing the
	 *		   pre-compiled statement, that is capable of returning the
	 *		   auto-generated keys designated by the given array of column
	 *		   names
	 * @exception SQLException if a database access error occurs
	 *
	 * @since 1.4
	 */
	public PreparedStatement prepareStatement(String sql, String columnNames[])
	throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/*
	 * This implemetation uses the jdbc3Types array to support the jdbc3
	 * datatypes.  Basically jdbc2 and jdbc3 are the same, except that
	 * jdbc3 adds some
	 */
	public int getSQLType(String pgTypeName)
	{
		int sqlType = Types.OTHER; // default value
		for (int i = 0;i < jdbc3Types.length;i++)
		{
			if (pgTypeName.equals(jdbc3Types[i]))
			{
				sqlType = jdbc3Typei[i];
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
	private static final String jdbc3Types[] = {
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
				"bit",
				"date",
				"time",
				"abstime", "timestamp", "timestamptz",
				"_bool", "_char", "_int2", "_int4", "_text",
				"_oid", "_varchar", "_int8", "_float4", "_float8",
				"_abstime", "_date", "_time", "_timestamp", "_numeric",
				"_bytea"
			};

	/*
	 * This table holds the JDBC type for each entry above.
	 *
	 * Note: This must be in the same order as above
	 *
	 * Tip: keep these grouped together by the Types. value
	 */
	private static final int jdbc3Typei[] = {
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
			Types.BIT,
			Types.DATE,
			Types.TIME,
			Types.TIMESTAMP, Types.TIMESTAMP, Types.TIMESTAMP,
			Types.ARRAY, Types.ARRAY, Types.ARRAY, Types.ARRAY, Types.ARRAY,
			Types.ARRAY, Types.ARRAY, Types.ARRAY, Types.ARRAY, Types.ARRAY,
			Types.ARRAY, Types.ARRAY, Types.ARRAY, Types.ARRAY, Types.ARRAY,
			Types.ARRAY
	   };

}


