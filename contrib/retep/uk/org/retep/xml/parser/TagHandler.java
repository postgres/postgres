package uk.org.retep.xml.parser;

import java.io.CharArrayWriter;
import java.io.IOException;
import java.util.List;
import java.util.Iterator;
import java.util.Map;
import java.util.HashSet;
import java.util.ArrayList;
import java.util.HashMap;
import org.xml.sax.AttributeList;
import org.xml.sax.HandlerBase;
import org.xml.sax.InputSource;
import org.xml.sax.Parser;
import org.xml.sax.SAXException;
import javax.xml.parsers.ParserConfigurationException;
import javax.xml.parsers.SAXParser;
import javax.xml.parsers.SAXParserFactory;

/**
 * This class implements the base of the XML handler. You create an instance,
 * register classes (who implement TagListener) that are interested in the tags
 * and pass it to SAX.
 *
 * <p>Or you create an instance, register the TagListeners and use the getParser()
 * method to create a Parser. Then start parsing by calling it's parse() method.
 */

public class TagHandler extends HandlerBase {

  /**
   * The current active level
   */
  private int level;

  /**
   * cache used to handle nesting of tags
   */
  private List contents;

  /**
   * cache used to handle nesting of tags
   */
  private List tags;

  /**
   * cache used to handle nesting of tags
   */
  private List args;

  // Current active content writer
  private CharArrayWriter content;

  // List of TagListener's who want to be fed data
  private HashSet tagListeners;

  /**
   * default constructor
   */
  public TagHandler() {
    level=0;
    contents = new ArrayList();
    tags = new ArrayList();
    args = new ArrayList();
    tagListeners = new HashSet();
  }

  /**
   * Called by SAX when a tag is begun. This simply creates a new level in the
   * cache and stores the parameters and tag name in there.
   */
  public void startElement(String p0, AttributeList p1) throws SAXException {

    // Now move up and fetch a CharArrayWriter from the cache
    // creating if this is the first time at this level
    if(contents.size()<=level) {
      contents.add(new CharArrayWriter());
      tags.add(p0);
      args.add(new HashMap());
    }

    content=(CharArrayWriter) contents.get(level);
    content.reset();

    // Also cache the tag's text and argument list
    tags.set(level,p0);

    HashMap h = (HashMap) args.get(level);
    h.clear();
    for(int i=p1.getLength()-1;i>-1;i--) {
      h.put(p1.getName(i),p1.getValue(i));
    }

    // Now notify any TagListeners
    Iterator it = tagListeners.iterator();
    while(it.hasNext())
      ( (TagListener) it.next() ).tagStart(level,p0,h);

    // Now move up a level
    level++;
  }

  /**
   * This is called by SAX at the end of a tag. This calls handleTag() and then
   * raises the level, so that the previous parent tag may continue.
   */
  public void endElement(String p0) throws SAXException {
    // move up a level retrieving that level's current content
    // Now this exception should never occur as the underlying parser should
    // actually trap it.
    if(level<1)
      throw new SAXException("Already at top level?");
    level--;

    // Now notify any TagListeners
    Iterator it = tagListeners.iterator();
    while(it.hasNext())
      ( (TagListener) it.next() ).tagContent(content);

    // allows large content to be released early
    content.reset();

    // Now reset content to the previous level
    content=(CharArrayWriter) contents.get(level);
  }

  /**
   * Called by SAX so that content between the start and end tags are captured.
   */
  public void characters(char[] p0, int p1, int p2) throws SAXException {
    content.write(p0,p1,p2);
  }

  /**
   * Adds a TagListener so that it is notified of tags as they are processed.
   * @param handler TagListener to add
   */
  public void addTagListener(TagListener h) {
    tagListeners.add(h);
  }

  /**
   * Removes the TagListener so it no longer receives notifications of tags
   */
  public void removeTagListener(TagListener h) {
    tagListeners.remove(h);
  }

  /**
   * This method returns a org.xml.sax.Parser object that will parse the
   * contents of a URI.
   *
   * <p>Normally you would call this method, then call the parse(uri) method of
   * the returned object.
   * @return org.xml.sax.Parser object
   */
  public Parser getParser()
  throws SAXException
  {
    try {
      SAXParserFactory spf = SAXParserFactory.newInstance();

      String validation = System.getProperty ("javax.xml.parsers.validation", "false");
      if (validation.equalsIgnoreCase("true"))
        spf.setValidating (true);

      SAXParser sp = spf.newSAXParser();
      Parser parser = sp.getParser ();

      parser.setDocumentHandler(this);

      return(parser);
    } catch(ParserConfigurationException pce) {
      throw new SAXException(pce.toString());
    }
  }

  /**
   * This method will parse the specified URI.
   *
   * <p>Internally this is the same as getParser().parse(uri);
   * @param uri The URI to parse
   */
  public void parse(String uri)
  throws IOException, SAXException
  {
    getParser().parse(uri);
  }

  /**
   * This method will parse the specified InputSource.
   *
   * <p>Internally this is the same as getParser().parse(is);
   * @param is The InputSource to parse
   */
  public void parse(InputSource is)
  throws IOException, SAXException
  {
    getParser().parse(is);
  }

}