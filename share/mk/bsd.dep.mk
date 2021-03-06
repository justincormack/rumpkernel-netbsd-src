#	$NetBSD: bsd.dep.mk,v 1.77 2013/03/05 21:59:01 christos Exp $

##### Basic targets
realdepend:	beforedepend .depend afterdepend
.ORDER:		beforedepend .depend afterdepend

beforedepend .depend afterdepend: # ensure existence

##### Default values
MKDEP?=			mkdep
MKDEP_SUFFIXES?=	.o

##### Build rules
# some of the rules involve .h sources, so remove them from mkdep line

.if defined(SRCS) && !empty(SRCS)
__acpp_flags=	${_ASM_TRADITIONAL_CPP}

.if defined(NODPSRCS)
.for f in ${SRCS} ${DPSRCS}
.if "${NODPSRCS:M${f}}" == ""
__DPSRCS.all+=	${f:C/\.(c|m|s|S|C|cc|cpp|cxx)$/.d/}
.endif
.endfor
beforedepend: ${DPSRCS}
.else
__DPSRCS.all+=	${SRCS:C/\.(c|m|s|S|C|cc|cpp|cxx)$/.d/} \
		${DPSRCS:C/\.(c|m|s|S|C|cc|cpp|cxx)$/.d/}
.endif
__DPSRCS.d=	${__DPSRCS.all:O:u:M*.d}
__DPSRCS.notd=	${__DPSRCS.all:O:u:N*.d}

.NOPATH: .depend ${__DPSRCS.d}

.if !empty(__DPSRCS.d)							# {
${__DPSRCS.d}: ${__DPSRCS.notd} ${DPSRCS}
.endif									# }

MKDEPSUFFLAGS=-s ${MKDEP_SUFFIXES:Q}

.if defined(MKDEPINCLUDES) && ${MKDEPINCLUDES} != "no"
.STALE:
	@echo Rebuilding dependency file: ${.ALLSRC}
	@rm -f ${.ALLSRC}
	@(cd ${.CURDIR} && ${MAKE} depend)
_MKDEP_MERGEFLAGS=-i
_MKDEP_FILEFLAGS=${MKDEPSUFFLAGS}
.else
_MKDEP_MERGEFLAGS=${MKDEPSUFFLAGS}
_MKDEP_FILEFLAGS=
.endif

.depend: ${__DPSRCS.d}
	${_MKTARGET_CREATE}
	rm -f .depend
	${MKDEP} ${_MKDEP_MERGEFLAGS} -d -f ${.TARGET} ${__DPSRCS.d}

.SUFFIXES: .d .s .S .c .C .cc .cpp .cxx .m

.c.d:
	${_MKTARGET_CREATE}
	${MKDEP} -f ${.TARGET} ${_MKDEP_FILEFLAGS} -- ${MKDEPFLAGS} \
	    ${CFLAGS:C/-([IDU])[  ]*/-\1/Wg:M-[IDU]*} \
	    ${CPPFLAGS} ${CPPFLAGS.${.IMPSRC:T}} ${.IMPSRC}

.m.d:
	${_MKTARGET_CREATE}
	${MKDEP} -f ${.TARGET} ${_MKDEP_FILEFLAGS} -- ${MKDEPFLAGS} \
	    ${OBJCFLAGS:C/-([IDU])[  ]*/-\1/Wg:M-[IDU]*} \
	    ${CPPFLAGS} ${CPPFLAGS.${.IMPSRC:T}} ${.IMPSRC}

.s.d .S.d:
	${_MKTARGET_CREATE}
	${MKDEP} -f ${.TARGET} ${_MKDEP_FILEFLAGS} -- ${MKDEPFLAGS} \
	    ${AFLAGS:C/-([IDU])[  ]*/-\1/Wg:M-[IDU]*} \
	    ${CPPFLAGS} ${CPPFLAGS.${.IMPSRC:T}} ${__acpp_flags} ${.IMPSRC}

.C.d .cc.d .cpp.d .cxx.d:
	${_MKTARGET_CREATE}
	${MKDEP} -f ${.TARGET} ${_MKDEP_FILEFLAGS} -- ${MKDEPFLAGS} \
	    ${CXXFLAGS:C/-([IDU])[  ]*/-\1/Wg:M-[IDU]*} \
	    ${CPPFLAGS} ${CPPFLAGS.${.IMPSRC:T}} ${.IMPSRC}

.endif # defined(SRCS) && !empty(SRCS)					# }

##### Clean rules
.if defined(SRCS) && !empty(SRCS)
CLEANDIRFILES+= .depend ${__DPSRCS.d} ${.CURDIR}/tags ${CLEANDEPEND}
.endif

##### Custom rules
.if !target(tags)
tags: ${SRCS}
.if defined(SRCS) && !empty(SRCS)
	-cd "${.CURDIR}"; ctags -f /dev/stdout ${.ALLSRC:N*.h} | \
	    ${TOOL_SED} "s;\${.CURDIR}/;;" > tags
.endif
.endif

##### Pull in related .mk logic
.include <bsd.clean.mk>
