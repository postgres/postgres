/**
 *
 * This class is used to tokenize the text output of postgres.
 *
 */

package postgresql;

import java.sql.*;
import java.util.*;

public class PGtokenizer
{
  protected Vector	tokens;
  
  public PGtokenizer(String string,char delim)
  {
    tokenize(string,delim);
  }
  
  /**
   * Tokenizes a new string
   */
  public int tokenize(String string,char delim)
  {
    tokens = new Vector();
    
    int nest=0,p,s;
    for(p=0,s=0;p<string.length();p++) {
      char c = string.charAt(p);
      
      // increase nesting if an open character is found
      if(c == '(' || c == '[')
	nest++;
      
      // decrease nesting if a close character is found
      if(c == ')' || c == ']')
	nest--;
      
      if(nest==0 && c==delim) {
	tokens.addElement(string.substring(s,p));
	s=p+1; // +1 to skip the delimiter
      }
      
    }
    
    // Don't forget the last token ;-)
    if(s<string.length())
      tokens.addElement(string.substring(s));
    
    return tokens.size();
  }
  
  public int getSize()
  {
    return tokens.size();
  }
  
  public String getToken(int n)
  {
    return (String)tokens.elementAt(n);
  }
  
  /**
   * This returns a new tokenizer based on one of our tokens
   */
  public PGtokenizer tokenizeToken(int n,char delim)
  {
    return new PGtokenizer(getToken(n),delim);
  }
  
  /**
   * This removes the lead/trailing strings from a string
   */
  public static String remove(String s,String l,String t)
  {
    if(s.startsWith(l))	s = s.substring(l.length());
    if(s.endsWith(t))	s = s.substring(0,s.length()-t.length());
    return s;
  }
  
  /**
   * This removes the lead/trailing strings from all tokens
   */
  public void remove(String l,String t)
  {
    for(int i=0;i<tokens.size();i++) {
      tokens.setElementAt(remove((String)tokens.elementAt(i),l,t),i);
    }
  }
  
  public static String removePara(String s)	{return remove(s,"(",")");}
  public void removePara()			{remove("(",")");}
  
  public static String removeBox(String s)	{return remove(s,"[","]");}
  public void removeBox()			{remove("[","]");}
  
  public static String removeAngle(String s)	{return remove(s,"<",">");}
  public void removeAngle()			{remove("<",">");}
}
