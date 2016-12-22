/*
 * Copyright (c) 2015-2016, Renesas Electronics Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SWDT_DRIVER_H_
#define SWDT_DRIVER_H_

#define	SWDT_FREQ_OSC_DIV_1_1		(0U)
#define	SWDT_FREQ_OSC_DIV_1_4		(1U)
#define	SWDT_FREQ_OSC_DIV_1_16		(2U)
#define	SWDT_FREQ_OSC_DIV_1_32		(3U)
#define	SWDT_FREQ_OSC_DIV_1_64		(4U)
#define	SWDT_FREQ_OSC_DIV_1_128		(5U)
#define	SWDT_FREQ_OSC_DIV_1_1024	(6U)
#define	SWDT_FREQ_EXPANDED		(7U)

#define	SWDT_SUCCESS			(0)
#define	SWDT_ERR_PARAMETER		(-1)
#define	SWDT_ERR_SEQUENCE		(-2)

int32_t swdt_start(uint16_t count, uint8_t clk,
		uint8_t expanded_clk, void (*cb)(void));
int32_t swdt_stop(void);
int32_t swdt_kick(void);

#endif /* SWDT_DRIVER_H_ */