/**
 *
 * This implements a path (a multiple segmented line, which may be closed)
 *
 */

package postgresql;

import java.io.*;
import java.sql.*;

public class PGpath implements Serializable
{
  public int npoints;
  public boolean open;
  public PGpoint point[];
  
  public PGpath(int num,PGpoint[] points,boolean open)
  {
    npoints = num;
    this.point = points;
    this.open = open;
  }
  
  /**
   * This constructor is used by the driver.
   */
  public PGpath(String s) throws SQLException
  {
    // First test to see if were open
    if(s.startsWith("[") && s.endsWith("]")) {
      open = true;
      s = PGtokenizer.removeBox(s);
    } else if(s.startsWith("(") && s.endsWith(")")) {
      open = false;
      s = PGtokenizer.removePara(s);
    } else
      throw new SQLException("cannot tell if path is open or closed");
    
    PGtokenizer t = new PGtokenizer(s,',');
    npoints = t.getSize();
    point = new PGpoint[npoints];
    for(int p=0;p<npoints;p++)
      point[p] = new PGpoint(t.getToken(p));
  }
  
  public boolean equals(Object obj)
  {
    PGpath p = (PGpath)obj;
    
    if(p.npoints != npoints)
      return false;
    
    if(p.open != open)
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
    StringBuffer b = new StringBuffer(open?"[":"(");
    
    for(int p=0;p<npoints;p++)
      b.append(point[p].toString());
    
    b.append(open?"]":")");
    
    return b.toString();
  }
  
  public boolean isOpen()
  {
    return open;
  }
  
  public boolean isClosed()
  {
    return !open;
  }
  
  public void closePath()
  {
    open = false;
  }
  
  public void openPath()
  {
    open = true;
  }
  
}
