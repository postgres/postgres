package org.postgresql.geometric;

import java.io.*;
import java.sql.*;
import org.postgresql.util.*;

/**
 * This  represents the box datatype within org.postgresql.
 */
public class PGbox extends PGobject implements Serializable,Cloneable
{
  /**
   * These are the two points.
   */
  public PGpoint point[] = new PGpoint[2];
  
  /**
   * @param x1 first x coordinate
   * @param y1 first y coordinate
   * @param x2 second x coordinate
   * @param y2 second y coordinate
   */
  public PGbox(double x1,double y1,double x2,double y2)
  {
    this();
    this.point[0] = new PGpoint(x1,y1);
    this.point[1] = new PGpoint(x2,y2);
  }
  
  /**
   * @param p1 first point
   * @param p2 second point
   */
  public PGbox(PGpoint p1,PGpoint p2)
  {
    this();
    this.point[0] = p1;
    this.point[1] = p2;
  }
  
  /**
   * @param s Box definition in PostgreSQL syntax
   * @exception SQLException if definition is invalid
   */
  public PGbox(String s) throws SQLException
  {
    this();
    setValue(s);
  }
  
  /**
   * Required constructor
   */
  public PGbox()
  {
    setType("box");
  }
  
  /**
   * This method sets the value of this object. It should be overidden,
   * but still called by subclasses.
   *
   * @param value a string representation of the value of the object
   * @exception SQLException thrown if value is invalid for this type
   */
  public void setValue(String value) throws SQLException
  {
    PGtokenizer t = new PGtokenizer(value,',');
    if(t.getSize() != 2)
      throw new PSQLException("postgresql.geo.box",value);
    
    point[0] = new PGpoint(t.getToken(0));
    point[1] = new PGpoint(t.getToken(1));
  }
  
  /**
   * @param obj Object to compare with
   * @return true if the two boxes are identical
   */
  public boolean equals(Object obj)
  {
    if(obj instanceof PGbox) {
      PGbox p = (PGbox)obj;
      return (p.point[0].equals(point[0]) && p.point[1].equals(point[1])) ||
	(p.point[0].equals(point[1]) && p.point[1].equals(point[0]));
    }
    return false;
  }
  
  /**
   * This must be overidden to allow the object to be cloned
   */
  public Object clone()
  {
    return new PGbox((PGpoint)point[0].clone(),(PGpoint)point[1].clone());
  }
  
  /**
   * @return the PGbox in the syntax expected by org.postgresql
   */
  public String getValue()
  {
    return point[0].toString()+","+point[1].toString();
  }
}
