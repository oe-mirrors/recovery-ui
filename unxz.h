/*
 * Copyright (C) 2017 Dream Property GmbH, Germany
 *                    https://dreambox.de/
 */

#ifndef UNXZ_H
#define UNXZ_H

#include <stdbool.h>
#include <stddef.h>

bool unxz(void *dst, size_t dst_size, const void *src, size_t src_size);

#endif /* UNXZ_H */
