#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

#include <event.h>

#include "ikcp.h"
#include "debug.h"
#include "xkcp_util.h"

#define	BUF_RECV_LEN	1500

static struct timeval TIMER_TV = {0, 10};

IQUEUE_HEAD(xkcp_task_list);

char response[] = {"hello world"};

static int
xkcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	struct xkcp_proxy_param *ptr = user;
	debug(LOG_DEBUG, "xkcp output [%d]", len);
	return sendto(ptr->udp_fd, buf, len, 0, &ptr->serveraddr, sizeof(ptr->serveraddr));
}

static void timer_event_cb(int nothing, short int which, void *ev)
{
	struct event *timeout = ev;
	struct xkcp_task *task;
	iqueue_head *task_list = &xkcp_task_list;
	iqueue_foreach(task, task_list, xkcp_task_type, head) {
		if (task->kcp) {
			ikcp_update(task->kcp, iclock());
		}
	}
	
	evtimer_add(ev, &TIMER_TV);
}


static void udp_cb(const int sock, short int which, void *arg)
{
	for(;;) {
		struct sockaddr_in server_sin;
		socklen_t server_sz = sizeof(server_sin);
		char buf[BUF_RECV_LEN] = {0};

		/* Recv the data, store the address of the sender in server_sin */
		int len = recvfrom(sock, &buf, sizeof(buf) - 1, 0, (struct sockaddr *) &server_sin, &server_sz);
		if ( len == -1) {
			perror("recvfrom()");
			event_loopbreak();
		} else if (len > 0) {
			int conv = ikcp_getconv(buf);
			ikcpcb *kcp_client = get_kcp_from_conv(int conv, &xkcp_task_list);
			if (kcp_client == NULL) {
				struct xkcp_proxy_param *param = malloc(sizeof(struct xkcp_proxy_param));
				memset(param, 0, sizeof(struct xkcp_proxy_param));
				memcpy(param->serveraddr, &server_in, server_sz);
				kcp_client = ikcp_create(conv, param);
				kcp_client->output	= xkcp_output;
				ikcp_wndsize(kcp_client, 128, 128);
				ikcp_nodelay(kcp_client, 0, 10, 0, 1);
			}
			
			ikcp_input(kcp_client, response, sizeof(response));
					
			struct xkcp_task *task = malloc(sizeof(struct xkcp_task));
			assert(task);
			task->kcp = kcp_client;
			task->svr_addr = &server_sin;
			add_task_tail(task, &xkcp_task_list);
		} else
			break;
	}
}

int main(int argc, char **argv)
{
	int ret, port, sock, fd[2];

	struct event timer_event, udp_event;
	struct sockaddr_in sin;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
	sin.sin_port = htons(9088);
	
	if (bind(sock, (struct sockaddr *) &sin, sizeof(sin))) {
		perror("bind()");
		exit(EXIT_FAILURE);
	}

	/* Initialize libevent */
	event_init();

	/* Add the clock event */
	evtimer_set(&timer_event, &timer_event_cb, &timer_event);
	evtimer_add(&timer_event, &TIMER_TV);
	
	/* Add the UDP event */
	event_set(&udp_event, sock, EV_READ|EV_PERSIST, udp_cb, NULL);
	event_add(&udp_event, 0);

	/* Enter the event loop; does not return. */
	event_dispatch();
	close(sock);
	return 0;
}