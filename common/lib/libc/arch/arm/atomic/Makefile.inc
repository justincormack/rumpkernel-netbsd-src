#	$NetBSD: Makefile.inc,v 1.13 2013/08/19 03:55:12 matt Exp $

ARMV6= ${CPUFLAGS:M-march=armv6*} ${CPUFLAGS:M-mcpu=arm11*}
ARMV6+= ${CFLAGS:M-march=armv6*:} ${CFLAGS:M-mcpu=arm11*}
ARMV6+= ${CPPFLAGS:M-march=armv6*:} ${CPPFLAGS:M-mcpu=arm11*}
ARMV7= ${CPUFLAGS:M-march=armv7*} ${CPUFLAGS:M-mcpu=cortex*}
ARMV7+= ${CFLAGS:M-march=armv7*:} ${CFLAGS:M-mcpu=cortex*}
ARMV7+= ${CPPFLAGS:M-march=armv7*:} ${CPPFLAGS:M-mcpu=cortex*}
.if empty(CPPFLAGS:M-D_STANDALONE) \
    && empty(CFLAGS:M-march=*) && empty(CFLAGS:M-mcpu=*) \
    && empty(CPPFLAGS:M-march=*) && empty(CPPFLAGS:M-mcpu=*) \
    && empty(CPUFLAGS:M-march=*) && empty(CPUFLAGS:M-mcpu=*)
ARMV6+= ${MACHINE_ARCH:Mearmv6*}
ARMV7+= ${MACHINE_ARCH:Mearmv7*}
.endif

.if defined(LIB) && (${LIB} == "kern" || ${LIB} == "c" || ${LIB} == "pthread" \
	|| ${LIB} == "rump")

.if empty(ARMV6) && empty(ARMV7)
SRCS.atomic+=	atomic_add_32_cas.c atomic_add_32_nv_cas.c \
		atomic_and_32_cas.c atomic_and_32_nv_cas.c \
		atomic_dec_32_cas.c atomic_dec_32_nv_cas.c \
		atomic_inc_32_cas.c atomic_inc_32_nv_cas.c \
		atomic_or_32_cas.c atomic_or_32_nv_cas.c \
		atomic_swap_32_cas.c membar_ops_nop.c
.else
SRCS.atomic+=	atomic_add_32.S atomic_and_32.S atomic_cas_32.S
SRCS.atomic+=	atomic_dec_32.S atomic_inc_32.S atomic_or_32.S
SRCS.atomic+=	atomic_swap.S membar_ops.S
SRCS.atomic+=	atomic_add_64.S atomic_and_64.S atomic_cas_64.S
SRCS.atomic+=	atomic_dec_64.S atomic_inc_64.S atomic_or_64.S
SRCS.atomic+=	atomic_swap_64.S
.endif

.endif

.if defined(LIB) && (${LIB} == "c" || ${LIB} == "pthread" || ${LIB} == "rump")

SRCS.atomic+=	atomic_simplelock.c
.if empty(ARMV7)
CPUFLAGS.atomic_simplelock.c+=	-marm
.endif

.endif

.if defined(LIB) && (${LIB} == "c" || ${LIB} == "pthread")

.if empty(ARMV6) && empty(ARMV7)
SRCS.atomic+=	atomic_init_testset.c
SRCS.atomic+=	atomic_cas_up.S
CPPFLAGS+= -D__HAVE_ASM_ATOMIC_CAS_UP
.else
SRCS.atomic+=	atomic_init_cas.c
.endif

.endif

.if !empty(ARMV6)
.for f in ${SRCS.atomic:M*.S}
CPUFLAGS.$f+=	-marm
.endfor
.endif

SRCS+=	${SRCS.atomic}
