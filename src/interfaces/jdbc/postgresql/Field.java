package postgresql;

import java.lang.*;
import java.sql.*;
import java.util.*;
import postgresql.*;

/**
 * postgresql.Field is a class used to describe fields in a PostgreSQL ResultSet
 *
 * @version 1.0 15-APR-1997
 * @author <A HREF="mailto:adrian@hottub.org">Adrian Hall</A>
 */
public class Field
{
	int length;		// Internal Length of this field
	int oid;		// OID of the type
	Connection conn;	// Connection Instantation
	String name;		// Name of this field

	int sql_type = -1;	// The entry in java.sql.Types for this field
	String type_name = null;// The sql type name

	/**
	 *	Construct a field based on the information fed to it.
	 *
	 * @param conn the connection this field came from
	 * @param name the name of the field
	 * @param oid the OID of the field
	 * @param len the length of the field
	 */
	public Field(Connection conn, String name, int oid, int length)
	{
		this.conn = conn;
		this.name = name;
		this.oid = oid;
		this.length = length;
	}

	/**
	 * the ResultSet and ResultMetaData both need to handle the SQL
	 * type, which is gained from another query.  Note that we cannot
	 * use getObject() in this, since getObject uses getSQLType().
	 *
	 * @return the entry in Types that refers to this field
	 * @exception SQLException if a database access error occurs
	 */
	public int getSQLType() throws SQLException
	{
		if (sql_type == -1)
		{
			ResultSet result = (postgresql.ResultSet)conn.ExecSQL("select typname from pg_type where oid = " + oid);
			if (result.getColumnCount() != 1 || result.getTupleCount() != 1)
				throw new SQLException("Unexpected return from query for type");
			result.next();
			type_name = result.getString(1);
			if (type_name.equals("int2"))				sql_type = Types.SMALLINT;
			else if (type_name.equals("int4"))			sql_type = Types.INTEGER;
			else if (type_name.equals("int8"))			sql_type = Types.BIGINT;
			else if (type_name.equals("cash"))			sql_type = Types.DECIMAL;
			else if (type_name.equals("money"))			sql_type = Types.DECIMAL;
			else if (type_name.equals("float4"))			sql_type = Types.REAL;
			else if (type_name.equals("float8"))			sql_type = Types.DOUBLE;
			else if (type_name.equals("bpchar"))			sql_type = Types.CHAR;
			else if (type_name.equals("varchar"))			sql_type = Types.VARCHAR;
			else if (type_name.equals("bool"))			sql_type = Types.BIT;
			else if (type_name.equals("date"))			sql_type = Types.DATE;
			else if (type_name.equals("time"))			sql_type = Types.TIME;
			else if (type_name.equals("abstime"))			sql_type = Types.TIMESTAMP;
			else							sql_type = Types.OTHER;
		}	
		return sql_type;
	}

	/**
	 * We also need to get the type name as returned by the back end.
	 * This is held in type_name AFTER a call to getSQLType.  Since
	 * we get this information within getSQLType (if it isn't already
	 * done), we can just call getSQLType and throw away the result.
	 *
	 * @return the String representation of the type of this field
	 * @exception SQLException if a database access error occurs
	 */
	public String getTypeName() throws SQLException
	{
		int sql = getSQLType();
		return type_name;
	}
}
