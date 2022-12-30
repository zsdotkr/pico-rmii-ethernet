/*
 * Copyright (c) 2023 zsdotkr@gmail.com
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __PROFILE_H__
#define __PROFILE_H__

#include "pico/stdlib.h"

#define USE_RMII_SM_STAT // for RMII SM statistics
//#define USE_TIMELAPSE   // for simple profiling

// ----- statistics
#ifdef USE_RMII_SM_STAT
	typedef struct
	{	uint32_t	rx_ok;
		uint32_t	tx_ok;
		uint32_t	rx_full;
		uint32_t	bad_crc;
		uint32_t	pbuf_empty;
		uint32_t	pbuf_err;
	} rmii_sm_stat_t;
	#define rmii_sm_stat_declare(name)			rmii_sm_stat_t name = {0};
	#define rmii_sm_stat_add(name_field, val)	name_field += (val);
	#define rmii_sm_stat_clr(name)				{	memset(&(name), 0, sizeof(rmii_sm_stat_t));	}
	void rmii_sm_stat_prt(rmii_sm_stat_t* name)
	{	int x = name->tx_ok + name->rx_ok + name->rx_full+ name->bad_crc+ name->pbuf_empty+ name->pbuf_err;
		if (x)
		{	printf("TX/RX %d %d RX-FULL/CRC/PBUF/ERR %d %d %d %d\n",
				name->tx_ok, name->rx_ok, name->rx_full, name->bad_crc, name->pbuf_empty, name->pbuf_err);
		}
	}
#else
	#define rmii_sm_stat_declare(name)			;
	#define rmii_sm_stat_add(name_field, val)	;
	#define rmii_sm_stat_clr(name)				;
	#define rmii_sm_stat_prt(name)				;
#endif


// ----- for simple profiling
#ifdef USE_TIMELAPSE
	typedef struct timelapse_t
	{	uint32_t			run;
		uint32_t			start;
		uint32_t			min, max;
		const char* 		title;
		struct timelapse_t* next;
	} timelapse_t;
	timelapse_t*	g_timelapse_head = NULL;

	#define timelapse_declare(name, titled)	static timelapse_t name  = {	.title = titled, .min = -1, .max = 0, .run = 0	};
	#define timelapse_link(name) 			{	name.next = g_timelapse_head;	g_timelapse_head = &name;	}
	#define timelapse_start(name)			{   name.start = time_us_32();  }
	#define timelapse_stop(name)			{	uint32_t diff = time_us_32() - name.start; \
												name.run++; \
												if (diff < name.min)	{	name.min = diff;	} \
												if (diff > name.max)	{	name.max = diff;	} \
											}
	void timelapse_prt()	{	timelapse_t* ptr = g_timelapse_head;
								int		wrap = 0;
								while (ptr)
								{	printf("%s R/N/X %d %d %d ", ptr->title, ptr->run, ptr->min, ptr->max);
                                    ptr->run = ptr->max = 0;
                                    ptr->min = -1;
									wrap ++;
									if (wrap == 4)	{	wrap = 0;	putchar('\n'); 	}
									ptr = ptr->next;
								}
							}
#else
	#define timelapse_declare(name, titled)			;
	#define timelapse_link(name)					;
	#define timelapse_start(name)					;
	#define timelapse_stop(name)					;
	#define timelapse_prt()							;

#endif

#endif // __PROFILE_H__
