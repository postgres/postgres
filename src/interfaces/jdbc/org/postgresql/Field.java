package org.postgresql;

import java.lang.*;
import java.sql.*;
import java.util.*;
import org.postgresql.*;
import org.postgresql.util.*;

/*
 * org.postgresql.Field is a class used to describe fields in a PostgreSQL
 * ResultSet
 */
public class Field
{
	private int length;		// Internal Length of this field
	private int oid;		// OID of the type
	private int mod;		// type modifier of this field
	private String name;		// Name of this field

	private Connection conn;	// Connection Instantation


	/*
	 * Construct a field based on the information fed to it.
	 *
	 * @param conn the connection this field came from
	 * @param name the name of the field
	 * @param oid the OID of the field
	 * @param len the length of the field
	 */
	public Field(Connection conn, String name, int oid, int length, int mod)
	{
		this.conn = conn;
		this.name = name;
		this.oid = oid;
		this.length = length;
		this.mod = mod;
	}

	/*
	 * Constructor without mod parameter.
	 *
	 * @param conn the connection this field came from
	 * @param name the name of the field
	 * @param oid the OID of the field
	 * @param len the length of the field
	 */
	public Field(Connection conn, String name, int oid, int length)
	{
		this(conn, name, oid, length, 0);
	}

	/*
	 * @return the oid of this Field's data type
	 */
	public int getOID()
	{
		return oid;
	}

	/*
	 * @return the mod of this Field's data type
	 */
	public int getMod()
	{
		return mod;
	}

	/*
	 * @return the name of this Field's data type
	 */
	public String getName()
	{
		return name;
	}

	/*
	 * @return the length of this Field's data type
	 */
	public int getLength()
	{
		return length;
	}

	/*
	 * We also need to get the PG type name as returned by the back end.
	 *
	 * @return the String representation of the PG type of this field
	 * @exception SQLException if a database access error occurs
	 */
	public String getPGType() throws SQLException
	{
		return conn.getPGType(oid);
	}

	/*
	 * We also need to get the java.sql.types type.
	 *
	 * @return the int representation of the java.sql.types type of this field
	 * @exception SQLException if a database access error occurs
	 */
	public int getSQLType() throws SQLException
	{
		return conn.getSQLType(oid);
	}

}
