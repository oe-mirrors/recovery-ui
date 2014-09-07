/*
 * Copyright (C) 2014 Dream Property GmbH, Germany
 *                    http://www.dream-multimedia-tv.de/
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lcd.h"
#include "lcdfont.h"

struct lcd {
	int fd;
	unsigned int width;
	unsigned int height;
	unsigned int bpp;
	unsigned int stride;
	unsigned int size;
	int x;
	int y;
	unsigned char *data;
};

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

unsigned int lcd_font_width(struct lcd *lcd)
{
	(void)lcd;
	return 6;
}

unsigned int lcd_font_height(struct lcd *lcd)
{
	(void)lcd;
	return 8;
}

void lcd_putc(struct lcd *lcd, char c)
{
	unsigned int row, column, data_index, font_index, mask;
	unsigned int font_width = lcd_font_width(lcd);
	unsigned int font_height = lcd_font_height(lcd);

	assert(lcd->bpp == 4);

	mask = (lcd->x & 1) ? 0x0f : 0xf0;
	font_index = (unsigned char)c * font_width;
	for (column = 0; column < font_width; column++) {
		if (lcd->x >= 0 && (size_t)lcd->x < lcd->width) {
			data_index = lcd->y * lcd->stride + lcd->x / 2;
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

struct lcd *lcd_open(void)
{
	const char device[] = "/dev/dbox/oled0";
	const unsigned int width = 128;
	const unsigned int height = 64;
	const unsigned int bpp = 4;
	const unsigned int stride = width * bpp / 8;
	const unsigned int size = stride * height;
	struct lcd *lcd;
	int fd;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "lcd: can't open %s: %m\n", device);
		return NULL;
	}

	lcd = malloc(sizeof(struct lcd) + size);
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

	return lcd;
}

void lcd_release(struct lcd *lcd)
{
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
		memset(&lcd->data[lcd->stride * y], 0, lcd->stride * height);
}

ssize_t lcd_write(struct lcd *lcd, const void *buf, size_t count)
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
