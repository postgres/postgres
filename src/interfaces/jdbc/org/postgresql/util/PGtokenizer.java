package org.postgresql.util;

import java.sql.*;
import java.util.*;

/**
 * This class is used to tokenize the text output of org.postgres.
 *
 * <p>It's mainly used by the geometric classes, but is useful in parsing any
 * output from custom data types output from org.postgresql.
 *
 * @see org.postgresql.geometric.PGbox
 * @see org.postgresql.geometric.PGcircle
 * @see org.postgresql.geometric.PGlseg
 * @see org.postgresql.geometric.PGpath
 * @see org.postgresql.geometric.PGpoint
 * @see org.postgresql.geometric.PGpolygon
 */
public class PGtokenizer
{
  // Our tokens
  protected Vector	tokens;
  
  /**
   * Create a tokeniser.
   *
   * <p>We could have used StringTokenizer to do this, however, we needed to
   * handle nesting of '(' ')' '[' ']' '&lt;' and '&gt;' as these are used
   * by the geometric data types.
   *
   * @param string containing tokens
   * @param delim single character to split the tokens
   */
  public PGtokenizer(String string,char delim)
  {
    tokenize(string,delim);
  }
  
  /**
   * This resets this tokenizer with a new string and/or delimiter.
   *
   * @param string containing tokens
   * @param delim single character to split the tokens
   */
  public int tokenize(String string,char delim)
  {
    tokens = new Vector();
    
    // nest holds how many levels we are in the current token.
    // if this is > 0 then we don't split a token when delim is matched.
    //
    // The Geometric datatypes use this, because often a type may have others
    // (usualls PGpoint) imbedded within a token.
    //
    // Peter 1998 Jan 6 - Added < and > to the nesting rules
    int nest=0,p,s;
    
    for(p=0,s=0;p<string.length();p++) {
      char c = string.charAt(p);
      
      // increase nesting if an open character is found
      if(c == '(' || c == '[' || c == '<')
	nest++;
      
      // decrease nesting if a close character is found
      if(c == ')' || c == ']' || c == '>')
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
  
  /**
   * @return the number of tokens available
   */
  public int getSize()
  {
    return tokens.size();
  }
  
  /**
   * @param n Token number ( 0 ... getSize()-1 )
   * @return The token value
   */
  public String getToken(int n)
  {
    return (String)tokens.elementAt(n);
  }
  
  /**
   * This returns a new tokenizer based on one of our tokens.
   *
   * The geometric datatypes use this to process nested tokens (usually
   * PGpoint).
   *
   * @param n Token number ( 0 ... getSize()-1 )
   * @param delim The delimiter to use
   * @return A new instance of PGtokenizer based on the token
   */
  public PGtokenizer tokenizeToken(int n,char delim)
  {
    return new PGtokenizer(getToken(n),delim);
  }
  
  /**
   * This removes the lead/trailing strings from a string
   * @param s Source string
   * @param l Leading string to remove
   * @param t Trailing string to remove
   * @return String without the lead/trailing strings
   */
  public static String remove(String s,String l,String t)
  {
    if(s.startsWith(l))	s = s.substring(l.length());
    if(s.endsWith(t))	s = s.substring(0,s.length()-t.length());
    return s;
  }
  
  /**
   * This removes the lead/trailing strings from all tokens
   * @param l Leading string to remove
   * @param t Trailing string to remove
   */
  public void remove(String l,String t)
  {
    for(int i=0;i<tokens.size();i++) {
      tokens.setElementAt(remove((String)tokens.elementAt(i),l,t),i);
    }
  }
  
  /**
   * Removes ( and ) from the beginning and end of a string
   * @param s String to remove from
   * @return String without the ( or )
   */
  public static String removePara(String s)
  {
    return remove(s,"(",")");
  }
  
  /**
   * Removes ( and ) from the beginning and end of all tokens
   * @return String without the ( or )
   */
  public void removePara()
  {
    remove("(",")");
  }
  
  /**
   * Removes [ and ] from the beginning and end of a string
   * @param s String to remove from
   * @return String without the [ or ]
   */
  public static String removeBox(String s)
  {
    return remove(s,"[","]");
  }
  
  /**
   * Removes [ and ] from the beginning and end of all tokens
   * @return String without the [ or ]
   */
  public void removeBox()
  {
    remove("[","]");
  }
  
  /**
   * Removes &lt; and &gt; from the beginning and end of a string
   * @param s String to remove from
   * @return String without the &lt; or &gt;
   */
  public static String removeAngle(String s)
  {
    return remove(s,"<",">");
  }
  
  /**
   * Removes &lt; and &gt; from the beginning and end of all tokens
   * @return String without the &lt; or &gt;
   */
  public void removeAngle()
  {
    remove("<",">");
  }
}
