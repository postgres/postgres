/**
 *
 * This implements a polygon (based on java.awt.Polygon)
 *
 */

package postgresql;

import java.io.*;
import java.sql.*;

public class PGpolygon implements Serializable
{
  public int npoints;
  
  public PGpoint point[];
  
  public PGpolygon(int num,PGpoint[] points)
  {
    npoints = num;
    this.point = points;
  }
  
  /**
   * This constructor is used by the driver.
   */
  public PGpolygon(String s) throws SQLException
  {
    PGtokenizer t = new PGtokenizer(PGtokenizer.removePara(s),',');
    npoints = t.getSize();
    point = new PGpoint[npoints];
    for(int p=0;p<npoints;p++)
      point[p] = new PGpoint(t.getToken(p));
  }
  
  public boolean equals(Object obj)
  {
    PGpolygon p = (PGpolygon)obj;
    
    if(p.npoints != npoints)
      return false;
    
    for(int i=0;i<npoints;i++)
      if(!point[i].equals(p.point[i]))
	return false;
    
    return true;
  }
  
  /**
   * This returns the polygon in the syntax expected by postgresql
   */
  public String toString()
  {
    StringBuffer b = new StringBuffer();
    for(int p=0;p<npoints;p++)
      b.append(point[p].toString());
    return b.toString();
  }
}
