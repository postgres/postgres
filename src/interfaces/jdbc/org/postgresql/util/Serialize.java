package org.postgresql.util;

import java.io.*;
import java.lang.*;
import java.lang.reflect.*;
import java.net.*;
import java.util.*;
import java.sql.*;

/**
 * This class uses PostgreSQL's object oriented features to store Java Objects.
 *
 * It does this by mapping a Java Class name to a table in the database. Each
 * entry in this new table then represents a Serialized instance of this
 * class. As each entry has an OID (Object IDentifier), this OID can be
 * included in another table.
 *
 * This is too complex to show here, and will be documented in the main
 * documents in more detail.
 *
 */
public class Serialize
{
  // This is the connection that the instance refers to
  protected org.postgresql.Connection conn;

  // This is the table name
  protected String tableName;

  // This is the class name
  protected String className;

  // This is the Class for this serialzed object
  protected Class ourClass;

  /**
   * This creates an instance that can be used to serialize or deserialize
   * a Java object from a PostgreSQL table.
   */
  public Serialize(org.postgresql.Connection c,String type) throws SQLException
  {
    try {
      conn = c;
      tableName = toPostgreSQL(type);
      className = type;
      ourClass = Class.forName(className);
    } catch(ClassNotFoundException cnfe) {
      throw new PSQLException("postgresql.serial.noclass",type);
    }

    // Second check, the type must be a table
    boolean status = false;
    ResultSet rs = conn.ExecSQL("select typname from pg_type,pg_class where typname=relname and typname='" + tableName + "'");
    if(rs!=null) {
      if(rs.next())
	status=true;
      rs.close();
    }
    // This should never occur, as org.postgresql has it's own internal checks
    if(!status)
      throw new PSQLException("postgresql.serial.table",type);

    // Finally cache the fields within the table
  }

  /**
   * Constructor when Object is passed in
   */
  public Serialize(org.postgresql.Connection c,Object o) throws SQLException
  {
    this(c, o.getClass().getName());
  }

  /**
   * Constructor when Class is passed in
   */
  public Serialize(org.postgresql.Connection c, Class cls) throws SQLException
  {
    this(c, cls.getName());
  }

  /**
   * This fetches an object from a table, given it's OID
   * @param oid The oid of the object
   * @return Object relating to oid
   * @exception SQLException on error
   */
  public Object fetch(int oid) throws SQLException
  {
    try {
      Object obj = ourClass.newInstance();

      // NB: we use java.lang.reflect here to prevent confusion with
      // the org.postgresql.Field

      // used getFields to get only public fields. We have no way to set values
      // for other declarations. Maybe look for setFieldName() methods?
      java.lang.reflect.Field f[] = ourClass.getFields();

      boolean hasOID=false;
      int oidFIELD=-1;
      StringBuffer sb = new StringBuffer("select");
      char sep=' ';

      // build a select for the fields. Look for the oid field to use in the where
      for(int i=0;i<f.length;i++) {
	String n = f[i].getName();
	if(n.equals("oid")) {
	  hasOID=true;
	  oidFIELD=i;
	}
	sb.append(sep);
	sb.append(n);
	sep=',';
      }
      sb.append(" from ");
      sb.append(tableName);
      sb.append(" where oid=");
      sb.append(oid);

      DriverManager.println("store: "+sb.toString());
      ResultSet rs = conn.ExecSQL(sb.toString());
      if(rs!=null) {
	if(rs.next()) {
	  for(int i=0;i<f.length;i++) {
            if ( !Modifier.isFinal(f[i].getModifiers()) ) {

              if (f[i].getType().getName().equals("short")){
                f[i].setShort(obj, rs.getShort(i+1));
              }
              else
              if (f[i].getType().getName().equals("char")){
                f[i].setChar(obj, rs.getString(i+1).toCharArray()[0]);
              }
              else
              if (f[i].getType().getName().equals("byte")){
                f[i].setByte(obj, rs.getByte(i+1));
              }
              else
              // booleans come out of pgsql as a t or an f
              if (f[i].getType().getName().equals("boolean")){
                if ( rs.getString(i+1).equals("t"))
                  f[i].setBoolean(obj, true);
                else
                  f[i].setBoolean(obj, false);
              }
              else{
	        f[i].set(obj,rs.getObject(i+1));
              }
            }
	  }
	}
	rs.close();
      } else
       throw new PSQLException("postgresql.unexpected");
      return obj;
    } catch(IllegalAccessException iae) {
      throw new SQLException(iae.toString());
    } catch(InstantiationException ie) {
      throw new SQLException(ie.toString());
    }
  }

  /**
   * This stores an object into a table, returning it's OID.<p>
   *
   * If the object has an int called OID, and it is > 0, then
   * that value is used for the OID, and the table will be updated.
   * If the value of OID is 0, then a new row will be created, and the
   * value of OID will be set in the object. This enables an object's
   * value in the database to be updateable.
   *
   * If the object has no int called OID, then the object is stored. However
   * if the object is later retrieved, amended and stored again, it's new
   * state will be appended to the table, and will not overwrite the old
   * entries.
   *
   * @param o Object to store (must implement Serializable)
   * @return oid of stored object
   * @exception SQLException on error
   */
  public int store(Object o) throws SQLException
  {
    try {
      // NB: we use java.lang.reflect here to prevent confusion with
      // the org.postgresql.Field

      // don't save private fields since we would not be able to fetch them
      java.lang.reflect.Field f[] = ourClass.getFields();

      boolean hasOID=false;
      int oidFIELD=-1;
      boolean update=false;

      // Find out if we have an oid value
      for(int i=0;i<f.length;i++) {
	String n = f[i].getName();
	if(n.equals("oid")) {
	  hasOID=true;
	  oidFIELD=i;

	  // We are an update if oid != 0
	  update = f[i].getInt(o)>0;
	}
      }

      StringBuffer sb = new StringBuffer(update?"update "+tableName+" set":"insert into " + tableName);
      char sep=update?' ':'(';
      for(int i=0;i<f.length;i++) {
	String n = f[i].getName();
	sb.append(sep);
	sb.append(n);
	sep=',';
	if(update) {
	  sb.append('=');
          // handle unset values
          if (f[i].get(o) == null)
            sb.append("null");
          else
	  if(f[i].getType().getName().equals("java.lang.String") ||
             f[i].getType().getName().equals("char")) {
	    sb.append('\'');
            // don't allow single qoutes or newlines in the string
	    sb.append(fixString(f[i].get(o).toString()));
	    sb.append('\'');
	  } else
	    sb.append(f[i].get(o).toString());
	}
      }

      if(!update) {
	sb.append(") values ");
	sep='(';
	for(int i=0;i<f.length;i++) {
	  sb.append(sep);
	  sep=',';
          // handle unset values
          if (f[i].get(o) == null)
            sb.append("null");
          else
	  if(f[i].getType().getName().equals("java.lang.String") ||
             f[i].getType().getName().equals("char")) {
	    sb.append('\'');
            // don't allow single quotes or newlines in the string
	    sb.append(fixString(f[i].get(o).toString()));
	    sb.append('\'');
	  } else
	    sb.append(f[i].get(o).toString());
	}
	sb.append(')');
      }

      DriverManager.println("store: "+sb.toString());
      org.postgresql.ResultSet rs = (org.postgresql.ResultSet)conn.ExecSQL(sb.toString());

      // fetch the OID for returning
      int oid=0;
      if(hasOID) {
	// If an update use the existing oid in the object
	f[oidFIELD].setInt(o,oid);
      }
      else {
        String statStr = rs.getStatusString();
        oid = Integer.parseInt(statStr.substring(statStr.indexOf(" ") + 1, statStr.lastIndexOf(" ")));
      }

      if(rs!=null) {
	rs.close();
      }

      return oid;

    } catch(IllegalAccessException iae) {
      throw new SQLException(iae.toString());
    }
  }

  /**
   *
   */
   private String fixString(String s) {

   int idx = -1;

     // handle null
     if (s == null)
       return "";

     // if the string has single quotes in it escape them
     if ((idx = s.indexOf("'")) > -1) {
       StringBuffer buf = new StringBuffer();
       StringTokenizer tok = new StringTokenizer(s, "'");
       // handle quote as 1St charater
       if (idx > 0) buf.append(tok.nextToken());

       while(tok.hasMoreTokens())
         buf.append("\\'").append(tok.nextToken());

       s = buf.toString();
     }

     // if the string has newlines in it convert them to \n
     if ((idx = s.indexOf("\n")) > -1) {
       StringBuffer buf = new StringBuffer();
       StringTokenizer tok = new StringTokenizer(s, "\n");
       if (idx > 0) buf.append(tok.nextToken());

       while(tok.hasMoreTokens())
         buf.append("\\n").append(tok.nextToken());

       s = buf.toString();
     }

     return s;

   }

  /**
   * This method is not used by the driver, but it creates a table, given
   * a Serializable Java Object. It should be used before serializing any
   * objects.
   * @param c Connection to database
   * @param o Object to base table on
   * @exception SQLException on error
   */
  public static void create(org.postgresql.Connection con,Object o) throws SQLException
  {
    create(con,o.getClass());
  }

  /**
   * This method is not used by the driver, but it creates a table, given
   * a Serializable Java Object. It should be used before serializing any
   * objects.
   * @param c Connection to database
   * @param o Class to base table on
   * @exception SQLException on error
   */
  public static void create(org.postgresql.Connection con,Class c) throws SQLException
  {
    if(c.isInterface())
      throw new PSQLException("postgresql.serial.interface");

    // See if the table exists
    String tableName = toPostgreSQL(c.getName());

    ResultSet rs = con.ExecSQL("select relname from pg_class where relname = '"+tableName+"'");
    if(!rs.next()) {
//      DriverManager.println("found "+rs.getString(1));
      // No entries returned, so the table doesn't exist

      StringBuffer sb = new StringBuffer("create table ");
      sb.append(tableName);
      char sep='(';

//      java.lang.reflect.Field[] fields = c.getDeclaredFields();
      java.lang.reflect.Field[] fields = c.getFields();
      for(int i=0;i<fields.length;i++) {
	Class type = fields[i].getType();

	// oid is a special field
	if(!fields[i].getName().equals("oid")) {
	  sb.append(sep);
	  sb.append(fields[i].getName());
	  sb.append(' ');
	  sep=',';

	  if(type.isArray()) {
	    // array handling
	  } else {
	    // convert the java type to org.postgresql, recursing if a class
	    // is found
	    String n = type.getName();
	    int j=0;
	    for(;j<tp.length && !tp[j][0].equals(n);j++);
	    if(j<tp.length)
	      sb.append(tp[j][1]);
	    else {
	      create(con, type);
	      sb.append(toPostgreSQL(n));
	    }
	  }
	}
      }
      sb.append(")");

      // Now create the table
      DriverManager.println("Serialize.create:"+sb);
      con.ExecSQL(sb.toString());
      rs.close();
    } else {
      DriverManager.println("Serialize.create: table "+tableName+" exists, skipping");
    }
  }

  // This is used to translate between Java primitives and PostgreSQL types.
  private static final String tp[][] = {
//    {"boolean",			"int1"},
    {"boolean",			"bool"},
    {"double",			"float8"},
    {"float",			"float4"},
    {"int",			"int4"},
//    {"long",			"int4"},
    {"long",			"int8"},
    {"short",			"int2"},
    {"java.lang.String",	"text"},
    {"java.lang.Integer",	"int4"},
    {"java.lang.Float",		"float4"},
    {"java.lang.Double",	"float8"},
    {"java.lang.Short",		"int2"},
    {"char",                    "char"},
    {"byte",                    "int2"}
  };

  /**
   * This converts a Java Class name to a org.postgresql table, by replacing . with
   * _<p>
   *
   * Because of this, a Class name may not have _ in the name.<p>
   * Another limitation, is that the entire class name (including packages)
   * cannot be longer than 32 characters (a limit forced by PostgreSQL).
   *
   * @param name Class name
   * @return PostgreSQL table name
   * @exception SQLException on error
   */
  public static String toPostgreSQL(String name) throws SQLException
  {
    name = name.toLowerCase();

    if(name.indexOf("_")>-1)
      throw new PSQLException("postgresql.serial.underscore");

    // Postgres table names can only be 32 character long
    // If the full class name with package is too long
    // then just use the class name. If the class name is
    // too long throw an exception.
    if(name.length() > 32) {
      name = name.substring(name.lastIndexOf(".") + 1);

      if(name.length()>32)
        throw new PSQLException("postgresql.serial.namelength",name,new Integer(name.length()));
    }

    return name.replace('.','_');
  }


  /**
   * This converts a org.postgresql table to a Java Class name, by replacing _ with
   * .<p>
   *
   * @param name PostgreSQL table name
   * @return Class name
   * @exception SQLException on error
   */
  public static String toClassName(String name) throws SQLException
  {
    name = name.toLowerCase();
    return name.replace('_','.');
  }

}
