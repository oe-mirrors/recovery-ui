#ifndef __RECOVERY_LCD_H__
#define __RECOVERY_LCD_H__

#include <stdbool.h>

struct lcd;

struct lcd *lcd_open(void);
void lcd_release(struct lcd *lcd);

void lcd_putc(struct lcd *lcd, char c);
void lcd_puts(struct lcd *lcd, const char *str);
int lcd_printf(struct lcd *lcd, const char *fmt, ...);
off_t lcd_seek(struct lcd *lcd, off_t offset, int whence);
ssize_t lcd_write(struct lcd *lcd, const void *buf, size_t count);

void lcd_clear(struct lcd *lcd, unsigned int height);
void lcd_set_x(struct lcd *lcd, int x);
void lcd_set_y(struct lcd *lcd, int y);
bool lcd_update(struct lcd *lcd);

unsigned int lcd_width(struct lcd *lcd);
unsigned int lcd_height(struct lcd *lcd);

unsigned int lcd_font_height(struct lcd *lcd);
unsigned int lcd_font_width(struct lcd *lcd);

#endif /* __RECOVERY_LCD_H__ */
