#	$NetBSD: Makefile,v 1.8 2013/09/19 01:49:07 rmind Exp $
#
# Public Domain.
#

.PATH:	${.CURDIR}/../../../../net/npf

LIB=	rumpnet_npf

SRCS=	npf.c npf_alg.c npf_conf.c npf_ctl.c npf_handler.c npf_bpf.c 
SRCS+=	npf_inet.c npf_mbuf.c npf_nat.c npf_ruleset.c npf_rproc.c 
SRCS+=	npf_sendpkt.c npf_session.c npf_state.c npf_state_tcp.c
SRCS+=	npf_tableset.c npf_tableset_ptree.c npf_worker.c
SRCS+=	if_npflog.c

SRCS+=	npf_alg_icmp.c

SRCS+=	npf_ext_log.c npf_ext_normalize.c npf_ext_rndblock.c

SRCS+=	component.c

CPPFLAGS+=	-D_NPF_TESTING
CPPFLAGS+=	-I${.CURDIR}/../../../librump/rumpvfs

.include <bsd.lib.mk>
.include <bsd.klinks.mk>
