#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "types.h"
#include "ci40_net.h"

/* ------------------------ lwIP includes --------------------------------- */
#include "lwip/err.h"
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "lwip/debug.h"
#include "netif/etharp.h"



#define NUM_DMA_DESCS 8
#define ETH_BUFF_SIZE 2048
#define INTERRUPT_MASK 0x1FFFF

unsigned char eth_rx_buff[NUM_DMA_DESCS][ETH_BUFF_SIZE] __attribute__ ((aligned (16)));
unsigned char eth_tx_buff[NUM_DMA_DESCS][ETH_BUFF_SIZE] __attribute__ ((aligned (16)));
struct tx_dma_desc dma_tx_desc[NUM_DMA_DESCS] __attribute__ ((aligned (16)));
struct rx_dma_desc dma_rx_desc[NUM_DMA_DESCS] __attribute__ ((aligned (16)));
int current_tx_descriptor = 0;
int current_rx_descriptor = 0;
int rx_count = 0;

//static const struct uip_eth_addr ethmac = {{0x00,0x19,0xF5,0xFF,0xFF,0xF0}};

#define VIRT2PHYS(addr) (addr & 0x0FFFFFFF)

u32 MII_regs[0x20];


#ifdef CI40_GMAC_DEBUG
#define DBG(fmt, args...)  printk(fmt, ## args)
#else
#define DBG(fmt, args...)  do { } while (0)
#endif

void vEthRxISR(void);

void read_mii_regs(void)
{
	int i;

	// Read MII regs into array
	for (i = 0; i < 0x20; i++) {
		writel((i << 6) | 1, DANUBE_NET_BASE_ADDR + GMAC_MII_ADDR);
		while (readl(DANUBE_NET_BASE_ADDR + GMAC_MII_ADDR) & 1);
		MII_regs[i] = readl(DANUBE_NET_BASE_ADDR + GMAC_MII_DATA);
	}
}

void ci40_net_init(u8 *mac_addr)
{
	int i;
	u16 addr[2];
	u16 ip_host[] = { 192, 168, 154, 10 };
	u16 ip_router[] = { 192, 168, 154, 252 };
	u16 ip_mask[] = { 255, 255, 255, 0 };
	//u8 mac_addr[] = {MAC_ADDR};
    u32 int_num = 5;
    u32 ext_int = ETH_INT_NUM;

	// Initialise DMA descriptors
	for (i = 0; i < NUM_DMA_DESCS; i ++) {

		dma_tx_desc[i].des3 = VIRT2PHYS((u32)&dma_tx_desc[(i+1) % NUM_DMA_DESCS]);
		dma_tx_desc[i].des2 = VIRT2PHYS((u32)&eth_tx_buff[i]);
		dma_tx_desc[i].des1.second_address_chained = 1;
		dma_tx_desc[i].des0.own = 0;

		dma_rx_desc[i].des3 = VIRT2PHYS((u32)&dma_rx_desc[(i+1) % NUM_DMA_DESCS]);
		dma_rx_desc[i].des2 = VIRT2PHYS((u32)&eth_rx_buff[i]);
		dma_rx_desc[i].des1.buffer1_size = 0x7F0;
		dma_rx_desc[i].des1.second_address_chained = 1;
		dma_rx_desc[i].des0.own = 1;

	}

	ci40_gmac_dma_init(DANUBE_NET_BASE_ADDR, 0, (u32)dma_tx_desc & 0x0FFFFFFF, (u32)dma_rx_desc & 0x0FFFFFFF);
	ci40_gmac_set_umac_addr(DANUBE_NET_BASE_ADDR, mac_addr,0);
	ci40_gmac_dma_operation_mode(DANUBE_NET_BASE_ADDR, SF_DMA_MODE, SF_DMA_MODE);
	ci40_gmac_core_init(DANUBE_NET_BASE_ADDR);

	// Reset the PHY
	writel( 0x8000, DANUBE_NET_BASE_ADDR + GMAC_MII_DATA);
	writel( 0x3 | (5 << 2), DANUBE_NET_BASE_ADDR + GMAC_MII_ADDR);
	while (readl(DANUBE_NET_BASE_ADDR + GMAC_MII_ADDR) & 1);
	writel( 0x8000, DANUBE_NET_BASE_ADDR + GMAC_MII_DATA);
	writel( 0x3 | (5 << 2), DANUBE_NET_BASE_ADDR + GMAC_MII_ADDR);
	while (readl(DANUBE_NET_BASE_ADDR + GMAC_MII_ADDR) & 1);

    pvPortInstallISR( int_num, vEthRxISR );
    vLevelTrigExternalNonEicInterrupt( ext_int, 1 );
    vRouteExternalNonEicInterrupt( ext_int, 1, int_num - 2 );
}

void ci40_net_send(s8 *data, s32 len)
{
	/* wait for a space */
	if (dma_tx_desc[current_tx_descriptor].des0.own) {
		writel(0xFFFFFFFF,DANUBE_NET_BASE_ADDR + DMA_XMT_POLL_DEMAND);
		return;
	}

	/* copy header into buffer */
	memcpy(eth_tx_buff[current_tx_descriptor],data,54);

	/* Copy data, if applicable */
	if (len > 54) {
		memcpy(eth_tx_buff[current_tx_descriptor]+54,(void*)&data[54],len - 54);
	}
	mips_flush_dcache();
	dma_tx_desc[current_tx_descriptor].des1.buffer1_size = len;
	dma_tx_desc[current_tx_descriptor].des1.first_segment = 1;
	dma_tx_desc[current_tx_descriptor].des1.last_segment = 1;
	dma_tx_desc[current_tx_descriptor].des0.own = 1;
	mips_flush_dcache();	
	writel(0xFFFFFFFF,DANUBE_NET_BASE_ADDR + DMA_XMT_POLL_DEMAND);
	current_tx_descriptor ++;
	current_tx_descriptor %= NUM_DMA_DESCS;
}

int ci40_net_len(void)
{
	int i,len;

	for (i = 0; i < NUM_DMA_DESCS; i ++) {
		// If filled
		mips_flush_dcache();
		if (dma_rx_desc[i].des0.own == 0) {
			len = dma_rx_desc[i].des0.frame_length;
			// return size
			current_rx_descriptor = i;
			return len;
		}
	}
	return 0;
}

void ci40_net_dump(void)
{
	int rx = current_rx_descriptor;

	// If filled
	if (dma_rx_desc[rx].des0.own == 0) {
		// copy data to uip_buf
		mips_flush_dcache();
		memset(&dma_rx_desc[rx].des0,0,sizeof(u32));
		dma_rx_desc[rx].des0.own = 1;
	}
}

void ci40_net_read(s8 *data, s32 len)
{
	int rx = current_rx_descriptor;

	// If filled
	if (dma_rx_desc[rx].des0.own == 0) {
		rx_count ++;
		// copy data to uip_buf
		memcpy(data, eth_rx_buff[rx], len);
		memset(&dma_rx_desc[rx].des0,0,sizeof(u32));
		dma_rx_desc[rx].des0.own = 1;
		current_rx_descriptor ++;
		current_rx_descriptor %= NUM_DMA_DESCS;
	}
}

s32 ci40_gmac_dma_init(u32 ioaddr, s32 pbl, u32 dma_tx, u32 dma_rx)
{
	u32 value = readl(ioaddr + DMA_BUS_MODE);
	/* DMA SW reset */
	value |= DMA_BUS_MODE_SFT_RESET;
	writel(value, ioaddr + DMA_BUS_MODE);
	do {} while ((readl(ioaddr + DMA_BUS_MODE) & DMA_BUS_MODE_SFT_RESET));

	value = /* DMA_BUS_MODE_FB | */ DMA_BUS_MODE_4PBL |
	    ((pbl << DMA_BUS_MODE_PBL_SHIFT) |
	     (pbl << DMA_BUS_MODE_RPBL_SHIFT));

#ifdef CONFIG_STMMAC_DA
	value |= DMA_BUS_MODE_DA;	/* Rx has priority over tx */
#endif
	value |= 4 << DMA_BUS_MODE_DSL_SHIFT;
	writel(value, ioaddr + DMA_BUS_MODE);

	/* Mask interrupts by writing to CSR7 */
	writel(DMA_INTR_DEFAULT_MASK, ioaddr + DMA_INTR_ENA);

	value = readl(ioaddr + DMA_CONTROL);
	value &= ~(DMA_CONTROL_SR | DMA_CONTROL_ST);
	writel(value, ioaddr + DMA_CONTROL);
 
	/* The base address of the RX/TX descriptor lists must be written into
	 * DMA CSR3 and CSR4, respectively. */
	writel(dma_tx, ioaddr + DMA_TX_BASE_ADDR);
	writel(dma_rx, ioaddr + DMA_RCV_BASE_ADDR);

	value = readl(ioaddr + DMA_CONTROL);
	value |= DMA_CONTROL_SR | DMA_CONTROL_ST | DMA_CONTROL_DT | DMA_CONTROL_TSF | DMA_CONTROL_RSF | DMA_CONTROL_FUF | DMA_CONTROL_RTC_32;
	writel(value, ioaddr + DMA_CONTROL);

	value = readl(ioaddr + DMA_INTR_ENA);
	value |= INTERRUPT_MASK;
	writel(value, ioaddr + DMA_INTR_ENA);

	return 0;
}

void ci40_gmac_core_init(u32 ioaddr)
{
	u32 value;

	value = readl(ioaddr + GMAC_CONTROL);
	value |= GMAC_CONTROL_JD;	// Jabber disable
	value |= GMAC_CONTROL_ACS;	// Automatic Pad Stripping
	value |= GMAC_CONTROL_IPC;	// Checksum Offload
	value |= GMAC_CONTROL_JE;	// Jumbo frame
	value |= GMAC_CONTROL_BE;	// Frame Burst Enable
    value |= GMAC_CONTROL_FES;	// Speed 0:10 1:100
    value |= GMAC_CONTROL_LUD;	// Link up/down
    value |= GMAC_CONTROL_RE;	// Receiver Enable
    value |= GMAC_CONTROL_TE;	// Transmitter Enable
	writel(value, ioaddr + GMAC_CONTROL);

#ifdef STMMAC_VLAN_TAG_USED
	/* Tag detection without filtering */
	writel(0x0, ioaddr + GMAC_VLAN_TAG);
#endif

	writel(0xffffffff, ioaddr + GMAC_HASH_HIGH);
	writel(0xffffffff, ioaddr + GMAC_HASH_LOW);

	return;
}


void ci40_gmac_set_umac_addr(u32 ioaddr, u8 *addr, u32 reg_n)
{
	stmmac_set_mac_addr(ioaddr, addr, GMAC_ADDR_HIGH(reg_n),
		GMAC_ADDR_LOW(reg_n));

}

void ci40_gmac_dma_operation_mode(u32 ioaddr, s32 txmode, s32 rxmode)
{
	u32 csr6 = readl(ioaddr + DMA_CONTROL);

	if (txmode == SF_DMA_MODE) {
		DBG(KERN_DEBUG "GMAC: enabling TX store and forward mode\n");
		/* Transmit COE type 2 cannot be done in cut-through mode. */
		csr6 |= DMA_CONTROL_TSF;
		/* Operating on second frame increase the performance
		 * especially when transmit store-and-forward is used.*/
		csr6 |= DMA_CONTROL_OSF;
	} else {
		DBG(KERN_DEBUG "GMAC: disabling TX store and forward mode"
			      " (threshold = %d)\n", txmode);
		csr6 &= ~DMA_CONTROL_TSF;
		csr6 &= DMA_CONTROL_TC_TX_MASK;
		/* Set the transmit threashold */
		if (txmode <= 32)
			csr6 |= DMA_CONTROL_TTC_32;
		else if (txmode <= 64)
			csr6 |= DMA_CONTROL_TTC_64;
		else if (txmode <= 128)
			csr6 |= DMA_CONTROL_TTC_128;
		else if (txmode <= 192)
			csr6 |= DMA_CONTROL_TTC_192;
		else
			csr6 |= DMA_CONTROL_TTC_256;
	}

	if (rxmode == SF_DMA_MODE) {
		DBG(KERN_DEBUG "GMAC: enabling RX store and forward mode\n");
		csr6 |= DMA_CONTROL_RSF;
	} else {
		DBG(KERN_DEBUG "GMAC: disabling RX store and forward mode"
			      " (threshold = %d)\n", rxmode);
		csr6 &= ~DMA_CONTROL_RSF;
		csr6 &= DMA_CONTROL_TC_RX_MASK;
		if (rxmode <= 32)
			csr6 |= DMA_CONTROL_RTC_32;
		else if (rxmode <= 64)
			csr6 |= DMA_CONTROL_RTC_64;
		else if (rxmode <= 96)
			csr6 |= DMA_CONTROL_RTC_96;
		else
			csr6 |= DMA_CONTROL_RTC_128;
	}

	writel(csr6, ioaddr + DMA_CONTROL);
}

void ci40_gmac_dma_clear_int(void)
{
	u32 value;

	value = readl(DANUBE_NET_BASE_ADDR + DMA_STATUS);
	value |= INTERRUPT_MASK;
	writel(value, DANUBE_NET_BASE_ADDR + DMA_STATUS);
}
