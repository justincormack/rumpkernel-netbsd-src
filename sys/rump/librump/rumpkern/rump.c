/*	$NetBSD: rump.c,v 1.275 2013/09/09 19:18:08 pooka Exp $	*/

/*
 * Copyright (c) 2007-2011 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: rump.c,v 1.275 2013/09/09 19:18:08 pooka Exp $");

#include <sys/systm.h>
#define ELFSIZE ARCH_ELFSIZE

#include <sys/param.h>
#include <sys/atomic.h>
#include <sys/buf.h>
#include <sys/callout.h>
#include <sys/conf.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/evcnt.h>
#include <sys/event.h>
#include <sys/exec_elf.h>
#include <sys/filedesc.h>
#include <sys/iostat.h>
#include <sys/kauth.h>
#include <sys/kcpuset.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kprintf.h>
#include <sys/kthread.h>
#include <sys/ksyms.h>
#include <sys/msgbuf.h>
#include <sys/module.h>
#include <sys/namei.h>
#include <sys/once.h>
#include <sys/percpu.h>
#include <sys/pipe.h>
#include <sys/pool.h>
#include <sys/pserialize.h>
#include <sys/queue.h>
#include <sys/reboot.h>
#include <sys/resourcevar.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/syscallvar.h>
#include <sys/timetc.h>
#include <sys/tty.h>
#include <sys/uidinfo.h>
#include <sys/vmem.h>
#include <sys/xcall.h>
#include <sys/simplelock.h>
#include <sys/cprng.h>

#include <rump/rumpuser.h>

#include <secmodel/suser/suser.h>

#include <prop/proplib.h>

#include <uvm/uvm_extern.h>
#include <uvm/uvm_readahead.h>

#include "rump_private.h"
#include "rump_net_private.h"
#include "rump_vfs_private.h"
#include "rump_dev_private.h"

char machine[] = MACHINE;

struct proc *initproc;

struct device rump_rootdev = {
	.dv_class = DV_VIRTUAL
};

#ifdef RUMP_WITHOUT_THREADS
int rump_threads = 0;
#else
int rump_threads = 1;
#endif

static int rump_hyp_syscall(int, void *, long *);
static int rump_hyp_rfork(void *, int, const char *);
static void rump_hyp_lwpexit(void);
static void rump_hyp_execnotify(const char *);

static void rump_component_addlocal(void);
static void rump_component_load(const struct rump_component *);
static struct lwp *bootlwp;

static char rump_msgbuf[16*1024]; /* 16k should be enough for std rump needs */

#ifdef LOCKDEBUG
const int rump_lockdebug = 1;
#else
const int rump_lockdebug = 0;
#endif
bool rump_ttycomponent = false;

static void
rump_aiodone_worker(struct work *wk, void *dummy)
{
	struct buf *bp = (struct buf *)wk;

	KASSERT(&bp->b_work == wk);
	bp->b_iodone(bp);
}

static int rump_inited;

void (*rump_vfs_drainbufs)(int);
void (*rump_vfs_fini)(void);
int  (*rump_vfs_makeonedevnode)(dev_t, const char *,
				devmajor_t, devminor_t) = (void *)nullop;
int  (*rump_vfs_makedevnodes)(dev_t, const char *, char,
			      devmajor_t, devminor_t, int) = (void *)nullop;

int rump__unavailable(void);
int rump__unavailable() {return EOPNOTSUPP;}

__weak_alias(biodone,rump__unavailable);
__weak_alias(sopoll,rump__unavailable);

void rump__unavailable_vfs_panic(void);
void rump__unavailable_vfs_panic() {panic("vfs component not available");}
__weak_alias(usermount_common_policy,rump__unavailable_vfs_panic);

/* easier to write vfs-less clients */
__weak_alias(rump_pub_etfs_register,rump__unavailable);
__weak_alias(rump_pub_etfs_register_withsize,rump__unavailable);
__weak_alias(rump_pub_etfs_remove,rump__unavailable);

rump_proc_vfs_init_fn rump_proc_vfs_init;
rump_proc_vfs_release_fn rump_proc_vfs_release;

static void add_linkedin_modules(const struct modinfo *const *, size_t);

/*
 * Create some sysctl nodes.  why only this you ask.  well, init_sysctl
 * is a kitchen sink in need of some gardening.  but i want to use
 * others today.  Furthermore, creating a whole kitchen sink full of
 * sysctl nodes is a waste of cycles for rump kernel bootstrap.
 */
static void
mksysctls(void)
{

	/* kern.hostname */
	sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_PERMANENT, CTLTYPE_NODE, "kern", NULL,
	    NULL, 0, NULL, 0, CTL_KERN, CTL_EOL);
	/* XXX: setting hostnamelen is missing */
	sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_PERMANENT|CTLFLAG_READWRITE, CTLTYPE_STRING, "hostname",
	    SYSCTL_DESCR("System hostname"), NULL, 0,
	    hostname, MAXHOSTNAMELEN, CTL_KERN, KERN_HOSTNAME, CTL_EOL);

	/* hw.pagesize */
	sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "hw", NULL,
	    NULL, 0, NULL, 0,
	    CTL_HW, CTL_EOL);
	sysctl_createv(NULL, 0, NULL, NULL,
	    CTLFLAG_PERMANENT|CTLFLAG_IMMEDIATE,
	    CTLTYPE_INT, "pagesize",
	    SYSCTL_DESCR("Software page size"),
	    NULL, PAGE_SIZE, NULL, 0,
	    CTL_HW, HW_PAGESIZE, CTL_EOL);
}

/* there's no convenient kernel entry point for this, so just craft out own */
static pid_t
spgetpid(void)
{

	return curproc->p_pid;
}

static const struct rumpuser_hyperup hyp = {
	.hyp_schedule		= rump_schedule,
	.hyp_unschedule		= rump_unschedule,
	.hyp_backend_unschedule	= rump_user_unschedule,
	.hyp_backend_schedule	= rump_user_schedule,
	.hyp_lwproc_switch	= rump_lwproc_switch,
	.hyp_lwproc_release	= rump_lwproc_releaselwp,
	.hyp_lwproc_rfork	= rump_hyp_rfork,
	.hyp_lwproc_newlwp	= rump_lwproc_newlwp,
	.hyp_lwproc_curlwp	= rump_lwproc_curlwp,
	.hyp_lwpexit		= rump_hyp_lwpexit,
	.hyp_syscall		= rump_hyp_syscall,
	.hyp_execnotify		= rump_hyp_execnotify,
	.hyp_getpid		= spgetpid,
};

int
rump_daemonize_begin(void)
{

	if (rump_inited)
		return EALREADY;

	return rumpuser_daemonize_begin();
}

int
rump_daemonize_done(int error)
{

	return rumpuser_daemonize_done(error);
}

RUMP_COMPONENT(RUMP_COMPONENT_POSTINIT)
{
	__link_set_decl(rump_components, struct rump_component);

	/*
	 * Trick compiler into generating references so that statically
	 * linked rump kernels are generated with the link set symbols.
	 */
	asm("" :: "r"(__start_link_set_rump_components));
	asm("" :: "r"(__stop_link_set_rump_components));
}

int
rump_init(void)
{
	char buf[256];
	struct timespec ts;
	int64_t sec;
	long nsec;
	struct lwp *l, *initlwp;
	int i, numcpu;

	/* not reentrant */
	if (rump_inited)
		return 0;
	else if (rump_inited == -1)
		panic("rump_init: host process restart required");
	else
		rump_inited = 1;

	/* initialize hypervisor */
	if (rumpuser_init(RUMPUSER_VERSION, &hyp) != 0) {
		rumpuser_dprintf("rumpuser init failed\n");
		return EINVAL;
	}

	/* retrieve env vars which affect the early stage of bootstrap */
	if (rumpuser_getparam("RUMP_THREADS", buf, sizeof(buf)) == 0) {
		rump_threads = *buf != '0';
	}
	if (rumpuser_getparam("RUMP_VERBOSE", buf, sizeof(buf)) == 0) {
		if (*buf != '0')
			boothowto = AB_VERBOSE;
	}

	if (rumpuser_getparam(RUMPUSER_PARAM_NCPU, buf, sizeof(buf)) != 0)
		panic("mandatory hypervisor configuration (NCPU) missing");
	numcpu = strtoll(buf, NULL, 10);
	if (numcpu < 1) {
		panic("rump kernels are not lightweight enough for \"%d\" CPUs",
		    numcpu);
	}

	rump_thread_init();
	rump_cpus_bootstrap(&numcpu);

	rumpuser_clock_gettime(RUMPUSER_CLOCK_RELWALL, &sec, &nsec);
	boottime.tv_sec = sec;
	boottime.tv_nsec = nsec;

	initmsgbuf(rump_msgbuf, sizeof(rump_msgbuf));
	aprint_verbose("%s%s", copyright, version);

	rump_intr_init(numcpu);

	/* init minimal lwp/cpu context */
	l = &lwp0;
	l->l_lid = 1;
	l->l_cpu = l->l_target_cpu = rump_cpu;
	l->l_fd = &filedesc0;

	/* lwp0 isn't created like other threads, so notify hypervisor here */
	rumpuser_curlwpop(RUMPUSER_LWP_CREATE, l);
	rumpuser_curlwpop(RUMPUSER_LWP_SET, l);

	rump_tsleep_init();

	rumpuser_mutex_init(&rump_giantlock, RUMPUSER_MTX_SPIN);
	ksyms_init();
	uvm_init();
	evcnt_init();

	kcpuset_sysinit();
	once_init();
	kernconfig_lock_init();
	prop_kern_init();

	kmem_init();
	kmeminit();

	uvm_ra_init();
	uao_init();

	mutex_obj_init();
	callout_startup();

	kprintf_init();
	pserialize_init();
	loginit();

	kauth_init();

	secmodel_init();

	rnd_init();

	/*
	 * Create the kernel cprng.  Yes, it's currently stubbed out
	 * to arc4random() for RUMP, but this won't always be so.
	 */
	kern_cprng = cprng_strong_create("kernel", IPL_VM,
					 CPRNG_INIT_ANY|CPRNG_REKEY_ANY);

	procinit();
	proc0_init();
	sysctl_init();
	uid_init();
	chgproccnt(0, 1);

	l->l_proc = &proc0;
	lwp_update_creds(l);

	lwpinit_specificdata();
	lwp_initspecific(&lwp0);

	rump_biglock_init();

	rump_scheduler_init(numcpu);
	/* revert temporary context and schedule a semireal context */
	rumpuser_curlwpop(RUMPUSER_LWP_CLEAR, l);
	initproc = &proc0; /* borrow proc0 before we get initproc started */
	rump_schedule();
	bootlwp = curlwp;

	percpu_init();
	inittimecounter();
	ntp_init();

	ts = boottime;
	tc_setclock(&ts);

	/* we are mostly go.  do per-cpu subsystem init */
	for (i = 0; i < numcpu; i++) {
		struct cpu_info *ci = cpu_lookup(i);

		/* attach non-bootstrap CPUs */
		if (i > 0) {
			rump_cpu_attach(ci);
			ncpu++;
		}

		callout_init_cpu(ci);
		softint_init(ci);
		xc_init_cpu(ci);
		pool_cache_cpu_init(ci);
		selsysinit(ci);
		percpu_init_cpu(ci);

		TAILQ_INIT(&ci->ci_data.cpu_ld_locks);
		__cpu_simple_lock_init(&ci->ci_data.cpu_ld_lock);

		aprint_verbose("cpu%d at thinair0: rump virtual cpu\n", i);
	}

	/* CPUs are up.  allow kernel threads to run */
	rump_thread_allow();

	mksysctls();
	kqueue_init();
	iostat_init();
	fd_sys_init();
	module_init();
	devsw_init();
	pipe_init();
	resource_init();
	procinit_sysctl();

	/* start page baroness */
	if (rump_threads) {
		if (kthread_create(PRI_PGDAEMON, KTHREAD_MPSAFE, NULL,
		    uvm_pageout, NULL, &uvm.pagedaemon_lwp, "pdaemon") != 0)
			panic("pagedaemon create failed");
	} else
		uvm.pagedaemon_lwp = NULL; /* doesn't match curlwp */

	/* process dso's */
	rumpuser_dl_bootstrap(add_linkedin_modules,
	    rump_kernelfsym_load, rump_component_load);

	rump_component_addlocal();
	rump_component_init(RUMP_COMPONENT_KERN);

	/* initialize factions, if present */
	rump_component_init(RUMP__FACTION_VFS);
	/* pnbuf_cache is used even without vfs */
	if (rump_component_count(RUMP__FACTION_VFS) == 0) {
		pnbuf_cache = pool_cache_init(MAXPATHLEN, 0, 0, 0, "pnbufpl",
		    NULL, IPL_NONE, NULL, NULL, NULL);
	}
	rump_component_init(RUMP__FACTION_NET);
	rump_component_init(RUMP__FACTION_DEV);
	KASSERT(rump_component_count(RUMP__FACTION_VFS) <= 1
	    && rump_component_count(RUMP__FACTION_NET) <= 1
	    && rump_component_count(RUMP__FACTION_DEV) <= 1);

	rump_component_init(RUMP_COMPONENT_KERN_VFS);

	/*
	 * if we initialized the tty component above, the tyttymtx is
	 * now initialized.  otherwise, we need to initialize it.
	 */
	if (!rump_ttycomponent)
		mutex_init(&tty_lock, MUTEX_DEFAULT, IPL_VM);

	cold = 0;

	/* aieeeedondest */
	if (rump_threads) {
		if (workqueue_create(&uvm.aiodone_queue, "aiodoned",
		    rump_aiodone_worker, NULL, 0, 0, WQ_MPSAFE))
			panic("aiodoned");
	}

	sysctl_finalize();

	module_init_class(MODULE_CLASS_ANY);

	if (rumpuser_getparam(RUMPUSER_PARAM_HOSTNAME,
	    hostname, MAXHOSTNAMELEN) != 0) {
		panic("mandatory hypervisor configuration (HOSTNAME) missing");
	}
	hostnamelen = strlen(hostname);

	sigemptyset(&sigcantmask);

	if (rump_threads)
		vmem_rehash_start();

	/*
	 * Create init (proc 1), used to attach implicit threads in rump.
	 * (note: must be done after vfsinit to get cwdi)
	 */
	initlwp = rump__lwproc_alloclwp(NULL);
	mutex_enter(proc_lock);
	initproc = proc_find_raw(1);
	mutex_exit(proc_lock);
	if (initproc == NULL)
		panic("where in the world is initproc?");

	/*
	 * Adjust syscall vector in case factions were dlopen()'d
	 * before calling rump_init().
	 * (modules will handle dynamic syscalls the usual way)
	 *
	 * Note: this will adjust the function vectors of
	 * syscalls which use a funcalias (getpid etc.), but
	 * it makes no difference.
	 */
	for (i = 0; i < SYS_NSYSENT; i++) {
		void *sym;

		if (rump_sysent[i].sy_flags & SYCALL_NOSYS ||
		    *syscallnames[i] == '#' ||
		    rump_sysent[i].sy_call == sys_nomodule)
			continue;

		/*
		 * deal with compat wrappers.  makesyscalls.sh should
		 * generate the necessary info instead of this hack,
		 * though.  ugly, fix it later.
		 */ 
#define CPFX "compat_"
#define CPFXLEN (sizeof(CPFX)-1)
		if (strncmp(syscallnames[i], CPFX, CPFXLEN) == 0) {
			const char *p = syscallnames[i] + CPFXLEN;
			size_t namelen;

			/* skip version number */
			while (*p >= '0' && *p <= '9')
				p++;
			if (p == syscallnames[i] + CPFXLEN || *p != '_')
				panic("invalid syscall name %s\n",
				    syscallnames[i]);

			/* skip over the next underscore */
			p++;
			namelen = p + (sizeof("rumpns_")-1) - syscallnames[i];

			strcpy(buf, "rumpns_");
			strcat(buf, syscallnames[i]);
			/* XXX: no strncat in the kernel */
			strcpy(buf+namelen, "sys_");
			strcat(buf, p);
#undef CPFX
#undef CPFXLEN
		} else {
			sprintf(buf, "rumpns_sys_%s", syscallnames[i]);
		}
		if ((sym = rumpuser_dl_globalsym(buf)) != NULL
		    && sym != rump_sysent[i].sy_call) {
#if 0
			rumpuser_dprintf("adjusting %s: %p (old %p)\n",
			    syscallnames[i], sym, rump_sysent[i].sy_call);
#endif
			rump_sysent[i].sy_call = sym;
		}
	}

	rump_component_init(RUMP_COMPONENT_POSTINIT);

	/* component inits done */
	bootlwp = NULL;

	/* open 0/1/2 for init */
	KASSERT(rump_lwproc_curlwp() == NULL);
	rump_lwproc_switch(initlwp);
	rump_consdev_init();
	rump_lwproc_switch(NULL);

	/* release cpu */
	rump_unschedule();

	return 0;
}
/* historic compat */
__strong_alias(rump__init,rump_init);

int
rump_init_server(const char *url)
{

	return rumpuser_sp_init(url, ostype, osrelease, MACHINE);
}

void
cpu_reboot(int howto, char *bootstr)
{
	int ruhow = 0;
	void *finiarg;

	printf("rump kernel halting...\n");

	if (!RUMP_LOCALPROC_P(curproc))
		finiarg = curproc->p_vmspace->vm_map.pmap;
	else
		finiarg = NULL;

	/* dump means we really take the dive here */
	if ((howto & RB_DUMP) || panicstr) {
		ruhow = RUMPUSER_PANIC;
		goto out;
	}

	/* try to sync */
	if (!((howto & RB_NOSYNC) || panicstr)) {
		if (rump_vfs_fini)
			rump_vfs_fini();
	}

	doshutdownhooks();

	/* your wish is my command */
	if (howto & RB_HALT) {
		printf("rump kernel halted\n");
		rumpuser_sp_fini(finiarg);
		for (;;) {
			rumpuser_clock_sleep(RUMPUSER_CLOCK_RELWALL, 10, 0);
		}
	}

	/* this function is __dead, we must exit */
 out:
	printf("halted\n");
	rumpuser_sp_fini(finiarg);
	rumpuser_exit(ruhow);
}

struct uio *
rump_uio_setup(void *buf, size_t bufsize, off_t offset, enum rump_uiorw rw)
{
	struct uio *uio;
	enum uio_rw uiorw;

	switch (rw) {
	case RUMPUIO_READ:
		uiorw = UIO_READ;
		break;
	case RUMPUIO_WRITE:
		uiorw = UIO_WRITE;
		break;
	default:
		panic("%s: invalid rw %d", __func__, rw);
	}

	uio = kmem_alloc(sizeof(struct uio), KM_SLEEP);
	uio->uio_iov = kmem_alloc(sizeof(struct iovec), KM_SLEEP);

	uio->uio_iov->iov_base = buf;
	uio->uio_iov->iov_len = bufsize;

	uio->uio_iovcnt = 1;
	uio->uio_offset = offset;
	uio->uio_resid = bufsize;
	uio->uio_rw = uiorw;
	UIO_SETUP_SYSSPACE(uio);

	return uio;
}

size_t
rump_uio_getresid(struct uio *uio)
{

	return uio->uio_resid;
}

off_t
rump_uio_getoff(struct uio *uio)
{

	return uio->uio_offset;
}

size_t
rump_uio_free(struct uio *uio)
{
	size_t resid;

	resid = uio->uio_resid;
	kmem_free(uio->uio_iov, sizeof(*uio->uio_iov));
	kmem_free(uio, sizeof(*uio));

	return resid;
}

kauth_cred_t
rump_cred_create(uid_t uid, gid_t gid, size_t ngroups, gid_t *groups)
{
	kauth_cred_t cred;
	int rv;

	cred = kauth_cred_alloc();
	kauth_cred_setuid(cred, uid);
	kauth_cred_seteuid(cred, uid);
	kauth_cred_setsvuid(cred, uid);
	kauth_cred_setgid(cred, gid);
	kauth_cred_setgid(cred, gid);
	kauth_cred_setegid(cred, gid);
	kauth_cred_setsvgid(cred, gid);
	rv = kauth_cred_setgroups(cred, groups, ngroups, 0, UIO_SYSSPACE);
	/* oh this is silly.  and by "this" I mean kauth_cred_setgroups() */
	assert(rv == 0);

	return cred;
}

void
rump_cred_put(kauth_cred_t cred)
{

	kauth_cred_free(cred);
}

static int compcounter[RUMP_COMPONENT_MAX];
static int compinited[RUMP_COMPONENT_MAX];

/*
 * Yea, this is O(n^2), but we're only looking at a handful of components.
 * Components are always initialized from the thread that called rump_init().
 * Could also free these when done with them, but prolly not worth it.
 */
struct compstore {
	const struct rump_component *cs_rc;
	LIST_ENTRY(compstore) cs_entries;
};
static LIST_HEAD(, compstore) cshead = LIST_HEAD_INITIALIZER(cshead);

/*
 * add components which are visible from the current object.
 */
static void
rump_component_addlocal(void)
{
	__link_set_decl(rump_components, struct rump_component);
	struct rump_component *const *rc;

	__link_set_foreach(rc, rump_components) {
		rump_component_load(*rc);
	}
}

static void
rump_component_load(const struct rump_component *rc)
{
	struct compstore *cs;

	KASSERT(curlwp == bootlwp);

	LIST_FOREACH(cs, &cshead, cs_entries) {
		if (rc == cs->cs_rc)
			return;
	}

	cs = kmem_alloc(sizeof(*cs), KM_SLEEP);
	cs->cs_rc = rc;
	LIST_INSERT_HEAD(&cshead, cs, cs_entries);
	KASSERT(rc->rc_type < RUMP_COMPONENT_MAX);
	compcounter[rc->rc_type]++;
}

int
rump_component_count(enum rump_component_type type)
{

	KASSERT(curlwp == bootlwp);
	KASSERT(type < RUMP_COMPONENT_MAX);
	return compcounter[type];
}

void
rump_component_init(enum rump_component_type type)
{
	struct compstore *cs;
	const struct rump_component *rc;

	KASSERT(curlwp == bootlwp);
	KASSERT(!compinited[type]);
	LIST_FOREACH(cs, &cshead, cs_entries) {
		rc = cs->cs_rc;
		if (rc->rc_type == type)
			rc->rc_init();
	}
	compinited[type] = 1;
}

/*
 * Initialize a module which has already been loaded and linked
 * with dlopen(). This is fundamentally the same as a builtin module.
 */
int
rump_module_init(const struct modinfo * const *mip, size_t nmodinfo)
{

	return module_builtin_add(mip, nmodinfo, true);
}

/*
 * Finish module (flawless victory, fatality!).
 */
int
rump_module_fini(const struct modinfo *mi)
{

	return module_builtin_remove(mi, true);
}

/*
 * Add loaded and linked module to the builtin list.  It will
 * later be initialized with module_init_class().
 */

static void
add_linkedin_modules(const struct modinfo * const *mip, size_t nmodinfo)
{

	module_builtin_add(mip, nmodinfo, false);
}

int
rump_kernelfsym_load(void *symtab, uint64_t symsize,
	char *strtab, uint64_t strsize)
{
	static int inited = 0;
	Elf64_Ehdr ehdr;

	if (inited)
		return EBUSY;
	inited = 1;

	/*
	 * Use 64bit header since it's bigger.  Shouldn't make a
	 * difference, since we're passing in all zeroes anyway.
	 */
	memset(&ehdr, 0, sizeof(ehdr));
	ksyms_addsyms_explicit(&ehdr, symtab, symsize, strtab, strsize);

	return 0;
}

static int
rump_hyp_syscall(int num, void *arg, long *retval)
{
	register_t regrv[2] = {0, 0};
	struct lwp *l;
	struct sysent *callp;
	int rv;

	if (__predict_false(num >= SYS_NSYSENT))
		return ENOSYS;

	callp = rump_sysent + num;
	l = curlwp;
	rv = sy_call(callp, l, (void *)arg, regrv);
	retval[0] = regrv[0];
	retval[1] = regrv[1];

	return rv;
}

static int
rump_hyp_rfork(void *priv, int flags, const char *comm)
{
	struct vmspace *newspace;
	struct proc *p;
	int error;

	if ((error = rump_lwproc_rfork(flags)) != 0)
		return error;

	/*
	 * Since it's a proxy proc, adjust the vmspace.
	 * Refcount will eternally be 1.
	 */
	p = curproc;
	newspace = kmem_zalloc(sizeof(*newspace), KM_SLEEP);
	newspace->vm_refcnt = 1;
	newspace->vm_map.pmap = priv;
	KASSERT(p->p_vmspace == vmspace_kernel());
	p->p_vmspace = newspace;
	if (comm)
		strlcpy(p->p_comm, comm, sizeof(p->p_comm));

	return 0;
}

/*
 * Order all lwps in a process to exit.  does *not* wait for them to drain.
 */
static void
rump_hyp_lwpexit(void)
{
	struct proc *p = curproc;
	uint64_t where;
	struct lwp *l;

	mutex_enter(p->p_lock);
	/*
	 * First pass: mark all lwps in the process with LW_RUMP_QEXIT
	 * so that they know they should exit.
	 */
	LIST_FOREACH(l, &p->p_lwps, l_sibling) {
		if (l == curlwp)
			continue;
		l->l_flag |= LW_RUMP_QEXIT;
	}
	mutex_exit(p->p_lock);

	/*
	 * Next, make sure everyone on all CPUs sees our status
	 * update.  This keeps threads inside cv_wait() and makes
	 * sure we don't access a stale cv pointer later when
	 * we wake up the threads.
	 */

	where = xc_broadcast(0, (xcfunc_t)nullop, NULL, NULL);
	xc_wait(where);

	/*
	 * Ok, all lwps are either:
	 *  1) not in the cv code
	 *  2) sleeping on l->l_private
	 *  3) sleeping on p->p_waitcv
	 *
	 * Either way, l_private is stable until we set PS_RUMP_LWPEXIT
	 * in p->p_sflag.
	 */

	mutex_enter(p->p_lock);
	LIST_FOREACH(l, &p->p_lwps, l_sibling) {
		if (l->l_private)
			cv_broadcast(l->l_private);
	}
	p->p_sflag |= PS_RUMP_LWPEXIT;
	cv_broadcast(&p->p_waitcv);
	mutex_exit(p->p_lock);
}

/*
 * Notify process that all threads have been drained and exec is complete.
 */
static void
rump_hyp_execnotify(const char *comm)
{
	struct proc *p = curproc;

	fd_closeexec();
	mutex_enter(p->p_lock);
	KASSERT(p->p_nlwps == 1 && p->p_sflag & PS_RUMP_LWPEXIT);
	p->p_sflag &= ~PS_RUMP_LWPEXIT;
	mutex_exit(p->p_lock);
	strlcpy(p->p_comm, comm, sizeof(p->p_comm));
}

int
rump_boot_gethowto()
{

	return boothowto;
}

void
rump_boot_sethowto(int howto)
{

	boothowto = howto;
}

int
rump_getversion(void)
{

	return __NetBSD_Version__;
}

/*
 * Note: may be called unscheduled.  Not fully safe since no locking
 * of allevents (currently that's not even available).
 */
void
rump_printevcnts()
{
	struct evcnt *ev;

	TAILQ_FOREACH(ev, &allevents, ev_list)
		rumpuser_dprintf("%s / %s: %" PRIu64 "\n",
		    ev->ev_group, ev->ev_name, ev->ev_count);
}

/*
 * If you use this interface ... well ... all bets are off.
 * The original purpose is for the p2k fs server library to be
 * able to use the same pid/lid for VOPs as the host kernel.
 */
void
rump_allbetsareoff_setid(pid_t pid, int lid)
{
	struct lwp *l = curlwp;
	struct proc *p = l->l_proc;

	l->l_lid = lid;
	p->p_pid = pid;
}

#include <sys/pserialize.h>

static void
ipiemu(void *a1, void *a2)
{

	xc__highpri_intr(NULL);
	pserialize_switchpoint();
}

void
rump_xc_highpri(struct cpu_info *ci)
{

	if (ci)
		xc_unicast(0, ipiemu, NULL, NULL, ci);
	else
		xc_broadcast(0, ipiemu, NULL, NULL);
}

int
rump_syscall(int num, void *data, size_t dlen, register_t *retval)
{
	struct proc *p;
	struct emul *e;
	struct sysent *callp;
	int rv;

	rump_schedule();
	p = curproc;
	e = p->p_emul;
#ifndef __HAVE_MINIMAL_EMUL
	KASSERT(num > 0 && num < e->e_nsysent);
#endif
	callp = e->e_sysent + num;

	rv = sy_call(callp, curlwp, data, retval);
	rump_unschedule();

	return rv;
}
