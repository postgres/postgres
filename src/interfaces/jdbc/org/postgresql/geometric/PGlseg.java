package org.postgresql.geometric;

import java.io.*;
import java.sql.*;
import org.postgresql.util.*;

/**
 * This implements a lseg (line segment) consisting of two points
 */
public class PGlseg extends PGobject implements Serializable,Cloneable
{
  /**
   * These are the two points.
   */
  public PGpoint point[] = new PGpoint[2];
  
  /**
   * @param x1 coordinate for first point
   * @param y1 coordinate for first point
   * @param x2 coordinate for second point
   * @param y2 coordinate for second point
   */
  public PGlseg(double x1,double y1,double x2,double y2)
  {
    this(new PGpoint(x1,y1),new PGpoint(x2,y2));
  }
  
  /**
   * @param p1 first point
   * @param p2 second point
   */
  public PGlseg(PGpoint p1,PGpoint p2)
  {
    this();
    this.point[0] = p1;
    this.point[1] = p2;
  }
  
  /**
   * @param s definition of the circle in PostgreSQL's syntax.
   * @exception SQLException on conversion failure
   */
  public PGlseg(String s) throws SQLException
  {
    this();
    setValue(s);
  }
  
  /**
   * reuired by the driver
   */
  public PGlseg()
  {
    setType("lseg");
  }
  
  /**
   * @param s Definition of the line segment in PostgreSQL's syntax
   * @exception SQLException on conversion failure
   */
  public void setValue(String s) throws SQLException
  {
    PGtokenizer t = new PGtokenizer(PGtokenizer.removeBox(s),',');
    if(t.getSize() != 2)
      throw new PSQLException("postgresql.geo.lseg");
    
    point[0] = new PGpoint(t.getToken(0));
    point[1] = new PGpoint(t.getToken(1));
  }
  
  /**
   * @param obj Object to compare with
   * @return true if the two boxes are identical
   */
  public boolean equals(Object obj)
  {
    if(obj instanceof PGlseg) {
      PGlseg p = (PGlseg)obj;
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
    return new PGlseg((PGpoint)point[0].clone(),(PGpoint)point[1].clone());
  }
  
  /**
   * @return the PGlseg in the syntax expected by org.postgresql
   */
  public String getValue()
  {
    return "["+point[0]+","+point[1]+"]";
  }
}
