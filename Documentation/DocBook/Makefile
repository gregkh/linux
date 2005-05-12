###
# This makefile is used to generate the kernel documentation,
# primarily based on in-line comments in various source files.
# See Documentation/kernel-doc-nano-HOWTO.txt for instruction in how
# to ducument the SRC - and how to read it.
# To add a new book the only step required is to add the book to the
# list of DOCBOOKS.

DOCBOOKS := wanbook.sgml z8530book.sgml mcabook.sgml videobook.sgml \
	    kernel-hacking.sgml kernel-locking.sgml via-audio.sgml \
	    deviceiobook.sgml procfs-guide.sgml tulip-user.sgml \
	    writing_usb_driver.sgml scsidrivers.sgml sis900.sgml \
	    kernel-api.sgml journal-api.sgml lsm.sgml usb.sgml \
	    gadget.sgml libata.sgml mtdnand.sgml librs.sgml

###
# The build process is as follows (targets):
#              (sgmldocs)
# file.tmpl --> file.sgml +--> file.ps  (psdocs)
#                         +--> file.pdf  (pdfdocs)
#                         +--> DIR=file  (htmldocs)
#                         +--> man/      (mandocs)

###
# The targets that may be used.
.PHONY:	sgmldocs psdocs pdfdocs htmldocs mandocs installmandocs

BOOKS := $(addprefix $(obj)/,$(DOCBOOKS))
sgmldocs: $(BOOKS)

PS := $(patsubst %.sgml, %.ps, $(BOOKS))
psdocs: $(PS)

PDF := $(patsubst %.sgml, %.pdf, $(BOOKS))
pdfdocs: $(PDF)

HTML := $(patsubst %.sgml, %.html, $(BOOKS))
htmldocs: $(HTML)

MAN := $(patsubst %.sgml, %.9, $(BOOKS))
mandocs: $(MAN)

installmandocs: mandocs
	$(MAKEMAN) install Documentation/DocBook/man

###
#External programs used
KERNELDOC = scripts/kernel-doc
DOCPROC   = scripts/basic/docproc
SPLITMAN  = $(PERL) $(srctree)/scripts/split-man
MAKEMAN   = $(PERL) $(srctree)/scripts/makeman

###
# DOCPROC is used for two purposes:
# 1) To generate a dependency list for a .tmpl file
# 2) To preprocess a .tmpl file and call kernel-doc with
#     appropriate parameters.
# The following rules are used to generate the .sgml documentation
# required to generate the final targets. (ps, pdf, html).
quiet_cmd_docproc = DOCPROC $@
      cmd_docproc = SRCTREE=$(srctree)/ $(DOCPROC) doc $< >$@
define rule_docproc
	set -e;								\
        $(if $($(quiet)cmd_$(1)),echo '  $($(quiet)cmd_$(1))';) 	\
        $(cmd_$(1)); 							\
        ( 								\
          echo 'cmd_$@ := $(cmd_$(1))'; 				\
          echo $@: `SRCTREE=$(srctree) $(DOCPROC) depend $<`; 		\
        ) > $(dir $@).$(notdir $@).cmd
endef

%.sgml: %.tmpl FORCE
	$(call if_changed_rule,docproc)

###
#Read in all saved dependency files 
cmd_files := $(wildcard $(foreach f,$(BOOKS),$(dir $(f)).$(notdir $(f)).cmd))

ifneq ($(cmd_files),)
  include $(cmd_files)
endif

###
# Changes in kernel-doc force a rebuild of all documentation
$(BOOKS): $(KERNELDOC)

###
# procfs guide uses a .c file as example code.
# This requires an explicit dependency
C-procfs-example = procfs_example.sgml
C-procfs-example2 = $(addprefix $(obj)/,$(C-procfs-example))
$(obj)/procfs-guide.sgml: $(C-procfs-example2)

###
# Rules to generate postscript, PDF and HTML
# db2html creates a directory. Generate a html file used for timestamp

quiet_cmd_db2ps = DB2PS   $@
      cmd_db2ps = db2ps -o $(dir $@) $<
%.ps : %.sgml
	@(which db2ps > /dev/null 2>&1) || \
	 (echo "*** You need to install DocBook stylesheets ***"; \
	  exit 1)
	$(call cmd,db2ps)

quiet_cmd_db2pdf = DB2PDF  $@
      cmd_db2pdf = db2pdf -o $(dir $@) $<
%.pdf : %.sgml
	@(which db2pdf > /dev/null 2>&1) || \
	 (echo "*** You need to install DocBook stylesheets ***"; \
	  exit 1)
	$(call cmd,db2pdf)

quiet_cmd_db2html = DB2HTML $@
      cmd_db2html = db2html -o $(patsubst %.html,%,$@) $< &&		      \
		echo '<a HREF="$(patsubst %.html,%,$(notdir $@))/book1.html"> \
         Goto $(patsubst %.html,%,$(notdir $@))</a><p>' > $@

%.html:	%.sgml
	@(which db2html > /dev/null 2>&1) || \
	 (echo "*** You need to install DocBook stylesheets ***"; \
	  exit 1)
	@rm -rf $@ $(patsubst %.html,%,$@)
	$(call cmd,db2html)
	@if [ ! -z "$(PNG-$(basename $(notdir $@)))" ]; then \
            cp $(PNG-$(basename $(notdir $@))) $(patsubst %.html,%,$@); fi

###
# Rule to generate man files - output is placed in the man subdirectory

%.9:	%.sgml
ifneq ($(KBUILD_SRC),)
	$(Q)mkdir -p $(objtree)/Documentation/DocBook/man
endif
	$(SPLITMAN) $< $(objtree)/Documentation/DocBook/man "$(VERSION).$(PATCHLEVEL).$(SUBLEVEL)"
	$(MAKEMAN) convert $(objtree)/Documentation/DocBook/man $<

###
# Rules to generate postscripts and PNG imgages from .fig format files
quiet_cmd_fig2eps = FIG2EPS $@
      cmd_fig2eps = fig2dev -Leps $< $@

%.eps: %.fig
	@(which fig2dev > /dev/null 2>&1) || \
	 (echo "*** You need to install transfig ***"; \
	  exit 1)
	$(call cmd,fig2eps)

quiet_cmd_fig2png = FIG2PNG $@
      cmd_fig2png = fig2dev -Lpng $< $@

%.png: %.fig
	@(which fig2dev > /dev/null 2>&1) || \
	 (echo "*** You need to install transfig ***"; \
	  exit 1)
	$(call cmd,fig2png)

###
# Rule to convert a .c file to inline SGML documentation
%.sgml: %.c
	@echo '  GEN     $@'
	@(                            \
	   echo "<programlisting>";   \
	   expand --tabs=8 < $< |     \
	   sed -e "s/&/\\&amp;/g"     \
	       -e "s/</\\&lt;/g"      \
	       -e "s/>/\\&gt;/g";     \
	   echo "</programlisting>")  > $@

###
# Help targets as used by the top-level makefile
dochelp:
	@echo  '  Linux kernel internal documentation in different formats:'
	@echo  '  sgmldocs (SGML), psdocs (Postscript), pdfdocs (PDF)'
	@echo  '  htmldocs (HTML), mandocs (man pages, use installmandocs to install)'

###
# Temporary files left by various tools
clean-files := $(DOCBOOKS) \
	$(patsubst %.sgml, %.dvi,  $(DOCBOOKS)) \
	$(patsubst %.sgml, %.aux,  $(DOCBOOKS)) \
	$(patsubst %.sgml, %.tex,  $(DOCBOOKS)) \
	$(patsubst %.sgml, %.log,  $(DOCBOOKS)) \
	$(patsubst %.sgml, %.out,  $(DOCBOOKS)) \
	$(patsubst %.sgml, %.ps,   $(DOCBOOKS)) \
	$(patsubst %.sgml, %.pdf,  $(DOCBOOKS)) \
	$(patsubst %.sgml, %.html, $(DOCBOOKS)) \
	$(patsubst %.sgml, %.9,    $(DOCBOOKS)) \
	$(C-procfs-example)

clean-dirs := $(patsubst %.sgml,%,$(DOCBOOKS))

#man put files in man subdir - traverse down
subdir- := man/
