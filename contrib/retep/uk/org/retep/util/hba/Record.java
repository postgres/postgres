package uk.org.retep.util.hba;

import uk.org.retep.util.Logger;
import uk.org.retep.util.misc.IPAddress;
import uk.org.retep.util.misc.WStringTokenizer;

/**
 * Used to store the entries of a pg_hba.conf file
 * @author
 * @version 1.0
 */

public class Record
{
  int       type;
  String    dbname;
  IPAddress ip;
  IPAddress mask;
  int       authType;
  String    authArg;

  public static final int TYPE_LOCAL    = 0;
  public static final int TYPE_HOST     = 1;
  public static final int TYPE_HOSTSSL  = 2;

  public static final String types[] = {
    "local","host","hostssl"
  };

  public static final int AUTH_TRUST    = 0;
  public static final int AUTH_PASSWORD = 1;
  public static final int AUTH_CRYPT    = 2;
  public static final int AUTH_IDENT    = 3;
  public static final int AUTH_KRB4     = 4;
  public static final int AUTH_KRB5     = 5;
  public static final int AUTH_REJECT   = 6;

  public static final String auths[] = {
    "trust","password","crypt",
    "ident",
    "krb4","krb5",
    "reject"
  };

  private static final String spc = " ";

  public Record()
  {
  }

  public int getType()
  {
    return type;
  }

  public void setType(int aType)
  {
    type=aType;
  }

  public String getDatabase()
  {
    return dbname;
  }

  public void setDatabase(String aDB)
  {
    dbname=aDB;
  }

  public int getAuthType()
  {
    return authType;
  }

  public void setAuthType(int aType)
  {
    authType=aType;
  }

  public String getAuthArgs()
  {
    return authArg;
  }

  public void setAuthArgs(String aArg)
  {
    authArg=aArg;
  }

  public IPAddress getIP()
  {
    return ip;
  }

  public void setIP(String aArg)
  {
    setIP(new IPAddress(aArg));
  }

  public void setIP(IPAddress aArg)
  {
    ip=aArg;
  }

    public IPAddress getMask()
  {
    return mask;
  }

  public void setMask(String aArg)
  {
    setMask(new IPAddress(aArg));
  }

  public void setMask(IPAddress aArg)
  {
    mask=aArg;
  }

  public String toString()
  {
    StringBuffer buf = new StringBuffer();
    write(buf);
    return buf.toString();
  }

  public void write(StringBuffer buf)
  {
    buf.append(types[type]).append(spc);

    if(type==TYPE_HOST || type==TYPE_HOSTSSL) {
      buf.append(getIP()).append(spc);
      buf.append(getMask()).append(spc);
    }

    buf.append(auths[authType]);

    // Now the authArg
    switch(type)
    {
      // These have no authArgs
      case AUTH_TRUST:
      case AUTH_REJECT:
      case AUTH_KRB4:
      case AUTH_KRB5:
        break;

      // These must have an arg
      case AUTH_IDENT:
        buf.append(spc).append(getAuthArgs());
        break;

      // These may have an optional arg
      case AUTH_PASSWORD:
      case AUTH_CRYPT:
        if(!(authArg==null || authArg.equals("")))
          buf.append(spc).append(getAuthArgs());
        break;
    }
  }

  private static WStringTokenizer tok;

  public static Record parseLine(String s)
  {
    Record res = new Record();
    int type;

    if(s==null || s.equals("") || s.startsWith("#"))
      return null;

    if(tok==null)
      tok=new WStringTokenizer();

    tok.setString(s);

    type=WStringTokenizer.matchToken(types,tok.nextToken());
    res.setType(type);

    res.setDatabase(tok.nextToken());

    if(type==TYPE_HOST || type==TYPE_HOSTSSL) {
      res.setIP(new IPAddress(tok.nextToken()));
      res.setMask(new IPAddress(tok.nextToken()));
    }

    res.setAuthType(WStringTokenizer.matchToken(auths,tok.nextToken()));
    res.setAuthArgs(tok.nextToken());

    return res;
  }

  public static final int VALID         = 0;
  public static final int INVALID_TYPE  = 1;
  public static final int INVALID_IPREQUIRED  = 2;

  /**
   * Validates the record
   */
  public int validate()
  {
    switch(type)
    {
      case TYPE_HOST:
      case TYPE_HOSTSSL:
        if(ip==null || ip.isInvalid()) {
          Logger.log(Logger.INFO,"pg_hba.conf: IP missing or invalid - repairing");
          setMask("127.0.0.1");
        }

        if(mask==null || mask.isInvalid() || !ip.validateMask(mask)) {
          Logger.log(Logger.INFO,"pg_hba.conf: IP address without mask - repairing");
          setMask(ip.getMask());
        }

        break;

      case TYPE_LOCAL:
        break;

      default:
        return INVALID_TYPE;
    }

    return VALID;
  }

  /*
# host       all        192.168.54.1   255.255.255.255    reject
# host       all        0.0.0.0        0.0.0.0            krb5
# host       all        192.168.0.0    255.255.0.0        ident     omicron
#

local        all                                           trust
host         all         127.0.0.1     255.255.255.255     trust
*/
}