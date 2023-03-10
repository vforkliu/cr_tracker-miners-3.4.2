/*
 * Copyright (C) 2016, Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include "tracker-seccomp.h"

#ifdef HAVE_LIBSECCOMP

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <seccomp.h>

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

#define ALLOW_RULE(call) G_STMT_START { \
	int allow_rule_syscall_number = seccomp_syscall_resolve_name (G_STRINGIFY (call)); \
	if (allow_rule_syscall_number == __NR_SCMP_ERROR || \
	    seccomp_rule_add (ctx, SCMP_ACT_ALLOW, allow_rule_syscall_number, 0) < 0) \
		goto out; \
} G_STMT_END

#define ERROR_RULE(call, error) G_STMT_START { \
	int error_rule_syscall_number = seccomp_syscall_resolve_name (G_STRINGIFY (call)); \
	if (error_rule_syscall_number == __NR_SCMP_ERROR || \
	    seccomp_rule_add (ctx, SCMP_ACT_ERRNO (error), error_rule_syscall_number, 0) < 0) \
		goto out; \
} G_STMT_END

static void
sigsys_handler (gint       signal,
                siginfo_t *info,
                gpointer   context)
{
	if (info->si_signo == SIGSYS &&
	    info->si_code == SYS_SECCOMP) {
		char *syscall_name;

		syscall_name = seccomp_syscall_resolve_num_arch (SCMP_ARCH_NATIVE,
		                                                 info->si_syscall);
		g_printerr ("Disallowed syscall \"%s\" caught in sandbox\n", syscall_name);
		free (syscall_name);
	}
}

static gboolean
initialize_sigsys_handler (void)
{
	struct sigaction act = { 0 };

	sigemptyset (&act.sa_mask);
	sigaddset (&act.sa_mask, SIGSYS);
	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = sigsys_handler;

	if (sigaction (SIGSYS, &act, NULL) < 0)
		return FALSE;

	return TRUE;
}

gboolean
tracker_seccomp_init (void)
{
	scmp_filter_ctx ctx;

	if (!initialize_sigsys_handler ())
		return FALSE;

	ctx = seccomp_init (SCMP_ACT_TRAP);
	if (ctx == NULL)
		return FALSE;

	/* Memory management */
	ALLOW_RULE (brk);
	ALLOW_RULE (get_mempolicy);
	ALLOW_RULE (set_mempolicy);
	ALLOW_RULE (mmap);
	ALLOW_RULE (mmap2);
	ALLOW_RULE (munmap);
	ALLOW_RULE (mremap);
	ALLOW_RULE (mprotect);
	ALLOW_RULE (madvise);
	ALLOW_RULE (mbind);
	ERROR_RULE (mlock, EPERM);
	ERROR_RULE (mlock2, EPERM);
	ERROR_RULE (munlock, EPERM);
	ERROR_RULE (mlockall, EPERM);
	ERROR_RULE (munlockall, EPERM);
	/* Process management */
	ALLOW_RULE (exit_group);
	ALLOW_RULE (getuid);
	ALLOW_RULE (getgid);
	ALLOW_RULE (getuid32);
	ALLOW_RULE (getegid);
	ALLOW_RULE (getegid32);
	ALLOW_RULE (geteuid);
	ALLOW_RULE (geteuid32);
	ALLOW_RULE (getppid);
	ALLOW_RULE (gettid);
	ALLOW_RULE (getpid);
	ALLOW_RULE (exit);
	ALLOW_RULE (getrusage);
	ALLOW_RULE (getrlimit);
	/* Basic filesystem access */
	ALLOW_RULE (fstat);
	ALLOW_RULE (fstat64);
	ALLOW_RULE (fstatat64);
	ALLOW_RULE (newfstatat);
	ALLOW_RULE (stat);
	ALLOW_RULE (stat64);
	ALLOW_RULE (statfs);
	ALLOW_RULE (statfs64);
	ALLOW_RULE (lstat);
	ALLOW_RULE (lstat64);
	ALLOW_RULE (statx);
	ALLOW_RULE (access);
	ALLOW_RULE (faccessat);
	ALLOW_RULE (faccessat2);
	ALLOW_RULE (getdents);
	ALLOW_RULE (getdents64);
	ALLOW_RULE (getcwd);
	ALLOW_RULE (readlink);
	ALLOW_RULE (readlinkat);
	ALLOW_RULE (utime);
	ALLOW_RULE (time);
	ALLOW_RULE (fsync);
	ALLOW_RULE (umask);
	ERROR_RULE (fchown, EPERM);
	/* Processes and threads */
	ALLOW_RULE (clone);
	ALLOW_RULE (clone3);
	ALLOW_RULE (futex);
	ALLOW_RULE (futex_time64);
	ALLOW_RULE (set_robust_list);
	ALLOW_RULE (rseq);
	ALLOW_RULE (rt_sigaction);
	ALLOW_RULE (rt_sigprocmask);
	ALLOW_RULE (sched_yield);
	ALLOW_RULE (sched_getaffinity);
	ALLOW_RULE (sched_setattr);
	ALLOW_RULE (nanosleep);
	ALLOW_RULE (clock_nanosleep);
	ALLOW_RULE (clock_nanosleep_time64);
	ALLOW_RULE (waitid);
	ALLOW_RULE (waitpid);
	ALLOW_RULE (wait4);
	/* Main loops */
	ALLOW_RULE (poll);
	ALLOW_RULE (ppoll);
	ALLOW_RULE (ppoll_time64);
	ALLOW_RULE (fcntl);
	ALLOW_RULE (fcntl64);
	ALLOW_RULE (eventfd);
	ALLOW_RULE (eventfd2);
	ALLOW_RULE (pipe);
	ALLOW_RULE (pipe2);
	ALLOW_RULE (epoll_create);
	ALLOW_RULE (epoll_create1);
	ALLOW_RULE (epoll_ctl);
	/* System */
	ALLOW_RULE (uname);
	ALLOW_RULE (sysinfo);
	ALLOW_RULE (prctl);
	ALLOW_RULE (getrandom);
	ALLOW_RULE (clock_gettime);
	ALLOW_RULE (clock_gettime64);
	ALLOW_RULE (clock_getres);
	ALLOW_RULE (gettimeofday);
	/* Descriptors */
	ALLOW_RULE (close);
	ALLOW_RULE (read);
	ALLOW_RULE (lseek);
	ALLOW_RULE (_llseek);
	ALLOW_RULE (fadvise64);
	ALLOW_RULE (fadvise64_64);
	ALLOW_RULE (arm_fadvise64_64);
	ALLOW_RULE (write);
	ALLOW_RULE (writev);
	ALLOW_RULE (dup);
	ALLOW_RULE (dup2);
	ALLOW_RULE (dup3);
	/* Needed by some GStreamer modules doing crazy stuff, less
	 * scary thanks to the restriction below about sockets being
	 * local.
	 */
	ALLOW_RULE (connect);
	ALLOW_RULE (send);
	ALLOW_RULE (sendto);
	ALLOW_RULE (sendmsg);
	ALLOW_RULE (recv);
	ALLOW_RULE (recvmsg);
	ALLOW_RULE (recvfrom);
	ALLOW_RULE (getsockname);
	ALLOW_RULE (getpeername);
	ALLOW_RULE (shutdown);

	/* Special requirements for socket/socketpair, only on AF_UNIX/AF_LOCAL */
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
	                      SCMP_CMP(0, SCMP_CMP_EQ, AF_UNIX)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
	                      SCMP_CMP(0, SCMP_CMP_EQ, AF_LOCAL)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(socketpair), 1,
	                      SCMP_CMP(0, SCMP_CMP_EQ, AF_UNIX)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(socketpair), 1,
	                      SCMP_CMP(0, SCMP_CMP_EQ, AF_LOCAL)) < 0)
		goto out;

	/* Special requirements for ioctl, allowed on stdout/stderr */
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
	                      SCMP_CMP(0, SCMP_CMP_EQ, 1)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(ioctl), 1,
	                      SCMP_CMP(0, SCMP_CMP_EQ, 2)) < 0)
		goto out;

	/* Special requirements for open/openat, allow O_RDONLY calls,
         * but fail if write permissions are requested.
	 */
	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(open), 1,
	                      SCMP_CMP(1, SCMP_CMP_MASKED_EQ, O_WRONLY | O_RDWR, 0)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ERRNO (EACCES), SCMP_SYS(open), 1,
	                      SCMP_CMP(1, SCMP_CMP_MASKED_EQ, O_WRONLY, O_WRONLY)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ERRNO (EACCES), SCMP_SYS(open), 1,
	                      SCMP_CMP(1, SCMP_CMP_MASKED_EQ, O_RDWR, O_RDWR)) < 0)
		goto out;

	if (seccomp_rule_add (ctx, SCMP_ACT_ALLOW, SCMP_SYS(openat), 1,
	                      SCMP_CMP(2, SCMP_CMP_MASKED_EQ, O_WRONLY | O_RDWR, 0)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ERRNO (EACCES), SCMP_SYS(openat), 1,
	                      SCMP_CMP(2, SCMP_CMP_MASKED_EQ, O_WRONLY, O_WRONLY)) < 0)
		goto out;
	if (seccomp_rule_add (ctx, SCMP_ACT_ERRNO (EACCES), SCMP_SYS(openat), 1,
	                      SCMP_CMP(2, SCMP_CMP_MASKED_EQ, O_RDWR, O_RDWR)) < 0)
		goto out;

	/* Syscalls may differ between libcs */
#if !defined(__GLIBC__)
	ALLOW_RULE (rt_sigreturn);
	ALLOW_RULE (readv);
#else
	ALLOW_RULE (pread64);
#endif

	g_debug ("Loading seccomp rules.");

	if (seccomp_load (ctx) >= 0) {
		/* Any seccomp filters loaded into the kernel are not affected. */
		seccomp_release (ctx);
		return TRUE;
	}

out:
	g_critical ("Failed to load seccomp rules.");
	seccomp_release (ctx);
	return FALSE;
}

#else /* HAVE_LIBSECCOMP */

gboolean
tracker_seccomp_init (void)
{
	g_warning ("No seccomp support compiled-in.");
	return TRUE;
}

#endif /* HAVE_LIBSECCOMP */
