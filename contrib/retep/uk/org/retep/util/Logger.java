package uk.org.retep.util;

import java.io.CharArrayWriter;
import java.io.PrintWriter;

public class Logger
{
  protected static int level;
  protected static PrintWriter logger;

  public static final int NONE  = -1;
  public static final int INFO  = 0;
  public static final int ERROR = 1;
  public static final int DEBUG = 2;
  public static final int ALL   = 3;

  static {
    level = NONE;
    logger = null;
  };

  private static final String levels[] = {
    "INFO :",
    "ERROR:",
    "DEBUG:",
    "ALL  :"
  };

  public static void setLevel(int aLevel)
  {
    // Incase we have not yet set a logger
    if(logger==null) {
      logger = new PrintWriter(System.out);
    }

    if(aLevel<NONE) {
      aLevel=NONE;
    } else if(aLevel>ALL) {
      aLevel=ALL;
    }

    level=aLevel;

    if(level>NONE) {
      log(INFO,"Log level changed to",level,levels[level]);
    }
  }

  public static void setLogger(PrintWriter pw)
  {
    if(logger!=null) {
      try {
        logger.flush();
        logger.close();
      } catch(Exception ex) {
        logger=pw;
        log(ERROR,"Exception while closing logger",ex);
      }
    }
    logger=pw;
  }

  public static void log(String msg)
  {
    log(INFO,msg);
  }

  public static void log(int aLevel,String msg)
  {
    write(aLevel,msg,null);
  }

  public static void log(int aLevel,String msg,int arg1)
  {
    Object o[] = {new Integer(arg1)};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,int arg1,Object arg2)
  {
    Object o[] = {new Integer(arg1),arg2};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,double arg1)
  {
    Object o[] = {new Double(arg1)};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,double arg1,Object arg2)
  {
    Object o[] = {new Double(arg1),arg2};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,Object arg1)
  {
    Object o[] = {arg1};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,Object arg1,Object arg2)
  {
    Object o[] = {arg1,arg2};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,Object arg1,Object arg2,Object arg3)
  {
    Object o[] = {arg1,arg2,arg3};
    write(aLevel,msg,o);
  }

  public static void log(int aLevel,String msg,Throwable t)
  {
    CharArrayWriter buffer = new CharArrayWriter();
    PrintWriter printWriter = new PrintWriter(buffer);
    t.printStackTrace(printWriter);
    Object o[] = {buffer.toString()};
    buffer.close();
    write(aLevel,msg,o);
  }

  private static void write(int aLevel,String aMsg,Object args[])
  {
    // Can't be above ALL
    if(aLevel>ALL) {
      aLevel=ALL;
    }

    // Ignore if below or equal to NONE
    if(aLevel<INFO || aLevel>level) {
      return;
    }

    logger.print("Logger:");
    logger.print(levels[aLevel]);
    logger.print(aMsg);
    if(args!=null) {
      for(int a=0;a<args.length;a++) {
        logger.print(":");
        logger.print(args[a]);
      }
    }
    logger.println();
    logger.flush();
  }

}
