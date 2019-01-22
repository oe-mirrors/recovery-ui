/*
 * Copyright (C) 2017 Dream Property GmbH, Germany
 *                    https://dreambox.de/
 */

#define _GNU_SOURCE
#include <assert.h>
#include <byteswap.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>
#include "lcd.h"
#include "lcdfont.h"
#include "lcdlogo_128x8_gray4.h"
#include "lcdlogo_400x240_rgb565_xz.h"
#include "lcdlogo_96x7_mono.h"
#include "unxz.h"

#define ARRAY_SIZE(x)	(sizeof((x)) / sizeof(*(x)))

struct color {
	unsigned int offset;
	unsigned int size;
};

struct lcd {
	enum display_type type;
	int fd;
	unsigned int width;
	unsigned int height;
	unsigned int bpp;
	unsigned int stride;
	unsigned int size;
	int x;
	int y;
	union {
		void *data;
		uint8_t *data8;
		uint16_t *data16;
		uint32_t *data32;
	};
	union {
		void *background;
		uint8_t *background8;
		uint16_t *background16;
		uint32_t *background32;
	};
	unsigned int fgcolor;
	const unsigned char *logo;
	size_t logo_size;
	struct color red;
	struct color green;
	struct color blue;
	struct color alpha;
	bool byteswap;
};

static unsigned char lcdlogo_400x240_rgb565[192000];
static bool lcdlogo_400x240_rgb565_decompressed;

static char *string_from_file(const char *filename)
{
	char *data = NULL;
	FILE *f;

	f = fopen(filename, "r");
	if (f != NULL) {
		size_t n = 0;
		if (getline(&data, &n, f) < 0)
			perror("getline");
		fclose(f);

		if (data != NULL) {
			n = strlen(data);
			while (n > 0 && isspace(data[--n]))
				data[n] = '\0';
		}
	}

	return data;
}

static unsigned long ulong_from_file(const char *filename, unsigned long dflt)
{
	unsigned long value;
	char *end = NULL;
	char *data;

	data = string_from_file(filename);
	if (data == NULL)
		return dflt;

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
	return 1 + (lcd->height + 120) / 240;
}

unsigned int lcd_font_width(struct lcd *lcd)
{
	return 6 * lcd_scale_factor(lcd);
}

unsigned int lcd_font_height(struct lcd *lcd)
{
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
					lcd->data8[data_index] |= mask;
				else
					lcd->data8[data_index] &= ~mask;
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
	unsigned int scale_factor = lcd_scale_factor(lcd);

	font_index = (unsigned char)c * font_width;
	for (column = 0; column < font_width; column++) {
		if (lcd->x >= 0 && (size_t)lcd->x < lcd->width) {
			data_index = lcd->y * lcd->stride + lcd->x * lcd->bpp / 8;
			for (row = 0; row < font_height; row++) {
				if (lcdfont[font_index / scale_factor] & (1 << (row / scale_factor)))
					lcd->data16[data_index / 2] = lcd->fgcolor;
				else
					lcd->data16[data_index / 2] = lcd->background16[data_index / 2];
				data_index += lcd->stride;
			}
		}
		font_index++;
		lcd->x++;
	}
}

static void lcd_putc_32bpp(struct lcd *lcd, char c)
{
	unsigned int row, column, data_index, font_index;
	unsigned int font_width = lcd_font_width(lcd);
	unsigned int font_height = lcd_font_height(lcd);
	unsigned int scale_factor = lcd_scale_factor(lcd);

	font_index = (unsigned char)c * font_width;
	for (column = 0; column < font_width; column++) {
		if (lcd->x >= 0 && (size_t)lcd->x < lcd->width) {
			data_index = lcd->y * lcd->stride + lcd->x * lcd->bpp / 8;
			for (row = 0; row < font_height; row++) {
				if (lcdfont[font_index / scale_factor] & (1 << (row / scale_factor)))
					lcd->data32[data_index / 4] = lcd->fgcolor;
				else
					lcd->data32[data_index / 4] = lcd->background32[data_index / 4];
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
	else if (lcd->bpp == 32)
		lcd_putc_32bpp(lcd, c);
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
	const char device[] = "/dev/fb0";
	const char tty[] = "/dev/tty0";
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	size_t size;
	void *buffer;
	int fd;

	fd = open(tty, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "lcd: can't open %s: %m\n", tty);
		return NULL;
	}

	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0) {
		perror("KDSETMODE");
		close(fd);
		return NULL;
	}

	close(fd);

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "lcd: can't open %s: %m\n", device);
		return NULL;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("FBIOGET_VSCREENINFO");
		close(fd);
		return NULL;
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_FSCREENINFO");
		close(fd);
		return NULL;
	}

	if ((ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) && errno != EINVAL) {
		perror("FBIOBLANK");
		close(fd);
		return NULL;
	}

	size = fix.line_length * var.yres;

	buffer = mmap(NULL, fix.line_length * var.yres_virtual, PROT_WRITE, MAP_SHARED, fd, 0);
	if (buffer == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return NULL;
	}

	lcd = malloc(sizeof(struct lcd) + size);
	if (lcd == NULL) {
		munmap(buffer, size);
		close(fd);
		return NULL;
	}

	memset(lcd, 0, sizeof(struct lcd) + size);
	lcd->fd = fd;
	lcd->width = var.xres;
	lcd->height = var.yres;
	lcd->bpp = var.bits_per_pixel;
	lcd->stride = fix.line_length;
	lcd->data = buffer + lcd->stride * var.yoffset;

	lcd->red.offset = var.red.offset;
	lcd->red.size = var.red.length;
	lcd->green.offset = var.green.offset;
	lcd->green.size = var.green.length;
	lcd->blue.offset = var.blue.offset;
	lcd->blue.size = var.blue.length;
	lcd->alpha.offset = var.transp.offset;
	lcd->alpha.size = var.transp.length;

	lcd->type = DISPLAY_TYPE_HDMI;
	lcd->size = size;
	lcd->background = (unsigned char *)&lcd[1];
	memset(lcd->background, 0, size);
	lcd->fgcolor = 0xffffffff;

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
	char *fmt;
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
	lcd->background = &lcd->data8[size];
	memset(lcd->background, 0, size);
	lcd->fgcolor = 0xffffffff;

	if (bpp == 16) {
		lcd->blue.offset = 0;
		lcd->blue.size = 5;
		lcd->green.offset = lcd->blue.size;
		lcd->green.size = 6;
		lcd->red.offset = lcd->blue.size + lcd->green.size;
		lcd->red.size = 5;
	}

	fmt = string_from_file("/proc/stb/lcd/colorformat");
	if (fmt != NULL) {
		if (!strcmp(fmt, "RGB_565_BE")) {
			lcd->red.offset = 0;
			lcd->red.size = 5;
			lcd->green.offset = lcd->red.size;
			lcd->green.size = 6;
			lcd->blue.offset = lcd->red.size + lcd->green.size;
			lcd->blue.size = 5;
			lcd->byteswap = true;
		}
		free(fmt);
	}

	return lcd;
}

struct lcd *display_open(enum display_type type)
{
	struct lcd *lcd = NULL;

	if (type == DISPLAY_TYPE_OLED)
		lcd = lcd_open();
	else if (type == DISPLAY_TYPE_HDMI)
		lcd = hdmi_open();

	if (lcd != NULL) {
		if (lcd->width == 128 && lcd->bpp == 4) {
			lcd->logo = lcdlogo_128x8_gray4;
			lcd->logo_size = sizeof(lcdlogo_128x8_gray4);
		} else if (lcd->width == 400 && lcd->height == 240 && lcd->bpp == 16) {
			if (!lcdlogo_400x240_rgb565_decompressed) {
				unxz(lcdlogo_400x240_rgb565, sizeof(lcdlogo_400x240_rgb565),
					lcdlogo_400x240_rgb565_xz, sizeof(lcdlogo_400x240_rgb565_xz));
				lcdlogo_400x240_rgb565_decompressed = true;
			}
			lcd->logo = lcdlogo_400x240_rgb565;
			lcd->logo_size = sizeof(lcdlogo_400x240_rgb565);
		} else {
			lcd->logo = lcdlogo_96x7_mono;
			lcd->logo_size = sizeof(lcdlogo_96x7_mono);
		}
	}

	return lcd;
}

void lcd_release(struct lcd *lcd)
{
	if (lcd->fd >= 0)
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
		memcpy(&lcd->data8[lcd->stride * y], &lcd->background8[lcd->stride * y], lcd->stride * height);
}

static ssize_t lcd_write(struct lcd *lcd, const void *buf, size_t count)
{
	off_t offset;

	offset = lcd_seek(lcd, 0, SEEK_CUR);
	if (offset + count > lcd->size)
		count = lcd->size - offset;

	if ((ssize_t)count >= 0) {
		memcpy(&lcd->data8[offset], buf, count);
		return count;
	}

	return -1;
}

void lcd_set_fgcolor(struct lcd *lcd, unsigned int argb)
{
	unsigned int a = ((argb >> 24) & 0xff) >> (8 - lcd->alpha.size);
	unsigned int r = ((argb >> 16) & 0xff) >> (8 - lcd->red.size);
	unsigned int g = ((argb >>  8) & 0xff) >> (8 - lcd->green.size);
	unsigned int b = ((argb >>  0) & 0xff) >> (8 - lcd->blue.size);

	lcd->fgcolor = (a << lcd->alpha.offset) |
	               (r << lcd->red.offset) |
	               (g << lcd->green.offset) |
	               (b << lcd->blue.offset);

	if (lcd->byteswap && lcd->bpp == 16)
		lcd->fgcolor = bswap_16(lcd->fgcolor);
}

void lcd_save_background(struct lcd *lcd)
{
	memcpy(lcd->background, lcd->data, lcd->size);
}

void lcd_write_logo(struct lcd *lcd)
{
	if (lcd->bpp == 16 && (lcd->width != 400 || lcd->height != 240)) {
		unsigned int scale_factor = lcd_scale_factor(lcd);
		unsigned char logo[lcd->logo_size * 16 * scale_factor];
		unsigned short *wptr = (unsigned short *)logo;
		unsigned int i, j, k, pixel;
		for (i = 0; i < lcd->logo_size; i++) {
			for (j = 0; j < 8; j++) {
				if (lcd->logo[i] & (1 << (7 - j)))
					pixel = lcd->fgcolor;
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
	} else if (lcd->bpp == 32) {
		unsigned int scale_factor = lcd_scale_factor(lcd);
		unsigned char logo[lcd->logo_size * 32 * scale_factor];
		unsigned int *wptr = (unsigned int *)logo;
		unsigned int i, j, k, pixel;
		for (i = 0; i < lcd->logo_size; i++) {
			for (j = 0; j < 8; j++) {
				if (lcd->logo[i] & (1 << (7 - j)))
					pixel = lcd->fgcolor;
				else
					pixel = 0;
				for (k = 0; k < scale_factor; k++)
					*wptr++ = pixel;
			}
		}
		for (i = 0; i < 7; i++) {
			for (j = 0; j < scale_factor; j++) {
				lcd_write(lcd, &logo[i * 96 * 4 * scale_factor], 96 * 4 * scale_factor);
				lcd_seek(lcd, lcd->stride, SEEK_CUR);
			}
		}
	} else {
		lcd_write(lcd, lcd->logo, lcd->logo_size);
	}
}

void lcd_get_logo_size(struct lcd *lcd, unsigned int *width, unsigned int *height)
{
	if (lcd->bpp == 4) {
		*width = 128;
		*height = 8;
	} else if (lcd->width == 400 && lcd->height == 240 && lcd->bpp == 16) {
		*width = 400;
		*height = 240;
	} else if (lcd->bpp >= 16) {
		unsigned int scale_factor = lcd_scale_factor(lcd);
		*width = 96 * scale_factor;
		*height = 7 * scale_factor;
	} else
		abort();
}
