/*
 * Copyright (c) 2021 Sandeep Mistry
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>

#include "lan8720a.h"

#include "hardware/dma.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "rmii_ethernet_phy_rx.pio.h"
#include "rmii_ethernet_phy_tx.pio.h"

#include "rmii_ethernet/netif.h"

// ------------------------------------------------------------------
// - Debug
// ------------------------------------------------------------------
#define DBG(x, y...)	printf(x "\n", ##y)
#define LOG(x, y...)	printf(x "\n", ##y)

// ------------------------------------------------------------------
// - extern
// ------------------------------------------------------------------
extern uint32_t fcs_crc32(const uint8_t *buf, int size);    // zs

// ------------------------------------------------------------------
// - Static Vars
// ------------------------------------------------------------------
static struct netif 		*s_rmii_if;

static struct netif_rmii_ethernet_config s_rmii_if_cfg = NETIF_RMII_ETHERNET_DEFAULT_CONFIG();
#define PICO_RMII_PIO 		(s_rmii_if_cfg.pio)
#define PICO_RMII_SM_RX 	(s_rmii_if_cfg.pio_sm_start)
#define PICO_RMII_SM_TX 	(s_rmii_if_cfg.pio_sm_start + 1)
#define PICO_RMII_RX_PIN 	(s_rmii_if_cfg.rx_pin_start)
#define PICO_RMII_TX_PIN 	(s_rmii_if_cfg.tx_pin_start)
#define PICO_RMII_MDIO_PIN 	(s_rmii_if_cfg.mdio_pin_start)
#define PICO_RMII_MDC_PIN 	(s_rmii_if_cfg.mdio_pin_start + 1)
#define PICO_RMII_RETCLK_PIN (s_rmii_if_cfg.retclk_pin)
#define PICO_RMII_MAC_ADDR 	(s_rmii_if_cfg.mac_addr)

static uint					s_rx_sm_off;		// start address of SM in PIO ram
static uint 				s_tx_sm_off;		// start address of SM in PIO ram

static int 					s_rx_dma_chn;		// DMA channel number for RX SM
static int 					s_tx_dma_chn;		// DMA channel number for TX SM

static dma_channel_config 	s_rx_dma_chn_cfg;	// DMA channel configuration for RX SM
static dma_channel_config 	s_tx_dma_chn_cfg;	// DMA channel configuration for TX SM

static uint8_t 				s_rx_frame[1518];	// temporary buffer for RX side DMA
static uint8_t 				s_tx_frame[1518];	// temporary buffer for TX side DMA

static int 					s_phy_addr = 0;		// LAN8720A PHY Address (auto-detected)

// ------------------------------------------------------------------
// - MDIO bit-bang
// ------------------------------------------------------------------

static void netif_rmii_ethernet_mdio_clock_out(int bit)
{	gpio_put(PICO_RMII_MDC_PIN, 0);			sleep_us(1);
	gpio_put(PICO_RMII_MDIO_PIN, bit);
	gpio_put(PICO_RMII_MDC_PIN, 1);			sleep_us(1);
}

static uint netif_rmii_ethernet_mdio_clock_in()
{	gpio_put(PICO_RMII_MDC_PIN, 0);			sleep_us(1);

	gpio_put(PICO_RMII_MDC_PIN, 1);

	int bit = gpio_get(PICO_RMII_MDIO_PIN);
	sleep_us(1);

	return bit;
}

static uint16_t netif_rmii_ethernet_mdio_read(uint addr, uint reg)
{	gpio_init(PICO_RMII_MDIO_PIN);
	gpio_init(PICO_RMII_MDC_PIN);

	gpio_set_dir(PICO_RMII_MDIO_PIN, GPIO_OUT);
	gpio_set_dir(PICO_RMII_MDC_PIN, GPIO_OUT);

	// PRE_32
	for (int i = 0; i < 32; i++)	{	netif_rmii_ethernet_mdio_clock_out(1);	}

	// ST
	netif_rmii_ethernet_mdio_clock_out(0);
	netif_rmii_ethernet_mdio_clock_out(1);

	// OP
	netif_rmii_ethernet_mdio_clock_out(1);
	netif_rmii_ethernet_mdio_clock_out(0);

	// PA5
	for (int i = 0; i < 5; i++)
	{	uint bit = (addr >> (4 - i)) & 0x01;

		netif_rmii_ethernet_mdio_clock_out(bit);
	}

	// RA5
	for (int i = 0; i < 5; i++)
	{	uint bit = (reg >> (4 - i)) & 0x01;

		netif_rmii_ethernet_mdio_clock_out(bit);
	}

	// TA
	gpio_set_dir(PICO_RMII_MDIO_PIN, GPIO_IN);
	netif_rmii_ethernet_mdio_clock_out(0);
	netif_rmii_ethernet_mdio_clock_out(0);

	uint16_t data = 0;

	for (int i = 0; i < 16; i++)
	{	data <<= 1;

		data |= netif_rmii_ethernet_mdio_clock_in();
	}

	return data;
}

void netif_rmii_ethernet_mdio_write(int addr, int reg, int val)
{	gpio_init(PICO_RMII_MDIO_PIN);
	gpio_init(PICO_RMII_MDC_PIN);

	gpio_set_dir(PICO_RMII_MDIO_PIN, GPIO_OUT);
	gpio_set_dir(PICO_RMII_MDC_PIN, GPIO_OUT);

	// PRE_32
	for (int i = 0; i < 32; i++)	{	netif_rmii_ethernet_mdio_clock_out(1);	}

	// ST
	netif_rmii_ethernet_mdio_clock_out(0);
	netif_rmii_ethernet_mdio_clock_out(1);

	// OP
	netif_rmii_ethernet_mdio_clock_out(0);
	netif_rmii_ethernet_mdio_clock_out(1);

	// PA5
	for (int i = 0; i < 5; i++)
	{	uint bit = (addr >> (4 - i)) & 0x01;

		netif_rmii_ethernet_mdio_clock_out(bit);
	}

	// RA5
	for (int i = 0; i < 5; i++)
	{	uint bit = (reg >> (4 - i)) & 0x01;

		netif_rmii_ethernet_mdio_clock_out(bit);
	}

	// TA
	netif_rmii_ethernet_mdio_clock_out(1);
	netif_rmii_ethernet_mdio_clock_out(0);

	for (int i = 0; i < 16; i++)
	{	uint bit = (val >> (15 - i)) & 0x01;

		netif_rmii_ethernet_mdio_clock_out(bit);
	}

	gpio_set_dir(PICO_RMII_MDIO_PIN, GPIO_IN);
}

// ------------------------------------------------------------------
// - Ethernet FCS
// ------------------------------------------------------------------
// zs, replace ethernet_frame_crc (bit base) to fcs_crc32 (byte base)

static uint ethernet_frame_length(const uint8_t *data, int length) // zs, replace to use byte base calculation
{	extern const uint32_t crc32_tab[];

    uint crc = 0xffffffff;  // Initial value.
    uint index = 0;

    while(--length >= 0)
    {   crc = crc32_tab[(crc ^ *data++) & 0xFF] ^ (crc >> 8);

        index++;

        uint inverted_crc = ~crc;

        if (memcmp(data, &inverted_crc, sizeof(inverted_crc)) == 0) {
            return index;
        }
    }

    return 0;
}

// ------------------------------------------------------------------
// - Ethernet Tx
// ------------------------------------------------------------------

static err_t netif_rmii_ethernet_output(struct netif *netif, struct pbuf *p)
{	// Wait until the buffer is again available
	// TODO: use a ping-pong buffer ?
	dma_channel_wait_for_finish_blocking(s_tx_dma_chn);

	// zs, useless memset(s_tx_frame, 0x00, sizeof(s_tx_frame));

	// Retrieve the data to send and copy in TX buffer
	// TODO: check for overflow?
	uint tot_len = 0;
	for (struct pbuf *q = p; q != NULL; q = q->next)
	{	memcpy(s_tx_frame + tot_len, q->payload, q->len);

		tot_len += q->len;

		if (q->len == q->tot_len)	{	break;	}
	}

	// Minimal Ethernet payload is 64 bytes, 4-bytes CRC included
	// Pad the payload to 64-4=60 bytes, and then add the CRC
	// TODO-zs : may need to fill zero for padding data area
	if (tot_len < 60)	{	tot_len = 60;	}

	// Append the CRC to the frame
	uint crc = fcs_crc32(s_tx_frame, tot_len);	// zs, replace ethernet_frame_crc
	for (int i = 0; i < 4; i++)	{	s_tx_frame[tot_len++] = ((uint8_t *)&crc)[i];	}

	// Setup and start the DMA to send the frame via the PIO RMII tansmitter
	dma_channel_configure(
		s_tx_dma_chn, &s_tx_dma_chn_cfg,
		((uint8_t *)&PICO_RMII_PIO->txf[PICO_RMII_SM_TX]) + 3,
		s_tx_frame,
		tot_len,
		true);

	return ERR_OK;
}

// ------------------------------------------------------------------
// - Ethernet Init
// ------------------------------------------------------------------

static err_t netif_rmii_ethernet_low_init(struct netif *netif)
{	// Prepare the interface
	s_rmii_if = netif;

	netif->linkoutput = netif_rmii_ethernet_output;
	netif->output = etharp_output;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;

	// Setup the MAC address
	if (PICO_RMII_MAC_ADDR != NULL)
	{	memcpy(netif->hwaddr, PICO_RMII_MAC_ADDR, 6);
	}
	else
	{	// generate one for unique board id
		pico_unique_board_id_t board_id;

		pico_get_unique_board_id(&board_id);

		netif->hwaddr[0] = 0xb8;
		netif->hwaddr[1] = 0x27;
		netif->hwaddr[2] = 0xeb;
		memcpy(&netif->hwaddr[3], &board_id.id[5], 3);
	}
	netif->hwaddr_len = ETH_HWADDR_LEN;

	// Init the RMII PIO programs
	s_rx_sm_off = pio_add_program(PICO_RMII_PIO, &rmii_ethernet_phy_rx_data_program);
	s_tx_sm_off = pio_add_program(PICO_RMII_PIO, &rmii_ethernet_phy_tx_data_program);

	// Configure the DMA channels
	s_rx_dma_chn = dma_claim_unused_channel(true);
	s_tx_dma_chn = dma_claim_unused_channel(true);

	s_rx_dma_chn_cfg = dma_channel_get_default_config(s_rx_dma_chn);

	channel_config_set_read_increment(&s_rx_dma_chn_cfg, false);
	channel_config_set_write_increment(&s_rx_dma_chn_cfg, true);
	channel_config_set_dreq(&s_rx_dma_chn_cfg, pio_get_dreq(PICO_RMII_PIO, PICO_RMII_SM_RX, false));
	channel_config_set_transfer_data_size(&s_rx_dma_chn_cfg, DMA_SIZE_8);

	s_tx_dma_chn_cfg = dma_channel_get_default_config(s_tx_dma_chn);

	channel_config_set_read_increment(&s_tx_dma_chn_cfg, true);
	channel_config_set_write_increment(&s_tx_dma_chn_cfg, false);
	channel_config_set_dreq(&s_tx_dma_chn_cfg, pio_get_dreq(PICO_RMII_PIO, PICO_RMII_SM_TX, true));
	channel_config_set_transfer_data_size(&s_tx_dma_chn_cfg, DMA_SIZE_8);

	// Configure the RMII TX state machine
	rmii_ethernet_phy_tx_init(PICO_RMII_PIO, PICO_RMII_SM_TX, s_tx_sm_off, PICO_RMII_TX_PIN, PICO_RMII_RETCLK_PIN, 1);

	// Auto-Detection LAN8720A PHY address
	for (int i = 0; i < 32; i++)
	{	if (netif_rmii_ethernet_mdio_read(i, 0) != 0xffff)
		{	s_phy_addr = i;
			DBG("LAN8720A PHY ADDR : %d", s_phy_addr);
			break;
		}
	}

	// Default mode is 10Mbps, auto-negociate disabled
	// Uncomment this to switch to 100Mbps, auto-negociate disabled
	// netif_rmii_ethernet_mdio_write(s_phy_addr, LAN8720A_BASIC_CONTROL_REG, 0x2000); // 100 Mbps, auto-negeotiate disabled

	// Or keep the following config to auto-negotiate 10/100Mbps
	// 0b0000_0001_1110_0001
	//           | |||⁻⁻⁻⁻⁻\__ 0b00001=IEEE 802.3
	//           | || \_______ 10BASE-T ability
	//           | | \________ 10BASE-T Full-Duplex ability
	//           |  \_________ 100BASE-T ability
	//            \___________ 100BASE-T Full-Duplex ability
	netif_rmii_ethernet_mdio_write(s_phy_addr, LAN8720A_AUTO_NEGO_REG,
								   LAN8720A_AUTO_NEGO_REG_IEEE802_3
									   // TODO: the PIO RX and TX are hardcoded to 100Mbps, make it configurable to uncomment this
									   // | LAN8720A_AUTO_NEGO_REG_10_ABI | LAN8720A_AUTO_NEGO_REG_10_FD_ABI
									   | LAN8720A_AUTO_NEGO_REG_100_ABI | LAN8720A_AUTO_NEGO_REG_100_FD_ABI);
	// Enable auto-negotiate
	netif_rmii_ethernet_mdio_write(s_phy_addr, LAN8720A_BASIC_CONTROL_REG, 0x1000);

	return ERR_OK;
}

// ------------------------------------------------------------------
// - Ethernet Rx
// ------------------------------------------------------------------

static void netif_rmii_ethernet_rx_dv_falling_callback(uint gpio, uint32_t events)
{	if ((PICO_RMII_RX_PIN + 2) == gpio)
	{
		pio_sm_set_enabled(PICO_RMII_PIO, PICO_RMII_SM_RX, false);
		dma_channel_abort(s_rx_dma_chn); // dma_hw->abort = 1u << s_rx_dma_chn;
		gpio_set_irq_enabled_with_callback(PICO_RMII_RX_PIN + 2, GPIO_IRQ_EDGE_FALL, false, netif_rmii_ethernet_rx_dv_falling_callback);
	}
}

void netif_rmii_ethernet_poll()
{	uint16_t mdio_read = netif_rmii_ethernet_mdio_read(s_phy_addr, 1);
	// printf("mdio_read: %016b\n", mdio_read);
	uint16_t link_status = (mdio_read & 0x04) >> 2;

	if (netif_is_link_up(s_rmii_if) ^ link_status)
	{	if (link_status)
		{	// printf("netif_set_link_up\n");
			netif_set_link_up(s_rmii_if);
		}
		else
		{	// printf("netif_set_link_down\n");
			netif_set_link_down(s_rmii_if);
		}
	}

	if (dma_channel_is_busy(s_rx_dma_chn))
	{
	}
	else
	{	uint rx_frame_length = ethernet_frame_length(s_rx_frame, sizeof(s_rx_frame));

		if (rx_frame_length)
		{	struct pbuf *p = pbuf_alloc(PBUF_RAW, rx_frame_length, PBUF_POOL);

			pbuf_take(p, s_rx_frame, rx_frame_length);

			if (s_rmii_if->input(p, s_rmii_if) != ERR_OK)
			{	pbuf_free(p);
			}
		}

		memset(s_rx_frame, 0x00, sizeof(s_rx_frame));

		dma_channel_configure(
			s_rx_dma_chn, &s_rx_dma_chn_cfg,
			s_rx_frame,
			((uint8_t *)&PICO_RMII_PIO->rxf[PICO_RMII_SM_RX]) + 3,
			1500,
			false);

		dma_channel_start(s_rx_dma_chn);

		rmii_ethernet_phy_rx_init(PICO_RMII_PIO, PICO_RMII_SM_RX, s_rx_sm_off, PICO_RMII_RX_PIN, 1);

		gpio_set_irq_enabled_with_callback(PICO_RMII_RX_PIN + 2, GPIO_IRQ_EDGE_FALL, true, &netif_rmii_ethernet_rx_dv_falling_callback);
	}

	sys_check_timeouts();
}

void netif_rmii_ethernet_loop()
{	while (1)	{	netif_rmii_ethernet_poll();	}
}

// ------------------------------------------------------------------
// - Extra
// ------------------------------------------------------------------

err_t netif_rmii_ethernet_init(struct netif *netif, struct netif_rmii_ethernet_config *config)
{
	if (config != NULL)
	{	memcpy(&s_rmii_if_cfg, config, sizeof(s_rmii_if_cfg));
	}

	// To set up a static IP, uncomment the folowing lines and comment the one using DHCP
	// const ip_addr_t ip = IPADDR4_INIT_BYTES(169, 254, 145, 200);
	// const ip_addr_t mask = IPADDR4_INIT_BYTES(255, 255, 0, 0);
	// const ip_addr_t gw = IPADDR4_INIT_BYTES(169, 254, 145, 164);
	// netif_add(netif, &ip, &mask, &gw, NULL, netif_rmii_ethernet_low_init, netif_input);

	// Set up the interface using DHCP
	netif_add(netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY, NULL, netif_rmii_ethernet_low_init, netif_input);

	netif->name[0] = 'e';
	netif->name[1] = '0';
}