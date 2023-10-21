// Description: bpf-mocker daemon
#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <linux/perf_event.h>
#include "bpf-mocker-event.h"
#include "bpf-mocker.skel.h"
#include "daemon_config.hpp"
#include "handle_bpf_event.hpp"
#include "daemon.hpp"

#define NSEC_PER_SEC 1000000000ULL

using namespace bpftime;

static volatile sig_atomic_t exiting = 0;
static bool verbose = false;
static bpf_event_handler handler({});

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_int(int signo)
{
	exiting = 1;
}


static int handle_event_rb(void *ctx, void *data, size_t data_sz)
{
	const struct event *e = (const struct event *)data;
	handler.handle_event(e);
	return 0;
}

void handle_lost_events(void *ctx, int cpu, __u64 lost_cnt)
{
	fprintf(stderr, "Lost %llu events on CPU #%d!\n", lost_cnt, cpu);
}

int bpftime::start_daemon(struct env env)
{
	LIBBPF_OPTS(bpf_object_open_opts, open_opts);
	struct ring_buffer *rb = NULL;
	struct bpf_mocker_bpf *obj = NULL;
	int err;

	libbpf_set_print(libbpf_print_fn);
	
	// update handler config
	handler = bpf_event_handler(env);
	verbose = env.verbose;

	if (signal(SIGINT, sig_int) == SIG_ERR) {
		fprintf(stderr, "can't set signal handler: %s\n",
			strerror(errno));
		err = 1;
		goto cleanup;
	}

	obj = bpf_mocker_bpf__open();
	if (!obj) {
		fprintf(stderr, "failed to open BPF object\n");
		goto cleanup;
	}

	/* initialize global data (filtering options) */
	obj->rodata->target_pid = env.pid;
	obj->rodata->disable_modify = true;
	obj->rodata->uprobe_perf_type = determine_uprobe_perf_type();
	obj->rodata->kprobe_perf_type = determine_kprobe_perf_type();

	err = bpf_mocker_bpf__load(obj);
	if (err) {
		fprintf(stderr, "failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	err = bpf_mocker_bpf__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		goto cleanup;
	}

	/* Set up ring buffer polling */
	rb = ring_buffer__new(bpf_map__fd(obj->maps.rb), handle_event_rb, NULL,
			      NULL);
	if (!rb) {
		err = -1;
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	/* main: poll */
	while (!exiting) {
		err = ring_buffer__poll(rb, 100 /* timeout, ms */);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "error polling perf buffer: %s\n",
				strerror(-err));
			goto cleanup;
		}
		/* reset err to return 0 if exiting */
		err = 0;
	}

cleanup:
	ring_buffer__free(rb);
	bpf_mocker_bpf__destroy(obj);

	return err != 0;
}