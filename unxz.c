/*
 * Copyright (C) 2017 Dream Property GmbH, Germany
 *                    https://dreambox.de/
 */

#include <lzma.h>
#include "unxz.h"

bool unxz(void *dst, size_t dst_size, const void *src, size_t src_size)
{
	lzma_stream strm = LZMA_STREAM_INIT;

	if (lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK)
		return false;

	strm.next_in = src;
	strm.avail_in = src_size;
	strm.next_out = dst;
	strm.avail_out = dst_size;

	return lzma_code(&strm, LZMA_RUN) == LZMA_STREAM_END;
}
