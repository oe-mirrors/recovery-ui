/*
 * Copyright (C) 2016 Dream Property GmbH, Germany
 *                    http://www.dream-multimedia-tv.de/
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lcd.h"
#include "lcdfont.h"
#include "lcdlogo_128x8_gray4.h"
#include "lcdlogo_96x7_mono.h"

#define ARRAY_SIZE(x)	(sizeof((x)) / sizeof(*(x)))

struct lcd {
	enum display_type type;
	union {
		int fd;
	};
	unsigned int width;
	unsigned int height;
	unsigned int bpp;
	unsigned int stride;
	unsigned int size;
	int x;
	int y;
	unsigned char *data;
	unsigned char *background;
};

static unsigned long ulong_from_file(const char *filename, unsigned long dflt)
{
	unsigned long value;
	char *data = 0;
	char *end = 0;
	size_t n = 0;
	ssize_t ret;
	FILE *f;

	f = fopen(filename, "r");
	if (f == NULL)
		return dflt;

	ret = getline(&data, &n, f);
	if (ret < 0) {
		perror("getline");
		abort();
	}

	fclose(f);

	errno = 0;
	value = strtoul(data, &end, 16);
	if (errno != 0) {
		perror("strtoul");
		abort();
	}

	if (data == end) {
		fprintf(stderr, "strtoul failed");
		abort();
	}

	free(data);
	return value;
}

off_t lcd_seek(struct lcd *lcd, off_t offset, int whence)
{
	off_t pixels = offset * 8 / lcd->bpp;

	switch (whence) {
	case SEEK_SET:
		lcd->x = 0;
		lcd->y = 0;
		break;
	case SEEK_CUR:
		break;
	case SEEK_END:
		lcd->x = 0;
		lcd->y = lcd->height;
		break;
	}

	pixels += lcd->y * lcd->width + lcd->x;
	lcd->x = pixels % lcd->width;
	lcd->y = pixels / lcd->width;

	return lcd->stride * lcd->y + lcd->x * lcd->bpp / 8;
}

bool lcd_update(struct lcd *lcd)
{
	ssize_t ret;

	if (lcd->type == DISPLAY_TYPE_OLED) {
		ret = write(lcd->fd, lcd->data, lcd->size);
		assert(ret == (ssize_t)lcd->size);
		if (ret < 0) {
			fprintf(stderr, "lcd: write error: %m\n");
			return false;
		}

		if ((size_t)ret != lcd->size) {
			fprintf(stderr, "lcd: short write\n");
			return false;
		}
	}

	return true;
}

unsigned int lcd_width(struct lcd *lcd)
{
	return lcd->width;
}

unsigned int lcd_height(struct lcd *lcd)
{
	return lcd->height;
}

static inline unsigned int lcd_scale_factor(struct lcd *lcd)
{
	return (lcd->bpp == 16 && lcd->width >= 400) ? 2 : 1;
}

unsigned int lcd_font_width(struct lcd *lcd)
{
	(void)lcd;
	return 6 * lcd_scale_factor(lcd);
}

unsigned int lcd_font_height(struct lcd *lcd)
{
	(void)lcd;
	return 8 * lcd_scale_factor(lcd);
}

static void lcd_putc_4bpp(struct lcd *lcd, char c)
{
	unsigned int row, column, data_index, font_index;
	unsigned int font_width = lcd_font_width(lcd);
	unsigned int font_height = lcd_font_height(lcd);
	unsigned int mask;

	mask = (lcd->x & 1) ? 0x0f : 0xf0;
	font_index = (unsigned char)c * font_width;
	for (column = 0; column < font_width; column++) {
		if (lcd->x >= 0 && (size_t)lcd->x < lcd->width) {
			data_index = lcd->y * lcd->stride + lcd->x * lcd->bpp / 8;
			for (row = 0; row < font_height; row++) {
				if (lcdfont[font_index] & (1 << row))
					lcd->data[data_index] |= mask;
				else
					lcd->data[data_index] &= ~mask;
				data_index += lcd->stride;
			}
		}
		mask ^= 0xff;
		font_index++;
		lcd->x++;
	}
}

static void lcd_putc_16bpp(struct lcd *lcd, char c)
{
	unsigned int row, column, data_index, font_index;
	unsigned int font_width = lcd_font_width(lcd);
	unsigned int font_height = lcd_font_height(lcd);
	unsigned int value = 0xffff;
	unsigned int scale_factor = lcd_scale_factor(lcd);
	const unsigned char *pixel;
	const unsigned char foreground[2] = {
		(value >> 0) & 0xff,
		(value >> 8) & 0xff,
	};

	font_index = (unsigned char)c * font_width;
	for (column = 0; column < font_width; column++) {
		if (lcd->x >= 0 && (size_t)lcd->x < lcd->width) {
			data_index = lcd->y * lcd->stride + lcd->x * lcd->bpp / 8;
			for (row = 0; row < font_height; row++) {
				if (lcdfont[font_index / scale_factor] & (1 << (row / scale_factor)))
					pixel = foreground;
				else
					pixel = &lcd->background[data_index];
				memcpy(&lcd->data[data_index], pixel, 2);
				data_index += lcd->stride;
			}
		}
		font_index++;
		lcd->x++;
	}
}

void lcd_putc(struct lcd *lcd, char c)
{
	if (lcd->bpp == 4)
		lcd_putc_4bpp(lcd, c);
	else if (lcd->bpp == 16)
		lcd_putc_16bpp(lcd, c);
	else
		abort();
}

void lcd_puts(struct lcd *lcd, const char *str)
{
	while (*str)
		lcd_putc(lcd, *str++);
}

int lcd_printf(struct lcd *lcd, const char *fmt, ...)
{
	char *str = NULL;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&str, fmt, ap);
	va_end(ap);

	if (ret >= 0) {
		lcd_puts(lcd, str);
		free(str);
	}

	return ret;
}

struct lcd *hdmi_open(void)
{
	struct lcd *lcd = NULL;

	return lcd;
}

struct lcd *lcd_open(void)
{
	const char device[] = "/dev/dbox/oled0";
	const unsigned int width = ulong_from_file("/proc/stb/lcd/xres", 128);
	const unsigned int height = ulong_from_file("/proc/stb/lcd/yres", 64);
	const unsigned int bpp = ulong_from_file("/proc/stb/lcd/bpp", 4);
	const unsigned int stride = width * bpp / 8;
	const unsigned int size = stride * height;
	struct lcd *lcd;
	int fd;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "lcd: can't open %s: %m\n", device);
		return NULL;
	}

	lcd = malloc(sizeof(struct lcd) + size * 2);
	if (lcd == NULL) {
		close(fd);
		return NULL;
	}

	memset(lcd, 0, sizeof(struct lcd));
	lcd->fd = fd;
	lcd->width = width;
	lcd->height = height;
	lcd->bpp = bpp;
	lcd->stride = stride;
	lcd->size = size;
	lcd->data = (unsigned char *)&lcd[1];
	lcd->background = &lcd->data[size];
	memset(lcd->background, 0, size);

	return lcd;
}

struct lcd *display_open(enum display_type type)
{
	if (type == DISPLAY_TYPE_OLED)
		return lcd_open();
	if (type == DISPLAY_TYPE_HDMI)
		return hdmi_open();

	return NULL;
}

void lcd_release(struct lcd *lcd)
{
	if (lcd->type == DISPLAY_TYPE_OLED)
		close(lcd->fd);
	free(lcd);
}

void lcd_set_x(struct lcd *lcd, int x)
{
	lcd->x = x;
}

void lcd_set_y(struct lcd *lcd, int y)
{
	lcd->y = y;
}

void lcd_clear(struct lcd *lcd, unsigned int height)
{
	int y = lcd->y;

	if (y < 0) {
		height += y;
		y = 0;
	}
	if (y + height > lcd->height)
		height = lcd->height - y;

	if ((int)height > 0)
		memcpy(&lcd->data[lcd->stride * y], &lcd->background[lcd->stride * y], lcd->stride * height);
}

static ssize_t lcd_write(struct lcd *lcd, const void *buf, size_t count)
{
	off_t offset;

	offset = lcd_seek(lcd, 0, SEEK_CUR);
	if (offset + count > lcd->size)
		count = lcd->size - offset;

	if ((ssize_t)count >= 0) {
		memcpy(&lcd->data[offset], buf, count);
		return count;
	}

	return -1;
}

void lcd_save_background(struct lcd *lcd)
{
	memcpy(lcd->background, lcd->data, lcd->size);
}

void lcd_write_logo(struct lcd *lcd)
{
	if (lcd->width == 128 && lcd->bpp == 4)
		lcd_write(lcd, lcdlogo_128x8_gray4, sizeof(lcdlogo_128x8_gray4));
	else if (lcd->bpp == 16) {
		unsigned int scale_factor = lcd_scale_factor(lcd);
		unsigned char logo[sizeof(lcdlogo_96x7_mono) * 16 * scale_factor];
		unsigned short *wptr = (unsigned short *)logo;
		unsigned int i, j, k, pixel;
		for (i = 0; i < sizeof(lcdlogo_96x7_mono); i++) {
			for (j = 0; j < 8; j++) {
				if (lcdlogo_96x7_mono[i] & (1 << (7 - j)))
					pixel = 0xffff;
				else
					pixel = 0;
				for (k = 0; k < scale_factor; k++)
					*wptr++ = pixel;
			}
		}
		for (i = 0; i < 7; i++) {
			for (j = 0; j < scale_factor; j++) {
				lcd_write(lcd, &logo[i * 96 * 2 * scale_factor], 96 * 2 * scale_factor);
				lcd_seek(lcd, lcd->stride, SEEK_CUR);
			}
		}
	} else
		abort();
}

void lcd_get_logo_size(struct lcd *lcd, unsigned int *width, unsigned int *height)
{
	if (lcd->bpp == 4) {
		*width = 128;
		*height = 8;
	} else if (lcd->bpp == 16) {
		unsigned int scale_factor = lcd_scale_factor(lcd);
		*width = 96 * scale_factor;
		*height = 7 * scale_factor;
	} else
		abort();
}
