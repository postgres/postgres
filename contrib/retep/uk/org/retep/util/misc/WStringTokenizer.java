package uk.org.retep.util.misc;

/**
 * Similar to StringTokenizer but handles white spaces and multiple delimiters
 * between tokens. It also handles quotes
 *
 * @author
 * @version 1.0
 */

public class WStringTokenizer
{
  String string;
  int pos,len;

  /**
   * Constructor
   */
  public WStringTokenizer()
  {
  }

  /**
   * Constructor: set the initial string
   * @param aString String to tokenise
   */
  public WStringTokenizer(String aString)
  {
    setString(aString);
  }

  /**
   * @param aString String to tokenise
   */
  public void setString(String aString)
  {
    string=aString;
    pos=0;
    len=string.length();
  }

  /**
   * @return true if more tokens may be possible
   */
  public boolean hasMoreTokens()
  {
    return !(string==null || pos==len);
  }

  /**
   * @return next token, null if complete.
   */
  public String nextToken()
  {
    char c;
    boolean q=false;

    if(!hasMoreTokens())
      return null;

    // find start of token
    while(pos<len) {
      c = string.charAt(pos);
      if(c=='\'' || c=='\"')
        q=!q;
      if(q || c==' '||c=='\t')
        pos++;
      else
        break;
    }

    // find last char of token
    int p=pos;
    while(pos<len) {
      c = string.charAt(pos);
      if(c=='\'' || c=='\"')
        q=!q;
      if(!q && (c==' '||c=='\t') )
        break;
      else
        pos++;
    }

    return string.substring(p,pos);
  }

  /**
   * Compare a string against an array of strings and return the index
   * @param t array to compare against (all lowercase)
   * @param s string to test
   * @return index in t of s, -1 if not present
   */
  public static int matchToken(String[] t,String s)
  {
    s=s.toLowerCase();
    for(int i=0;i<t.length;i++)
      if(t[i].equals(s))
        return i;
    return -1;
  }

}