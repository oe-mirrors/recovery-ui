/*
 * Copyright (C) 2014 Dream Property GmbH, Germany
 *                    http://www.dream-multimedia-tv.de/
 */

#define _GNU_SOURCE
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "lcd.h"

static bool hostname_is_blacklisted(const char *host)
{
	return !strcmp(host, "localhost");
}

static int read_ifaddr_by_family(int family, char *host, unsigned int hostlen)
{
	struct ifaddrs *ifaddr, *ifa;
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
		status = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_storage), host, hostlen, NULL, 0, 0);
		if (status == EAI_AGAIN || (status == 0 && hostname_is_blacklisted(host)))
			status = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_storage), host, hostlen, NULL, 0, NI_NUMERICHOST);
		if (status != 0) {
			fprintf(stderr, "getnameinfo: %s (family=%d)\n", gai_strerror(status), ifa->ifa_addr->sa_family);
			continue;
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

int main(void)
{
	const char rescue_mode[] = "RESCUE MODE";
	const char wait_msg[] = "Waiting for DHCP";
	const char progress[] = "-\\|/";
	char host[NI_MAXHOST];
	size_t hostlen;
	struct lcd *lcd;
	size_t display_width, display_height;
	size_t font_width, font_height;
	size_t max_chars;
	int family = AF_UNSPEC;
	unsigned int n;
	bool force_redraw = true;
	unsigned int update_interval = 30;

	lcd = lcd_open();
	if (lcd == NULL)
		return 1;

	display_width = lcd_width(lcd);
	display_height = lcd_height(lcd);
	font_width = lcd_font_width(lcd);
	font_height = lcd_font_height(lcd);
	max_chars = display_width / font_width;

	lcd_clear(lcd, display_height);
	lcd_set_y(lcd, 16);
	lcd_write_logo(lcd);
	lcd_set_x(lcd, (display_width - strlen(rescue_mode) * font_width) / 2);
	lcd_set_y(lcd, display_height - font_height * 4);
	lcd_puts(lcd, rescue_mode);
	lcd_update(lcd);

	lcd_set_y(lcd, display_height - font_height * 2);
	for (n = 0; ; n++) {
		// If we have an address, update every 30 seconds,
		// otherwise retry every second.
		if (family == AF_UNSPEC || (n % update_interval) == 0) {
			family = read_ifaddr(host, sizeof(host));
			if (family == AF_UNSPEC) {
				lcd_set_x(lcd, (display_width - (strlen(wait_msg) + 2) * font_width) / 2);
				lcd_clear(lcd, font_height);
				lcd_printf(lcd, "%s %c", wait_msg, progress[n % 4]);
				lcd_update(lcd);
				update_interval = 30;
				sleep(1);
				continue;
			}

			hostlen = strlen(host) + 8;
			if (family == AF_INET6)
				hostlen += 2;

			force_redraw = true;
		}

		if (hostlen <= max_chars) {
			if (!force_redraw) {
				update_interval = 30;
				sleep(1);
				continue;
			}
			force_redraw = false;
			lcd_set_x(lcd, (display_width - hostlen * font_width) / 2);
			lcd_clear(lcd, font_height);
		} else {
			static int count = 0;
			static int incr = -1;
			int extra_pixels;

			extra_pixels = (hostlen * font_width) - display_width;
			// length might have changed, reset
			if (count < 0 || count > extra_pixels) {
				count = 0;
				incr = -1;
			}

			lcd_set_x(lcd, -count);

			// change scroll direction
			if (count == 0 || count == extra_pixels)
				incr = -incr;

			count += incr;
		}

		if (family == AF_INET6)
			lcd_printf(lcd, "http://[%s]/", host);
		else
			lcd_printf(lcd, "http://%s/", host);

		lcd_update(lcd);

		update_interval = 300;
		usleep(100 * 1000);
	}

	lcd_release(lcd);
	return 0;
}
