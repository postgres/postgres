package postgresql;

import java.lang.*;
import java.sql.*;
import java.util.*;
import postgresql.*;

/**
 * postgresql.PG_Object is a class used to describe unknown types 
 * An unknown type is any type that is unknown by JDBC Standards
 *
 * @version 1.0 15-APR-1997
 * @author <A HREF="mailto:adrian@hottub.org">Adrian Hall</A>
 */
public class PG_Object
{
  public String	type;
  public String	value;
  
  /**
   *	Constructor for the PostgreSQL generic object
   *
   * @param type a string describing the type of the object
   * @param value a string representation of the value of the object
   */
  public PG_Object(String type, String value) throws SQLException
  {
    this.type = type;
    this.value = value;
  }
  
  /**
   * This returns true if the object is a 'box'
   */
  public boolean isBox()
  {
    return type.equals("box");
  }
  
  /**
   * This returns a PGbox object, or null if it's not
   * @return PGbox
   */
  public PGbox getBox() throws SQLException
  {
    if(isBox())
      return new PGbox(value);
    return null;
  }
  
  /**
   * This returns true if the object is a 'point'
   */
  public boolean isCircle()
  {
    return type.equals("circle");
  }
  
  /**
   * This returns a PGcircle object, or null if it's not
   * @return PGcircle
   */
  public PGcircle getCircle() throws SQLException
  {
    if(isCircle())
      return new PGcircle(value);
    return null;
  }
  
  /**
   * This returns true if the object is a 'lseg' (line segment)
   */
  public boolean isLseg()
  {
    return type.equals("lseg");
  }
  
  /**
   * This returns a PGlsegobject, or null if it's not
   * @return PGlseg
   */
  public PGlseg getLseg() throws SQLException
  {
    if(isLseg())
      return new PGlseg(value);
    return null;
  }
  
  /**
   * This returns true if the object is a 'path'
   */
  public boolean isPath()
  {
    return type.equals("path");
  }
  
  /**
   * This returns a PGpath object, or null if it's not
   * @return PGpath
   */
  public PGpath getPath() throws SQLException
  {
    if(isPath())
      return new PGpath(value);
    return null;
  }
  
  /**
   * This returns true if the object is a 'point'
   */
  public boolean isPoint()
  {
    return type.equals("point");
  }
  
  /**
   * This returns a PGpoint object, or null if it's not
   * @return PGpoint object
   */
  public PGpoint getPoint() throws SQLException
  {
    if(isPoint())
      return new PGpoint(value);
    return null;
  }
  
  /**
   * This returns true if the object is a 'polygon'
   */
  public boolean isPolygon()
  {
    return type.equals("polygon");
  }
  
  /**
   * This returns a PGpolygon object, or null if it's not
   * @return PGpolygon
   */
  public PGpolygon getPolygon() throws SQLException
  {
    if(isPolygon())
      return new PGpolygon(value);
    return null;
  }
}
