package org.postgresql.jdbc3;


import java.sql.*;

public abstract class AbstractJdbc3DatabaseMetaData extends org.postgresql.jdbc2.AbstractJdbc2DatabaseMetaData
{

	public AbstractJdbc3DatabaseMetaData(AbstractJdbc3Connection conn)
	{
		super(conn);
	}


	/**
	 * Retrieves whether this database supports savepoints.
	 *
	 * @return <code>true</code> if savepoints are supported;
	 *		   <code>false</code> otherwise
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public boolean supportsSavepoints() throws SQLException
	{
		return false;
	}

	/**
	 * Retrieves whether this database supports named parameters to callable
	 * statements.
	 *
	 * @return <code>true</code> if named parameters are supported;
	 *		   <code>false</code> otherwise
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public boolean supportsNamedParameters() throws SQLException
	{
		return false;
	}

	/**
	 * Retrieves whether it is possible to have multiple <code>ResultSet</code> objects
	 * returned from a <code>CallableStatement</code> object
	 * simultaneously.
	 *
	 * @return <code>true</code> if a <code>CallableStatement</code> object
	 *		   can return multiple <code>ResultSet</code> objects
	 *		   simultaneously; <code>false</code> otherwise
	 * @exception SQLException if a datanase access error occurs
	 * @since 1.4
	 */
	public boolean supportsMultipleOpenResults() throws SQLException
	{
		return false;
	}

	/**
	 * Retrieves whether auto-generated keys can be retrieved after
	 * a statement has been executed.
	 *
	 * @return <code>true</code> if auto-generated keys can be retrieved
	 *		   after a statement has executed; <code>false</code> otherwise
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public boolean supportsGetGeneratedKeys() throws SQLException
	{
		return false;
	}

	/**
	 * Retrieves a description of the user-defined type (UDT) hierarchies defined in a
	 * particular schema in this database. Only the immediate super type/
	 * sub type relationship is modeled.
	 * <P>
	 * Only supertype information for UDTs matching the catalog,
	 * schema, and type name is returned. The type name parameter
	 * may be a fully-qualified name. When the UDT name supplied is a
	 * fully-qualified name, the catalog and schemaPattern parameters are
	 * ignored.
	 * <P>
	 * If a UDT does not have a direct super type, it is not listed here.
	 * A row of the <code>ResultSet</code> object returned by this method
	 * describes the designated UDT and a direct supertype. A row has the following
	 * columns:
	 *	<OL>
	 *	<LI><B>TYPE_CAT</B> String => the UDT's catalog (may be <code>null</code>)
	 *	<LI><B>TYPE_SCHEM</B> String => UDT's schema (may be <code>null</code>)
	 *	<LI><B>TYPE_NAME</B> String => type name of the UDT
	 *	<LI><B>SUPERTYPE_CAT</B> String => the direct super type's catalog 
	 *							 (may be <code>null</code>)
	 *	<LI><B>SUPERTYPE_SCHEM</B> String => the direct super type's schema 
	 *							   (may be <code>null</code>)
	 *	<LI><B>SUPERTYPE_NAME</B> String => the direct super type's name
	 *	</OL>
	 *
	 * <P><B>Note:</B> If the driver does not support type hierarchies, an
	 * empty result set is returned.
	 *
	 * @param catalog a catalog name; "" retrieves those without a catalog;
	 *		  <code>null</code> means drop catalog name from the selection criteria
	 * @param schemaPattern a schema name pattern; "" retrieves those
	 *		  without a schema
	 * @param typeNamePattern a UDT name pattern; may be a fully-qualified
	 *		  name
	 * @return a <code>ResultSet</code> object in which a row gives information
	 *		   about the designated UDT
	 * @throws SQLException if a database access error occurs
	 * @since 1.4
	 */
	public ResultSet getSuperTypes(String catalog, String schemaPattern,
								   String typeNamePattern) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves a description of the table hierarchies defined in a particular
	 * schema in this database.
	 *
	 * <P>Only supertable information for tables matching the catalog, schema
	 * and table name are returned. The table name parameter may be a fully-
	 * qualified name, in which case, the catalog and schemaPattern parameters
	 * are ignored. If a table does not have a super table, it is not listed here.
	 * Supertables have to be defined in the same catalog and schema as the
	 * sub tables. Therefore, the type description does not need to include
	 * this information for the supertable.
	 *
	 * <P>Each type description has the following columns:
	 *	<OL>
	 *	<LI><B>TABLE_CAT</B> String => the type's catalog (may be <code>null</code>)
	 *	<LI><B>TABLE_SCHEM</B> String => type's schema (may be <code>null</code>)
	 *	<LI><B>TABLE_NAME</B> String => type name
	 *	<LI><B>SUPERTABLE_NAME</B> String => the direct super type's name
	 *	</OL>
	 *
	 * <P><B>Note:</B> If the driver does not support type hierarchies, an
	 * empty result set is returned.
	 *
	 * @param catalog a catalog name; "" retrieves those without a catalog;
	 *		  <code>null</code> means drop catalog name from the selection criteria
	 * @param schemaPattern a schema name pattern; "" retrieves those
	 *		  without a schema
	 * @param tableNamePattern a table name pattern; may be a fully-qualified
	 *		  name
	 * @return a <code>ResultSet</code> object in which each row is a type description
	 * @throws SQLException if a database access error occurs
	 * @since 1.4
	 */
	public ResultSet getSuperTables(String catalog, String schemaPattern,
									String tableNamePattern) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves a description of the given attribute of the given type
	 * for a user-defined type (UDT) that is available in the given schema
	 * and catalog.
	 * <P>
	 * Descriptions are returned only for attributes of UDTs matching the
	 * catalog, schema, type, and attribute name criteria. They are ordered by
	 * TYPE_SCHEM, TYPE_NAME and ORDINAL_POSITION. This description
	 * does not contain inherited attributes.
	 * <P>
	 * The <code>ResultSet</code> object that is returned has the following
	 * columns:
	 * <OL>
	 *	<LI><B>TYPE_CAT</B> String => type catalog (may be <code>null</code>)
	 *	<LI><B>TYPE_SCHEM</B> String => type schema (may be <code>null</code>)
	 *	<LI><B>TYPE_NAME</B> String => type name
	 *	<LI><B>ATTR_NAME</B> String => attribute name
	 *	<LI><B>DATA_TYPE</B> short => attribute type SQL type from java.sql.Types
	 *	<LI><B>ATTR_TYPE_NAME</B> String => Data source dependent type name.
	 *	For a UDT, the type name is fully qualified. For a REF, the type name is
	 *	fully qualified and represents the target type of the reference type.
	 *	<LI><B>ATTR_SIZE</B> int => column size.  For char or date
	 *		types this is the maximum number of characters; for numeric or
	 *		decimal types this is precision.
	 *	<LI><B>DECIMAL_DIGITS</B> int => the number of fractional digits
	 *	<LI><B>NUM_PREC_RADIX</B> int => Radix (typically either 10 or 2)
	 *	<LI><B>NULLABLE</B> int => whether NULL is allowed
	 *		<UL>
	 *		<LI> attributeNoNulls - might not allow NULL values
	 *		<LI> attributeNullable - definitely allows NULL values
	 *		<LI> attributeNullableUnknown - nullability unknown
	 *		</UL>
	 *	<LI><B>REMARKS</B> String => comment describing column (may be <code>null</code>)
	 *	<LI><B>ATTR_DEF</B> String => default value (may be <code>null</code>)
	 *	<LI><B>SQL_DATA_TYPE</B> int => unused
	 *	<LI><B>SQL_DATETIME_SUB</B> int => unused
	 *	<LI><B>CHAR_OCTET_LENGTH</B> int => for char types the
	 *		 maximum number of bytes in the column
	 *	<LI><B>ORDINAL_POSITION</B> int => index of column in table
	 *		(starting at 1)
	 *	<LI><B>IS_NULLABLE</B> String => "NO" means column definitely
	 *		does not allow NULL values; "YES" means the column might
	 *		allow NULL values.	An empty string means unknown.
	 *	<LI><B>SCOPE_CATALOG</B> String => catalog of table that is the
	 *		scope of a reference attribute (<code>null</code> if DATA_TYPE isn't REF)
	 *	<LI><B>SCOPE_SCHEMA</B> String => schema of table that is the
	 *		scope of a reference attribute (<code>null</code> if DATA_TYPE isn't REF)
	 *	<LI><B>SCOPE_TABLE</B> String => table name that is the scope of a
	 *		reference attribute (<code>null</code> if the DATA_TYPE isn't REF)
	 * <LI><B>SOURCE_DATA_TYPE</B> short => source type of a distinct type or user-generated
	 *		Ref type,SQL type from java.sql.Types (<code>null</code> if DATA_TYPE
	 *		isn't DISTINCT or user-generated REF)
	 *	</OL>
	 * @param catalog a catalog name; must match the catalog name as it
	 *		  is stored in the database; "" retrieves those without a catalog;
	 *		  <code>null</code> means that the catalog name should not be used to narrow
	 *		  the search
	 * @param schemaPattern a schema name pattern; must match the schema name
	 *		  as it is stored in the database; "" retrieves those without a schema;
	 *		  <code>null</code> means that the schema name should not be used to narrow
	 *		  the search
	 * @param typeNamePattern a type name pattern; must match the
	 *		  type name as it is stored in the database
	 * @param attributeNamePattern an attribute name pattern; must match the attribute
	 *		  name as it is declared in the database
	 * @return a <code>ResultSet</code> object in which each row is an
	 *		   attribute description
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public ResultSet getAttributes(String catalog, String schemaPattern,
								   String typeNamePattern, String attributeNamePattern)
	throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves whether this database supports the given result set holdability.
	 *
	 * @param holdability one of the following constants:
	 *			<code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *			<code>ResultSet.CLOSE_CURSORS_AT_COMMIT<code>
	 * @return <code>true</code> if so; <code>false</code> otherwise
	 * @exception SQLException if a database access error occurs
	 * @see Connection
	 * @since 1.4
	 */
	public boolean supportsResultSetHoldability(int holdability) throws SQLException
	{
		return true;
	}

	/**
	 * Retrieves the default holdability of this <code>ResultSet</code>
	 * object.
	 *
	 * @return the default holdability; either
	 *		   <code>ResultSet.HOLD_CURSORS_OVER_COMMIT</code> or
	 *		   <code>ResultSet.CLOSE_CURSORS_AT_COMMIT</code>
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public int getResultSetHoldability() throws SQLException
	{
		return ResultSet.HOLD_CURSORS_OVER_COMMIT;
	}

	/**
	 * Retrieves the major version number of the underlying database.
	 *
	 * @return the underlying database's major version
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public int getDatabaseMajorVersion() throws SQLException
	{
		return connection.getServerMajorVersion();
	}

	/**
	 * Retrieves the minor version number of the underlying database.
	 *
	 * @return underlying database's minor version
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public int getDatabaseMinorVersion() throws SQLException
	{
		return connection.getServerMinorVersion();
	}

	/**
	 * Retrieves the major JDBC version number for this
	 * driver.
	 *
	 * @return JDBC version major number
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public int getJDBCMajorVersion() throws SQLException
	{
		return 3; // This class implements JDBC 3.0
	}

	/**
	 * Retrieves the minor JDBC version number for this
	 * driver.
	 *
	 * @return JDBC version minor number
	 * @exception SQLException if a database access error occurs
	 * @since 1.4
	 */
	public int getJDBCMinorVersion() throws SQLException
	{
		return 0; // This class implements JDBC 3.0
	}

	/**
	 * Indicates whether the SQLSTATEs returned by <code>SQLException.getSQLState</code>
	 * is X/Open (now known as Open Group) SQL CLI or SQL99.
	 * @return the type of SQLSTATEs, one of:
	 *		  sqlStateXOpen or
	 *		  sqlStateSQL99
	 * @throws SQLException if a database access error occurs
	 * @since 1.4
	 */
	public int getSQLStateType() throws SQLException
	{
		return DatabaseMetaData.sqlStateSQL99;
	}

	/**
	 * Indicates whether updates made to a LOB are made on a copy or directly
	 * to the LOB.
	 * @return <code>true</code> if updates are made to a copy of the LOB;
	 *		   <code>false</code> if updates are made directly to the LOB
	 * @throws SQLException if a database access error occurs
	 * @since 1.4
	 */
	public boolean locatorsUpdateCopy() throws SQLException
	{
		/*
		 * Currently LOB's aren't updateable at all, so it doesn't
		 * matter what we return.  We don't throw the notImplemented
		 * Exception because the 1.5 JDK's CachedRowSet calls this
		 * method regardless of wether large objects are used.
		 */
		return true;
	}

	/**
	 * Retrieves weather this database supports statement pooling.
	 *
	 * @return <code>true</code> is so;
		<code>false</code> otherwise
	 * @throws SQLExcpetion if a database access error occurs
	 * @since 1.4
	 */
	public boolean supportsStatementPooling() throws SQLException
	{
		return false;
	}

}
