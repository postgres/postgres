<!-- $Header: /cvsroot/pgsql/doc/src/sgml/stylesheet.dsl,v 1.2 2001/02/13 22:35:15 petere Exp $ -->
<!DOCTYPE style-sheet PUBLIC "-//James Clark//DTD DSSSL Style Sheet//EN" [

<!-- must turn on one of these with -i on the jade command line -->
<!ENTITY % output-html          "IGNORE">
<!ENTITY % output-print         "IGNORE">

<![ %output-html; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN" CDATA DSSSL>
]]>

<![ %output-print; [
<!ENTITY dbstyle PUBLIC "-//Norman Walsh//DOCUMENT DocBook Print Stylesheet//EN" CDATA DSSSL>
]]>

]>

<style-sheet>
 <style-specification use="docbook">
  <style-specification-body> 

(define pgsql-docs-list "pgsql-docs@postgresql.org")

(define %refentry-xref-manvolnum% #f)

<![ %output-html; [
;; customize the html stylesheet

(define %generate-legalnotice-link% #t)
(define %html-ext%              ".html")
(define %link-mailto-url%       (string-append "mailto:" pgsql-docs-list))
(define %use-id-as-filename%    #t)

;; Returns the depth of auto TOC that should be made at the nd-level
(define (toc-depth nd)
  (cond ((string=? (gi nd) (normalize "book")) 3)
	((string=? (gi nd) (normalize "set")) 2)
	(else 1)))

;; Put date of creation into header
(define %html-header-tags% 
  (list (list "META" '("NAME" "creation") (list "CONTENT" (time->string (time) #t)))))

]]> <!-- %output-html -->

<![ %output-print; [
;; customize the print stylesheet

(define %default-quadding%      'justify)
(define bop-footnotes           #t)
(if tex-backend
    (define %hyphenation%       #t))

]]> <!-- %output-print -->

  </style-specification-body>
 </style-specification>

 <external-specification id="docbook" document="dbstyle">
</style-sheet>
