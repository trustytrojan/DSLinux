/*
 * drivers/serial/mpsc.c
 *
 * Generic driver for the MPSC (UART mode) on Marvell parts (e.g., GT64240,
 * GT64260, MV64340, MV64360, GT96100, ... ).
 *
 * Author: Mark A. Greer <mgreer@mvista.com>
 *
 * Based on an old MPSC driver that was in the linuxppc tree.  It appears to
 * have been created by Chris Zankel (formerly of MontaVista) but there
 * is no proper Copyright so I'm not sure.  Apparently, parts were also
 * taken from PPCBoot (now U-Boot).  Also based on drivers/serial/8250.c
 * by Russell King.
 *
 * 2004 (c) MontaVista, Software, Inc.  This file is licensed under
 * the terms of the GNU General Public License version 2.  This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */
/*
 * The MPSC interface is much like a typical network controller's interface.
 * That is, you set up separate rings of descriptors for transmitting and
 * receiving data.  There is also a pool of buffers with (one buffer per
 * descriptor) that incoming data are dma'd into or outgoing data are dma'd
 * out of.
 *
 * The MPSC requires two other controllers to be able to work.  The Baud Rate
 * Generator (BRG) provides a clock at programmable frequencies which determines
 * the baud rate.  The Serial DMA Controller (SDMA) takes incoming data from the
 * MPSC and DMA's it into memory or DMA's outgoing data and passes it to the
 * MPSC.  It is actually the SDMA interrupt that the driver uses to keep the
 * transmit and receive "engines" going (i.e., indicate data has been
 * transmitted or received).
 *
 * NOTES:
 *
 * 1) Some chips have an erratum where several regs cannot be
 * read.  To work around that, we keep a local copy of those regs in
 * 'mpsc_port_info'.
 *
 * 2) Some chips have an erratum where the ctlr will hang when the SDMA ctlr
 * accesses system mem with coherency enabled.  For that reason, the driver
 * assumes that coherency for that ctlr has been disabled.  This means
 * that when in a cache coherent system, the driver has to manually manage
 * the data cache on the areas that it touches because the dma_* macro are
 * basically no-ops.
 *
 * 3) There is an erratum (on PPC) where you can't use the instruction to do
 * a DMA_TO_DEVICE/cache clean so DMA_BIDIRECTIONAL/flushes are used in places
 * where a DMA_TO_DEVICE/clean would have [otherwise] sufficed.
 *
 * 4) AFAICT, hardware flow control isn't supported by the controller --MAG.
 */

#include "mpsc.h"

/*
 * Define how this driver is known to the outside (we've been assigned a
 * range on the "Low-density serial ports" major).
 */
#define MPSC_MAJOR		204
#define MPSC_MINOR_START	44
#define	MPSC_DRIVER_NAME	"MPSC"
#define	MPSC_DEVFS_NAME		"ttymm/"
#define	MPSC_DEV_NAME		"ttyMM"
#define	MPSC_VERSION		"1.00"

static struct mpsc_port_info mpsc_ports[MPSC_NUM_CTLRS];
static struct mpsc_shared_regs mpsc_shared_regs;
static struct uart_driver mpsc_reg;

static void mpsc_start_rx(struct mpsc_port_info *pi);
static void mpsc_free_ring_mem(struct mpsc_port_info *pi);
static void mpsc_release_port(struct uart_port *port);
/*
 ******************************************************************************
 *
 * Baud Rate Generator Routines (BRG)
 *
 ******************************************************************************
 */
static void
mpsc_brg_init(struct mpsc_port_info *pi, u32 clk_src)
{
	u32	v;

	v = (pi->mirror_regs) ? pi->BRG_BCR_m : readl(pi->brg_base + BRG_BCR);
	v = (v & ~(0xf << 18)) | ((clk_src & 0xf) << 18);

	if (pi->brg_can_tune)
		v &= ~(1 << 25);

	if (pi->mirror_regs)
		pi->BRG_BCR_m = v;
	writel(v, pi->brg_base + BRG_BCR);

	writel(readl(pi->brg_base + BRG_BTR) & 0xffff0000,
		pi->brg_base + BRG_BTR);
	return;
}

static void
mpsc_brg_enable(struct mpsc_port_info *pi)
{
	u32	v;

	v = (pi->mirror_regs) ? pi->BRG_BCR_m : readl(pi->brg_base + BRG_BCR);
	v |= (1 << 16);

	if (pi->mirror_regs)
		pi->BRG_BCR_m = v;
	writel(v, pi->brg_base + BRG_BCR);
	return;
}

static void
mpsc_brg_disable(struct mpsc_port_info *pi)
{
	u32	v;

	v = (pi->mirror_regs) ? pi->BRG_BCR_m : readl(pi->brg_base + BRG_BCR);
	v &= ~(1 << 16);

	if (pi->mirror_regs)
		pi->BRG_BCR_m = v;
	writel(v, pi->brg_base + BRG_BCR);
	return;
}

static inline void
mpsc_set_baudrate(struct mpsc_port_info *pi, u32 baud)
{
	/*
	 * To set the baud, we adjust the CDV field in the BRG_BCR reg.
	 * From manual: Baud = clk / ((CDV+1)*2) ==> CDV = (clk / (baud*2)) - 1.
	 * However, the input clock is divided by 16 in the MPSC b/c of how
	 * 'MPSC_MMCRH' was set up so we have to divide the 'clk' used in our
	 * calculation by 16 to account for that.  So the real calculation
	 * that accounts for the way the mpsc is set up is:
	 * CDV = (clk / (baud*2*16)) - 1 ==> CDV = (clk / (baud << 5)) - 1.
	 */
	u32	cdv = (pi->port.uartclk / (baud << 5)) - 1;
	u32	v;

	mpsc_brg_disable(pi);
	v = (pi->mirror_regs) ? pi->BRG_BCR_m : readl(pi->brg_base + BRG_BCR);
	v = (v & 0xffff0000) | (cdv & 0xffff);

	if (pi->mirror_regs)
		pi->BRG_BCR_m = v;
	writel(v, pi->brg_base + BRG_BCR);
	mpsc_brg_enable(pi);

	return;
}

/*
 ******************************************************************************
 *
 * Serial DMA Routines (SDMA)
 *
 ******************************************************************************
 */

static void
mpsc_sdma_burstsize(struct mpsc_port_info *pi, u32 burst_size)
{
	u32	v;

	pr_debug("mpsc_sdma_burstsize[%d]: burst_size: %d\n",
	    pi->port.line, burst_size);

	burst_size >>= 3; /* Divide by 8 b/c reg values are 8-byte chunks */

	if (burst_size < 2)
		v = 0x0;	/* 1 64-bit word */
	else if (burst_size < 4)
		v = 0x1;	/* 2 64-bit words */
	else if (burst_size < 8)
		v = 0x2;	/* 4 64-bit words */
	else
		v = 0x3;	/* 8 64-bit words */

	writel((readl(pi->sdma_base + SDMA_SDC) & (0x3 << 12)) | (v << 12),
		pi->sdma_base + SDMA_SDC);
	return;
}

static void
mpsc_sdma_init(struct mpsc_port_info *pi, u32 burst_size)
{
	pr_debug("mpsc_sdma_init[%d]: burst_size: %d\n", pi->port.line,
		burst_size);

	writel((readl(pi->sdma_base + SDMA_SDC) & 0x3ff) | 0x03f,
		pi->sdma_base + SDMA_SDC);
	mpsc_sdma_burstsize(pi, burst_size);
	return;
}

static inline u32
mpsc_sdma_intr_mask(struct mpsc_port_info *pi, u32 mask)
{
	u32	old, v;

	pr_debug("mpsc_sdma_intr_mask[%d]: mask: 0x%x\n", pi->port.line, mask);

	old = v = (pi->mirror_regs) ? pi->shared_regs->SDMA_INTR_MASK_m :
		readl(pi->shared_regs->sdma_intr_base + SDMA_INTR_MASK);

	mask &= 0xf;
	if (pi->port.line)
		mask <<= 8;
	v &= ~mask;

	if (pi->mirror_regs)
		pi->shared_regs->SDMA_INTR_MASK_m = v;
	writel(v, pi->shared_regs->sdma_intr_base + SDMA_INTR_MASK);

	if (pi->port.line)
		old >>= 8;
	return old & 0xf;
}

static inline void
mpsc_sdma_intr_unmask(struct mpsc_port_info *pi, u32 mask)
{
	u32	v;

	pr_debug("mpsc_sdma_intr_unmask[%d]: mask: 0x%x\n", pi->port.line,mask);

	v = (pi->mirror_regs) ? pi->shared_regs->SDMA_INTR_MASK_m :
		readl(pi->shared_regs->sdma_intr_base + SDMA_INTR_MASK);

	mask &= 0xf;
	if (pi->port.line)
		mask <<= 8;
	v |= mask;

	if (pi->mirror_regs)
		pi->shared_regs->SDMA_INTR_MASK_m = v;
	writel(v, pi->shared_regs->sdma_intr_base + SDMA_INTR_MASK);
	return;
}

static inline void
mpsc_sdma_intr_ack(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_sdma_intr_ack[%d]: Acknowledging IRQ\n", pi->port.line);

	if (pi->mirror_regs)
		pi->shared_regs->SDMA_INTR_CAUSE_m = 0;
	writel(0, pi->shared_regs->sdma_intr_base + SDMA_INTR_CAUSE);
	return;
}

static inline void
mpsc_sdma_set_rx_ring(struct mpsc_port_info *pi, struct mpsc_rx_desc *rxre_p)
{
	pr_debug("mpsc_sdma_set_rx_ring[%d]: rxre_p: 0x%x\n",
		pi->port.line, (u32) rxre_p);

	writel((u32)rxre_p, pi->sdma_base + SDMA_SCRDP);
	return;
}

static inline void
mpsc_sdma_set_tx_ring(struct mpsc_port_info *pi, struct mpsc_tx_desc *txre_p)
{
	writel((u32)txre_p, pi->sdma_base + SDMA_SFTDP);
	writel((u32)txre_p, pi->sdma_base + SDMA_SCTDP);
	return;
}

static inline void
mpsc_sdma_cmd(struct mpsc_port_info *pi, u32 val)
{
	u32	v;

	v = readl(pi->sdma_base + SDMA_SDCM);
	if (val)
		v |= val;
	else
		v = 0;
	wmb();
	writel(v, pi->sdma_base + SDMA_SDCM);
	wmb();
	return;
}

static inline uint
mpsc_sdma_tx_active(struct mpsc_port_info *pi)
{
	return readl(pi->sdma_base + SDMA_SDCM) & SDMA_SDCM_TXD;
}

static inline void
mpsc_sdma_start_tx(struct mpsc_port_info *pi)
{
	struct mpsc_tx_desc *txre, *txre_p;

	/* If tx isn't running & there's a desc ready to go, start it */
	if (!mpsc_sdma_tx_active(pi)) {
		txre = (struct mpsc_tx_desc *)(pi->txr +
			(pi->txr_tail * MPSC_TXRE_SIZE));
		dma_cache_sync((void *) txre, MPSC_TXRE_SIZE, DMA_FROM_DEVICE);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			invalidate_dcache_range((ulong)txre,
				(ulong)txre + MPSC_TXRE_SIZE);
#endif

		if (be32_to_cpu(txre->cmdstat) & SDMA_DESC_CMDSTAT_O) {
			txre_p = (struct mpsc_tx_desc *)(pi->txr_p +
							 (pi->txr_tail *
							  MPSC_TXRE_SIZE));

			mpsc_sdma_set_tx_ring(pi, txre_p);
			mpsc_sdma_cmd(pi, SDMA_SDCM_STD | SDMA_SDCM_TXD);
		}
	}

	return;
}

static inline void
mpsc_sdma_stop(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_sdma_stop[%d]: Stopping SDMA\n", pi->port.line);

	/* Abort any SDMA transfers */
	mpsc_sdma_cmd(pi, 0);
	mpsc_sdma_cmd(pi, SDMA_SDCM_AR | SDMA_SDCM_AT);

	/* Clear the SDMA current and first TX and RX pointers */
	mpsc_sdma_set_tx_ring(pi, NULL);
	mpsc_sdma_set_rx_ring(pi, NULL);

	/* Disable interrupts */
	mpsc_sdma_intr_mask(pi, 0xf);
	mpsc_sdma_intr_ack(pi);

	return;
}

/*
 ******************************************************************************
 *
 * Multi-Protocol Serial Controller Routines (MPSC)
 *
 ******************************************************************************
 */

static void
mpsc_hw_init(struct mpsc_port_info *pi)
{
	u32	v;

	pr_debug("mpsc_hw_init[%d]: Initializing hardware\n", pi->port.line);

	/* Set up clock routing */
	if (pi->mirror_regs) {
		v = pi->shared_regs->MPSC_MRR_m;
		v &= ~0x1c7;
		pi->shared_regs->MPSC_MRR_m = v;
		writel(v, pi->shared_regs->mpsc_routing_base + MPSC_MRR);

		v = pi->shared_regs->MPSC_RCRR_m;
		v = (v & ~0xf0f) | 0x100;
		pi->shared_regs->MPSC_RCRR_m = v;
		writel(v, pi->shared_regs->mpsc_routing_base + MPSC_RCRR);

		v = pi->shared_regs->MPSC_TCRR_m;
		v = (v & ~0xf0f) | 0x100;
		pi->shared_regs->MPSC_TCRR_m = v;
		writel(v, pi->shared_regs->mpsc_routing_base + MPSC_TCRR);
	}
	else {
		v = readl(pi->shared_regs->mpsc_routing_base + MPSC_MRR);
		v &= ~0x1c7;
		writel(v, pi->shared_regs->mpsc_routing_base + MPSC_MRR);

		v = readl(pi->shared_regs->mpsc_routing_base + MPSC_RCRR);
		v = (v & ~0xf0f) | 0x100;
		writel(v, pi->shared_regs->mpsc_routing_base + MPSC_RCRR);

		v = readl(pi->shared_regs->mpsc_routing_base + MPSC_TCRR);
		v = (v & ~0xf0f) | 0x100;
		writel(v, pi->shared_regs->mpsc_routing_base + MPSC_TCRR);
	}

	/* Put MPSC in UART mode & enabel Tx/Rx egines */
	writel(0x000004c4, pi->mpsc_base + MPSC_MMCRL);

	/* No preamble, 16x divider, low-latency,  */
	writel(0x04400400, pi->mpsc_base + MPSC_MMCRH);

	if (pi->mirror_regs) {
		pi->MPSC_CHR_1_m = 0;
		pi->MPSC_CHR_2_m = 0;
	}
	writel(0, pi->mpsc_base + MPSC_CHR_1);
	writel(0, pi->mpsc_base + MPSC_CHR_2);
	writel(pi->mpsc_max_idle, pi->mpsc_base + MPSC_CHR_3);
	writel(0, pi->mpsc_base + MPSC_CHR_4);
	writel(0, pi->mpsc_base + MPSC_CHR_5);
	writel(0, pi->mpsc_base + MPSC_CHR_6);
	writel(0, pi->mpsc_base + MPSC_CHR_7);
	writel(0, pi->mpsc_base + MPSC_CHR_8);
	writel(0, pi->mpsc_base + MPSC_CHR_9);
	writel(0, pi->mpsc_base + MPSC_CHR_10);

	return;
}

static inline void
mpsc_enter_hunt(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_enter_hunt[%d]: Hunting...\n", pi->port.line);

	if (pi->mirror_regs) {
		writel(pi->MPSC_CHR_2_m | MPSC_CHR_2_EH,
			pi->mpsc_base + MPSC_CHR_2);
		/* Erratum prevents reading CHR_2 so just delay for a while */
		udelay(100);
	}
	else {
		writel(readl(pi->mpsc_base + MPSC_CHR_2) | MPSC_CHR_2_EH,
			pi->mpsc_base + MPSC_CHR_2);

		while (readl(pi->mpsc_base + MPSC_CHR_2) & MPSC_CHR_2_EH)
			udelay(10);
	}

	return;
}

static inline void
mpsc_freeze(struct mpsc_port_info *pi)
{
	u32	v;

	pr_debug("mpsc_freeze[%d]: Freezing\n", pi->port.line);

	v = (pi->mirror_regs) ? pi->MPSC_MPCR_m :
		readl(pi->mpsc_base + MPSC_MPCR);
	v |= MPSC_MPCR_FRZ;

	if (pi->mirror_regs)
		pi->MPSC_MPCR_m = v;
	writel(v, pi->mpsc_base + MPSC_MPCR);
	return;
}

static inline void
mpsc_unfreeze(struct mpsc_port_info *pi)
{
	u32	v;

	v = (pi->mirror_regs) ? pi->MPSC_MPCR_m :
		readl(pi->mpsc_base + MPSC_MPCR);
	v &= ~MPSC_MPCR_FRZ;

	if (pi->mirror_regs)
		pi->MPSC_MPCR_m = v;
	writel(v, pi->mpsc_base + MPSC_MPCR);

	pr_debug("mpsc_unfreeze[%d]: Unfrozen\n", pi->port.line);
	return;
}

static inline void
mpsc_set_char_length(struct mpsc_port_info *pi, u32 len)
{
	u32	v;

	pr_debug("mpsc_set_char_length[%d]: char len: %d\n", pi->port.line,len);

	v = (pi->mirror_regs) ? pi->MPSC_MPCR_m :
		readl(pi->mpsc_base + MPSC_MPCR);
	v = (v & ~(0x3 << 12)) | ((len & 0x3) << 12);

	if (pi->mirror_regs)
		pi->MPSC_MPCR_m = v;
	writel(v, pi->mpsc_base + MPSC_MPCR);
	return;
}

static inline void
mpsc_set_stop_bit_length(struct mpsc_port_info *pi, u32 len)
{
	u32	v;

	pr_debug("mpsc_set_stop_bit_length[%d]: stop bits: %d\n",
		pi->port.line, len);

	v = (pi->mirror_regs) ? pi->MPSC_MPCR_m :
		readl(pi->mpsc_base + MPSC_MPCR);

	v = (v & ~(1 << 14)) | ((len & 0x1) << 14);

	if (pi->mirror_regs)
		pi->MPSC_MPCR_m = v;
	writel(v, pi->mpsc_base + MPSC_MPCR);
	return;
}

static inline void
mpsc_set_parity(struct mpsc_port_info *pi, u32 p)
{
	u32	v;

	pr_debug("mpsc_set_parity[%d]: parity bits: 0x%x\n", pi->port.line, p);

	v = (pi->mirror_regs) ? pi->MPSC_CHR_2_m :
		readl(pi->mpsc_base + MPSC_CHR_2);

	p &= 0x3;
	v = (v & ~0xc000c) | (p << 18) | (p << 2);

	if (pi->mirror_regs)
		pi->MPSC_CHR_2_m = v;
	writel(v, pi->mpsc_base + MPSC_CHR_2);
	return;
}

/*
 ******************************************************************************
 *
 * Driver Init Routines
 *
 ******************************************************************************
 */

static void
mpsc_init_hw(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_init_hw[%d]: Initializing\n", pi->port.line);

	mpsc_brg_init(pi, pi->brg_clk_src);
	mpsc_brg_enable(pi);
	mpsc_sdma_init(pi, dma_get_cache_alignment());	/* burst a cacheline */
	mpsc_sdma_stop(pi);
	mpsc_hw_init(pi);

	return;
}

static int
mpsc_alloc_ring_mem(struct mpsc_port_info *pi)
{
	int rc = 0;

	pr_debug("mpsc_alloc_ring_mem[%d]: Allocating ring mem\n",
		pi->port.line);

	if (!pi->dma_region) {
		if (!dma_supported(pi->port.dev, 0xffffffff)) {
			printk(KERN_ERR "MPSC: Inadequate DMA support\n");
			rc = -ENXIO;
		}
		else if ((pi->dma_region = dma_alloc_noncoherent(pi->port.dev,
			MPSC_DMA_ALLOC_SIZE, &pi->dma_region_p, GFP_KERNEL))
			== NULL) {

			printk(KERN_ERR "MPSC: Can't alloc Desc region\n");
			rc = -ENOMEM;
		}
	}

	return rc;
}

static void
mpsc_free_ring_mem(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_free_ring_mem[%d]: Freeing ring mem\n", pi->port.line);

	if (pi->dma_region) {
		dma_free_noncoherent(pi->port.dev, MPSC_DMA_ALLOC_SIZE,
			  pi->dma_region, pi->dma_region_p);
		pi->dma_region = NULL;
		pi->dma_region_p = (dma_addr_t) NULL;
	}

	return;
}

static void
mpsc_init_rings(struct mpsc_port_info *pi)
{
	struct mpsc_rx_desc *rxre;
	struct mpsc_tx_desc *txre;
	dma_addr_t dp, dp_p;
	u8 *bp, *bp_p;
	int i;

	pr_debug("mpsc_init_rings[%d]: Initializing rings\n", pi->port.line);

	BUG_ON(pi->dma_region == NULL);

	memset(pi->dma_region, 0, MPSC_DMA_ALLOC_SIZE);

	/*
	 * Descriptors & buffers are multiples of cacheline size and must be
	 * cacheline aligned.
	 */
	dp = ALIGN((u32) pi->dma_region, dma_get_cache_alignment());
	dp_p = ALIGN((u32) pi->dma_region_p, dma_get_cache_alignment());

	/*
	 * Partition dma region into rx ring descriptor, rx buffers,
	 * tx ring descriptors, and tx buffers.
	 */
	pi->rxr = dp;
	pi->rxr_p = dp_p;
	dp += MPSC_RXR_SIZE;
	dp_p += MPSC_RXR_SIZE;

	pi->rxb = (u8 *) dp;
	pi->rxb_p = (u8 *) dp_p;
	dp += MPSC_RXB_SIZE;
	dp_p += MPSC_RXB_SIZE;

	pi->rxr_posn = 0;

	pi->txr = dp;
	pi->txr_p = dp_p;
	dp += MPSC_TXR_SIZE;
	dp_p += MPSC_TXR_SIZE;

	pi->txb = (u8 *) dp;
	pi->txb_p = (u8 *) dp_p;

	pi->txr_head = 0;
	pi->txr_tail = 0;

	/* Init rx ring descriptors */
	dp = pi->rxr;
	dp_p = pi->rxr_p;
	bp = pi->rxb;
	bp_p = pi->rxb_p;

	for (i = 0; i < MPSC_RXR_ENTRIES; i++) {
		rxre = (struct mpsc_rx_desc *)dp;

		rxre->bufsize = cpu_to_be16(MPSC_RXBE_SIZE);
		rxre->bytecnt = cpu_to_be16(0);
		rxre->cmdstat = cpu_to_be32(SDMA_DESC_CMDSTAT_O |
					    SDMA_DESC_CMDSTAT_EI |
					    SDMA_DESC_CMDSTAT_F |
					    SDMA_DESC_CMDSTAT_L);
		rxre->link = cpu_to_be32(dp_p + MPSC_RXRE_SIZE);
		rxre->buf_ptr = cpu_to_be32(bp_p);

		dp += MPSC_RXRE_SIZE;
		dp_p += MPSC_RXRE_SIZE;
		bp += MPSC_RXBE_SIZE;
		bp_p += MPSC_RXBE_SIZE;
	}
	rxre->link = cpu_to_be32(pi->rxr_p);	/* Wrap last back to first */

	/* Init tx ring descriptors */
	dp = pi->txr;
	dp_p = pi->txr_p;
	bp = pi->txb;
	bp_p = pi->txb_p;

	for (i = 0; i < MPSC_TXR_ENTRIES; i++) {
		txre = (struct mpsc_tx_desc *)dp;

		txre->link = cpu_to_be32(dp_p + MPSC_TXRE_SIZE);
		txre->buf_ptr = cpu_to_be32(bp_p);

		dp += MPSC_TXRE_SIZE;
		dp_p += MPSC_TXRE_SIZE;
		bp += MPSC_TXBE_SIZE;
		bp_p += MPSC_TXBE_SIZE;
	}
	txre->link = cpu_to_be32(pi->txr_p);	/* Wrap last back to first */

	dma_cache_sync((void *) pi->dma_region, MPSC_DMA_ALLOC_SIZE,
		DMA_BIDIRECTIONAL);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			flush_dcache_range((ulong)pi->dma_region,
				(ulong)pi->dma_region + MPSC_DMA_ALLOC_SIZE);
#endif

	return;
}

static void
mpsc_uninit_rings(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_uninit_rings[%d]: Uninitializing rings\n",pi->port.line);

	BUG_ON(pi->dma_region == NULL);

	pi->rxr = 0;
	pi->rxr_p = 0;
	pi->rxb = NULL;
	pi->rxb_p = NULL;
	pi->rxr_posn = 0;

	pi->txr = 0;
	pi->txr_p = 0;
	pi->txb = NULL;
	pi->txb_p = NULL;
	pi->txr_head = 0;
	pi->txr_tail = 0;

	return;
}

static int
mpsc_make_ready(struct mpsc_port_info *pi)
{
	int rc;

	pr_debug("mpsc_make_ready[%d]: Making cltr ready\n", pi->port.line);

	if (!pi->ready) {
		mpsc_init_hw(pi);
		if ((rc = mpsc_alloc_ring_mem(pi)))
			return rc;
		mpsc_init_rings(pi);
		pi->ready = 1;
	}

	return 0;
}

/*
 ******************************************************************************
 *
 * Interrupt Handling Routines
 *
 ******************************************************************************
 */

static inline int
mpsc_rx_intr(struct mpsc_port_info *pi, struct pt_regs *regs)
{
	struct mpsc_rx_desc *rxre;
	struct tty_struct *tty = pi->port.info->tty;
	u32	cmdstat, bytes_in, i;
	int	rc = 0;
	u8	*bp;
	char	flag = TTY_NORMAL;

	pr_debug("mpsc_rx_intr[%d]: Handling Rx intr\n", pi->port.line);

	rxre = (struct mpsc_rx_desc *)(pi->rxr + (pi->rxr_posn*MPSC_RXRE_SIZE));

	dma_cache_sync((void *)rxre, MPSC_RXRE_SIZE, DMA_FROM_DEVICE);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
	if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
		invalidate_dcache_range((ulong)rxre,
			(ulong)rxre + MPSC_RXRE_SIZE);
#endif

	/*
	 * Loop through Rx descriptors handling ones that have been completed.
	 */
	while (!((cmdstat = be32_to_cpu(rxre->cmdstat)) & SDMA_DESC_CMDSTAT_O)){
		bytes_in = be16_to_cpu(rxre->bytecnt);

		/* Following use of tty struct directly is deprecated */
		if (unlikely((tty->flip.count + bytes_in) >= TTY_FLIPBUF_SIZE)){
			if (tty->low_latency)
				tty_flip_buffer_push(tty);
			/*
			 * If this failed then we will throw awa the bytes
			 * but mst do so to clear interrupts.
			 */
		}

		bp = pi->rxb + (pi->rxr_posn * MPSC_RXBE_SIZE);
		dma_cache_sync((void *) bp, MPSC_RXBE_SIZE, DMA_FROM_DEVICE);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			invalidate_dcache_range((ulong)bp,
				(ulong)bp + MPSC_RXBE_SIZE);
#endif

		/*
		 * Other than for parity error, the manual provides little
		 * info on what data will be in a frame flagged by any of
		 * these errors.  For parity error, it is the last byte in
		 * the buffer that had the error.  As for the rest, I guess
		 * we'll assume there is no data in the buffer.
		 * If there is...it gets lost.
		 */
		if (unlikely(cmdstat & (SDMA_DESC_CMDSTAT_BR |
			SDMA_DESC_CMDSTAT_FR | SDMA_DESC_CMDSTAT_OR))) {

			pi->port.icount.rx++;

			if (cmdstat & SDMA_DESC_CMDSTAT_BR) {	/* Break */
				pi->port.icount.brk++;

				if (uart_handle_break(&pi->port))
					goto next_frame;
			}
			else if (cmdstat & SDMA_DESC_CMDSTAT_FR)/* Framing */
				pi->port.icount.frame++;
			else if (cmdstat & SDMA_DESC_CMDSTAT_OR) /* Overrun */
				pi->port.icount.overrun++;

			cmdstat &= pi->port.read_status_mask;

			if (cmdstat & SDMA_DESC_CMDSTAT_BR)
				flag = TTY_BREAK;
			else if (cmdstat & SDMA_DESC_CMDSTAT_FR)
				flag = TTY_FRAME;
			else if (cmdstat & SDMA_DESC_CMDSTAT_OR)
				flag = TTY_OVERRUN;
			else if (cmdstat & SDMA_DESC_CMDSTAT_PE)
				flag = TTY_PARITY;
		}

		if (uart_handle_sysrq_char(&pi->port, *bp, regs)) {
			bp++;
			bytes_in--;
			goto next_frame;
		}

		if ((unlikely(cmdstat & (SDMA_DESC_CMDSTAT_BR |
			SDMA_DESC_CMDSTAT_FR | SDMA_DESC_CMDSTAT_OR))) &&
			!(cmdstat & pi->port.ignore_status_mask))

			tty_insert_flip_char(tty, *bp, flag);
		else {
			for (i=0; i<bytes_in; i++)
				tty_insert_flip_char(tty, *bp++, TTY_NORMAL);

			pi->port.icount.rx += bytes_in;
		}

next_frame:
		rxre->bytecnt = cpu_to_be16(0);
		wmb();
		rxre->cmdstat = cpu_to_be32(SDMA_DESC_CMDSTAT_O |
					    SDMA_DESC_CMDSTAT_EI |
					    SDMA_DESC_CMDSTAT_F |
					    SDMA_DESC_CMDSTAT_L);
		wmb();
		dma_cache_sync((void *)rxre, MPSC_RXRE_SIZE, DMA_BIDIRECTIONAL);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			flush_dcache_range((ulong)rxre,
				(ulong)rxre + MPSC_RXRE_SIZE);
#endif

		/* Advance to next descriptor */
		pi->rxr_posn = (pi->rxr_posn + 1) & (MPSC_RXR_ENTRIES - 1);
		rxre = (struct mpsc_rx_desc *)(pi->rxr +
			(pi->rxr_posn * MPSC_RXRE_SIZE));
		dma_cache_sync((void *)rxre, MPSC_RXRE_SIZE, DMA_FROM_DEVICE);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			invalidate_dcache_range((ulong)rxre,
				(ulong)rxre + MPSC_RXRE_SIZE);
#endif

		rc = 1;
	}

	/* Restart rx engine, if its stopped */
	if ((readl(pi->sdma_base + SDMA_SDCM) & SDMA_SDCM_ERD) == 0)
		mpsc_start_rx(pi);

	tty_flip_buffer_push(tty);
	return rc;
}

static inline void
mpsc_setup_tx_desc(struct mpsc_port_info *pi, u32 count, u32 intr)
{
	struct mpsc_tx_desc *txre;

	txre = (struct mpsc_tx_desc *)(pi->txr +
		(pi->txr_head * MPSC_TXRE_SIZE));

	txre->bytecnt = cpu_to_be16(count);
	txre->shadow = txre->bytecnt;
	wmb();			/* ensure cmdstat is last field updated */
	txre->cmdstat = cpu_to_be32(SDMA_DESC_CMDSTAT_O | SDMA_DESC_CMDSTAT_F |
				    SDMA_DESC_CMDSTAT_L | ((intr) ?
							   SDMA_DESC_CMDSTAT_EI
							   : 0));
	wmb();
	dma_cache_sync((void *) txre, MPSC_TXRE_SIZE, DMA_BIDIRECTIONAL);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
	if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
		flush_dcache_range((ulong)txre,
			(ulong)txre + MPSC_TXRE_SIZE);
#endif

	return;
}

static inline void
mpsc_copy_tx_data(struct mpsc_port_info *pi)
{
	struct circ_buf *xmit = &pi->port.info->xmit;
	u8 *bp;
	u32 i;

	/* Make sure the desc ring isn't full */
	while (CIRC_CNT(pi->txr_head, pi->txr_tail, MPSC_TXR_ENTRIES) <
	       (MPSC_TXR_ENTRIES - 1)) {
		if (pi->port.x_char) {
			/*
			 * Ideally, we should use the TCS field in
			 * CHR_1 to put the x_char out immediately but
			 * errata prevents us from being able to read
			 * CHR_2 to know that its safe to write to
			 * CHR_1.  Instead, just put it in-band with
			 * all the other Tx data.
			 */
			bp = pi->txb + (pi->txr_head * MPSC_TXBE_SIZE);
			*bp = pi->port.x_char;
			pi->port.x_char = 0;
			i = 1;
		}
		else if (!uart_circ_empty(xmit) && !uart_tx_stopped(&pi->port)){
			i = min((u32) MPSC_TXBE_SIZE,
				(u32) uart_circ_chars_pending(xmit));
			i = min(i, (u32) CIRC_CNT_TO_END(xmit->head, xmit->tail,
				UART_XMIT_SIZE));
			bp = pi->txb + (pi->txr_head * MPSC_TXBE_SIZE);
			memcpy(bp, &xmit->buf[xmit->tail], i);
			xmit->tail = (xmit->tail + i) & (UART_XMIT_SIZE - 1);

			if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
				uart_write_wakeup(&pi->port);
		}
		else /* All tx data copied into ring bufs */
			return;

		dma_cache_sync((void *) bp, MPSC_TXBE_SIZE, DMA_BIDIRECTIONAL);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			flush_dcache_range((ulong)bp,
				(ulong)bp + MPSC_TXBE_SIZE);
#endif
		mpsc_setup_tx_desc(pi, i, 1);

		/* Advance to next descriptor */
		pi->txr_head = (pi->txr_head + 1) & (MPSC_TXR_ENTRIES - 1);
	}

	return;
}

static inline int
mpsc_tx_intr(struct mpsc_port_info *pi)
{
	struct mpsc_tx_desc *txre;
	int rc = 0;

	if (!mpsc_sdma_tx_active(pi)) {
		txre = (struct mpsc_tx_desc *)(pi->txr +
			(pi->txr_tail * MPSC_TXRE_SIZE));

		dma_cache_sync((void *) txre, MPSC_TXRE_SIZE, DMA_FROM_DEVICE);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			invalidate_dcache_range((ulong)txre,
				(ulong)txre + MPSC_TXRE_SIZE);
#endif

		while (!(be32_to_cpu(txre->cmdstat) & SDMA_DESC_CMDSTAT_O)) {
			rc = 1;
			pi->port.icount.tx += be16_to_cpu(txre->bytecnt);
			pi->txr_tail = (pi->txr_tail+1) & (MPSC_TXR_ENTRIES-1);

			/* If no more data to tx, fall out of loop */
			if (pi->txr_head == pi->txr_tail)
				break;

			txre = (struct mpsc_tx_desc *)(pi->txr +
				(pi->txr_tail * MPSC_TXRE_SIZE));
			dma_cache_sync((void *) txre, MPSC_TXRE_SIZE,
				DMA_FROM_DEVICE);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
			if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
				invalidate_dcache_range((ulong)txre,
					(ulong)txre + MPSC_TXRE_SIZE);
#endif
		}

		mpsc_copy_tx_data(pi);
		mpsc_sdma_start_tx(pi);	/* start next desc if ready */
	}

	return rc;
}

/*
 * This is the driver's interrupt handler.  To avoid a race, we first clear
 * the interrupt, then handle any completed Rx/Tx descriptors.  When done
 * handling those descriptors, we restart the Rx/Tx engines if they're stopped.
 */
static irqreturn_t
mpsc_sdma_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct mpsc_port_info *pi = dev_id;
	ulong iflags;
	int rc = IRQ_NONE;

	pr_debug("mpsc_sdma_intr[%d]: SDMA Interrupt Received\n",pi->port.line);

	spin_lock_irqsave(&pi->port.lock, iflags);
	mpsc_sdma_intr_ack(pi);
	if (mpsc_rx_intr(pi, regs))
		rc = IRQ_HANDLED;
	if (mpsc_tx_intr(pi))
		rc = IRQ_HANDLED;
	spin_unlock_irqrestore(&pi->port.lock, iflags);

	pr_debug("mpsc_sdma_intr[%d]: SDMA Interrupt Handled\n", pi->port.line);
	return rc;
}

/*
 ******************************************************************************
 *
 * serial_core.c Interface routines
 *
 ******************************************************************************
 */
static uint
mpsc_tx_empty(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;
	ulong iflags;
	uint rc;

	spin_lock_irqsave(&pi->port.lock, iflags);
	rc = mpsc_sdma_tx_active(pi) ? 0 : TIOCSER_TEMT;
	spin_unlock_irqrestore(&pi->port.lock, iflags);

	return rc;
}

static void
mpsc_set_mctrl(struct uart_port *port, uint mctrl)
{
	/* Have no way to set modem control lines AFAICT */
	return;
}

static uint
mpsc_get_mctrl(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;
	u32 mflags, status;

	status = (pi->mirror_regs) ? pi->MPSC_CHR_10_m :
		readl(pi->mpsc_base + MPSC_CHR_10);

	mflags = 0;
	if (status & 0x1)
		mflags |= TIOCM_CTS;
	if (status & 0x2)
		mflags |= TIOCM_CAR;

	return mflags | TIOCM_DSR;	/* No way to tell if DSR asserted */
}

static void
mpsc_stop_tx(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;

	pr_debug("mpsc_stop_tx[%d]\n", port->line);

	mpsc_freeze(pi);
	return;
}

static void
mpsc_start_tx(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;

	mpsc_unfreeze(pi);
	mpsc_copy_tx_data(pi);
	mpsc_sdma_start_tx(pi);

	pr_debug("mpsc_start_tx[%d]\n", port->line);
	return;
}

static void
mpsc_start_rx(struct mpsc_port_info *pi)
{
	pr_debug("mpsc_start_rx[%d]: Starting...\n", pi->port.line);

	if (pi->rcv_data) {
		mpsc_enter_hunt(pi);
		mpsc_sdma_cmd(pi, SDMA_SDCM_ERD);
	}
	return;
}

static void
mpsc_stop_rx(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;

	pr_debug("mpsc_stop_rx[%d]: Stopping...\n", port->line);

	mpsc_sdma_cmd(pi, SDMA_SDCM_AR);
	return;
}

static void
mpsc_enable_ms(struct uart_port *port)
{
	return;			/* Not supported */
}

static void
mpsc_break_ctl(struct uart_port *port, int ctl)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;
	ulong	flags;
	u32	v;

	v = ctl ? 0x00ff0000 : 0;

	spin_lock_irqsave(&pi->port.lock, flags);
	if (pi->mirror_regs)
		pi->MPSC_CHR_1_m = v;
	writel(v, pi->mpsc_base + MPSC_CHR_1);
	spin_unlock_irqrestore(&pi->port.lock, flags);

	return;
}

static int
mpsc_startup(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;
	u32 flag = 0;
	int rc;

	pr_debug("mpsc_startup[%d]: Starting up MPSC, irq: %d\n",
		port->line, pi->port.irq);

	if ((rc = mpsc_make_ready(pi)) == 0) {
		/* Setup IRQ handler */
		mpsc_sdma_intr_ack(pi);

		/* If irq's are shared, need to set flag */
		if (mpsc_ports[0].port.irq == mpsc_ports[1].port.irq)
			flag = SA_SHIRQ;

		if (request_irq(pi->port.irq, mpsc_sdma_intr, flag,
				"mpsc/sdma", pi))
			printk(KERN_ERR "MPSC: Can't get SDMA IRQ %d\n",
			       pi->port.irq);

		mpsc_sdma_intr_unmask(pi, 0xf);
		mpsc_sdma_set_rx_ring(pi, (struct mpsc_rx_desc *)(pi->rxr_p +
			(pi->rxr_posn * MPSC_RXRE_SIZE)));
	}

	return rc;
}

static void
mpsc_shutdown(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;

	pr_debug("mpsc_shutdown[%d]: Shutting down MPSC\n", port->line);

	mpsc_sdma_stop(pi);
	free_irq(pi->port.irq, pi);
	return;
}

static void
mpsc_set_termios(struct uart_port *port, struct termios *termios,
		 struct termios *old)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;
	u32 baud;
	ulong flags;
	u32 chr_bits, stop_bits, par;

	pi->c_iflag = termios->c_iflag;
	pi->c_cflag = termios->c_cflag;

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		chr_bits = MPSC_MPCR_CL_5;
		break;
	case CS6:
		chr_bits = MPSC_MPCR_CL_6;
		break;
	case CS7:
		chr_bits = MPSC_MPCR_CL_7;
		break;
	case CS8:
	default:
		chr_bits = MPSC_MPCR_CL_8;
		break;
	}

	if (termios->c_cflag & CSTOPB)
		stop_bits = MPSC_MPCR_SBL_2;
	else
		stop_bits = MPSC_MPCR_SBL_1;

	par = MPSC_CHR_2_PAR_EVEN;
	if (termios->c_cflag & PARENB)
		if (termios->c_cflag & PARODD)
			par = MPSC_CHR_2_PAR_ODD;
#ifdef	CMSPAR
		if (termios->c_cflag & CMSPAR) {
			if (termios->c_cflag & PARODD)
				par = MPSC_CHR_2_PAR_MARK;
			else
				par = MPSC_CHR_2_PAR_SPACE;
		}
#endif

	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk);

	spin_lock_irqsave(&pi->port.lock, flags);

	uart_update_timeout(port, termios->c_cflag, baud);

	mpsc_set_char_length(pi, chr_bits);
	mpsc_set_stop_bit_length(pi, stop_bits);
	mpsc_set_parity(pi, par);
	mpsc_set_baudrate(pi, baud);

	/* Characters/events to read */
	pi->rcv_data = 1;
	pi->port.read_status_mask = SDMA_DESC_CMDSTAT_OR;

	if (termios->c_iflag & INPCK)
		pi->port.read_status_mask |= SDMA_DESC_CMDSTAT_PE |
		    SDMA_DESC_CMDSTAT_FR;

	if (termios->c_iflag & (BRKINT | PARMRK))
		pi->port.read_status_mask |= SDMA_DESC_CMDSTAT_BR;

	/* Characters/events to ignore */
	pi->port.ignore_status_mask = 0;

	if (termios->c_iflag & IGNPAR)
		pi->port.ignore_status_mask |= SDMA_DESC_CMDSTAT_PE |
		    SDMA_DESC_CMDSTAT_FR;

	if (termios->c_iflag & IGNBRK) {
		pi->port.ignore_status_mask |= SDMA_DESC_CMDSTAT_BR;

		if (termios->c_iflag & IGNPAR)
			pi->port.ignore_status_mask |= SDMA_DESC_CMDSTAT_OR;
	}

	/* Ignore all chars if CREAD not set */
	if (!(termios->c_cflag & CREAD))
		pi->rcv_data = 0;
	else
		mpsc_start_rx(pi);

	spin_unlock_irqrestore(&pi->port.lock, flags);
	return;
}

static const char *
mpsc_type(struct uart_port *port)
{
	pr_debug("mpsc_type[%d]: port type: %s\n", port->line,MPSC_DRIVER_NAME);
	return MPSC_DRIVER_NAME;
}

static int
mpsc_request_port(struct uart_port *port)
{
	/* Should make chip/platform specific call */
	return 0;
}

static void
mpsc_release_port(struct uart_port *port)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;

	if (pi->ready) {
		mpsc_uninit_rings(pi);
		mpsc_free_ring_mem(pi);
		pi->ready = 0;
	}

	return;
}

static void
mpsc_config_port(struct uart_port *port, int flags)
{
	return;
}

static int
mpsc_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	struct mpsc_port_info *pi = (struct mpsc_port_info *)port;
	int rc = 0;

	pr_debug("mpsc_verify_port[%d]: Verifying port data\n", pi->port.line);

	if (ser->type != PORT_UNKNOWN && ser->type != PORT_MPSC)
		rc = -EINVAL;
	else if (pi->port.irq != ser->irq)
		rc = -EINVAL;
	else if (ser->io_type != SERIAL_IO_MEM)
		rc = -EINVAL;
	else if (pi->port.uartclk / 16 != ser->baud_base) /* Not sure */
		rc = -EINVAL;
	else if ((void *)pi->port.mapbase != ser->iomem_base)
		rc = -EINVAL;
	else if (pi->port.iobase != ser->port)
		rc = -EINVAL;
	else if (ser->hub6 != 0)
		rc = -EINVAL;

	return rc;
}

static struct uart_ops mpsc_pops = {
	.tx_empty     = mpsc_tx_empty,
	.set_mctrl    = mpsc_set_mctrl,
	.get_mctrl    = mpsc_get_mctrl,
	.stop_tx      = mpsc_stop_tx,
	.start_tx     = mpsc_start_tx,
	.stop_rx      = mpsc_stop_rx,
	.enable_ms    = mpsc_enable_ms,
	.break_ctl    = mpsc_break_ctl,
	.startup      = mpsc_startup,
	.shutdown     = mpsc_shutdown,
	.set_termios  = mpsc_set_termios,
	.type         = mpsc_type,
	.release_port = mpsc_release_port,
	.request_port = mpsc_request_port,
	.config_port  = mpsc_config_port,
	.verify_port  = mpsc_verify_port,
};

/*
 ******************************************************************************
 *
 * Console Interface Routines
 *
 ******************************************************************************
 */

#ifdef CONFIG_SERIAL_MPSC_CONSOLE
static void
mpsc_console_write(struct console *co, const char *s, uint count)
{
	struct mpsc_port_info *pi = &mpsc_ports[co->index];
	u8 *bp, *dp, add_cr = 0;
	int i;

	while (mpsc_sdma_tx_active(pi))
		udelay(100);

	while (count > 0) {
		bp = dp = pi->txb + (pi->txr_head * MPSC_TXBE_SIZE);

		for (i = 0; i < MPSC_TXBE_SIZE; i++) {
			if (count == 0)
				break;

			if (add_cr) {
				*(dp++) = '\r';
				add_cr = 0;
			}
			else {
				*(dp++) = *s;

				if (*(s++) == '\n') { /* add '\r' after '\n' */
					add_cr = 1;
					count++;
				}
			}

			count--;
		}

		dma_cache_sync((void *) bp, MPSC_TXBE_SIZE, DMA_BIDIRECTIONAL);
#if defined(CONFIG_PPC32) && !defined(CONFIG_NOT_COHERENT_CACHE)
		if (pi->cache_mgmt) /* GT642[46]0 Res #COMM-2 */
			flush_dcache_range((ulong)bp,
				(ulong)bp + MPSC_TXBE_SIZE);
#endif
		mpsc_setup_tx_desc(pi, i, 0);
		pi->txr_head = (pi->txr_head + 1) & (MPSC_TXR_ENTRIES - 1);
		mpsc_sdma_start_tx(pi);

		while (mpsc_sdma_tx_active(pi))
			udelay(100);

		pi->txr_tail = (pi->txr_tail + 1) & (MPSC_TXR_ENTRIES - 1);
	}

	return;
}

static int __init
mpsc_console_setup(struct console *co, char *options)
{
	struct mpsc_port_info *pi;
	int baud, bits, parity, flow;

	pr_debug("mpsc_console_setup[%d]: options: %s\n", co->index, options);

	if (co->index >= MPSC_NUM_CTLRS)
		co->index = 0;

	pi = &mpsc_ports[co->index];

	baud = pi->default_baud;
	bits = pi->default_bits;
	parity = pi->default_parity;
	flow = pi->default_flow;

	if (!pi->port.ops)
		return -ENODEV;

	spin_lock_init(&pi->port.lock);	/* Temporary fix--copied from 8250.c */

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(&pi->port, co, baud, parity, bits, flow);
}

static struct console mpsc_console = {
	.name   = MPSC_DEV_NAME,
	.write  = mpsc_console_write,
	.device = uart_console_device,
	.setup  = mpsc_console_setup,
	.flags  = CON_PRINTBUFFER,
	.index  = -1,
	.data   = &mpsc_reg,
};

static int __init
mpsc_late_console_init(void)
{
	pr_debug("mpsc_late_console_init: Enter\n");

	if (!(mpsc_console.flags & CON_ENABLED))
		register_console(&mpsc_console);
	return 0;
}

late_initcall(mpsc_late_console_init);

#define MPSC_CONSOLE	&mpsc_console
#else
#define MPSC_CONSOLE	NULL
#endif
/*
 ******************************************************************************
 *
 * Dummy Platform Driver to extract & map shared register regions
 *
 ******************************************************************************
 */
static void
mpsc_resource_err(char *s)
{
	printk(KERN_WARNING "MPSC: Platform device resource error in %s\n", s);
	return;
}

static int
mpsc_shared_map_regs(struct platform_device *pd)
{
	struct resource	*r;

	if ((r = platform_get_resource(pd, IORESOURCE_MEM,
		MPSC_ROUTING_BASE_ORDER)) && request_mem_region(r->start,
		MPSC_ROUTING_REG_BLOCK_SIZE, "mpsc_routing_regs")) {

		mpsc_shared_regs.mpsc_routing_base = ioremap(r->start,
			MPSC_ROUTING_REG_BLOCK_SIZE);
		mpsc_shared_regs.mpsc_routing_base_p = r->start;
	}
	else {
		mpsc_resource_err("MPSC routing base");
		return -ENOMEM;
	}

	if ((r = platform_get_resource(pd, IORESOURCE_MEM,
		MPSC_SDMA_INTR_BASE_ORDER)) && request_mem_region(r->start,
		MPSC_SDMA_INTR_REG_BLOCK_SIZE, "sdma_intr_regs")) {

		mpsc_shared_regs.sdma_intr_base = ioremap(r->start,
			MPSC_SDMA_INTR_REG_BLOCK_SIZE);
		mpsc_shared_regs.sdma_intr_base_p = r->start;
	}
	else {
		iounmap(mpsc_shared_regs.mpsc_routing_base);
		release_mem_region(mpsc_shared_regs.mpsc_routing_base_p,
			MPSC_ROUTING_REG_BLOCK_SIZE);
		mpsc_resource_err("SDMA intr base");
		return -ENOMEM;
	}

	return 0;
}

static void
mpsc_shared_unmap_regs(void)
{
	if (!mpsc_shared_regs.mpsc_routing_base) {
		iounmap(mpsc_shared_regs.mpsc_routing_base);
		release_mem_region(mpsc_shared_regs.mpsc_routing_base_p,
			MPSC_ROUTING_REG_BLOCK_SIZE);
	}
	if (!mpsc_shared_regs.sdma_intr_base) {
		iounmap(mpsc_shared_regs.sdma_intr_base);
		release_mem_region(mpsc_shared_regs.sdma_intr_base_p,
			MPSC_SDMA_INTR_REG_BLOCK_SIZE);
	}

	mpsc_shared_regs.mpsc_routing_base = NULL;
	mpsc_shared_regs.sdma_intr_base = NULL;

	mpsc_shared_regs.mpsc_routing_base_p = 0;
	mpsc_shared_regs.sdma_intr_base_p = 0;

	return;
}

static int
mpsc_shared_drv_probe(struct device *dev)
{
	struct platform_device		*pd = to_platform_device(dev);
	struct mpsc_shared_pdata	*pdata;
	int				 rc = -ENODEV;

	if (pd->id == 0) {
		if (!(rc = mpsc_shared_map_regs(pd)))  {
			pdata = (struct mpsc_shared_pdata *)dev->platform_data;

			mpsc_shared_regs.MPSC_MRR_m = pdata->mrr_val;
			mpsc_shared_regs.MPSC_RCRR_m= pdata->rcrr_val;
			mpsc_shared_regs.MPSC_TCRR_m= pdata->tcrr_val;
			mpsc_shared_regs.SDMA_INTR_CAUSE_m =
				pdata->intr_cause_val;
			mpsc_shared_regs.SDMA_INTR_MASK_m =
				pdata->intr_mask_val;

			rc = 0;
		}
	}

	return rc;
}

static int
mpsc_shared_drv_remove(struct device *dev)
{
	struct platform_device	*pd = to_platform_device(dev);
	int	rc = -ENODEV;

	if (pd->id == 0) {
		mpsc_shared_unmap_regs();
		mpsc_shared_regs.MPSC_MRR_m = 0;
		mpsc_shared_regs.MPSC_RCRR_m = 0;
		mpsc_shared_regs.MPSC_TCRR_m = 0;
		mpsc_shared_regs.SDMA_INTR_CAUSE_m = 0;
		mpsc_shared_regs.SDMA_INTR_MASK_m = 0;
		rc = 0;
	}

	return rc;
}

static struct device_driver mpsc_shared_driver = {
	.name	= MPSC_SHARED_NAME,
	.bus	= &platform_bus_type,
	.probe	= mpsc_shared_drv_probe,
	.remove	= mpsc_shared_drv_remove,
};

/*
 ******************************************************************************
 *
 * Driver Interface Routines
 *
 ******************************************************************************
 */
static struct uart_driver mpsc_reg = {
	.owner       = THIS_MODULE,
	.driver_name = MPSC_DRIVER_NAME,
	.devfs_name  = MPSC_DEVFS_NAME,
	.dev_name    = MPSC_DEV_NAME,
	.major       = MPSC_MAJOR,
	.minor       = MPSC_MINOR_START,
	.nr          = MPSC_NUM_CTLRS,
	.cons        = MPSC_CONSOLE,
};

static int
mpsc_drv_map_regs(struct mpsc_port_info *pi, struct platform_device *pd)
{
	struct resource	*r;

	if ((r = platform_get_resource(pd, IORESOURCE_MEM, MPSC_BASE_ORDER)) &&
		request_mem_region(r->start, MPSC_REG_BLOCK_SIZE, "mpsc_regs")){

		pi->mpsc_base = ioremap(r->start, MPSC_REG_BLOCK_SIZE);
		pi->mpsc_base_p = r->start;
	}
	else {
		mpsc_resource_err("MPSC base");
		return -ENOMEM;
	}

	if ((r = platform_get_resource(pd, IORESOURCE_MEM,
		MPSC_SDMA_BASE_ORDER)) && request_mem_region(r->start,
		MPSC_SDMA_REG_BLOCK_SIZE, "sdma_regs")) {

		pi->sdma_base = ioremap(r->start,MPSC_SDMA_REG_BLOCK_SIZE);
		pi->sdma_base_p = r->start;
	}
	else {
		mpsc_resource_err("SDMA base");
		return -ENOMEM;
	}

	if ((r = platform_get_resource(pd,IORESOURCE_MEM,MPSC_BRG_BASE_ORDER))
		&& request_mem_region(r->start, MPSC_BRG_REG_BLOCK_SIZE,
		"brg_regs")) {

		pi->brg_base = ioremap(r->start, MPSC_BRG_REG_BLOCK_SIZE);
		pi->brg_base_p = r->start;
	}
	else {
		mpsc_resource_err("BRG base");
		return -ENOMEM;
	}

	return 0;
}

static void
mpsc_drv_unmap_regs(struct mpsc_port_info *pi)
{
	if (!pi->mpsc_base) {
		iounmap(pi->mpsc_base);
		release_mem_region(pi->mpsc_base_p, MPSC_REG_BLOCK_SIZE);
	}
	if (!pi->sdma_base) {
		iounmap(pi->sdma_base);
		release_mem_region(pi->sdma_base_p, MPSC_SDMA_REG_BLOCK_SIZE);
	}
	if (!pi->brg_base) {
		iounmap(pi->brg_base);
		release_mem_region(pi->brg_base_p, MPSC_BRG_REG_BLOCK_SIZE);
	}

	pi->mpsc_base = NULL;
	pi->sdma_base = NULL;
	pi->brg_base = NULL;

	pi->mpsc_base_p = 0;
	pi->sdma_base_p = 0;
	pi->brg_base_p = 0;

	return;
}

static void
mpsc_drv_get_platform_data(struct mpsc_port_info *pi,
	struct platform_device *pd, int num)
{
	struct mpsc_pdata	*pdata;

	pdata = (struct mpsc_pdata *)pd->dev.platform_data;

	pi->port.uartclk = pdata->brg_clk_freq;
	pi->port.iotype = UPIO_MEM;
	pi->port.line = num;
	pi->port.type = PORT_MPSC;
	pi->port.fifosize = MPSC_TXBE_SIZE;
	pi->port.membase = pi->mpsc_base;
	pi->port.mapbase = (ulong)pi->mpsc_base;
	pi->port.ops = &mpsc_pops;

	pi->mirror_regs = pdata->mirror_regs;
	pi->cache_mgmt = pdata->cache_mgmt;
	pi->brg_can_tune = pdata->brg_can_tune;
	pi->brg_clk_src = pdata->brg_clk_src;
	pi->mpsc_max_idle = pdata->max_idle;
	pi->default_baud = pdata->default_baud;
	pi->default_bits = pdata->default_bits;
	pi->default_parity = pdata->default_parity;
	pi->default_flow = pdata->default_flow;

	/* Initial values of mirrored regs */
	pi->MPSC_CHR_1_m = pdata->chr_1_val;
	pi->MPSC_CHR_2_m = pdata->chr_2_val;
	pi->MPSC_CHR_10_m = pdata->chr_10_val;
	pi->MPSC_MPCR_m = pdata->mpcr_val;
	pi->BRG_BCR_m = pdata->bcr_val;

	pi->shared_regs = &mpsc_shared_regs;

	pi->port.irq = platform_get_irq(pd, 0);

	return;
}

static int
mpsc_drv_probe(struct device *dev)
{
	struct platform_device	*pd = to_platform_device(dev);
	struct mpsc_port_info	*pi;
	int			rc = -ENODEV;

	pr_debug("mpsc_drv_probe: Adding MPSC %d\n", pd->id);

	if (pd->id < MPSC_NUM_CTLRS) {
		pi = &mpsc_ports[pd->id];

		if (!(rc = mpsc_drv_map_regs(pi, pd))) {
			mpsc_drv_get_platform_data(pi, pd, pd->id);

			if (!(rc = mpsc_make_ready(pi)))
				if (!(rc = uart_add_one_port(&mpsc_reg,
					&pi->port)))
					rc = 0;
				else {
					mpsc_release_port(
						(struct uart_port *)pi);
					mpsc_drv_unmap_regs(pi);
				}
			else
				mpsc_drv_unmap_regs(pi);
		}
	}

	return rc;
}

static int
mpsc_drv_remove(struct device *dev)
{
	struct platform_device	*pd = to_platform_device(dev);

	pr_debug("mpsc_drv_exit: Removing MPSC %d\n", pd->id);

	if (pd->id < MPSC_NUM_CTLRS) {
		uart_remove_one_port(&mpsc_reg, &mpsc_ports[pd->id].port);
		mpsc_release_port((struct uart_port *)&mpsc_ports[pd->id].port);
		mpsc_drv_unmap_regs(&mpsc_ports[pd->id]);
		return 0;
	}
	else
		return -ENODEV;
}

static struct device_driver mpsc_driver = {
	.name	= MPSC_CTLR_NAME,
	.bus	= &platform_bus_type,
	.probe	= mpsc_drv_probe,
	.remove	= mpsc_drv_remove,
};

static int __init
mpsc_drv_init(void)
{
	int	rc;

	printk(KERN_INFO "Serial: MPSC driver $Revision$\n");

	memset(mpsc_ports, 0, sizeof(mpsc_ports));
	memset(&mpsc_shared_regs, 0, sizeof(mpsc_shared_regs));

	if (!(rc = uart_register_driver(&mpsc_reg))) {
		if (!(rc = driver_register(&mpsc_shared_driver))) {
			if ((rc = driver_register(&mpsc_driver))) {
				driver_unregister(&mpsc_shared_driver);
				uart_unregister_driver(&mpsc_reg);
			}
		}
		else
			uart_unregister_driver(&mpsc_reg);
	}

	return rc;

}

static void __exit
mpsc_drv_exit(void)
{
	driver_unregister(&mpsc_driver);
	driver_unregister(&mpsc_shared_driver);
	uart_unregister_driver(&mpsc_reg);
	memset(mpsc_ports, 0, sizeof(mpsc_ports));
	memset(&mpsc_shared_regs, 0, sizeof(mpsc_shared_regs));
	return;
}

module_init(mpsc_drv_init);
module_exit(mpsc_drv_exit);

MODULE_AUTHOR("Mark A. Greer <mgreer@mvista.com>");
MODULE_DESCRIPTION("Generic Marvell MPSC serial/UART driver $Revision$");
MODULE_VERSION(MPSC_VERSION);
MODULE_LICENSE("GPL");
MODULE_ALIAS_CHARDEV_MAJOR(MPSC_MAJOR);
