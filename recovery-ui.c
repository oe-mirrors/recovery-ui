/*
 * Copyright (C) 2017 Dream Property GmbH, Germany
 *                    http://www.dream-multimedia-tv.de/
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include "lcd.h"

static bool hostname_is_blacklisted(const char *host)
{
	return !strcmp(host, "localhost");
}

static bool hostname_is_valid(const char *host)
{
	size_t len = strlen(host);

	if (len < 1 || len > 255)
		return false;

	while (*host) {
		char c = *host++;
		if (!(c >= '0' && c <= '9') &&
		    !(c >= 'a' && c <= 'z') &&
		    !(c >= 'A' && c <= 'Z') &&
		    !(c == '-') &&
		    !(c == '.'))
			return false;
	}

	return true;
}

static bool hostname_is_numeric(const char *host, int family)
{
	struct addrinfo *res = NULL;
	struct addrinfo hints;
	int status;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;

	status = getaddrinfo(host, "0", &hints, &res);
	freeaddrinfo(res);
	return status == 0;
}

static bool hostname_matches_numerichost(const char *host, const char *numerichost, int family)
{
	struct addrinfo *res = NULL;
	struct addrinfo hints;
	bool match = false;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = 0;

	if (getaddrinfo(host, "0", &hints, &res) == 0) {
		struct addrinfo *ai;
		for (ai = res; ai != NULL; ai = ai->ai_next) {
			char buf[NI_MAXHOST];
			if (getnameinfo(ai->ai_addr, ai->ai_addrlen, buf, sizeof(buf), NULL, 0, NI_NUMERICHOST) == 0) {
				match = !strcmp(buf, numerichost);
				if (match)
					break;
			}
		}

		freeaddrinfo(res);
	}

	return match;
}

static bool hostname_is_plausible(const char *host, const char *numerichost, int family)
{
	if (hostname_is_blacklisted(host))
		fprintf(stderr, "Hostname is blacklisted: '%s'\n", host);
	else if (!hostname_is_valid(host))
		fprintf(stderr, "Hostname is invalid: '%s'\n", host);
	else if (hostname_is_numeric(host, family))
		fprintf(stderr, "Hostname looks like a numeric address: '%s'\n", host);
	else if (!hostname_matches_numerichost(host, numerichost, family))
		fprintf(stderr, "Hostname doesn't resolve to my address: '%s'\n", host);
	else
		return true;

	return false;
}

static int read_ifaddr_by_family(int family, char *host, unsigned int hostlen)
{
	struct ifaddrs *ifaddr, *ifa;
	char numerichost[NI_MAXHOST];
	int ret = AF_UNSPEC;
	int status;

	if (getifaddrs(&ifaddr) < 0) {
		perror("getifaddrs");
		return ret;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
		if (!(ifa->ifa_flags & IFF_RUNNING))
			continue;
		if (ifa->ifa_addr->sa_family == AF_UNSPEC ||
		    ifa->ifa_addr->sa_family == AF_PACKET)
			continue;
		if (family != AF_UNSPEC && ifa->ifa_addr->sa_family != family)
			continue;

		status = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_storage), numerichost, sizeof(numerichost), NULL, 0, NI_NUMERICHOST);
		if (status != 0) {
			fprintf(stderr, "getnameinfo: %s (family=%d)\n", gai_strerror(status), ifa->ifa_addr->sa_family);
			continue;
		}

		status = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_storage), host, hostlen, NULL, 0, NI_NAMEREQD);
		if (status != 0 || !hostname_is_plausible(host, numerichost, ifa->ifa_addr->sa_family)) {
			strncpy(host, numerichost, hostlen);
			host[hostlen - 1] = '\0';
		}

		if (ifa->ifa_addr->sa_family == AF_INET6) {
			struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ifa->ifa_addr;
			if (in6->sin6_scope_id != 0)
				continue;
		}
		ret = ifa->ifa_addr->sa_family;
		break;
	}

	freeifaddrs(ifaddr);
	return ret;
}

static int read_ifaddr(char *host, unsigned int hostlen)
{
	int family;

	family = read_ifaddr_by_family(AF_INET, host, hostlen);
	if (family == AF_UNSPEC)
		family = read_ifaddr_by_family(AF_INET6, host, hostlen);
	if (family == AF_UNSPEC)
		family = read_ifaddr_by_family(AF_UNSPEC, host, hostlen);

	return family;
}

static bool timer_set(int fd, unsigned int ms)
{
	struct itimerspec it = {
		.it_value = {
			.tv_sec = ms / 1000,
			.tv_nsec = (ms % 1000) * 1000000,
		},
	};

	if (timerfd_settime(fd, 0, &it, NULL) < 0) {
		perror("timerfd_settime");
		return false;
	}

	return true;
}

static int timer_add(unsigned int ms)
{
	int fd;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0) {
		perror("timerfd_create");
		return -1;
	}

	if (!timer_set(fd, ms)) {
		close(fd);
		return -1;
	}

	return fd;
}

static void __epoll_ctl(int epollfd, int op, int fd, void *ptr)
{
	struct epoll_event ev;

	ev.events = EPOLLIN;
	if (ptr == NULL)
		ev.data.fd = fd;
	else
		ev.data.ptr = ptr;

	if (epoll_ctl(epollfd, op, fd, &ev) == -1)
		perror("epoll_ctl");
}

static inline void epoll_add(int epollfd, int fd, void *ptr)
{
	__epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, ptr);
}

static inline void epoll_del(int epollfd, int fd)
{
	__epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
}

struct display_state {
	struct lcd *display;
	size_t margin_top;
	size_t margin_bottom;
	size_t margin_left;
	size_t margin_right;
	size_t display_width;
	size_t display_height;
	size_t font_width;
	size_t font_height;
	size_t max_chars;
	int count;
	int incr;
	int timerfd;
};

static bool state_init(struct display_state *st, enum display_type type)
{
	static const char rescue_mode[] = "RESCUE MODE";
	unsigned int logo_width, logo_height;
	struct lcd *display;
	int fd;

	memset(st, 0, sizeof(struct display_state));

	display = display_open(type);
	if (display == NULL)
		return false;

	fd = timer_add(0);
	if (fd < 0) {
		lcd_release(display);
		return false;
	}

	st->timerfd = fd;
	st->count = 0;
	st->incr = -1;

	st->display = display;
	st->display_width = lcd_width(display);
	st->display_height = lcd_height(display);
	st->font_width = lcd_font_width(display);
	st->font_height = lcd_font_height(display);
	st->max_chars = st->display_width / st->font_width;

	if (type == DISPLAY_TYPE_OLED) {
		st->margin_top = 16;
		st->margin_bottom = 0;
		st->margin_left = 0;
		st->margin_right = 0;
	} else if (type == DISPLAY_TYPE_HDMI) {
		st->margin_top = st->display_height * 7 / 100;
		st->margin_bottom = st->margin_top;
		st->margin_left = st->display_width * 7 / 100;
		st->margin_right = st->margin_left;
	}

	lcd_clear(display, st->display_height);

	lcd_get_logo_size(display, &logo_width, &logo_height);
	if (logo_width == st->display_width)
		st->margin_left = st->margin_right = 0;
	if (logo_height == st->display_height)
		st->margin_top = st->margin_bottom = 0;

	lcd_set_x(display, (st->display_width - logo_width) / 2);
	lcd_set_y(display, st->margin_top);

	lcd_write_logo(display);
	lcd_save_background(display);
	lcd_set_fgcolor(display, 0xffffd200);

	lcd_set_x(display, (st->display_width - strlen(rescue_mode) * st->font_width) / 2);
	lcd_set_y(display, st->display_height - st->font_height * 4 - st->margin_bottom);

	lcd_puts(display, rescue_mode);

	lcd_update(display);

	lcd_set_y(display, st->display_height - st->font_height * 2 - st->margin_bottom);

	return true;
}

static void state_print_msg(struct display_state *st, const char *msg)
{
	struct lcd *display = st->display;
	size_t len;

	if (display == NULL)
		return;

	lcd_clear(display, st->font_height);

	len = strlen(msg);
	if (len > st->max_chars) {
		lcd_set_x(display, 0);
		lcd_printf(display, "%.*s...", st->max_chars - 3, msg);
	} else {
		lcd_set_x(display, (st->display_width - len * st->font_width) / 2);
		lcd_printf(display, "%s", msg);
	}

	lcd_update(display);
}

static void state_print_wait_msg(struct display_state *st, unsigned int n)
{
	static const char wait_msg[] = "Waiting for DHCP";
	static const char progress[] = "-\\|/";
	struct lcd *display = st->display;

	if (display == NULL)
		return;

	timer_set(st->timerfd, 0);

	lcd_clear(display, st->font_height);

	lcd_set_x(display, (st->display_width - (strlen(wait_msg) + 2) * st->font_width) / 2);
	lcd_printf(display, "%s %c", wait_msg, progress[n % 4]);

	lcd_update(display);
}

static void state_print_url(struct display_state *st, int family, const char *host)
{
	struct lcd *display = st->display;
	size_t hostlen;
	int extra_pixels;

	if (display == NULL)
		return;

	hostlen = strlen(host) + 8;
	if (family == AF_INET6)
		hostlen += 2;

	if (hostlen <= st->max_chars) {
		timer_set(st->timerfd, 0);

		lcd_set_x(display, (st->display_width - hostlen * st->font_width) / 2);
	} else {
		timer_set(st->timerfd, 100);

		extra_pixels = (hostlen * st->font_width) - st->display_width;
		// length might have changed, reset
		if (st->count < 0 || st->count > extra_pixels) {
			st->count = 0;
			st->incr = -1;
		}

		lcd_set_x(display, -st->count);

		// change scroll direction
		if (st->count == 0 || st->count == extra_pixels)
			st->incr = -st->incr;

		st->count += st->incr;
	}

	lcd_clear(display, st->font_height);

	if (family == AF_INET6)
		lcd_printf(display, "http://[%s]/", host);
	else
		lcd_printf(display, "http://%s/", host);

	lcd_update(display);
}

static void state_exit(struct display_state *st)
{
	struct lcd *display = st->display;

	if (display == NULL)
		return;

	lcd_release(display);
}

static int mnl_event(const struct nlmsghdr *nlh, void *data)
{
	struct rtmsg *rtm = mnl_nlmsg_get_payload(nlh);
	bool *update = data;

	switch (nlh->nlmsg_type) {
	case RTM_NEWROUTE:
	case RTM_DELROUTE:
		break;
	default:
		return MNL_CB_OK;
	}

	switch (rtm->rtm_family) {
	case AF_INET:
		if (rtm->rtm_dst_len == 32)
			return MNL_CB_OK;
		break;
	case AF_INET6:
		if (rtm->rtm_dst_len == 128)
			return MNL_CB_OK;
		break;
	default:
		return MNL_CB_OK;
	}

	if (rtm->rtm_table != RT_TABLE_MAIN ||
	    rtm->rtm_scope >= RT_SCOPE_HOST ||
	    rtm->rtm_type != RTN_UNICAST)
		return MNL_CB_OK;

	*update = true;
	return MNL_CB_OK;
}

#define MAX_EVENTS (DISPLAY_TYPE_MAX + 1)

int main(void)
{
	const char fifo[] = "/run/recovery-ui.fifo";
	char host[NI_MAXHOST];
	struct display_state state[DISPLAY_TYPE_MAX];
	int family = AF_UNSPEC;
	unsigned int n = 0;
	enum display_type type;
	struct stat st;
	int epollfd;
	int timerfd;
	int fifofd;
	char linebuf[1024];
	size_t pos = 0;
	bool update = true;
	struct mnl_socket *nl;
	int mnlfd;

	epollfd = epoll_create1(EPOLL_CLOEXEC);
	if (epollfd < 0) {
		perror("epoll_create1");
		return 1;
	}

	timerfd = timer_add(0);
	if (timerfd < 0)
		return 1;

	epoll_add(epollfd, timerfd, NULL);

	nl = mnl_socket_open2(NETLINK_ROUTE, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (nl == NULL) {
		perror("mnl_socket_open");
		return 1;
	}

	if (mnl_socket_bind(nl, RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE, MNL_SOCKET_AUTOPID) < 0) {
		perror("mnl_socket_bind");
		return 1;
	}

	mnlfd = mnl_socket_get_fd(nl);
	epoll_add(epollfd, mnlfd, NULL);

	if (stat(fifo, &st) < 0 || !S_ISFIFO(st.st_mode)) {
		unlink(fifo);
		if (mkfifo(fifo, 0600) < 0)
			perror("mkfifo");
	}

	fifofd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fifofd < 0)
		perror("fifo");
	else
		epoll_add(epollfd, fifofd, NULL);

	for (type = DISPLAY_TYPE_MIN; type < DISPLAY_TYPE_MAX; type++)
		if (state_init(&state[type], type))
			epoll_add(epollfd, state[type].timerfd, &state[type]);

	for (;;) {
		struct epoll_event events[MAX_EVENTS];
		int i, nfds;

		if (update) {
			family = read_ifaddr(host, sizeof(host));
			for (type = DISPLAY_TYPE_MIN; type < DISPLAY_TYPE_MAX; type++) {
				if (family == AF_UNSPEC)
					state_print_wait_msg(&state[type], n);
				else
					state_print_url(&state[type], family, host);
			}
			n++;

			if (family == AF_UNSPEC)
				timer_set(timerfd, 1000);
			else
				timer_set(timerfd, 0);

			update = false;
		}

		nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			perror("epoll_wait");
			return 1;
		}

		for (i = 0; i < nfds; i++) {
			if (events[i].data.fd == timerfd) {
				update = true;
			} else if (events[i].data.fd == mnlfd) {
				char mnlbuf[MNL_SOCKET_BUFFER_SIZE];
				ssize_t ret;
				do {
					ret = mnl_socket_recvfrom(nl, mnlbuf, sizeof(mnlbuf));
					if (ret <= 0)
						break;
					ret = mnl_cb_run(mnlbuf, ret, 0, 0, mnl_event, &update);
				} while (ret > MNL_CB_STOP);
			} else if (events[i].data.fd == fifofd) {
				char *s;
				for (;;) {
					ssize_t ret = read(fifofd, &linebuf[pos], sizeof(linebuf) - pos - 1);
					if (ret < 0) {
						if (errno != EAGAIN)
							perror("read");
						break;
					}
					if (ret == 0) {
						epoll_del(epollfd, fifofd);
						close(fifofd);
						pos = 0;

						fifofd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
						if (fifofd < 0)
							perror("fifo");
						else
							epoll_add(epollfd, fifofd, NULL);
						break;
					}
					pos += ret;
					if (pos + 1 == sizeof(linebuf) && linebuf[pos] != '\n') {
						strcpy(linebuf, "...");
						pos = 3;
						break;
					}
					linebuf[pos] = '\0';
					s = linebuf;
					for (;;) {
						char *p = strsep(&s, "\n");
						if (s == NULL) {
							pos = strlen(p);
							if (pos > 0 && p != linebuf)
								memmove(linebuf, p, pos + 1);
							break;
						}
						for (type = DISPLAY_TYPE_MIN; type < DISPLAY_TYPE_MAX; type++)
							state_print_msg(&state[type], p);
					}
				}
				timer_set(timerfd, 30000);
			} else {
				struct display_state *st = events[i].data.ptr;
				state_print_url(st, family, host);
			}
		}
	}

	for (type = DISPLAY_TYPE_MIN; type < DISPLAY_TYPE_MAX; type++)
		state_exit(&state[type]);

	return 0;
}
