<!-- $Header: /cvsroot/pgsql/doc/src/sgml/stylesheet.dsl,v 1.24.2.2 2004/06/29 20:46:35 petere Exp $ -->
<!DOCTYPE style-sheet PUBLIC "-//James Clark//DTD DSSSL Style Sheet//EN" [

<!-- must turn on one of these with -i on the jade command line -->
<!ENTITY % output-html          "IGNORE">
<!ENTITY % output-print         "IGNORE">
<!ENTITY % output-text          "IGNORE">

<![ %output-html; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN" CDATA DSSSL>
]]>

<![ %output-print; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook Print Stylesheet//EN" CDATA DSSSL>
]]>

<![ %output-text; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN" CDATA DSSSL>
]]>

]>

<style-sheet>
 <style-specification use="docbook">
  <style-specification-body> 

<!-- general customization ......................................... -->

<!-- (applicable to all output formats) -->

(define draft-mode              #f)

(define pgsql-docs-list "pgsql-docs@postgresql.org")

;; Don't show manpage volume numbers
(define %refentry-xref-manvolnum% #f)

;; Don't use graphics for callouts.  (We could probably do that, but
;; it needs extra work.)
(define %callout-graphics%      #f)

;; Show comments during the development stage.
(define %show-comments%         draft-mode)

;; Don't append period if run-in title ends with any of these
;; characters.  We had to add the colon here.  This is fixed in
;; stylesheets version 1.71, so it can be removed sometime.
(define %content-title-end-punct% 
  '(#\. #\! #\? #\:))

;; No automatic punctuation after honorific name parts
(define %honorific-punctuation% "")

;; Change display of some elements
(element command ($mono-seq$))
(element envar ($mono-seq$))
(element lineannotation ($italic-seq$))
(element literal ($mono-seq$))
(element option ($mono-seq$))
(element parameter ($mono-seq$))
(element structfield ($mono-seq$))
(element structname ($mono-seq$))
(element symbol ($mono-seq$))
(element token ($mono-seq$))
(element type ($mono-seq$))
(element varname ($mono-seq$))
(element (programlisting emphasis) ($bold-seq$)) ;; to highlight sections of code

;; Special support for Tcl synopses
(element optional
  (if (equal? (attribute-string (normalize "role")) "tcl")
      (make sequence
        (literal "?")
        ($charseq$)
        (literal "?"))
      (make sequence
        (literal %arg-choice-opt-open-str%)
        ($charseq$)
        (literal %arg-choice-opt-close-str%))))

;; Avoid excessive cross-reference labels
(define (auto-xref-indirect? target ancestor)
  (cond
;   ;; Always add indirect references to another book
;   ((member (gi ancestor) (book-element-list))
;    #t)
   ;; Add indirect references to the section or component a block
   ;; is in iff chapters aren't autolabelled.  (Otherwise "Figure 1-3"
   ;; is sufficient)
   ((and (member (gi target) (block-element-list))
         (not %chapter-autolabel%))
    #t)
   ;; Add indirect references to the component a section is in if
   ;; the sections are not autolabelled
   ((and (member (gi target) (section-element-list))
         (member (gi ancestor) (component-element-list))
         (not %section-autolabel%))
    #t)
   (else #f)))


;; Bibliography things

;; Use the titles of bibliography entries in cross-references
(define biblio-xref-title       #t)

;; Process bibliography entry components in the order shown below, not
;; in the order they appear in the document.  (I suppose this should
;; be made to fit some publishing standard.)
(define %biblioentry-in-entry-order% #f)

(define (biblioentry-inline-elements)
  (list
   (normalize "author")
   (normalize "authorgroup")
   (normalize "title")
   (normalize "subtitle")   
   (normalize "volumenum")
   (normalize "edition")
   (normalize "othercredit")
   (normalize "contrib")
   (normalize "editor")
   (normalize "publishername")
   (normalize "confgroup")
   (normalize "publisher")
   (normalize "isbn")
   (normalize "issn")
   (normalize "pubsnumber")
   (normalize "date")
   (normalize "pubdate")
   (normalize "pagenums")
   (normalize "bibliomisc")))

(mode biblioentry-inline-mode

  (element confgroup
    (make sequence
      (literal "Proc. ")
      (next-match)))

  (element isbn
    (make sequence
      (literal "ISBN ")
      (process-children)))

  (element issn
    (make sequence
      (literal "ISSN ")
      (process-children))))


;; The rules in the default stylesheet for productname format it as a
;; paragraph.  This may be suitable for productname directly within
;; *info, but it's nonsense when productname is used inline, as we do.
(mode set-titlepage-recto-mode
  (element (para productname) ($charseq$)))
(mode set-titlepage-verso-mode
  (element (para productname) ($charseq$)))
(mode book-titlepage-recto-mode
  (element (para productname) ($charseq$)))
(mode book-titlepage-verso-mode
  (element (para productname) ($charseq$)))
;; Add more here if needed...


<!-- HTML output customization ..................................... -->

<![ %output-html; [

(define %section-autolabel%     #t)
(define %label-preface-sections% #f)
(define %generate-legalnotice-link% #t)
(define %html-ext%              ".html")
(define %root-filename%         "index")
(define %link-mailto-url%       (string-append "mailto:" pgsql-docs-list))
(define %use-id-as-filename%    #t)
(define %stylesheet%            "stylesheet.css")
(define %graphic-default-extension% "gif")
(define %gentext-nav-use-ff%    #t)
(define %body-attr%             '())

;; Returns the depth of auto TOC that should be made at the nd-level
(define (toc-depth nd)
  (cond ((string=? (gi nd) (normalize "book")) 2)
	((string=? (gi nd) (normalize "set")) 2)
	((string=? (gi nd) (normalize "part")) 2)
	((string=? (gi nd) (normalize "chapter")) 2)
	(else 1)))

;; Put a horizontal line in the set TOC (just like the book TOC looks)
(define (set-titlepage-separator side)
  (if (equal? side 'recto)
      (make empty-element gi: "HR")
      (empty-sosofo)))

;; Put date of creation into HTML header
(define %html-header-tags% 
  (list (list "META" '("NAME" "creation") (list "CONTENT" (time->string (time) #t)))))

;; Create an index
(define html-index #t)


;; Block elements are allowed in PARA in DocBook, but not in P in
;; HTML.  With %fix-para-wrappers% turned on, the stylesheets attempt
;; to avoid putting block elements in HTML P tags by outputting
;; additional end/begin P pairs around them.
(define %fix-para-wrappers% #t)

;; ...but we need to do some extra work to make the above apply to PRE
;; as well.  (mostly pasted from dbverb.dsl)
(define ($verbatim-display$ indent line-numbers?)
  (let ((content (make element gi: "PRE"
                       attributes: (list
                                    (list "CLASS" (gi)))
                       (if (or indent line-numbers?)
                           ($verbatim-line-by-line$ indent line-numbers?)
                           (process-children)))))
    (if %shade-verbatim%
        (make element gi: "TABLE"
              attributes: ($shade-verbatim-attr$)
              (make element gi: "TR"
                    (make element gi: "TD"
                          content)))
	(make sequence
	  (para-check)
	  content
	  (para-check 'restart)))))

;; ...and for notes.
(element note
  (make sequence
    (para-check)
    ($admonition$)
    (para-check 'restart)))

;;; XXX The above is very ugly.  It might be better to run 'tidy' on
;;; the resulting *.html files.


;; Format multiple terms in varlistentry vertically, instead
;; of comma-separated.
(element (varlistentry term)
  (make sequence
    (process-children-trim)
    (if (not (last-sibling?))
        (make empty-element gi: "BR")
        (empty-sosofo))))

]]> <!-- %output-html -->


<!-- Print output customization .................................... -->

<![ %output-print; [

(define %section-autolabel%     #t)
(define %default-quadding%      'justify)

;; Don't know how well hyphenation works with other backends.  Might
;; turn this on if desired.
(define %hyphenation%
  (if tex-backend #t #f))

;; Put footnotes at the bottom of the page (rather than end of
;; section), and put the URLs of links into footnotes.
;;
;; bop-footnotes only works with TeX, otherwise it's ignored.  But
;; when both of these are #t and TeX is used, you need at least
;; stylesheets 1.73 because otherwise you don't get any footnotes at
;; all for the links.
(define bop-footnotes           #t)
(define %footnote-ulinks%       #t)

(define %refentry-new-page%     #t)
(define %refentry-keep%         #f)

;; Indentation of verbatim environments.  (This should really be done
;; with start-indent in DSSSL.)
(define %indent-programlisting-lines% "    ")
(define %indent-screen-lines% "    ")
(define %indent-synopsis-lines% "    ")


;; Default graphic format: Jadetex wants eps, pdfjadetex wants pdf.
;; (Note that pdfjadetex will not accept eps, that's why we need to
;; create a different .tex file for each.)  What works with RTF?

(define texpdf-output #f) ;; override from command line

(define %graphic-default-extension%
  (cond (tex-backend (if texpdf-output "pdf" "eps"))
	(rtf-backend "gif")
	(else "XXX")))

;; Need to add pdf here so that the above works.  Default setup
;; doesn't know about PDF.
(define preferred-mediaobject-extensions
  (list "eps" "ps" "jpg" "jpeg" "pdf" "png"))


;; Don't show links when citing a bibliography entry.  This fouls up
;; the footnumber counting.  To get the link, one can still look into
;; the bibliography itself.
(mode xref-title-mode
  (element ulink
    (process-children)))


;; Format legalnotice justified and with space between paragraphs.
(mode book-titlepage-verso-mode
  (element (legalnotice para)
    (make paragraph
      use: book-titlepage-verso-style	;; alter this if ever it needs to appear elsewhere
      quadding: %default-quadding%
      line-spacing: (* 0.8 (inherited-line-spacing))
      font-size: (* 0.8 (inherited-font-size))
      space-before: (* 0.8 %para-sep%)
      space-after: (* 0.8 %para-sep%)
      first-line-start-indent: (if (is-first-para)
				   (* 0.8 %para-indent-firstpara%)
				   (* 0.8 %para-indent%))
      (process-children))))


;; Fix spacing problems in variablelists

(element (varlistentry term)
  (make paragraph
    space-before: (if (first-sibling?)
		      %para-sep%
		      0pt)
    keep-with-next?: #t
    (process-children)))

(define %varlistentry-indent% 2em)

(element (varlistentry listitem)
  (make sequence
    start-indent: (+ (inherited-start-indent) %varlistentry-indent%)
    (process-children)))


;; Whitespace fixes for itemizedlists and orderedlists

(define (process-listitem-content)
  (if (absolute-first-sibling?)
      (make sequence
        (process-children-trim))
      (next-match)))


;; Default stylesheets format simplelists as tables.  This spells
;; trouble for Jade.  So we just format them as plain lines.

(define %simplelist-indent% 1em)

(define (my-simplelist-vert members)
  (make display-group
    space-before: %para-sep%
    space-after: %para-sep%
    start-indent: (+ %simplelist-indent% (inherited-start-indent))
    (process-children)))

(element simplelist
  (let ((type (attribute-string (normalize "type")))
        (cols (if (attribute-string (normalize "columns"))
                  (if (> (string->number (attribute-string (normalize "columns"))) 0)
                      (string->number (attribute-string (normalize "columns")))
                      1)
                  1))
        (members (select-elements (children (current-node)) (normalize "member"))))
    (cond
       ((equal? type (normalize "inline"))
	(if (equal? (gi (parent (current-node)))
		    (normalize "para"))
	    (process-children)
	    (make paragraph
	      space-before: %para-sep%
	      space-after: %para-sep%
	      start-indent: (inherited-start-indent))))
       ((equal? type (normalize "vert"))
        (my-simplelist-vert members))
       ((equal? type (normalize "horiz"))
        (simplelist-table 'row    cols members)))))
 
(element member
  (let ((type (inherited-attribute-string (normalize "type"))))
    (cond
     ((equal? type (normalize "inline"))
      (make sequence
	(process-children)
	(if (not (last-sibling?))
	    (literal ", ")
	    (literal ""))))
      ((equal? type (normalize "vert"))
       (make paragraph
	 space-before: 0pt
	 space-after: 0pt))
      ((equal? type (normalize "horiz"))
       (make paragraph
	 quadding: 'start
	 (process-children))))))


;; Jadetex doesn't handle links to the content of tables, so
;; indexterms that point to table entries will go nowhere.  We fix
;; this by pointing the index entry to the table itself instead, which
;; should be equally useful in practice.

(define (find-parent-table nd)
  (let ((table (ancestor-member nd ($table-element-list$))))
    (if (node-list-empty? table)
	nd
	table)))	 

;; (The function below overrides the one in print/dbindex.dsl.)

(define (indexentry-link nd)
  (let* ((id        (attribute-string (normalize "role") nd))
         (prelim-target (find-indexterm id))
         (target    (find-parent-table prelim-target))
         (preferred (not (node-list-empty?
                          (select-elements (children (current-node))
                                           (normalize "emphasis")))))
         (sosofo    (if (node-list-empty? target)
                        (literal "?")
                        (make link
                          destination: (node-list-address target)
                          (with-mode toc-page-number-mode
                            (process-node-list target))))))
    (if preferred
        (make sequence
          font-weight: 'bold
          sosofo)
        sosofo)))


;; By default, the part and reference title pages get wrong page
;; numbers: The first title page gets roman numerals carried over from
;; preface/toc -- we want arabic numerals.  We also need to make sure
;; that page-number-restart is set of #f explicitly, because otherwise
;; it will carry over from the previous component, which is not good.
;;
;; (This looks worse than it is.  It's copied from print/dbttlpg.dsl
;; and common/dbcommon.dsl and modified in minor detail.)

(define (first-part?)
  (let* ((book (ancestor (normalize "book")))
	 (nd   (ancestor-member (current-node)
				(append
				 (component-element-list)
				 (division-element-list))))
	 (bookch (children book)))
    (let loop ((nl bookch))
      (if (node-list-empty? nl)
	  #f
	  (if (equal? (gi (node-list-first nl)) (normalize "part"))
	      (if (node-list=? (node-list-first nl) nd)
		  #t
		  #f)
	      (loop (node-list-rest nl)))))))

(define (first-reference?)
  (let* ((book (ancestor (normalize "book")))
	 (nd   (ancestor-member (current-node)
				(append
				 (component-element-list)
				 (division-element-list))))
	 (bookch (children book)))
    (let loop ((nl bookch))
      (if (node-list-empty? nl)
	  #f
	  (if (equal? (gi (node-list-first nl)) (normalize "reference"))
	      (if (node-list=? (node-list-first nl) nd)
		  #t
		  #f)
	      (loop (node-list-rest nl)))))))


(define (part-titlepage elements #!optional (side 'recto))
  (let ((nodelist (titlepage-nodelist 
		   (if (equal? side 'recto)
		       (reference-titlepage-recto-elements)
		       (reference-titlepage-verso-elements))
		   elements))
        ;; partintro is a special case...
	(partintro (node-list-first
		    (node-list-filter-by-gi elements (list (normalize "partintro"))))))
    (if (part-titlepage-content? elements side)
	(make simple-page-sequence
	  page-n-columns: %titlepage-n-columns%
	  ;; Make sure that page number format is correct.
	  page-number-format: ($page-number-format$)
	  ;; Make sure that the page number is set to 1 if this is the
	  ;; first part in the book
	  page-number-restart?: (first-part?)
	  input-whitespace-treatment: 'collapse
	  use: default-text-style
	  
	  ;; This hack is required for the RTF backend. If an external-graphic
	  ;; is the first thing on the page, RTF doesn't seem to do the right
	  ;; thing (the graphic winds up on the baseline of the first line
	  ;; of the page, left justified).  This "one point rule" fixes
	  ;; that problem.
	  (make paragraph
	    line-spacing: 1pt
	    (literal ""))
      
	  (let loop ((nl nodelist) (lastnode (empty-node-list)))
	    (if (node-list-empty? nl)
		(empty-sosofo)
		(make sequence
		  (if (or (node-list-empty? lastnode)
			  (not (equal? (gi (node-list-first nl))
				       (gi lastnode))))
		      (part-titlepage-before (node-list-first nl) side)
		      (empty-sosofo))
		  (cond
		   ((equal? (gi (node-list-first nl)) (normalize "subtitle"))
		    (part-titlepage-subtitle (node-list-first nl) side))
		   ((equal? (gi (node-list-first nl)) (normalize "title"))
		    (part-titlepage-title (node-list-first nl) side))
		   (else
		    (part-titlepage-default (node-list-first nl) side)))
		  (loop (node-list-rest nl) (node-list-first nl)))))

	  (if (and %generate-part-toc%
		   %generate-part-toc-on-titlepage%
		   (equal? side 'recto))
	      (make display-group
		(build-toc (current-node)
			   (toc-depth (current-node))))
	      (empty-sosofo))

	  ;; PartIntro is a special case
	  (if (and (equal? side 'recto)
		   (not (node-list-empty? partintro))
		   %generate-partintro-on-titlepage%)
	      ($process-partintro$ partintro #f)
	      (empty-sosofo)))

	(empty-sosofo))))


(define (reference-titlepage elements #!optional (side 'recto))
  (let ((nodelist (titlepage-nodelist 
		   (if (equal? side 'recto)
		       (reference-titlepage-recto-elements)
		       (reference-titlepage-verso-elements))
		   elements))
        ;; partintro is a special case...
	(partintro (node-list-first
		    (node-list-filter-by-gi elements (list (normalize "partintro"))))))
    (if (reference-titlepage-content? elements side)
	(make simple-page-sequence
	  page-n-columns: %titlepage-n-columns%
	  ;; Make sure that page number format is correct.
	  page-number-format: ($page-number-format$)
	  ;; Make sure that the page number is set to 1 if this is the
	  ;; first part in the book
	  page-number-restart?: (first-reference?)
	  input-whitespace-treatment: 'collapse
	  use: default-text-style
	  
	  ;; This hack is required for the RTF backend. If an external-graphic
	  ;; is the first thing on the page, RTF doesn't seem to do the right
	  ;; thing (the graphic winds up on the baseline of the first line
	  ;; of the page, left justified).  This "one point rule" fixes
	  ;; that problem.
	  (make paragraph
	    line-spacing: 1pt
	    (literal ""))
      
	  (let loop ((nl nodelist) (lastnode (empty-node-list)))
	    (if (node-list-empty? nl)
		(empty-sosofo)
		(make sequence
		  (if (or (node-list-empty? lastnode)
			  (not (equal? (gi (node-list-first nl))
				       (gi lastnode))))
		      (reference-titlepage-before (node-list-first nl) side)
		      (empty-sosofo))
		  (cond
		   ((equal? (gi (node-list-first nl)) (normalize "author"))
		    (reference-titlepage-author (node-list-first nl) side))
		   ((equal? (gi (node-list-first nl)) (normalize "authorgroup"))
		    (reference-titlepage-authorgroup (node-list-first nl) side))
		   ((equal? (gi (node-list-first nl)) (normalize "corpauthor"))
		    (reference-titlepage-corpauthor (node-list-first nl) side))
		   ((equal? (gi (node-list-first nl)) (normalize "editor"))
		    (reference-titlepage-editor (node-list-first nl) side))
		   ((equal? (gi (node-list-first nl)) (normalize "subtitle"))
		    (reference-titlepage-subtitle (node-list-first nl) side))
		   ((equal? (gi (node-list-first nl)) (normalize "title"))
		    (reference-titlepage-title (node-list-first nl) side))
		   (else
		    (reference-titlepage-default (node-list-first nl) side)))
		  (loop (node-list-rest nl) (node-list-first nl)))))

	  (if (and %generate-reference-toc%
		   %generate-reference-toc-on-titlepage%
		   (equal? side 'recto))
	      (make display-group
		(build-toc (current-node)
			   (toc-depth (current-node))))
	      (empty-sosofo))

	  ;; PartIntro is a special case
	  (if (and (equal? side 'recto)
		   (not (node-list-empty? partintro))
		   %generate-partintro-on-titlepage%)
	      ($process-partintro$ partintro #f)
	      (empty-sosofo)))

	(empty-sosofo))))

]]> <!-- %output-print -->


<!-- Plain text output customization ............................... -->

<!--
This is used for making the INSTALL file and others.  We customize the
HTML stylesheets to be suitable for dumping plain text (via Netscape,
Lynx, or similar).
-->

<![ %output-text; [

(define %section-autolabel% #f)
(define %chapter-autolabel% #f)
(define $generate-chapter-toc$ (lambda () #f))

;; For text output, produce "ASCII markup" for emphasis and such.

(define ($asterix-seq$ #!optional (sosofo (process-children)))
  (make sequence
    (literal "*")
    sosofo
    (literal "*")))
 
(define ($dquote-seq$ #!optional (sosofo (process-children)))
  (make sequence
    (literal (gentext-start-quote))
    sosofo
    (literal (gentext-end-quote))))
 
(element (para command) ($dquote-seq$))
(element (para emphasis) ($asterix-seq$))
(element (para filename) ($dquote-seq$))
(element (para option) ($dquote-seq$))
(element (para replaceable) ($dquote-seq$))
(element (para userinput) ($dquote-seq$))

]]> <!-- %output-text -->

  </style-specification-body>
 </style-specification>

 <external-specification id="docbook" document="dbstyle">
</style-sheet>
