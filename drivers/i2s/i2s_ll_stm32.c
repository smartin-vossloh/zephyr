/*
 * Copyright (c) 2018 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_i2s

#include <string.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/dma/dma_stm32.h>
#include <soc.h>
#include <stm32_ll_rcc.h>
#include <stm32_ll_spi.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>

#include "i2s_ll_stm32.h"
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2s_ll_stm32);

#if __DCACHE_PRESENT == 1
#define DCACHE_INVALIDATE(addr, size) \
	SCB_InvalidateDCache_by_Addr((uint32_t *)addr, size)
#define DCACHE_CLEAN(addr, size) \
	SCB_CleanDCache_by_Addr((uint32_t *)addr, size)
#else
#define DCACHE_INVALIDATE(addr, size) {; }
#define DCACHE_CLEAN(addr, size) {; }
#endif

#define MODULO_INC(val, max) { val = (++val < max) ? val : 0; }

static int i2s_stm32_configure_stream(struct stream *stream, const struct i2s_config *i2s_cfg);
static int i2s_stm32_trigger_half_duplex(const struct device *dev, struct stream *stream,
			     enum i2s_trigger_cmd cmd);
#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
static int i2s_stm32_trigger_full_duplex(const struct device *dev, enum i2s_trigger_cmd cmd);
#endif

static unsigned int div_round_closest(uint32_t dividend, uint32_t divisor)
{
	return (dividend + (divisor / 2U)) / divisor;
}

/*
 * Get data from the queue
 */
static int queue_get(struct ring_buf *rb, void **mem_block, size_t *size)
{
	unsigned int key;

	key = irq_lock();

	if (rb->tail == rb->head) {
		/* Ring buffer is empty */
		irq_unlock(key);
		return -ENOMEM;
	}

	*mem_block = rb->buf[rb->tail].mem_block;
	*size = rb->buf[rb->tail].size;
	MODULO_INC(rb->tail, rb->len);

	irq_unlock(key);

	return 0;
}

/*
 * Put data in the queue
 */
static int queue_put(struct ring_buf *rb, void *mem_block, size_t size)
{
	uint16_t head_next;
	unsigned int key;

	key = irq_lock();

	head_next = rb->head;
	MODULO_INC(head_next, rb->len);

	if (head_next == rb->tail) {
		/* Ring buffer is full */
		irq_unlock(key);
		return -ENOMEM;
	}

	rb->buf[rb->head].mem_block = mem_block;
	rb->buf[rb->head].size = size;
	rb->head = head_next;

	irq_unlock(key);

	return 0;
}

static int i2s_stm32_enable_clock(const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
	const struct device *clk;
	int ret;

	clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);

	if (!device_is_ready(clk)) {
		LOG_ERR("clock control device not ready");
		return -ENODEV;
	}

	ret = clock_control_on(clk, (clock_control_subsys_t *) &cfg->pclken);
	if (ret != 0) {
		LOG_ERR("Could not enable I2S clock");
		return -EIO;
	}

	return 0;
}

#ifdef CONFIG_I2S_STM32_USE_PLLI2S_ENABLE
#define PLLI2S_MAX_MS_TIME	1 /* PLLI2S lock time is 300us max */
static uint16_t plli2s_ms_count;

#define z_pllr(v) LL_RCC_PLLI2SR_DIV_ ## v
#define pllr(v) z_pllr(v)
#endif

static int i2s_stm32_set_clock(const struct device *dev,
			       uint32_t bit_clk_freq)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
#ifdef CONFIG_SOC_SERIES_STM32H7X
	uint32_t pll_src = LL_RCC_PLL_GetSource();
#else
	uint32_t pll_src = LL_RCC_PLL_GetMainSource();
#endif
	int freq_in;
	uint8_t i2s_div, i2s_odd;

	freq_in = (pll_src == LL_RCC_PLLSOURCE_HSI) ?
		   HSI_VALUE : CONFIG_CLOCK_STM32_HSE_CLOCK;

#ifdef CONFIG_I2S_STM32_USE_PLLI2S_ENABLE
	/* Set PLLI2S */
	LL_RCC_PLLI2S_Disable();
	LL_RCC_PLLI2S_ConfigDomain_I2S(pll_src,
				       CONFIG_I2S_STM32_PLLI2S_PLLM,
				       CONFIG_I2S_STM32_PLLI2S_PLLN,
				       pllr(CONFIG_I2S_STM32_PLLI2S_PLLR));
	LL_RCC_PLLI2S_Enable();

	/* wait until PLLI2S gets locked */
	while (!LL_RCC_PLLI2S_IsReady()) {
		if (plli2s_ms_count++ > PLLI2S_MAX_MS_TIME) {
			return -EIO;
		}

		/* wait 1 ms */
		k_sleep(K_MSEC(1));
	}
	LOG_DBG("PLLI2S is locked");

	/* Adjust freq_in according to PLLM, PLLN, PLLR */
	float freq_tmp;

	freq_tmp = freq_in / CONFIG_I2S_STM32_PLLI2S_PLLM;
	freq_tmp *= CONFIG_I2S_STM32_PLLI2S_PLLN;
	freq_tmp /= CONFIG_I2S_STM32_PLLI2S_PLLR;
	freq_in = (int) freq_tmp;
#endif /* CONFIG_I2S_STM32_USE_PLLI2S_ENABLE */

	/* Select clock source */
#ifdef CONFIG_SOC_SERIES_STM32H7X
	freq_in = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI123);
	LL_RCC_SetSPIClockSource(cfg->i2s_clk_sel);
#else
	LL_RCC_SetI2SClockSource(cfg->i2s_clk_sel);
#endif

	/*
	 * The ratio between input clock (I2SxClk) and output
	 * clock on the pad (I2S_CK) is obtained using the
	 * following formula:
	 *   (i2s_div * 2) + i2s_odd
	 */
	i2s_div = div_round_closest(freq_in, bit_clk_freq);
	i2s_odd = (i2s_div & 0x1) ? 1 : 0;
	i2s_div >>= 1;

	LOG_DBG("freq_in: %d - bit_clk_freq: %d", freq_in, bit_clk_freq);
	LOG_DBG("i2s_div: %d - i2s_odd: %d", i2s_div, i2s_odd);

	LL_I2S_SetPrescalerLinear(cfg->i2s, i2s_div);
	LL_I2S_SetPrescalerParity(cfg->i2s, i2s_odd);

	return 0;
}

static int i2s_stm32_configure(const struct device *dev, enum i2s_dir dir,
			       const struct i2s_config *i2s_cfg)
{
	const struct i2s_stm32_cfg *const cfg = dev->config;
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream_rx = &dev_data->rx;
	struct stream *stream_tx = &dev_data->tx;
	uint32_t bit_clk_freq;
	int ret;

	if (dir == I2S_DIR_RX) {
		ret = i2s_stm32_configure_stream(stream_rx, i2s_cfg);
		if (ret != 0) {
			return ret;
		}
	} else if (dir == I2S_DIR_TX) {
		ret = i2s_stm32_configure_stream(stream_tx, i2s_cfg);
		if (ret != 0) {
			return ret;
		}
	} else if (dir == I2S_DIR_BOTH) {

#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
		ret = i2s_stm32_configure_stream(stream_rx, i2s_cfg);
		if (ret != 0) {
			return ret;
		}
		ret = i2s_stm32_configure_stream(stream_tx, i2s_cfg);
		if (ret != 0) {
			return ret;
		}
#else
		return -ENOSYS;
#endif
	} else {
		LOG_ERR("Either RX or TX direction must be selected");
		return -EINVAL;
	}

	/* set I2S bitclock */
	bit_clk_freq = i2s_cfg->frame_clk_freq *
		       i2s_cfg->word_size * i2s_cfg->channels;

	ret = i2s_stm32_set_clock(dev, bit_clk_freq);
	if (ret < 0) {
		return ret;
	}

	/* set I2S Master Clock */
	if (i2s_cfg->options & I2S_OPT_FRAME_CLK_SLAVE ||
	    i2s_cfg->options & I2S_OPT_BIT_CLK_SLAVE) {
		LL_I2S_EnableMasterClock(cfg->i2s);
	} else {
		LL_I2S_DisableMasterClock(cfg->i2s);
	}

	/* set I2S */
	if (i2s_cfg->options & I2S_OPT_IO_SWAP) {
		LL_I2S_EnableIOSwap(cfg->i2s);
	}

	/* set I2S Data Format */
	if (i2s_cfg->word_size == 16U) {
		LL_I2S_SetDataFormat(cfg->i2s, LL_I2S_DATAFORMAT_16B);
	} else if (i2s_cfg->word_size == 24U) {
		LL_I2S_SetDataFormat(cfg->i2s, LL_I2S_DATAFORMAT_24B);
	} else if (i2s_cfg->word_size == 32U) {
		LL_I2S_SetDataFormat(cfg->i2s, LL_I2S_DATAFORMAT_32B);
	} else {
		LOG_ERR("invalid word size");
		return -EINVAL;
	}

	/* set I2S Standard */
	switch (i2s_cfg->format & I2S_FMT_DATA_FORMAT_MASK) {
	case I2S_FMT_DATA_FORMAT_I2S:
		LL_I2S_SetStandard(cfg->i2s, LL_I2S_STANDARD_PHILIPS);
		break;

	case I2S_FMT_DATA_FORMAT_PCM_SHORT:
		LL_I2S_SetStandard(cfg->i2s, LL_I2S_STANDARD_PCM_SHORT);
		break;

	case I2S_FMT_DATA_FORMAT_PCM_LONG:
		LL_I2S_SetStandard(cfg->i2s, LL_I2S_STANDARD_PCM_LONG);
		break;

	case I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED:
		LL_I2S_SetStandard(cfg->i2s, LL_I2S_STANDARD_MSB);
		break;

	case I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED:
		LL_I2S_SetStandard(cfg->i2s, LL_I2S_STANDARD_LSB);
		break;

	default:
		LOG_ERR("Unsupported I2S data format");
		return -EINVAL;
	}

	/* set I2S clock polarity */
	if ((i2s_cfg->format & I2S_FMT_CLK_FORMAT_MASK) == I2S_FMT_BIT_CLK_INV)
		LL_I2S_SetClockPolarity(cfg->i2s, LL_I2S_POLARITY_HIGH);
	else
		LL_I2S_SetClockPolarity(cfg->i2s, LL_I2S_POLARITY_LOW);

	if (dir == I2S_DIR_RX || dir == I2S_DIR_BOTH) {
		stream_rx->state = I2S_STATE_READY;
	}

	if (dir == I2S_DIR_TX || dir == I2S_DIR_BOTH) {
		stream_tx->state = I2S_STATE_READY;
	}

	return 0;
}

static int i2s_stm32_trigger(const struct device *dev, enum i2s_dir dir,
			     enum i2s_trigger_cmd cmd)
{
	struct i2s_stm32_data *const dev_data = dev->data;

	if (dir == I2S_DIR_RX) {
		return i2s_stm32_trigger_half_duplex(dev, &dev_data->rx, cmd);
	} else if (dir == I2S_DIR_TX) {
		return i2s_stm32_trigger_half_duplex(dev, &dev_data->tx, cmd);
	} else if (dir == I2S_DIR_BOTH) {
#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
		return i2s_stm32_trigger_full_duplex(dev, cmd);
#else
		return -ENOSYS;
#endif
	} else {
		LOG_ERR("Either RX or TX direction must be selected");
		return -EINVAL;
	}

	return 0;
}

static int i2s_stm32_read(const struct device *dev, void **mem_block,
			  size_t *size)
{
	struct i2s_stm32_data *const dev_data = dev->data;
	int ret;

	if (dev_data->rx.state == I2S_STATE_NOT_READY) {
		LOG_ERR("device not ready");
		return -EIO;
	}

	if (dev_data->rx.state == I2S_STATE_ERROR) {
		LOG_ERR("Error status");
		return -EIO;
	}

	ret = k_sem_take(&dev_data->rx.sem,
			 SYS_TIMEOUT_MS(dev_data->rx.cfg.timeout));
	if (ret < 0) {
		return ret;
	}

	/* Get data from the beginning of RX queue */
	ret = queue_get(&dev_data->rx.mem_block_queue, mem_block, size);
	if (ret < 0) {
		LOG_ERR("error queue_get : %d", ret);
	}

	return ret;
}

static int i2s_stm32_write(const struct device *dev, void *mem_block,
			   size_t size)
{
	struct i2s_stm32_data *const dev_data = dev->data;
	int ret;

	if (dev_data->tx.state != I2S_STATE_RUNNING &&
	    dev_data->tx.state != I2S_STATE_READY) {
		LOG_ERR("invalid state %d", dev_data->tx.state);
		return -EIO;
	}

	ret = k_sem_take(&dev_data->tx.sem,
			 SYS_TIMEOUT_MS(dev_data->tx.cfg.timeout));
	if (ret < 0) {
		return ret;
	}

	/* Add data to the end of the TX queue */
	ret = queue_put(&dev_data->tx.mem_block_queue, mem_block, size);
	if (ret < 0) {
		LOG_ERR("error queue_put : %d", ret);
	}

	return ret;
}

static const struct i2s_driver_api i2s_stm32_driver_api = {
	.configure = i2s_stm32_configure,
	.read = i2s_stm32_read,
	.write = i2s_stm32_write,
	.trigger = i2s_stm32_trigger,
};

#define STM32_DMA_NUM_CHANNELS		8
static const struct device *active_dma_rx_channel[STM32_DMA_NUM_CHANNELS];
static const struct device *active_dma_tx_channel[STM32_DMA_NUM_CHANNELS];

static int reload_dma(const struct device *dev_dma, uint32_t channel,
		      struct dma_config *dcfg, void *src, void *dst,
		      uint32_t blk_size)
{
	int ret;

	ret = dma_reload(dev_dma, channel, (uint32_t)src, (uint32_t)dst, blk_size);
	if (ret < 0) {
		return ret;
	}

	ret = dma_start(dev_dma, channel);

	return ret;
}

static int start_dma(const struct device *dev_dma, uint32_t channel,
		     struct dma_config *dcfg, void *src,
		     bool src_addr_increment, void *dst,
		     bool dst_addr_increment, uint8_t fifo_threshold,
		     uint32_t blk_size)
{
	struct dma_block_config blk_cfg;
	int ret;

	memset(&blk_cfg, 0, sizeof(blk_cfg));
	blk_cfg.block_size = blk_size;
	blk_cfg.source_address = (uint32_t)src;
	blk_cfg.dest_address = (uint32_t)dst;
	if (src_addr_increment) {
		blk_cfg.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	} else {
		blk_cfg.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	}
	if (dst_addr_increment) {
		blk_cfg.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	} else {
		blk_cfg.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	}
	blk_cfg.fifo_mode_control = fifo_threshold;

	dcfg->head_block = &blk_cfg;

	ret = dma_config(dev_dma, channel, dcfg);
	if (ret < 0) {
		return ret;
	}

	ret = dma_start(dev_dma, channel);

	return ret;
}

static const struct device *get_dev_from_rx_dma_channel(uint32_t dma_channel);
static const struct device *get_dev_from_tx_dma_channel(uint32_t dma_channel);
static void rx_stream_disable(struct stream *stream, const struct device *dev);
static void tx_stream_disable(struct stream *stream, const struct device *dev);
#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
static void full_duplex_stream_disable(const struct device *dev);
#endif
/* This function is executed in the interrupt context */
static void dma_rx_callback(const struct device *dma_dev, void *arg,
			    uint32_t channel, int status)
{
	const struct device *dev = get_dev_from_rx_dma_channel(channel);
	const struct i2s_stm32_cfg *cfg = dev->config;
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream = &dev_data->rx;
	void *mblk_tmp;
	int ret;
#ifdef CONFIG_I2S_STM32_RX_OVERRUN_DROP
	bool rx_overrun = false;
#endif
	if (status != 0) {
		LOG_ERR("%d : error status %d", __LINE__, status);
		ret = -EIO;
		stream->state = I2S_STATE_ERROR;
		goto rx_disable;
	}

	__ASSERT_NO_MSG(stream->mem_block != NULL);

	/* Stop reception if there was an error */
	if (stream->state == I2S_STATE_ERROR) {
		LOG_ERR("%d : error stream status", __LINE__);
		goto rx_disable;
	}

	mblk_tmp = stream->mem_block;

	/* Prepare to receive the next data block */
	ret = k_mem_slab_alloc(stream->cfg.mem_slab, &stream->mem_block,
			       K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("%d : error alloc %d", __LINE__, ret);
		stream->state = I2S_STATE_ERROR;
		goto rx_disable;
	}

	DCACHE_INVALIDATE(stream->mem_block, stream->cfg.mem_slab->block_size);

	ret = reload_dma(stream->dev_dma, stream->dma_channel,
			&stream->dma_cfg,
#ifdef CONFIG_SOC_SERIES_STM32H7X
			(void *) &(cfg->i2s->RXDR),
#else
			(void *)LL_SPI_DMA_GetRegAddr(cfg->i2s),
#endif
			stream->mem_block,
			stream->cfg.block_size);
	if (ret < 0) {
		LOG_ERR("Failed to start RX DMA transfer: %d", ret);
		goto rx_disable;
	}

	/* Assure cache coherency after DMA write operation */
	DCACHE_INVALIDATE(mblk_tmp, stream->cfg.block_size);

	/* All block data received */
	ret = queue_put(&stream->mem_block_queue, mblk_tmp,
			stream->cfg.block_size);

#ifdef CONFIG_I2S_STM32_RX_OVERRUN_DROP
	if (ret == -ENOMEM) {
		k_mem_slab_free(stream->cfg.mem_slab, &mblk_tmp);
		rx_overrun = true;
		ret = 0;
	}
#endif
	if (ret < 0) {
		stream->state = I2S_STATE_ERROR;
		goto rx_disable;
	}

#ifdef CONFIG_I2S_STM32_RX_OVERRUN_DROP
	if(rx_overrun == false) {
		k_sem_give(&stream->sem);
	}
#else
	k_sem_give(&stream->sem);
#endif


	/* Stop reception if we were requested */
	if (stream->state == I2S_STATE_STOPPING) {
		LOG_DBG("%d : stopping stream", __LINE__);
		stream->state = I2S_STATE_READY;
		goto rx_disable;
	}

	return;

rx_disable:
#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
	if (LL_I2S_GetTransferMode(cfg->i2s) == LL_I2S_MODE_MASTER_FULL_DUPLEX ||
	    LL_I2S_GetTransferMode(cfg->i2s) == LL_I2S_MODE_SLAVE_FULL_DUPLEX) {
		LOG_ERR("%s : error", __func__);
		struct stream *stream_tx = &dev_data->tx;
		stream_tx->state = I2S_STATE_ERROR;
		full_duplex_stream_disable(dev);
	} else {
		rx_stream_disable(stream, dev);
	}
#else
	rx_stream_disable(stream, dev);
#endif
}

static void dma_tx_callback(const struct device *dma_dev, void *arg,
			    uint32_t channel, int status)
{
	const struct device *dev = get_dev_from_tx_dma_channel(channel);
	const struct i2s_stm32_cfg *cfg = dev->config;
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream = &dev_data->tx;
	int ret;

	if (status != 0) {
		ret = -EIO;
		stream->state = I2S_STATE_ERROR;
		LOG_ERR("error status: %d", status);
		goto tx_disable;
	}

	__ASSERT_NO_MSG(stream->mem_block != NULL);

	/* All block data sent */

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	if (stream->tx_underrun == false) {
		k_mem_slab_free(stream->cfg.mem_slab, &stream->last_mem_block);
		stream->last_mem_block = stream->mem_block;
		stream->last_mem_block_size = stream->mem_block_size;
	}
#else
	k_mem_slab_free(stream->cfg.mem_slab, &stream->mem_block);
#endif

	stream->mem_block = NULL;
	stream->mem_block_size = 0;

	/* Stop transmission if there was an error */
	if (stream->state == I2S_STATE_ERROR) {
		LOG_ERR("TX error detected");
		goto tx_disable;
	}

	/* Stop transmission if we were requested */
	if (stream->last_block) {
		stream->state = I2S_STATE_READY;
		goto tx_disable;
	}

	/* Prepare to send the next data block */
	ret = queue_get(&stream->mem_block_queue, &stream->mem_block,
			&stream->mem_block_size);

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	if (ret == -ENOMEM) {
		stream->mem_block = stream->last_mem_block;
		stream->mem_block_size = stream->last_mem_block_size;
		ret = 0;
		stream->tx_underrun = true;
	} else if (ret == 0) {
		stream->tx_underrun = false;
	}
#endif

	if (ret < 0) {
		if (stream->state == I2S_STATE_STOPPING) {
			stream->state = I2S_STATE_READY;
		} else {
			stream->state = I2S_STATE_ERROR;
			LOG_ERR("queue get error: %d", ret);
		}
		goto tx_disable;
	}

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	if (stream->tx_underrun == false) {
		k_sem_give(&stream->sem);
	}
#else
	k_sem_give(&stream->sem);
#endif
	/* Assure cache coherency before DMA read operation */
	DCACHE_CLEAN(stream->mem_block, stream->mem_block_size);

	ret = reload_dma(stream->dev_dma, stream->dma_channel,
			&stream->dma_cfg,
			stream->mem_block,
#ifdef CONFIG_SOC_SERIES_STM32H7X
			(void *) &(cfg->i2s->TXDR),
#else
			(void *)LL_SPI_DMA_GetRegAddr(cfg->i2s),
#endif
			stream->cfg.block_size);
	if (ret < 0) {
		LOG_DBG("Failed to start TX DMA transfer: %d", ret);
		goto tx_disable;
	}

	return;

tx_disable:
#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
	if (LL_I2S_GetTransferMode(cfg->i2s) == LL_I2S_MODE_MASTER_FULL_DUPLEX ||
	    LL_I2S_GetTransferMode(cfg->i2s) == LL_I2S_MODE_SLAVE_FULL_DUPLEX) {
		LOG_ERR("%s : error", __func__);
		struct stream *stream_rx = &dev_data->rx;
		stream_rx->state = I2S_STATE_ERROR;
		full_duplex_stream_disable(dev);
	} else {
		tx_stream_disable(stream, dev);
	}
#else
	tx_stream_disable(stream, dev);
#endif
}

static uint32_t i2s_stm32_irq_count;
static uint32_t i2s_stm32_irq_ovr_count;
#ifdef CONFIG_SOC_SERIES_STM32H7X
static uint32_t i2s_stm32_irq_udr_count;
static uint32_t i2s_stm32_irq_fre_count;
#endif
static void i2s_stm32_isr(const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream = &dev_data->rx;

	LOG_ERR("ISR: %s: err=%d", dev->name, (int)LL_I2S_ReadReg(cfg->i2s, SR));
	stream->state = I2S_STATE_ERROR;

#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
	if (LL_I2S_GetTransferMode(cfg->i2s) == LL_I2S_MODE_MASTER_FULL_DUPLEX ||
	    LL_I2S_GetTransferMode(cfg->i2s) == LL_I2S_MODE_SLAVE_FULL_DUPLEX) {
		struct stream *stream_tx = &dev_data->tx;
		stream_tx->state = I2S_STATE_ERROR;
	}
#endif
	/* OVR error must be explicitly cleared */
	if (LL_I2S_IsActiveFlag_OVR(cfg->i2s)) {
		i2s_stm32_irq_ovr_count++;
		LL_I2S_ClearFlag_OVR(cfg->i2s);
	}
#ifdef CONFIG_SOC_SERIES_STM32H7X
	/* UDR error must be explicitly cleared */
	if (LL_I2S_IsActiveFlag_UDR(cfg->i2s)) {
		i2s_stm32_irq_udr_count++;
		LL_I2S_ClearFlag_UDR(cfg->i2s);
	}

	/* FRE error must be explicitly cleared */
	if (LL_I2S_IsActiveFlag_FRE(cfg->i2s)) {
		i2s_stm32_irq_fre_count++;
		LL_I2S_ClearFlag_FRE(cfg->i2s);
	}
#endif

	i2s_stm32_irq_count++;
}

static int i2s_stm32_initialize(const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
	struct i2s_stm32_data *const dev_data = dev->data;
	int ret, i;

	/* Enable I2S clock propagation */
	ret = i2s_stm32_enable_clock(dev);
	if (ret < 0) {
		LOG_ERR("clock enabling failed: %d",  ret);
		return -EIO;
	}

	/* Configure dt provided device signals when available */
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("I2S pinctrl setup failed (%d)", ret);
		return ret;
	}

	cfg->irq_config(dev);

	k_sem_init(&dev_data->rx.sem, 0, CONFIG_I2S_STM32_RX_BLOCK_COUNT);
	k_sem_init(&dev_data->tx.sem, CONFIG_I2S_STM32_TX_BLOCK_COUNT,
		   CONFIG_I2S_STM32_TX_BLOCK_COUNT);

	for (i = 0; i < STM32_DMA_NUM_CHANNELS; i++) {
		active_dma_rx_channel[i] = NULL;
		active_dma_tx_channel[i] = NULL;
	}

	/* Get the binding to the DMA device */
	if (!device_is_ready(dev_data->tx.dev_dma)) {
		LOG_ERR("%s device not ready", dev_data->tx.dev_dma->name);
		return -ENODEV;
	}
	if (!device_is_ready(dev_data->rx.dev_dma)) {
		LOG_ERR("%s device not ready", dev_data->rx.dev_dma->name);
		return -ENODEV;
	}

	LOG_INF("%s inited", dev->name);

	return 0;
}

static int rx_stream_start_dma(struct stream *stream, const struct device *dev, bool full_duplex)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
	int ret;

	ret = k_mem_slab_alloc(stream->cfg.mem_slab, &stream->mem_block,
			       K_NO_WAIT);
	if (ret < 0) {
		return ret;
	}

	if (stream->master) {
		if (full_duplex) {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_MASTER_FULL_DUPLEX);
		} else {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_MASTER_RX);
		}
	} else {
		if (full_duplex) {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_SLAVE_FULL_DUPLEX);
		} else {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_SLAVE_RX);
		}
	}

	/* remember active RX DMA channel (used in callback) */
	active_dma_rx_channel[stream->dma_channel] = dev;

	ret = start_dma(stream->dev_dma, stream->dma_channel,
			&stream->dma_cfg,
#ifdef CONFIG_SOC_SERIES_STM32H7X
			(void *) &(cfg->i2s->RXDR),
#else
			(void *)LL_SPI_DMA_GetRegAddr(cfg->i2s),
#endif
			stream->src_addr_increment, stream->mem_block,
			stream->dst_addr_increment, stream->fifo_threshold,
			stream->cfg.block_size);
	if (ret < 0) {
		LOG_ERR("Failed to start RX DMA transfer: %d", ret);
		return ret;
	}

	return 0;
}

static int tx_stream_start_dma(struct stream *stream, const struct device *dev, bool full_duplex)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
	int ret;

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	/* Initialize  the block to send in underrun context*/
	ret = k_mem_slab_alloc(stream->cfg.mem_slab, &stream->last_mem_block, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR("k_mem_slab_alloc error %d", ret);
		return -ENOMEM;
	}
	stream->last_mem_block_size = stream->cfg.block_size;
	memset(stream->last_mem_block, 0, stream->last_mem_block_size);
	stream->tx_underrun = false;
#endif

	ret = queue_get(&stream->mem_block_queue, &stream->mem_block,
			&stream->mem_block_size);

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	if (ret == -ENOMEM) {
		stream->mem_block = stream->last_mem_block;
		stream->mem_block_size = stream->last_mem_block_size;
		ret = 0;
		stream->tx_underrun = true;
	}
#endif

	if (ret < 0) {
		return ret;
	}

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	if(stream->tx_underrun == false) {
		k_sem_give(&stream->sem);
	}
#else
	k_sem_give(&stream->sem);
#endif

	/* Assure cache coherency before DMA read operation */
	DCACHE_CLEAN(stream->mem_block, stream->mem_block_size);

	if (stream->master) {
		if (full_duplex) {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_MASTER_FULL_DUPLEX);
		} else {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_MASTER_TX);
		}
	} else {
		if (full_duplex) {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_SLAVE_FULL_DUPLEX);
		} else {
			LL_I2S_SetTransferMode(cfg->i2s, LL_I2S_MODE_SLAVE_TX);
		}
	}

	/* remember active TX DMA channel (used in callback) */
	active_dma_tx_channel[stream->dma_channel] = dev;

	ret = start_dma(stream->dev_dma, stream->dma_channel,
			&stream->dma_cfg,
			stream->mem_block, stream->src_addr_increment,
#ifdef CONFIG_SOC_SERIES_STM32H7X
			(void *) &(cfg->i2s->TXDR),
#else
			(void *)LL_SPI_DMA_GetRegAddr(cfg->i2s),
#endif
			stream->dst_addr_increment, stream->fifo_threshold,
			stream->cfg.block_size);
	if (ret < 0) {
		LOG_ERR("Failed to start TX DMA transfer: %d", ret);
		return ret;
	}

	return 0;
}

static int rx_stream_start(struct stream *stream, const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;

	int ret = rx_stream_start_dma(stream, dev, false);

	if (ret < 0) {
		LOG_ERR("Failed to start RX DMA transfer: %d", ret);
		return ret;
	}

	LL_I2S_EnableDMAReq_RX(cfg->i2s);
#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_I2S_EnableIT_FRE(cfg->i2s);
	LL_I2S_EnableIT_OVR(cfg->i2s);
	LL_I2S_EnableIT_UDR(cfg->i2s);
#else
	LL_I2S_EnableIT_ERR(cfg->i2s);
#endif
	LL_I2S_Enable(cfg->i2s);

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_SPI_StartMasterTransfer(cfg->i2s);
#endif

	return 0;
}

static int tx_stream_start(struct stream *stream, const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;

	int ret = tx_stream_start_dma(stream, dev, false);
	if (ret < 0) {
		LOG_ERR("Failed to start TX DMA transfer: %d", ret);
		return ret;
	}

	LL_I2S_EnableDMAReq_TX(cfg->i2s);

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_I2S_EnableIT_FRE(cfg->i2s);
	LL_I2S_EnableIT_OVR(cfg->i2s);
	LL_I2S_EnableIT_UDR(cfg->i2s);
#else
	LL_I2S_EnableIT_ERR(cfg->i2s);
#endif
	LL_I2S_Enable(cfg->i2s);

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_SPI_StartMasterTransfer(cfg->i2s);
#endif

	return 0;
}

#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
static int full_duplex_stream_start(const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream_rx = &dev_data->rx;
	struct stream *stream_tx = &dev_data->tx;
	int ret;

	ret = rx_stream_start_dma(stream_rx, dev, true);
	if (ret < 0) {
		LOG_ERR("Failed to start RX DMA transfer: %d", ret);
		return ret;
	}

	ret = tx_stream_start_dma(stream_tx, dev, true);
	if (ret < 0) {
		LOG_ERR("Failed to start TX DMA transfer: %d", ret);
		return ret;
	}

	LL_I2S_EnableDMAReq_RX(cfg->i2s);
	LL_I2S_EnableDMAReq_TX(cfg->i2s);

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_I2S_EnableIT_FRE(cfg->i2s);
	LL_I2S_EnableIT_OVR(cfg->i2s);
	LL_I2S_EnableIT_UDR(cfg->i2s);
#else
	LL_I2S_EnableIT_ERR(cfg->i2s);
#endif
	LL_I2S_Enable(cfg->i2s);

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_SPI_StartMasterTransfer(cfg->i2s);
#endif

	return 0;
}
#endif

static void rx_stream_disable(struct stream *stream, const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_SPI_SuspendMasterTransfer(cfg->i2s);
#endif

	LL_I2S_DisableDMAReq_RX(cfg->i2s);
#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_I2S_DisableIT_FRE(cfg->i2s);
	LL_I2S_DisableIT_OVR(cfg->i2s);
	LL_I2S_DisableIT_UDR(cfg->i2s);
#else
	LL_I2S_DisableIT_ERR(cfg->i2s);
#endif

	dma_stop(stream->dev_dma, stream->dma_channel);
	if (stream->mem_block != NULL) {
		k_mem_slab_free(stream->cfg.mem_slab, &stream->mem_block);
		stream->mem_block = NULL;
	}

	LL_I2S_Disable(cfg->i2s);

	active_dma_rx_channel[stream->dma_channel] = NULL;
}

static void tx_stream_disable(struct stream *stream, const struct device *dev)
{
	const struct i2s_stm32_cfg *cfg = dev->config;

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_SPI_SuspendMasterTransfer(cfg->i2s);
#endif

	LL_I2S_DisableDMAReq_TX(cfg->i2s);
#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_I2S_DisableIT_FRE(cfg->i2s);
	LL_I2S_DisableIT_OVR(cfg->i2s);
	LL_I2S_DisableIT_UDR(cfg->i2s);
#else
	LL_I2S_DisableIT_ERR(cfg->i2s);
#endif

	dma_stop(stream->dev_dma, stream->dma_channel);
	if (stream->mem_block != NULL) {
		k_mem_slab_free(stream->cfg.mem_slab, &stream->mem_block);
		stream->mem_block = NULL;
	}

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	k_mem_slab_free(stream->cfg.mem_slab, &stream->last_mem_block);
#endif

	LL_I2S_Disable(cfg->i2s);

	active_dma_tx_channel[stream->dma_channel] = NULL;
}

#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
static void full_duplex_stream_disable(const struct device *dev)
{
	struct i2s_stm32_data *const dev_data = dev->data;
	const struct i2s_stm32_cfg *cfg = dev->config;
	struct stream *stream_rx = &dev_data->rx;
	struct stream *stream_tx = &dev_data->tx;

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_SPI_SuspendMasterTransfer(cfg->i2s);
#endif

	LL_I2S_DisableDMAReq_RX(cfg->i2s);
	LL_I2S_DisableDMAReq_TX(cfg->i2s);

#ifdef CONFIG_SOC_SERIES_STM32H7X
	LL_I2S_DisableIT_FRE(cfg->i2s);
	LL_I2S_DisableIT_OVR(cfg->i2s);
	LL_I2S_DisableIT_UDR(cfg->i2s);
#else
	LL_I2S_DisableIT_ERR(cfg->i2s);
#endif

	dma_stop(stream_rx->dev_dma, stream_rx->dma_channel);
	if (stream_rx->mem_block != NULL) {
		k_mem_slab_free(stream_rx->cfg.mem_slab, &stream_rx->mem_block);
		stream_rx->mem_block = NULL;
	}

	dma_stop(stream_tx->dev_dma, stream_tx->dma_channel);
	if (stream_tx->mem_block != NULL) {
		k_mem_slab_free(stream_tx->cfg.mem_slab, &stream_tx->mem_block);
		stream_tx->mem_block = NULL;
	}

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	k_mem_slab_free(stream_tx->cfg.mem_slab, &stream_tx->last_mem_block);
#endif

	LL_I2S_Disable(cfg->i2s);

	active_dma_rx_channel[stream_rx->dma_channel] = NULL;
	active_dma_tx_channel[stream_tx->dma_channel] = NULL;
}
#endif

static void rx_queue_drop(struct stream *stream)
{
	size_t size;
	void *mem_block;

	while (queue_get(&stream->mem_block_queue, &mem_block, &size) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, &mem_block);
	}

	k_sem_reset(&stream->sem);
}

static void tx_queue_drop(struct stream *stream)
{
	size_t size;
	void *mem_block;
	unsigned int n = 0U;

	while (queue_get(&stream->mem_block_queue, &mem_block, &size) == 0) {
		k_mem_slab_free(stream->cfg.mem_slab, &mem_block);
		n++;
	}

#ifdef CONFIG_I2S_STM32_TX_UNDERRUN_LAST_REPEAT
	k_mem_slab_free(stream->cfg.mem_slab, &stream->last_mem_block);
#endif

	for (; n > 0; n--) {
		k_sem_give(&stream->sem);
	}
}

#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
static void full_duplex_queue_drop(const struct device *dev)
{
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream_rx = &dev_data->rx;
	struct stream *stream_tx = &dev_data->tx;

	stream_rx->queue_drop(stream_rx);
	stream_tx->queue_drop(stream_tx);
}
#endif

static const struct device *get_dev_from_rx_dma_channel(uint32_t dma_channel)
{
	return active_dma_rx_channel[dma_channel];
}

static const struct device *get_dev_from_tx_dma_channel(uint32_t dma_channel)
{
	return active_dma_tx_channel[dma_channel];
}

static int i2s_stm32_configure_stream(struct stream *stream, const struct i2s_config *i2s_cfg)
{
	if (stream->state != I2S_STATE_NOT_READY &&
	    stream->state != I2S_STATE_READY) {
		LOG_ERR("invalid state");
		return -EINVAL;
	}

	stream->master = true;
	if (i2s_cfg->options & I2S_OPT_FRAME_CLK_SLAVE ||
	    i2s_cfg->options & I2S_OPT_BIT_CLK_SLAVE) {
		stream->master = false;
	}

	if (i2s_cfg->frame_clk_freq == 0U) {
		stream->queue_drop(stream);
		memset(&stream->cfg, 0, sizeof(struct i2s_config));
		stream->state = I2S_STATE_NOT_READY;
		return 0;
	}

	memcpy(&stream->cfg, i2s_cfg, sizeof(struct i2s_config));

	stream->tx_underrun = false;
	stream->last_mem_block = NULL;
	stream->last_mem_block_size = 0;
	stream->mem_block_size = 0;

	return 0;
}

static int i2s_stm32_trigger_half_duplex(const struct device *dev, struct stream *stream,
			     enum i2s_trigger_cmd cmd)
{
	unsigned int key;
	int ret;

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (stream->state != I2S_STATE_READY) {
			LOG_ERR("START trigger: invalid state %d",
				    stream->state);
			return -EIO;
		}

		__ASSERT_NO_MSG(stream->mem_block == NULL);

		ret = stream->stream_start(stream, dev);
		if (ret < 0) {
			LOG_ERR("START trigger failed %d", ret);
			return ret;
		}
		stream->state = I2S_STATE_RUNNING;
		stream->last_block = false;
		break;

	case I2S_TRIGGER_STOP:
		key = irq_lock();
		if (stream->state != I2S_STATE_RUNNING) {
			irq_unlock(key);
			LOG_ERR("STOP trigger: invalid state");
			return -EIO;
		}
		irq_unlock(key);
		stream->stream_disable(stream, dev);
		stream->queue_drop(stream);
		stream->state = I2S_STATE_READY;
		stream->last_block = true;
		break;

	case I2S_TRIGGER_DRAIN:
		key = irq_lock();
		if (stream->state != I2S_STATE_RUNNING) {
			irq_unlock(key);
			LOG_ERR("DRAIN trigger: invalid state");
			return -EIO;
		}
		stream->stream_disable(stream, dev);
		stream->queue_drop(stream);
		stream->state = I2S_STATE_READY;
		irq_unlock(key);
		break;

	case I2S_TRIGGER_DROP:
		if (stream->state == I2S_STATE_NOT_READY) {
			LOG_ERR("DROP trigger: invalid state");
			return -EIO;
		}
		stream->stream_disable(stream, dev);
		stream->queue_drop(stream);
		stream->state = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_PREPARE:
		if (stream->state != I2S_STATE_ERROR) {
			LOG_ERR("PREPARE trigger: invalid state");
			return -EIO;
		}
		stream->state = I2S_STATE_READY;
		stream->queue_drop(stream);
		break;

	default:
		LOG_ERR("Unsupported trigger command");
		return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_I2S_STM32_FULL_DUPLEX
static int i2s_stm32_trigger_full_duplex(const struct device *dev, enum i2s_trigger_cmd cmd)
{
	struct i2s_stm32_data *const dev_data = dev->data;
	struct stream *stream_rx = &dev_data->rx;
	struct stream *stream_tx = &dev_data->tx;
	unsigned int key;
	int ret;

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (stream_rx->state != I2S_STATE_READY) {
			LOG_ERR("START trigger stream rx: invalid state %d ",
				    stream_rx->state);
			return -EIO;
		}

		if (stream_tx->state != I2S_STATE_READY) {
			LOG_ERR("START trigger stream tx: invalid state %d",
				    stream_tx->state);
			return -EIO;
		}

		__ASSERT_NO_MSG(stream_rx->mem_block == NULL);
		__ASSERT_NO_MSG(stream_tx->mem_block == NULL);

		ret = full_duplex_stream_start(dev);

		if (ret < 0) {
			LOG_ERR("START trigger failed %d", ret);
			return ret;
		}

		stream_rx->state = I2S_STATE_RUNNING;
		stream_rx->last_block = false;

		stream_tx->state = I2S_STATE_RUNNING;
		stream_tx->last_block = false;
		break;

	case I2S_TRIGGER_STOP:
		key = irq_lock();
		if (stream_rx->state != I2S_STATE_RUNNING) {
			LOG_ERR("STOP trigger stream rx: invalid state %d ",
				    stream_rx->state);
			return -EIO;
		}

		if (stream_tx->state != I2S_STATE_RUNNING) {
			LOG_ERR("STOP trigger stream tx: invalid state %d",
				    stream_tx->state);
			return -EIO;
		}


		irq_unlock(key);
		full_duplex_stream_disable(dev);
		full_duplex_queue_drop(dev);

		stream_rx->state = I2S_STATE_READY;
		stream_rx->last_block = true;

		stream_tx->state = I2S_STATE_READY;
		stream_tx->last_block = true;

		break;

	case I2S_TRIGGER_DRAIN:
		key = irq_lock();
		if (stream_rx->state != I2S_STATE_RUNNING) {
			LOG_ERR("DRAIN trigger stream rx: invalid state %d ",
				    stream_rx->state);
			return -EIO;
		}

		if (stream_tx->state != I2S_STATE_RUNNING) {
			LOG_ERR("DRAIN trigger stream tx: invalid state %d",
				    stream_tx->state);
			return -EIO;
		}

		full_duplex_stream_disable(dev);
		full_duplex_queue_drop(dev);

		stream_rx->state = I2S_STATE_READY;
		stream_tx->state = I2S_STATE_READY;

		irq_unlock(key);
		break;

	case I2S_TRIGGER_DROP:
		if (stream_rx->state != I2S_STATE_NOT_READY) {
			LOG_ERR("DROP trigger stream rx: invalid state %d ",
				    stream_rx->state);
			return -EIO;
		}

		if (stream_tx->state != I2S_STATE_NOT_READY) {
			LOG_ERR("DROP trigger stream tx: invalid state %d",
				    stream_tx->state);
			return -EIO;
		}


		full_duplex_stream_disable(dev);
		full_duplex_queue_drop(dev);

		stream_rx->state = I2S_STATE_READY;
		stream_tx->state = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_PREPARE:
		if (stream_rx->state != I2S_STATE_ERROR) {
			LOG_ERR("PREPARE trigger stream rx: invalid state %d ",
				    stream_rx->state);
			return -EIO;
		}

		if (stream_tx->state != I2S_STATE_ERROR) {
			LOG_ERR("PREPARE trigger stream tx: invalid state %d",
				    stream_tx->state);
			return -EIO;
		}

		stream_rx->state = I2S_STATE_READY;
		stream_tx->state = I2S_STATE_READY;

		full_duplex_queue_drop(dev);
		break;

	default:
		LOG_ERR("Unsupported trigger command");
		return -EINVAL;
	}

	return 0;
}
#endif

/* src_dev and dest_dev should be 'MEMORY' or 'PERIPHERAL'. */
#define I2S_DMA_CHANNEL_INIT(index, dir, dir_cap, src_dev, dest_dev)	\
.dir = {								\
	.dev_dma = DEVICE_DT_GET(DT_DMAS_CTLR_BY_NAME(DT_NODELABEL(i2s##index), dir)),\
	.dma_channel = \
		DT_DMAS_CELL_BY_NAME(DT_NODELABEL(i2s##index), dir, channel),\
	.dma_cfg = {							\
		.block_count = 2,					\
		.dma_slot = \
		    DT_DMAS_CELL_BY_NAME(DT_NODELABEL(i2s##index), dir, slot),\
		.channel_direction = src_dev##_TO_##dest_dev,		\
		.source_data_size = 2,  /* 16bit default */		\
		.dest_data_size = 2,    /* 16bit default */		\
		.source_burst_length = 1, /* SINGLE transfer */		\
		.dest_burst_length = 1,					\
		.channel_priority = STM32_DMA_CONFIG_PRIORITY(		\
	 DT_DMAS_CELL_BY_NAME(DT_NODELABEL(i2s##index), dir, channel_config)),\
		.dma_callback = dma_##dir##_callback,			\
	},								\
	.src_addr_increment = STM32_DMA_CONFIG_##src_dev##_ADDR_INC(	\
	 DT_DMAS_CELL_BY_NAME(DT_NODELABEL(i2s##index), dir, channel_config)),\
	.dst_addr_increment = STM32_DMA_CONFIG_##dest_dev##_ADDR_INC(	\
	 DT_DMAS_CELL_BY_NAME(DT_NODELABEL(i2s##index), dir, channel_config)),\
	.fifo_threshold = STM32_DMA_FEATURES_FIFO_THRESHOLD(		\
	 DT_DMAS_CELL_BY_NAME(DT_NODELABEL(i2s##index), dir, channel_config)),\
	.stream_start = dir##_stream_start,				\
	.stream_disable = dir##_stream_disable,				\
	.queue_drop = dir##_queue_drop,					\
	.mem_block_queue.buf = dir##_##index##_ring_buf,		\
	.mem_block_queue.len = ARRAY_SIZE(dir##_##index##_ring_buf)	\
}

#define I2S_INIT(index, clk_sel)					\
									\
static void i2s_stm32_irq_config_func_##index(const struct device *dev);\
									\
PINCTRL_DT_DEFINE(DT_NODELABEL(i2s##index));				\
									\
static const struct i2s_stm32_cfg i2s_stm32_config_##index = {		\
	.i2s = (SPI_TypeDef *) DT_REG_ADDR(DT_NODELABEL(i2s##index)),	\
	.pclken = {							\
		.enr = DT_CLOCKS_CELL(DT_NODELABEL(i2s##index), bits),	\
		.bus = DT_CLOCKS_CELL(DT_NODELABEL(i2s##index), bus),	\
	},								\
	.i2s_clk_sel = CLK_SEL_##clk_sel,				\
	.pcfg = PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2s##index)),	\
	.irq_config = i2s_stm32_irq_config_func_##index,		\
};									\
									\
struct queue_item rx_##index##_ring_buf[CONFIG_I2S_STM32_RX_BLOCK_COUNT + 1];\
struct queue_item tx_##index##_ring_buf[CONFIG_I2S_STM32_TX_BLOCK_COUNT + 1];\
									\
static struct i2s_stm32_data i2s_stm32_data_##index = {			\
	UTIL_AND(DT_DMAS_HAS_NAME(DT_NODELABEL(i2s##index), rx),	\
		I2S_DMA_CHANNEL_INIT(index, rx, RX, PERIPHERAL, MEMORY)),\
	UTIL_AND(DT_DMAS_HAS_NAME(DT_NODELABEL(i2s##index), tx),	\
		I2S_DMA_CHANNEL_INIT(index, tx, TX, MEMORY, PERIPHERAL)),\
};									\
DEVICE_DT_DEFINE(DT_NODELABEL(i2s##index),				\
		    &i2s_stm32_initialize, NULL,			\
		    &i2s_stm32_data_##index,				\
		    &i2s_stm32_config_##index, POST_KERNEL,		\
		    CONFIG_I2S_INIT_PRIORITY, &i2s_stm32_driver_api);	\
									\
static void i2s_stm32_irq_config_func_##index(const struct device *dev)	\
{									\
	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(i2s##index)),			\
		    DT_IRQ(DT_NODELABEL(i2s##index), priority),		\
		    i2s_stm32_isr, DEVICE_DT_GET(DT_NODELABEL(i2s##index)), 0);\
	irq_enable(DT_IRQN(DT_NODELABEL(i2s##index)));			\
}

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2s1), okay)
I2S_INIT(1, 2)
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2s2), okay)
I2S_INIT(2, 1)
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2s3), okay)
I2S_INIT(3, 1)
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2s4), okay)
I2S_INIT(4, 2)
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2s5), okay)
I2S_INIT(5, 2)
#endif
