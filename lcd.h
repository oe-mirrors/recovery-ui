#ifndef __RECOVERY_LCD_H__
#define __RECOVERY_LCD_H__

#include <stdbool.h>

enum display_type {
	DISPLAY_TYPE_MIN,
	DISPLAY_TYPE_OLED = DISPLAY_TYPE_MIN,
	DISPLAY_TYPE_HDMI,
	DISPLAY_TYPE_MAX,
};

struct lcd;

struct lcd *display_open(enum display_type);
void lcd_release(struct lcd *lcd);

void lcd_putc(struct lcd *lcd, char c);
void lcd_puts(struct lcd *lcd, const char *str);
int lcd_printf(struct lcd *lcd, const char *fmt, ...);
off_t lcd_seek(struct lcd *lcd, off_t offset, int whence);
void lcd_write_logo(struct lcd *lcd);
void lcd_save_background(struct lcd *lcd);
void lcd_set_fgcolor(struct lcd *lcd, unsigned int argb);

void lcd_clear(struct lcd *lcd, unsigned int height);
void lcd_set_x(struct lcd *lcd, int x);
void lcd_set_y(struct lcd *lcd, int y);
bool lcd_update(struct lcd *lcd);

unsigned int lcd_width(struct lcd *lcd);
unsigned int lcd_height(struct lcd *lcd);

unsigned int lcd_font_height(struct lcd *lcd);
unsigned int lcd_font_width(struct lcd *lcd);

void lcd_get_logo_size(struct lcd *lcd, unsigned int *width, unsigned int *height);

#endif /* __RECOVERY_LCD_H__ */
