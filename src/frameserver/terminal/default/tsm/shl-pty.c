/*
 * SHL - PTY Helpers
 *
 * Copyright (c) 2011-2014 David Herrmann <dh.herrmann@gmail.com>
 * Dedicated to the Public Domain
 */

/*
 * PTY Helpers
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#ifdef __APPLE__
#include <util.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#ifndef IUTF8
#define IUTF8 0x00004000
#endif
#elif defined(__BSD)
#include <libutil.h>
#ifndef IUTF8
#define IUTF8 0x00004000
#endif
#else
#include <pty.h>
#endif

#ifndef SIGUNUSED
#define SIGUNUSED 31
#endif

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>
#include "shl-macro.h"
#include "shl-pty.h"
#include "shl-ring.h"

#ifndef TIOCSIG
#include <asm/ioctls.h>
#endif

#define SHL_PTY_BUFSIZE 16384

/*
 * PTY
 * A PTY object represents a single PTY connection between a master and a
 * child. The child process is fork()ed so the caller controls what program
 * will be run.
 *
 * Programs like /bin/login tend to perform a vhangup() on their TTY
 * before running the login procedure. This also causes the pty master
 * to get a EPOLLHUP event as long as no client has the TTY opened.
 * This means, we cannot use the TTY connection as reliable way to track
 * the client. Instead, we _must_ rely on the PID of the client to track
 * them.
 * However, this has the side effect that if the client forks and the
 * parent exits, we loose them and restart the client. But this seems to
 * be the expected behavior so we implement it here.
 *
 * Unfortunately, epoll always polls for EPOLLHUP so as long as the
 * vhangup() is ongoing, we will _always_ get EPOLLHUP and cannot sleep.
 * This gets worse if the client closes the TTY but doesn't exit.
 * Therefore, the fd must be edge-triggered in the epoll-set so we
 * only get the events once they change. This has to be taken into account by
 * the user of shl_pty. As many event-loops don't support edge-triggered
 * behavior, you can use the shl_pty_bridge interface.
 *
 * Note that shl_pty does not track SIGHUP, you need to do that yourself
 * and call shl_pty_close() once the client exited.
 */

struct shl_pty {
	unsigned long ref;
	int fd;
	pid_t child;
	char in_buf[SHL_PTY_BUFSIZE];
	struct shl_ring out_buf;

	shl_pty_input_fn fn_input;
	void *fn_input_data;
};

enum shl_pty_msg {
	SHL_PTY_FAILED,
	SHL_PTY_SETUP,
};

static char pty_recv(int fd)
{
	int r;
	char d;

	do {
		r = read(fd, &d, 1);
	} while (r < 0 && (errno == EINTR || errno == EAGAIN));

	return (r <= 0) ? SHL_PTY_FAILED : d;
}

static int pty_send(int fd, char d)
{
	int r;

	do {
		r = write(fd, &d, 1);
	} while (r < 0 && (errno == EINTR || errno == EAGAIN));

	return (r == 1) ? 0 : -EINVAL;
}

static int pty_setup_child(int slave,
			   unsigned short term_width,
			   unsigned short term_height)
{
	struct termios attr;
	struct winsize ws;

	/* get terminal attributes */
	if (tcgetattr(slave, &attr) < 0)
		return -errno;

	/* erase character should be normal backspace, PLEASEEE! */
	attr.c_cc[VERASE] = 010;
	/* always set UTF8 flag */
	attr.c_iflag |= IUTF8;

	/* set changed terminal attributes */
	if (tcsetattr(slave, TCSANOW, &attr) < 0)
		return -errno;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = term_width;
	ws.ws_row = term_height;

	if (ioctl(slave, TIOCSWINSZ, &ws) < 0)
		return -errno;

	if (dup2(slave, STDIN_FILENO) != STDIN_FILENO ||
	    dup2(slave, STDOUT_FILENO) != STDOUT_FILENO ||
	    dup2(slave, STDERR_FILENO) != STDERR_FILENO)
		return -errno;

	return 0;
}

static int pty_init_child(int fd)
{
	int r;
	sigset_t sigset;
	char *slave_name;
	int slave, i;
	pid_t pid;

	/* unlockpt() requires unset signal-handlers */
	sigemptyset(&sigset);
	r = sigprocmask(SIG_SETMASK, &sigset, NULL);
	if (r < 0)
		return -errno;

	for (i = 1; i < SIGUNUSED; ++i)
		signal(i, SIG_DFL);

	r = grantpt(fd);
	if (r < 0)
		return -errno;

	r = unlockpt(fd);
	if (r < 0)
		return -errno;

	slave_name = ptsname(fd);
	if (!slave_name)
		return -errno;

	/* open slave-TTY */
	slave = open(slave_name, O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (slave < 0)
		return -errno;

	/* open session so we loose our controlling TTY */
	pid = setsid();
	if (pid < 0) {
		close(slave);
		return -errno;
	}

	/* set controlling TTY */
	r = ioctl(slave, TIOCSCTTY, 0);
	if (r < 0) {
		close(slave);
		return -errno;
	}

	return slave;
}

pid_t shl_pty_open(struct shl_pty **out,
		   shl_pty_input_fn fn_input,
		   void *fn_input_data,
		   unsigned short term_width,
		   unsigned short term_height)
{
	_shl_pty_unref_ struct shl_pty *pty = NULL;
	_shl_close_ int fd = -1;
	int slave, r, comm[2];
	pid_t pid;
	char d;

	if (!out)
		return -EINVAL;

	pty = calloc(1, sizeof(*pty));
	if (!pty)
		return -ENOMEM;

	pty->ref = 1;
	pty->fd = -1;
	pty->fn_input = fn_input;
	pty->fn_input_data = fn_input_data;

	fd = posix_openpt(O_RDWR | O_NOCTTY);
	int flags = fcntl(fd, F_GETFD);

	if (fd < 0)
		return -errno;

	r = pipe(comm);
	if (r < 0)
		return -errno;

	fcntl(comm[0], F_SETFD, FD_CLOEXEC);
	fcntl(comm[1], F_SETFD, FD_CLOEXEC);

	pid = fork();
	if (pid < 0) {
		/* error */
		pid = -errno;
		close(comm[0]);
		close(comm[1]);
		return pid;
	} else if (!pid) {
		/* child */
		slave = pty_init_child(fd);
		if (slave < 0)
			exit(1);

		close(comm[0]);
		close(fd);
		fd = -1;
		free(pty);
		pty = NULL;

		r = pty_setup_child(slave, term_width, term_height);
		if (r < 0)
			exit(1);

		/* close slave if it's not one of the std-fds */
		if (slave > 2)
			close(slave);

		/* wake parent */
		pty_send(comm[1], SHL_PTY_SETUP);
		close(comm[1]);

		*out = NULL;
		return pid;
	}

	/* parent */
	pty->fd = fd;
	pty->child = pid;

	close(comm[1]);
	fd = -1;

	/* wait for child setup */
	d = pty_recv(comm[0]);
	close(comm[0]);
	if (d != SHL_PTY_SETUP)
		return -EINVAL;

	*out = pty;
	pty = NULL;
	return pid;
}

void shl_pty_ref(struct shl_pty *pty)
{
	if (!pty || !pty->ref)
		return;

	++pty->ref;
}

void shl_pty_unref(struct shl_pty *pty)
{
	if (!pty || !pty->ref || --pty->ref)
		return;

	shl_pty_close(pty);
	shl_ring_clear(&pty->out_buf);
	free(pty);
}

void shl_pty_close(struct shl_pty *pty)
{
	if (!pty || pty->fd < 0)
		return;

	close(pty->fd);
	pty->fd = -1;
}

bool shl_pty_is_open(struct shl_pty *pty)
{
	return pty && pty->fd >= 0;
}

int shl_pty_get_fd(struct shl_pty *pty)
{
	if (!pty)
		return -EINVAL;

	return pty->fd >= 0 ? pty->fd : -EPIPE;
}

pid_t shl_pty_get_child(struct shl_pty *pty)
{
	if (!pty)
		return -EINVAL;

	return pty->child > 0 ? pty->child : -ECHILD;
}

static int pty_write(struct shl_pty *pty)
{
	struct iovec vec[2];
	unsigned int i;
	size_t num;
	ssize_t r;

	/*
	 * Same as pty_read(), we're edge-triggered so we need to call write()
	 * until either all data is written or it return EAGAIN. We call it
	 * twice and if it still writes successfully, we return EAGAIN. If we
	 * bail out early, we also return EAGAIN if there's still data.
	 */

	for (i = 0; i < 2; ++i) {
		num = shl_ring_peek(&pty->out_buf, vec);
		if (!num)
			return 0;

		r = writev(pty->fd, vec, (int)num);
		if (r < 0) {
			if (errno == EAGAIN)
				return 0;
			if (errno == EINTR)
				return -EAGAIN;

			return -errno;
		} else if (!r) {
			return -EPIPE;
		} else {
			shl_ring_pull(&pty->out_buf, (size_t)r);
		}
	}

	return shl_ring_get_size(&pty->out_buf) > 0 ? -EAGAIN : 0;
}

static int pty_read(struct shl_pty *pty)
{
	unsigned int i;
	ssize_t len;
	size_t nr = 0;

	/*
	 * We're edge-triggered, means we need to read the whole queue. This,
	 * however, might cause us to stall if the writer is faster than we
	 * are. Therefore, we read twice and if the second read still returned
	 * data, we return -EAGAIN and let the caller deal with rescheduling the
	 * dispatcher.
	 */

	for (i = 0; i < 2; ++i) {
		len = read(pty->fd, pty->in_buf, sizeof(pty->in_buf) - 1);
		if (len < 0) {
			if (errno == EAGAIN)
				return 0;
			if (errno == EINTR)
				return 0;

			return -errno;
		} else if (!len) {
			return -EPIPE;
		} else if (len > 0 && pty->fn_input) {
			/* set terminating zero for debugging safety */
			pty->in_buf[len] = 0;
			pty->fn_input(pty,
				pty->fn_input_data, pty->in_buf, len);
			nr += len;
		}
	}

	return nr;
}

int shl_pty_dispatch(struct shl_pty *pty)
{
	int r;

	if (!shl_pty_is_open(pty))
		return -ENODEV;

	pty_write(pty);
	return 0;
}

int shl_pty_write(struct shl_pty *pty, const char *u8, size_t len)
{
	if (!shl_pty_is_open(pty))
		return -ENODEV;

	int rv = shl_ring_push(&pty->out_buf, u8, len);
	pty_write(pty);
	return rv;
}

int shl_pty_signal(struct shl_pty *pty, int sig)
{
/* TIOCSIG isn't in any pledge profile */
#ifdef __OpenBSD__
	return -1;
#endif

	if (!shl_pty_is_open(pty))
		return -ENODEV;

	return ioctl(pty->fd, TIOCSIG, sig) < 0 ? -errno : 0;
}

int shl_pty_resize(struct shl_pty *pty,
		   unsigned short term_width,
		   unsigned short term_height)
{
	struct winsize ws = {
		.ws_col = term_width,
		.ws_row = term_height
	};

	if (!shl_pty_is_open(pty))
		return -ENODEV;

	/*
	 * This will send SIGWINCH to the pty slave foreground process group.
	 * We will also get one, but we don't need it.
	 */
	return ioctl(pty->fd, TIOCSWINSZ, &ws) < 0 ? -errno : 0;
}
