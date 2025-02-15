/*
 * Copyright (c) 2023 zsdotkr@gmail.com
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define USE_TWO_RX_SM

#include <string.h>

#include "lan8720a.h"

#include "hardware/dma.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "pico/sem.h"			// use semaphore to inform Ethernet RX event

#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#ifdef USE_TWO_RX_SM
	#include "rmii_ethernet_phy_rx_2.pio.h"
#else
	#include "rmii_ethernet_phy_rx.pio.h"
#endif
#include "rmii_ethernet_phy_tx.pio.h"

#include "rmii_ethernet/netif.h"

#include "profile.h"

// ------------------------------------------------------------------
// - Debug
// ------------------------------------------------------------------
#define DBG(x, y...)	printf(x "\n", ##y)
#define LOG(x, y...)	printf(x "\n", ##y)

#define likely(x)		__builtin_expect((x),1)
#define unlikely(x)		__builtin_expect((x),0)

// ------------------------------------------------------------------
// - extern
// ------------------------------------------------------------------
extern uint32_t fcs_crc32(const uint8_t *buf, int size);	// byte based crc32 calculation

// ------------------------------------------------------------------
// - Static Vars
// ------------------------------------------------------------------
static struct netif 		*s_rmii_if;

static struct netif_rmii_ethernet_config s_rmii_if_cfg = NETIF_RMII_ETHERNET_DEFAULT_CONFIG();
#define PICO_RMII_PIO 		(s_rmii_if_cfg.pio)
#define PICO_RMII_SM_RX 	(s_rmii_if_cfg.pio_sm_start)
#define PICO_RMII_SM_TX 	(s_rmii_if_cfg.pio_sm_start + 1)
#define PICO_RMII_SM_RX_2 	(s_rmii_if_cfg.pio_sm_start + 2)
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

#ifdef USE_TWO_RX_SM
static int 					s_rx_dma_chn_2;		// DMA channel number for second RX SM
static dma_channel_config 	s_rx_dma_chn_cfg_2;	// DMA channel configuration for RX SM
#endif

// ----- buffer for RMII RX
#define ETH_FRAME_LEN		(1514+4+6)			// 1514(MAC ~ payload) + 4(FCS) + 6(reserved for data boundary guard or VLAN??)
#define MAX_RX_FRAME		4					// 4 frame needs for iperf/TCP test (21Mbps), adjust as your application needs
static volatile int			s_rx_frame_head;	// updated in ISR code
static volatile int			s_rx_frame_rear;	// updated in netif_rmii_ethernet_poll()
typedef struct
{	int						len;				// length of data
	uint8_t 				data[ETH_FRAME_LEN];
} rx_frame_t;
static rx_frame_t			s_rx_frame[MAX_RX_FRAME];	// buffer between RX-SM ~ DMA
static uint8_t				s_rx_frame_dummy[2];		// dummy memory for DMA when s_rx_frame == full
static int 					s_rx_frame_idx[2];

static semaphore_t			s_rx_frame_sem;		// to trigger packet receiving event from ISR code to netif_rmii_ethernet_poll()

static int 					s_phy_addr = 0;		// LAN8720A PHY Address (auto-detected)

// ----- etc
#define time_after(now, expire) (((int32_t)(expire) - (int32_t)(now)) < 0) //  return 1(now > expire), 0 (now < expire)

// ----- profile & statistics
rmii_sm_stat_declare(s_sm_stat);
timelapse_declare(tl_crc, "CRC");
timelapse_declare(tl_rx, "RX");
timelapse_declare(tl_tx, "TX");
timelapse_declare(tl_net, "NET");


// ------------------------------------------------------------------
// - MDIO bit-bang
// ------------------------------------------------------------------

static void netif_rmii_ethernet_mdio_clock_out(int bit)
{	gpio_put(PICO_RMII_MDC_PIN, 0);			busy_wait_us(2);
	gpio_put(PICO_RMII_MDIO_PIN, bit);
	gpio_put(PICO_RMII_MDC_PIN, 1);			busy_wait_us(2);
}

static uint netif_rmii_ethernet_mdio_clock_in()
{	gpio_put(PICO_RMII_MDC_PIN, 0);			busy_wait_us(2);

	int bit = gpio_get(PICO_RMII_MDIO_PIN);

	gpio_put(PICO_RMII_MDC_PIN, 1);
	busy_wait_us(2);

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

#if 0 // zs, useless, code to search end-of-frame using calculated CRC but already know the packet length
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
#endif

// ------------------------------------------------------------------
// - Ethernet Tx
// ------------------------------------------------------------------

static err_t netif_rmii_ethernet_output(struct netif *netif, struct pbuf *p)
{	// TODO: use a ping-pong buffer ? => zs, No. meaningless. tested

	static uint8_t	tx_frame[ETH_FRAME_LEN];	// MUST be static, accessed by DMA at background

	timelapse_start(tl_tx);

	dma_channel_wait_for_finish_blocking(s_tx_dma_chn);

	// assemble fragmented pbufs to a single buffer for DMA access
	uint tot_len = 0;
	for (struct pbuf *q = p; q != NULL; q = q->next)
	{	memcpy(tx_frame + tot_len, q->payload, q->len);

		tot_len += q->len;

		if (q->len == q->tot_len)	{	break;	}
	}

	// Minimal Ethernet payload is 64 bytes, 4-bytes CRC included
	// Pad the payload to 64-4=60 bytes, and then add the CRC
	// TODO-zs : may need to fill zero for padding data area ??
	if (tot_len < 60)	{	tot_len = 60;	}

	// Append the CRC to the frame
	timelapse_start(tl_crc);
	uint crc = fcs_crc32(tx_frame, tot_len);	// zs, replace ethernet_frame_crc
	for (int i = 0; i < 4; i++)	{	tx_frame[tot_len++] = ((uint8_t *)&crc)[i];	}
	timelapse_stop(tl_crc);

	// Setup and start the DMA to send the frame via the PIO RMII tansmitter
	dma_channel_configure(
		s_tx_dma_chn, &s_tx_dma_chn_cfg,
		((uint8_t *)&PICO_RMII_PIO->txf[PICO_RMII_SM_TX]) + 3,
		tx_frame,
		tot_len,
		true);

	rmii_sm_stat_add(s_sm_stat.tx_ok, 1);
	timelapse_stop(tl_tx);

	return ERR_OK;
}

// ------------------------------------------------------------------
// - Ethernet Rx
// ------------------------------------------------------------------
void __time_critical_func(rx_sm_isr_run)(int sm_no)
{	int sm_idx, frame_idx, dma_no;

#ifdef USE_TWO_RX_SM
	if (sm_no == PICO_RMII_SM_RX)
	{	dma_no = s_rx_dma_chn;		sm_idx = 0; 	frame_idx = s_rx_frame_idx[0]; 		}
	else
	{	dma_no = s_rx_dma_chn_2;	sm_idx = 1; 	frame_idx = s_rx_frame_idx[1]; 		}
#else
	dma_no = s_rx_dma_chn;		sm_idx = 0; 	frame_idx = s_rx_frame_idx[0];
#endif
	int	is_real_rx = dma_hw->ch[dma_no].ctrl_trig & DMA_CH0_CTRL_TRIG_INCR_WRITE_BITS;

	// 1. abort DMA
	dma_channel_abort(dma_no);

	// 2. calculate length
	if (is_real_rx)
	{	uint32_t	offset = dma_channel_hw_addr(dma_no)->write_addr;
		uint32_t 	start = (uint32_t)s_rx_frame[frame_idx].data;

		s_rx_frame[frame_idx].len = offset - start;
	}

	// 3. prepare DMA
	int			rear = s_rx_frame_rear;
	int 		next = (s_rx_frame_head != (MAX_RX_FRAME -1)) ? s_rx_frame_head + 1 : 0;

	int			is_full;
	int 		sem_permit = sem_available(&s_rx_frame_sem);

	if 	(is_real_rx)
	{	if (sem_permit <= (MAX_RX_FRAME-3))	{	is_full = 0;	}
		else								{	is_full = 1;	}
	}
	else
	{	if (sem_permit <= (MAX_RX_FRAME-2))	{	is_full = 0;	}
		else								{	is_full = 1;	}
	}

	if (unlikely(is_full))
	{	dma_hw->ch[dma_no].ctrl_trig &= ~DMA_CH0_CTRL_TRIG_INCR_WRITE_BITS;
		dma_channel_set_write_addr(dma_no, &s_rx_frame_dummy[sm_idx], false);
		rmii_sm_stat_add(s_sm_stat.rx_full, 1);
	}
	else
	{	dma_hw->ch[dma_no].ctrl_trig |= DMA_CH0_CTRL_TRIG_INCR_WRITE_BITS;
		dma_channel_set_write_addr(dma_no, s_rx_frame[next].data, false);
		s_rx_frame_head = next;
		s_rx_frame_idx[sm_idx] = next;

		rmii_sm_stat_add(s_sm_stat.rx_ok, 1);
	}
	dma_channel_set_trans_count(dma_no, sizeof(s_rx_frame[0].data), true);

	// 4. resume SM
	PICO_RMII_PIO->irq |= (0x01 << sm_no);

	if (is_real_rx)	{	sem_release(&s_rx_frame_sem);	}
}

void __time_critical_func(rx_sm_isr_handler) (void) // to locate code in RAM
{	int		sm_no, next;

#ifndef USE_TWO_RX_SM
	sm_no = PICO_RMII_SM_RX;
#else
	if (PICO_RMII_PIO->irq & (1<<PICO_RMII_SM_RX))			{	sm_no = PICO_RMII_SM_RX;	next = PICO_RMII_SM_RX_2; 	}
	else if (PICO_RMII_PIO->irq & (1<<PICO_RMII_SM_RX_2))	{	sm_no = PICO_RMII_SM_RX_2;	next = PICO_RMII_SM_RX; 	}
	else
	{	// FIXME can happen??
		return;
	}
#endif

	rx_sm_isr_run(sm_no);
#ifdef USE_TWO_RX_SM
	if (PICO_RMII_PIO->irq & (1<<next))	{	rx_sm_isr_run(next);}
#endif
}

void netif_rmii_ethernet_poll()
{	static uint32_t		mdio_poll_expire = 0;

	{	uint32_t	now = time_us_32();
		if (time_after(now, mdio_poll_expire))
		{	uint16_t mdio_read = netif_rmii_ethernet_mdio_read(s_phy_addr, 1);
			uint16_t link_status = (mdio_read & 0x04) >> 2;

			if (netif_is_link_up(s_rmii_if) ^ link_status)
			{	// TODO need control stop/start SM/DMA ??
				if (link_status)	{	netif_set_link_up(s_rmii_if);	}
				else				{	netif_set_link_down(s_rmii_if);	}
			}
			mdio_poll_expire = now + (1000*1000); 	// 1sec interval

			rmii_sm_stat_prt(&s_sm_stat);
			rmii_sm_stat_clr(s_sm_stat);

			timelapse_prt();
		}
	}

	// check & clear deadlock between two RX SM forcefully
#ifdef USE_TWO_RX_SM
	{	uint32_t irq_mask = (1<<(4+PICO_RMII_SM_RX)) | (1<<(4+PICO_RMII_SM_RX_2));
		if ((PICO_RMII_PIO->irq & irq_mask) == irq_mask)
		{	pio_interrupt_clear(PICO_RMII_PIO, 4 + PICO_RMII_SM_RX);
			DBG("RX SM Deadlock cleared");
		}
	}
#endif
	if (sem_acquire_timeout_ms(&s_rx_frame_sem, 100) == true)
	{	timelapse_start(tl_rx);

		rx_frame_t* pframe = &s_rx_frame[s_rx_frame_rear];

		uint32_t	*crc_in = (uint32_t*)(&pframe->data[pframe->len - 4]);
		timelapse_start(tl_crc);
		uint32_t	crc_calc = fcs_crc32(pframe->data, pframe->len - 4);
		timelapse_stop(tl_crc);

		int		rx_len;

		if (memcmp(&crc_calc, crc_in, 4) == 0)	{	rx_len = pframe->len - 4; }
		else									{	rx_len = 0;	}

		// DBG("RXD H/R %d %d", s_rx_frame_head, s_rx_frame_rear);

		if (unlikely(rx_len == 0))
		{	rmii_sm_stat_add(s_sm_stat.bad_crc, 1);
			s_rx_frame_rear = (s_rx_frame_rear != (MAX_RX_FRAME-1)) ? s_rx_frame_rear + 1 : 0;
		}
		else
		{	struct pbuf *p = pbuf_alloc(PBUF_RAW, rx_len, PBUF_POOL);

			if (unlikely(p == NULL))
			{	rmii_sm_stat_add(s_sm_stat.pbuf_empty, 1);
				s_rx_frame_rear = (s_rx_frame_rear != (MAX_RX_FRAME-1)) ? s_rx_frame_rear + 1 : 0;
			}
			else
			{	if (pbuf_take(p, pframe->data, rx_len) == ERR_OK)
				{	// update rear indicator befre time-consuming input() job
					s_rx_frame_rear = (s_rx_frame_rear != (MAX_RX_FRAME-1)) ? s_rx_frame_rear + 1 : 0;

					timelapse_start(tl_net);
					if (unlikely(s_rmii_if->input(p, s_rmii_if) != ERR_OK))
					{	rmii_sm_stat_add(s_sm_stat.pbuf_err, 1);
						pbuf_free(p);
					}
					timelapse_stop(tl_net);
				}
				else
				{	rmii_sm_stat_add(s_sm_stat.pbuf_err, 1);
					s_rx_frame_rear = (s_rx_frame_rear != (MAX_RX_FRAME-1)) ? s_rx_frame_rear + 1 : 0;
				}
			}
		}
		timelapse_stop(tl_rx);
	}
	sys_check_timeouts();
}

// ------------------------------------------------------------------
// - Ethernet Init
// ------------------------------------------------------------------

static err_t netif_rmii_ethernet_low_init(struct netif *netif)
{
	s_rmii_if = netif;

	netif->linkoutput = netif_rmii_ethernet_output;
	netif->output = etharp_output;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;

	// Setup the MAC address
	if (PICO_RMII_MAC_ADDR != NULL)
	{	memcpy(netif->hwaddr, PICO_RMII_MAC_ADDR, 6);
	}
	else // generate one for unique board id
	{	pico_unique_board_id_t board_id;

		pico_get_unique_board_id(&board_id);

		netif->hwaddr[0] = 0xb8;
		netif->hwaddr[1] = 0x27;
		netif->hwaddr[2] = 0xeb;
		memcpy(&netif->hwaddr[3], &board_id.id[5], 3);
	}
	netif->hwaddr_len = ETH_HWADDR_LEN;
	DBG("MAC : %02x:%02x:%02x:%02x:%02x:%02x",
		netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
		netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);

	// Init s_rx_frame & semaphore
	s_rx_frame_head = s_rx_frame_rear = 0;
	for (int i = 0; i < MAX_RX_FRAME; i++)	{	s_rx_frame[i].len = 0;	}
	sem_init(&s_rx_frame_sem, 0, MAX_RX_FRAME);

	// Init the RMII PIO programs
#ifdef USE_TWO_RX_SM
	s_rx_sm_off = pio_add_program(PICO_RMII_PIO, &rmii_ethernet_phy_rx_2_data_program);
#else
	s_rx_sm_off = pio_add_program(PICO_RMII_PIO, &rmii_ethernet_phy_rx_data_program);
#endif
	s_tx_sm_off = pio_add_program(PICO_RMII_PIO, &rmii_ethernet_phy_tx_data_program);

	// Configure the DMA channels
	s_rx_dma_chn = dma_claim_unused_channel(true);
	s_tx_dma_chn = dma_claim_unused_channel(true);
#ifdef USE_TWO_RX_SM
	s_rx_dma_chn_2 = dma_claim_unused_channel(true);
#endif
#ifdef USE_TWO_RX_SM
	DBG("DMA RX %d %d TX %d", s_rx_dma_chn, s_rx_dma_chn_2, s_tx_dma_chn);
#else
	DBG("DMA RX %d TX %d", s_rx_dma_chn, s_tx_dma_chn);
#endif

	s_rx_dma_chn_cfg = dma_channel_get_default_config(s_rx_dma_chn);

	channel_config_set_read_increment(&s_rx_dma_chn_cfg, false);
	channel_config_set_write_increment(&s_rx_dma_chn_cfg, true);
	channel_config_set_dreq(&s_rx_dma_chn_cfg, pio_get_dreq(PICO_RMII_PIO, PICO_RMII_SM_RX, false));
	channel_config_set_transfer_data_size(&s_rx_dma_chn_cfg, DMA_SIZE_8);

#ifdef USE_TWO_RX_SM
	s_rx_dma_chn_cfg_2 = dma_channel_get_default_config(s_rx_dma_chn_2);

	channel_config_set_read_increment(&s_rx_dma_chn_cfg_2, false);
	channel_config_set_write_increment(&s_rx_dma_chn_cfg_2, true);
	channel_config_set_dreq(&s_rx_dma_chn_cfg_2, pio_get_dreq(PICO_RMII_PIO, PICO_RMII_SM_RX_2, false));
	channel_config_set_transfer_data_size(&s_rx_dma_chn_cfg_2, DMA_SIZE_8);
#endif

	s_tx_dma_chn_cfg = dma_channel_get_default_config(s_tx_dma_chn);

	channel_config_set_read_increment(&s_tx_dma_chn_cfg, true);
	channel_config_set_write_increment(&s_tx_dma_chn_cfg, false);
	channel_config_set_dreq(&s_tx_dma_chn_cfg, pio_get_dreq(PICO_RMII_PIO, PICO_RMII_SM_TX, true));
	channel_config_set_transfer_data_size(&s_tx_dma_chn_cfg, DMA_SIZE_8);

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

	// Configure & Start RX DMA
	dma_channel_configure(
		s_rx_dma_chn, &s_rx_dma_chn_cfg,
		s_rx_frame[0].data,
		((uint8_t*)&PICO_RMII_PIO->rxf[PICO_RMII_SM_RX]) + 3,
		sizeof(s_rx_frame[0].data),
		true
	);
	s_rx_frame_idx[0] = 0;

#ifdef USE_TWO_RX_SM
	dma_channel_configure(
		s_rx_dma_chn_2, &s_rx_dma_chn_cfg_2,
		s_rx_frame[1].data,
		((uint8_t*)&PICO_RMII_PIO->rxf[PICO_RMII_SM_RX_2]) + 3,
		sizeof(s_rx_frame[1].data),
		true
	);
	s_rx_frame_idx[1] = 1;
	s_rx_frame_head = 1;
#endif

	// Install ISR #3 callback for RX-SM
	irq_set_exclusive_handler(PIO0_IRQ_0, rx_sm_isr_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    PICO_RMII_PIO->inte0 |= (PIO_IRQ0_INTE_SM0_BITS<<PICO_RMII_SM_RX);
#ifdef USE_TWO_RX_SM
    PICO_RMII_PIO->inte0 |= (PIO_IRQ0_INTE_SM0_BITS<<PICO_RMII_SM_RX_2);
#endif

	// Configure & Start the RMII SM
	rmii_ethernet_phy_tx_init(PICO_RMII_PIO, PICO_RMII_SM_TX, s_tx_sm_off, PICO_RMII_TX_PIN, PICO_RMII_RETCLK_PIN, 1);
#ifdef USE_TWO_RX_SM
	rmii_ethernet_phy_rx_2_init(PICO_RMII_PIO, PICO_RMII_SM_RX, s_rx_sm_off, PICO_RMII_RX_PIN, 1);
	rmii_ethernet_phy_rx_2_init(PICO_RMII_PIO, PICO_RMII_SM_RX_2, s_rx_sm_off, PICO_RMII_RX_PIN, 1);
#else
	rmii_ethernet_phy_rx_init(PICO_RMII_PIO, PICO_RMII_SM_RX, s_rx_sm_off, PICO_RMII_RX_PIN, 1);
#endif
	return ERR_OK;
}

void netif_rmii_ethernet_loop()
{
#ifdef USE_TWO_RX_SM
	pio_interrupt_clear(PICO_RMII_PIO, 4 + PICO_RMII_SM_RX);	// trigger first sm
	DBG("Trigger RX SM");
#endif
	while (1)	{	netif_rmii_ethernet_poll();	}
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

	timelapse_link(tl_crc);
	timelapse_link(tl_net);
	timelapse_link(tl_rx);
	timelapse_link(tl_tx);
}

