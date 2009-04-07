#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <fcntl.h>
#include <poll.h>

#include "marker.h"
#include "tracer.h"
#include "localerr.h"
#include "ustcomm.h"
#include "relay.h" /* FIXME: remove */

//#define USE_CLONE

#define USTSIGNAL SIGIO

#define MAX_MSG_SIZE (100)
#define MSG_NOTIF 1
#define MSG_REGISTER_NOTIF 2

char consumer_stack[10000];

struct list_head blocked_consumers = LIST_HEAD_INIT(blocked_consumers);

static struct ustcomm_app ustcomm_app;

struct tracecmd { /* no padding */
	uint32_t size;
	uint16_t command;
};

//struct listener_arg {
//	int pipe_fd;
//};

struct trctl_msg {
	/* size: the size of all the fields except size itself */
	uint32_t size;
	uint16_t type;
	/* Only the necessary part of the payload is transferred. It
         * may even be none of it.
         */
	char payload[94];
};

struct consumer_channel {
	int fd;
	struct ltt_channel_struct *chan;
};

struct blocked_consumer {
	int fd_consumer;
	int fd_producer;
	int tmp_poll_idx;

	/* args to ustcomm_send_reply */
	struct ustcomm_server server;
	struct ustcomm_source src;

	/* args to ltt_do_get_subbuf */
	struct rchan_buf *rbuf;
	struct ltt_channel_buf_struct *lttbuf;

	struct list_head list;
};

static void print_markers(void)
{
	struct marker_iter iter;

	lock_markers();
	marker_iter_reset(&iter);
	marker_iter_start(&iter);

	while(iter.marker) {
		fprintf(stderr, "marker: %s_%s \"%s\"\n", iter.marker->channel, iter.marker->name, iter.marker->format);
		marker_iter_next(&iter);
	}
	unlock_markers();
}

void do_command(struct tracecmd *cmd)
{
}

void receive_commands()
{
}

int fd_notif = -1;
void notif_cb(void)
{
	int result;
	struct trctl_msg msg;

	/* FIXME: fd_notif should probably be protected by a spinlock */

	if(fd_notif == -1)
		return;

	msg.type = MSG_NOTIF;
	msg.size = sizeof(msg.type);

	/* FIXME: don't block here */
	result = write(fd_notif, &msg, msg.size+sizeof(msg.size));
	if(result == -1) {
		PERROR("write");
		return;
	}
}

static int inform_consumer_daemon(void)
{
	ustcomm_request_consumer(getpid(), "metadata");
	ustcomm_request_consumer(getpid(), "ust");
}

void process_blocked_consumers(void)
{
	int n_fds = 0;
	struct pollfd *fds;
	struct blocked_consumer *bc;
	int idx = 0;
	char inbuf;
	int result;

	list_for_each_entry(bc, &blocked_consumers, list) {
		n_fds++;
	}

	fds = (struct pollfd *) malloc(n_fds * sizeof(struct pollfd));
	if(fds == NULL) {
		ERR("malloc returned NULL");
		return;
	}

	list_for_each_entry(bc, &blocked_consumers, list) {
		fds[idx].fd = bc->fd_producer;
		fds[idx].events = POLLIN;
		bc->tmp_poll_idx = idx;
		idx++;
	}

	result = poll(fds, n_fds, 0);
	if(result == -1) {
		PERROR("poll");
		return -1;
	}

	list_for_each_entry(bc, &blocked_consumers, list) {
		if(fds[bc->tmp_poll_idx].revents) {
			long consumed_old = 0;
			char *reply;

			result = read(bc->fd_producer, &inbuf, 1);
			if(result == -1) {
				PERROR("read");
				continue;
			}
			if(result == 0) {
				DBG("PRODUCER END");

				close(bc->fd_producer);

				__list_del(bc->list.prev, bc->list.next);

				result = ustcomm_send_reply(&bc->server, "END", &bc->src);
				if(result < 0) {
					ERR("ustcomm_send_reply failed");
					continue;
				}

				continue;
			}

			result = ltt_do_get_subbuf(bc->rbuf, bc->lttbuf, &consumed_old);
			if(result == -EAGAIN) {
				WARN("missed buffer?");
				continue;
			}
			else if(result < 0) {
				DBG("ltt_do_get_subbuf: error: %s", strerror(-result));
			}
			asprintf(&reply, "%s %ld", "OK", consumed_old);
			result = ustcomm_send_reply(&bc->server, reply, &bc->src);
			if(result < 0) {
				ERR("ustcomm_send_reply failed");
				free(reply);
				continue;
			}
			free(reply);

			__list_del(bc->list.prev, bc->list.next);
		}
	}

}

int listener_main(void *p)
{
	int result;

	DBG("LISTENER");

	for(;;) {
		uint32_t size;
		struct sockaddr_un addr;
		socklen_t addrlen = sizeof(addr);
		char trace_name[] = "auto";
		char trace_type[] = "ustrelay";
		char *recvbuf;
		int len;
		struct ustcomm_source src;

		process_blocked_consumers();

		result = ustcomm_app_recv_message(&ustcomm_app, &recvbuf, &src, 5);
		if(result < 0) {
			WARN("error in ustcomm_app_recv_message");
			continue;
		}
		else if(result == 0) {
			/* no message */
			continue;
		}

		DBG("received a message! it's: %s\n", recvbuf);
		len = strlen(recvbuf);

		if(!strcmp(recvbuf, "print_markers")) {
			print_markers();
		}
		else if(!strcmp(recvbuf, "trace_setup")) {
			DBG("trace setup");

			result = ltt_trace_setup(trace_name);
			if(result < 0) {
				ERR("ltt_trace_setup failed");
				return;
			}

			result = ltt_trace_set_type(trace_name, trace_type);
			if(result < 0) {
				ERR("ltt_trace_set_type failed");
				return;
			}
		}
		else if(!strcmp(recvbuf, "trace_alloc")) {
			DBG("trace alloc");

			result = ltt_trace_alloc(trace_name);
			if(result < 0) {
				ERR("ltt_trace_alloc failed");
				return;
			}
		}
		else if(!strcmp(recvbuf, "trace_start")) {
			DBG("trace start");

			result = ltt_trace_start(trace_name);
			if(result < 0) {
				ERR("ltt_trace_start failed");
				continue;
			}
		}
		else if(!strcmp(recvbuf, "trace_stop")) {
			DBG("trace stop");

			result = ltt_trace_stop(trace_name);
			if(result < 0) {
				ERR("ltt_trace_stop failed");
				return;
			}
		}
		else if(!strcmp(recvbuf, "trace_destroy")) {

			DBG("trace destroy");

			result = ltt_trace_destroy(trace_name);
			if(result < 0) {
				ERR("ltt_trace_destroy failed");
				return;
			}
		}
		else if(nth_token_is(recvbuf, "get_shmid", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_shmid");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_shmid: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				CPRINTF("cannot find trace!");
				return 1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;
				struct rchan_buf *rbuf = rchan->buf;
				struct ltt_channel_struct *ltt_channel = (struct ltt_channel_struct *)rchan->private_data;
				struct ltt_channel_buf_struct *ltt_buf = ltt_channel->buf;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					char *reply;

					DBG("the shmid for the requested channel is %d", rbuf->shmid);
					DBG("the shmid for its buffer structure is %d", ltt_channel->buf_shmid);
					asprintf(&reply, "%d %d", rbuf->shmid, ltt_channel->buf_shmid);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: get_shmid: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "get_n_subbufs", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_n_subbufs");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_n_subbufs: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				CPRINTF("cannot find trace!");
				return 1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					char *reply;

					DBG("the n_subbufs for the requested channel is %d", rchan->n_subbufs);
					asprintf(&reply, "%d", rchan->n_subbufs);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: get_n_subbufs: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "get_subbuf_size", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_subbuf_size");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_subbuf_size: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				CPRINTF("cannot find trace!");
				return 1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					char *reply;

					DBG("the subbuf_size for the requested channel is %d", rchan->subbuf_size);
					asprintf(&reply, "%d", rchan->subbuf_size);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: get_subbuf_size: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "load_probe_lib", 0) == 1) {
			char *libfile;

			libfile = nth_token(recvbuf, 1);

			DBG("load_probe_lib loading %s", libfile);
		}
		else if(nth_token_is(recvbuf, "get_subbuffer", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_subbuf");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_subbuf: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				CPRINTF("cannot find trace!");
				return 1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					struct rchan_buf *rbuf = rchan->buf;
					struct ltt_channel_buf_struct *lttbuf = trace->channels[i].buf;
					char *reply;
					long consumed_old=0;
					int fd;
					struct blocked_consumer *bc;

					bc = (struct blocked_consumer *) malloc(sizeof(struct blocked_consumer));
					if(bc == NULL) {
						ERR("malloc returned NULL");
						goto next_cmd;
					}
					bc->fd_consumer = src.fd;
					bc->fd_producer = lttbuf->data_ready_fd_read;
					bc->rbuf = rbuf;
					bc->lttbuf = lttbuf;
					bc->src = src;
					bc->server = ustcomm_app.server;

					list_add(&bc->list, &blocked_consumers);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "put_subbuffer", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;
			long consumed_old;
			char *consumed_old_str;
			char *endptr;

			DBG("put_subbuf");

			channel_name = strdup_malloc(nth_token(recvbuf, 1));
			if(channel_name == NULL) {
				ERR("put_subbuf_size: cannot parse channel");
				goto next_cmd;
			}

			consumed_old_str = strdup_malloc(nth_token(recvbuf, 2));
			if(consumed_old_str == NULL) {
				ERR("put_subbuf: cannot parse consumed_old");
				goto next_cmd;
			}
			consumed_old = strtol(consumed_old_str, &endptr, 10);
			if(*endptr != '\0') {
				ERR("put_subbuf: invalid value for consumed_old");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				CPRINTF("cannot find trace!");
				return 1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					struct rchan_buf *rbuf = rchan->buf;
					struct ltt_channel_buf_struct *lttbuf = trace->channels[i].buf;
					char *reply;
					long consumed_old=0;

					result = ltt_do_put_subbuf(rbuf, lttbuf, consumed_old);
					if(result < 0) {
						WARN("ltt_do_put_subbuf: error");
					}
					else {
						DBG("ltt_do_put_subbuf: success");
					}
					asprintf(&reply, "%s", "OK", consumed_old);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: put_subbuf: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}

			free(channel_name);
			free(consumed_old_str);
		}
//		else if(nth_token_is(recvbuf, "get_notifications", 0) == 1) {
//			struct ltt_trace_struct *trace;
//			char trace_name[] = "auto";
//			int i;
//			char *channel_name;
//
//			DBG("get_notifications");
//
//			channel_name = strdup_malloc(nth_token(recvbuf, 1));
//			if(channel_name == NULL) {
//				ERR("put_subbuf_size: cannot parse channel");
//				goto next_cmd;
//			}
//
//			ltt_lock_traces();
//			trace = _ltt_trace_find(trace_name);
//			ltt_unlock_traces();
//
//			if(trace == NULL) {
//				CPRINTF("cannot find trace!");
//				return 1;
//			}
//
//			for(i=0; i<trace->nr_channels; i++) {
//				struct rchan *rchan = trace->channels[i].trans_channel_data;
//				int fd;
//
//				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
//					struct rchan_buf *rbuf = rchan->buf;
//					struct ltt_channel_buf_struct *lttbuf = trace->channels[i].buf;
//
//					result = fd = ustcomm_app_detach_client(&ustcomm_app, &src);
//					if(result == -1) {
//						ERR("ustcomm_app_detach_client failed");
//						goto next_cmd;
//					}
//
//					lttbuf->wake_consumer_arg = (void *) fd;
//
//					smp_wmb();
//
//					lttbuf->call_wake_consumer = 1;
//
//					break;
//				}
//			}
//
//			free(channel_name);
//		}
		else {
			ERR("unable to parse message: %s", recvbuf);
		}

	next_cmd:
		free(recvbuf);
	}
}

static char listener_stack[16384];

void create_listener(void)
{
	int result;
	static char listener_stack[16384];
	//char *listener_stack = malloc(16384);

#ifdef USE_CLONE
	result = clone(listener_main, listener_stack+sizeof(listener_stack)-1, CLONE_FS | CLONE_FILES | CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, NULL);
	if(result == -1) {
		perror("clone");
	}
#else
	pthread_t thread;

	pthread_create(&thread, NULL, listener_main, NULL);
#endif
}

/* The signal handler itself. Signals must be setup so there cannot be
   nested signals. */

void sighandler(int sig)
{
	static char have_listener = 0;
	DBG("sighandler");

	if(!have_listener) {
		create_listener();
		have_listener = 1;
	}
}

/* Called by the app signal handler to chain it to us. */

void chain_signal(void)
{
	sighandler(USTSIGNAL);
}

static int init_socket(void)
{
	return ustcomm_init_app(getpid(), &ustcomm_app);
}

static void destroy_socket(void)
{
//	int result;
//
//	if(mysocketfile[0] == '\0')
//		return;
//
//	result = unlink(mysocketfile);
//	if(result == -1) {
//		PERROR("unlink");
//	}
}

static int init_signal_handler(void)
{
	/* Attempt to handler SIGIO. If the main program wants to
	 * handle it, fine, it'll override us. They it'll have to
	 * use the chaining function.
	 */

	int result;
	struct sigaction act;

	result = sigemptyset(&act.sa_mask);
	if(result == -1) {
		PERROR("sigemptyset");
		return -1;
	}

	act.sa_handler = sighandler;
	act.sa_flags = SA_RESTART;

	/* Only defer ourselves. Also, try to restart interrupted
	 * syscalls to disturb the traced program as little as possible.
	 */
	result = sigaction(SIGIO, &act, NULL);
	if(result == -1) {
		PERROR("sigaction");
		return -1;
	}

	return 0;
}

static void auto_probe_connect(struct marker *m)
{
	int result;

	result = ltt_marker_connect(m->channel, m->name, "default");
	if(result)
		ERR("ltt_marker_connect");

	DBG("just auto connected marker %s %s to probe default", m->channel, m->name);
}

static void __attribute__((constructor(101))) init0()
{
	DBG("UST_AUTOPROBE constructor");
	if(getenv("UST_AUTOPROBE")) {
		marker_set_new_marker_cb(auto_probe_connect);
	}
}

static void fini(void);

static void __attribute__((constructor(1000))) init()
{
	int result;

	DBG("UST_TRACE constructor");

	/* Must create socket before signal handler to prevent races.
         */
	result = init_socket();
	if(result == -1) {
		ERR("init_socket error");
		return;
	}
	result = init_signal_handler();
	if(result == -1) {
		ERR("init_signal_handler error");
		return;
	}

	if(getenv("UST_TRACE")) {
		char trace_name[] = "auto";
		char trace_type[] = "ustrelay";

		DBG("starting early tracing");

		/* Ensure marker control is initialized */
		init_marker_control();

		/* Ensure relay is initialized */
		init_ustrelay_transport();

		/* Ensure markers are initialized */
		init_markers();

		/* In case. */
		ltt_channels_register("ust");

		result = ltt_trace_setup(trace_name);
		if(result < 0) {
			ERR("ltt_trace_setup failed");
			return;
		}

		result = ltt_trace_set_type(trace_name, trace_type);
		if(result < 0) {
			ERR("ltt_trace_set_type failed");
			return;
		}

		result = ltt_trace_alloc(trace_name);
		if(result < 0) {
			ERR("ltt_trace_alloc failed");
			return;
		}

		result = ltt_trace_start(trace_name);
		if(result < 0) {
			ERR("ltt_trace_start failed");
			return;
		}
		//start_consumer();
		inform_consumer_daemon();
	}


	return;

	/* should decrementally destroy stuff if error */

}

/* This is only called if we terminate normally, not with an unhandled signal,
 * so we cannot rely on it. */

static void __attribute__((destructor)) fini()
{
	int result;

	/* if trace running, finish it */

	DBG("destructor stopping traces");

	result = ltt_trace_stop("auto");
	if(result == -1) {
		ERR("ltt_trace_stop error");
	}

	result = ltt_trace_destroy("auto");
	if(result == -1) {
		ERR("ltt_trace_destroy error");
	}

	/* FIXME: wait for the consumer to be done */
	//DBG("waiting 5 sec for consume");
	//sleep(5);

	destroy_socket();
}