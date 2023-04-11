/*
 * Copyright (c) 2021 Sandeep Mistry
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hardware/regs/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/pll.h"

#include "lwip/dhcp.h"
#include "lwip/init.h"

#include "lwip/apps/lwiperf.h"

#include "rmii_ethernet/netif.h"

void report(void *arg, enum lwiperf_report_type report_type,
			const ip_addr_t *local_addr, u16_t local_port, const ip_addr_t *remote_addr, u16_t remote_port,
			u32_t bytes_transferred, u32_t ms_duration, u32_t bandwidth_kbitpsec)
{
	printf("IPERF report: type=%d, remote: %s:%d, total bytes: %" U32_F ", duration in ms: %" U32_F ", kbits/s: %" U32_F "\n",
		   (int)report_type, ipaddr_ntoa(remote_addr), (int)remote_port, bytes_transferred, ms_duration, bandwidth_kbitpsec);
}

void netif_link_callback(struct netif *netif)
{	printf("netif link status changed %s\n",
		   netif_is_link_up(netif) ? "up" : "down");
}

void netif_status_callback(struct netif *netif)
{	printf("netif status changed %s\n", ip4addr_ntoa(netif_ip4_addr(netif)));
}

extern void cli_init(void);
extern void cli_run(void);

int main()
{
	// LWIP network interface
	struct netif netif;

	struct netif_rmii_ethernet_config netif_config = {
		pio0, // PIO:            0
		0,	  // pio SM:         0 and 1
		6,	  // rx pin start:   6, 7, 8    => RX0, RX1, CRS
		10,	  // tx pin start:   10, 11, 12 => TX0, TX1, TX-EN
		14,	  // mdio pin start: 14, 15   => ?MDIO, MDC
		23,	  // rmii clock:     21, 23, 24 or 25 => RETCLK
		NULL, // MAC address (optional - NULL generates one based on flash id)
	};

	// Temporarily switch to crystal clock
	clock_configure(clk_sys,
					CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
					CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
					12 * MHZ,
					12 * MHZ);

	// Configure PLL sys to 1500 / 5 / 3 = 100MHz
	pll_init(pll_sys, 1, 1500 * MHZ, 5, 3);

	// Switch back to PLL
	clock_configure(clk_sys,
					CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
					CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
					100 * MHZ,
					100 * MHZ);

	// Configure clock output on GPIO21 at (100MHz) / 2 = 50MHz
	clock_gpio_init(netif_config.retclk_pin, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, 2);

	// Initialize stdio after the clock change
	stdio_init_all();
	sleep_ms(5000);

	printf("pico rmii ethernet - iperf TCP server\n");

	// Initilize LWIP in NO_SYS mode
	lwip_init();

	// Initialize the PIO-based RMII Ethernet network interface
	netif_rmii_ethernet_init(&netif, &netif_config);

	// Assign callbacks for link and status
	netif_set_link_callback(&netif, netif_link_callback);
	netif_set_status_callback(&netif, netif_status_callback);

	// Set the default interface and bring it up
	netif_set_default(&netif);
	netif_set_up(&netif);

	// Start DHCP client and httpd
	dhcp_start(&netif);

	// Setup core 1 to monitor the RMII ethernet interface
	// This let's core 0 do other things :)
	multicore_launch_core1(netif_rmii_ethernet_loop);

	lwiperf_start_tcp_server_default(report, NULL);
	printf("iperf TCP server launched\n");

	cli_init();
	while (1)
	{	tight_loop_contents();
		cli_run();
	}

	return 0;
}
