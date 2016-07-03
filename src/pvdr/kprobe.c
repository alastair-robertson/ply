/*
 * Copyright 2015-2016 Tobias Waldekranz <tobias@waldekranz.com>
 *
 * This file is part of ply.
 *
 * ply is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, under the terms of version 2 of the
 * License.
 *
 * ply is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ply.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <linux/version.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include "../ply.h"
#include "../bpf-syscall.h"
#include "pvdr.h"

typedef struct kprobe {
	const char *type;
	FILE *ctrl;
	int bfd;

	struct {
		int cap, len;
		int *fds;
	} efds;
} kprobe_t;

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
		int cpu, int group_fd, unsigned long flags)
{
	int ret;

	ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
		      group_fd, flags);
	return ret;
}

static int kprobe_event_id(kprobe_t *kp, const char *func)
{
	FILE *fp;
	char *ev_id, ev_str[16];

	fprintf(kp->ctrl, "%s %s\n", kp->type, func);
	fflush(kp->ctrl);

	asprintf(&ev_id, "/sys/kernel/debug/tracing/events/kprobes/%s_%s_0/id",
		 kp->type, func);
	fp = fopen(ev_id, "r");
	free(ev_id);
	if (!fp) {
		_pe("unable to create kprobe for \"%s\"", func);
		return -EIO;
	}

	fgets(ev_str, sizeof(ev_str), fp);
	fclose(fp);
	return strtol(ev_str, NULL, 0);
}

static int kprobe_attach_one(kprobe_t *kp, const char *func)
{
	struct perf_event_attr attr = {};
	int efd, i, id;

	id = kprobe_event_id(kp, func);
	if (id < 0)
		return id;

	attr.type = PERF_TYPE_TRACEPOINT;
	attr.sample_type = PERF_SAMPLE_RAW;
	attr.sample_period = 1;
	attr.wakeup_events = 1;
	attr.config = id;

	for (i = 0; i < /* sysconf(_SC_NPROCESSORS_ONLN) */ 1; i++) {
		efd = perf_event_open(&attr, -1/*pid*/, i/*cpu*/, -1/*group_fd*/, 0);
		if (efd < 0) {
			perror("perf_event_open");
			return -errno;
		}

		if (ioctl(efd, PERF_EVENT_IOC_ENABLE, 0)) {
			perror("perf enable");
			close(efd);
			return -errno;
		}

		if (!i && ioctl(efd, PERF_EVENT_IOC_SET_BPF, kp->bfd)) {
			_pe("perf-set-bpf: %s", func);
			close(efd);
			return -errno;
		}

		if (kp->efds.len == kp->efds.cap) {
			size_t sz = kp->efds.cap * sizeof(*kp->efds.fds);

			kp->efds.fds = realloc(kp->efds.fds, sz << 1);
			assert(kp->efds.fds);
			kp->efds.cap <<= 1;
		}

		kp->efds.fds[kp->efds.len++] = efd;
	}
	return 1;
}

static int kprobe_attach_pattern(kprobe_t *kp, const char *pattern)
{
	FILE *ksyms;
	char *line;
	int err = 0;

	ksyms = fopen("/sys/kernel/debug/tracing/available_filter_functions", "r");
	if (!ksyms) {
		perror("no kernel symbols available");
		return -ENOENT;
	}

	line = malloc(256);
	assert(line);
	while (err >= 0 && fgets(line, 256, ksyms)) {
		char *end;

		if (strchr(line, '.'))
			continue;

		end = strchr(line, '\n');
		if (end)
			*end = '\0';

		if (fnmatch(pattern, line, 0))
			continue;

		err = kprobe_attach_one(kp, line);
		if (err == -EEXIST)
			err = 0;
	}
	free(line);
	fclose(ksyms);
	return (err < 0) ? err : kp->efds.len;
}

static int __kprobe_setup(node_t *probe, prog_t *prog, const char *type)
{
	kprobe_t *kp;
	char *func;

	kp = malloc(sizeof(*kp));
	assert(kp);

	kp->type = type;
	kp->efds.fds = calloc(1, sizeof(*kp->efds.fds));
	assert(kp->efds.fds);
	kp->efds.cap = 1;
	kp->efds.len = 0;

	probe->dyn.probe.pvdr_priv = kp;

	_d("");
	kp->ctrl = fopen("/sys/kernel/debug/tracing/kprobe_events", "a+");
	if (!kp->ctrl) {
		perror("unable to open kprobe_events");
		return -EIO;
	}

	kp->bfd = bpf_prog_load(prog->insns, prog->ip - prog->insns);
	if (kp->bfd < 0) {
		perror("bpf_prog_load");
		fprintf(stderr, "This version of ply is compiled against kernel version %d.%d.%d\n",
		                 (LINUX_VERSION_CODE>>16)&0xf,
		                 (LINUX_VERSION_CODE>> 8)&0xf,
		                 (LINUX_VERSION_CODE>> 0)&0xf);
		fprintf(stderr, "bpf verifier:\n%s\n", bpf_log_buf);
		return -EINVAL;
	}
	
	func = strchr(probe->string, ':') + 1;
	if (strchr(func, '?') || strchr(func, '*'))
		return kprobe_attach_pattern(kp, func);
	else
		return kprobe_attach_one(kp, func);
}

static int kprobe_setup(node_t *probe, prog_t *prog)
{
	return __kprobe_setup(probe, prog, "p");
}

static int kretprobe_setup(node_t *probe, prog_t *prog)
{
	return __kprobe_setup(probe, prog, "r");
}

static int kprobe_teardown(node_t *probe)
{
	kprobe_t *kp = probe->dyn.probe.pvdr_priv;
	int i;

	for (i = 0; i < kp->efds.len; i++)
		close(kp->efds.fds[i]);

	free(kp->efds.fds);
	free(kp);
	return 0;
}

static int kprobe_compile(node_t *call, prog_t *prog)
{
	return builtin_compile(call, prog);
}

static int kprobe_loc_assign(node_t *call)
{
	return builtin_loc_assign(call);
}

static int kprobe_annotate(node_t *call)
{
	return builtin_annotate(call);
}

pvdr_t kprobe_pvdr = {
	.name = "kprobe",
	.annotate   = kprobe_annotate,
	.loc_assign = kprobe_loc_assign,
	.compile    = kprobe_compile,
	.setup      = kprobe_setup,
	.teardown   = kprobe_teardown,
};

pvdr_t kretprobe_pvdr = {
	.name = "kretprobe",
	.annotate   = kprobe_annotate,
	.loc_assign = kprobe_loc_assign,
	.compile    = kprobe_compile,
	.setup      = kretprobe_setup,
	.teardown   = kprobe_teardown,
};

__attribute__((constructor))
static void kprobe_pvdr_register(void)
{
	pvdr_register(   &kprobe_pvdr);
	pvdr_register(&kretprobe_pvdr);
}
