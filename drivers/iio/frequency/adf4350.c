/*
 * ADF4350/ADF4351 SPI Wideband Synthesizer driver
 *
 * Copyright 2012 Analog Devices Inc.
 *
 * Licensed under the GPL-2.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/gcd.h>
#include <linux/gpio.h>
#include <asm/div64.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/frequency/adf4350.h>

enum {
	ADF4350_FREQ,
	ADF4350_FREQ_REFIN,
	ADF4350_FREQ_RESOLUTION,
	ADF4350_PWRDOWN,
};

struct adf4350_state {
	struct spi_device		*spi;
	struct regulator		*reg;
	struct adf4350_platform_data	*pdata;
	unsigned long			clkin;
	unsigned long			chspc; /* Channel Spacing */
	unsigned long			fpfd; /* Phase Frequency Detector */
	unsigned long			min_out_freq;
	unsigned			r0_fract;
	unsigned			r0_int;
	unsigned			r1_mod;
	unsigned			r4_rf_div_sel;
	unsigned long			regs[6];
	unsigned long			regs_hw[6];

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	__be32				val ____cacheline_aligned;
};

static struct adf4350_platform_data default_pdata = {
	.clkin = 122880000,
	.channel_spacing = 10000,
	.r2_user_settings = ADF4350_REG2_PD_POLARITY_POS |
			    ADF4350_REG2_CHARGE_PUMP_CURR_uA(2500),
	.r3_user_settings = ADF4350_REG3_12BIT_CLKDIV_MODE(0),
	.r4_user_settings = ADF4350_REG4_OUTPUT_PWR(3) |
			    ADF4350_REG4_MUTE_TILL_LOCK_EN,
	.gpio_lock_detect = -1,
};

static int adf4350_sync_config(struct adf4350_state *st)
{
	int ret, i, doublebuf = 0;

	for (i = ADF4350_REG5; i >= ADF4350_REG0; i--) {
		if ((st->regs_hw[i] != st->regs[i]) ||
			((i == ADF4350_REG0) && doublebuf)) {

			switch (i) {
			case ADF4350_REG1:
			case ADF4350_REG4:
				doublebuf = 1;
				break;
			}

			st->val  = cpu_to_be32(st->regs[i] | i);
			ret = spi_write(st->spi, &st->val, 4);
			if (ret < 0)
				return ret;
			st->regs_hw[i] = st->regs[i];
			dev_dbg(&st->spi->dev, "[%d] 0x%X\n",
				i, (u32)st->regs[i] | i);
		}
	}
	return 0;
}

static int adf4350_reg_access(struct iio_dev *indio_dev,
			      unsigned reg, unsigned writeval,
			      unsigned *readval)
{
	struct adf4350_state *st = iio_priv(indio_dev);
	int ret;

	if (reg > ADF4350_REG5)
		return -EINVAL;

	mutex_lock(&indio_dev->mlock);
	if (readval == NULL) {
		st->regs[reg] = writeval & ~(BIT(0) | BIT(1) | BIT(2));
		ret = adf4350_sync_config(st);
	} else {
		*readval =  st->regs_hw[reg];
		ret = 0;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static int adf4350_tune_r_cnt(struct adf4350_state *st, unsigned short r_cnt)
{
	struct adf4350_platform_data *pdata = st->pdata;

	do {
		r_cnt++;
		st->fpfd = (st->clkin * (pdata->ref_doubler_en ? 2 : 1)) /
			   (r_cnt * (pdata->ref_div2_en ? 2 : 1));
	} while (st->fpfd > ADF4350_MAX_FREQ_PFD);

	return r_cnt;
}

static int adf4350_set_freq(struct adf4350_state *st, unsigned long long freq)
{
	struct adf4350_platform_data *pdata = st->pdata;
	u64 tmp;
	u32 div_gcd, prescaler, chspc;
	u16 mdiv, r_cnt = 0;
	u8 band_sel_div;

	if (freq > ADF4350_MAX_OUT_FREQ || freq < st->min_out_freq)
		return -EINVAL;

	if (freq > ADF4350_MAX_FREQ_45_PRESC) {
		prescaler = ADF4350_REG1_PRESCALER;
		mdiv = 75;
	} else {
		prescaler = 0;
		mdiv = 23;
	}

	st->r4_rf_div_sel = 0;

	while (freq < ADF4350_MIN_VCO_FREQ) {
		freq <<= 1;
		st->r4_rf_div_sel++;
	}

	/*
	 * Allow a predefined reference division factor
	 * if not set, compute our own
	 */
	if (pdata->ref_div_factor)
		r_cnt = pdata->ref_div_factor - 1;

	chspc = st->chspc;

	do  {
		do {
			do {
				r_cnt = adf4350_tune_r_cnt(st, r_cnt);
				st->r1_mod = st->fpfd / chspc;
				if (r_cnt > ADF4350_MAX_R_CNT) {
					/* try higher spacing values */
					chspc++;
					r_cnt = 0;
				}
			} while ((st->r1_mod > ADF4350_MAX_MODULUS) && r_cnt);
		} while (r_cnt == 0);

		tmp = freq * (u64)st->r1_mod + (st->fpfd > 1);
		do_div(tmp, st->fpfd); /* Div round closest (n + d/2)/d */
		st->r0_fract = do_div(tmp, st->r1_mod);
		st->r0_int = tmp;
	} while (mdiv > st->r0_int);

	band_sel_div = DIV_ROUND_UP(st->fpfd, ADF4350_MAX_BANDSEL_CLK);

	if (st->r0_fract && st->r1_mod) {
		div_gcd = gcd(st->r1_mod, st->r0_fract);
		st->r1_mod /= div_gcd;
		st->r0_fract /= div_gcd;
	} else {
		st->r0_fract = 0;
		st->r1_mod = 1;
	}

	dev_dbg(&st->spi->dev, "VCO: %llu Hz, PFD %lu Hz\n"
		"REF_DIV %d, R0_INT %d, R0_FRACT %d\n"
		"R1_MOD %d, RF_DIV %d\nPRESCALER %s, BAND_SEL_DIV %d\n",
		freq, st->fpfd, r_cnt, st->r0_int, st->r0_fract, st->r1_mod,
		1 << st->r4_rf_div_sel, prescaler ? "8/9" : "4/5",
		band_sel_div);

	st->regs[ADF4350_REG0] = ADF4350_REG0_INT(st->r0_int) |
				 ADF4350_REG0_FRACT(st->r0_fract);

	st->regs[ADF4350_REG1] = ADF4350_REG1_PHASE(1) |
				 ADF4350_REG1_MOD(st->r1_mod) |
				 prescaler;

	st->regs[ADF4350_REG2] =
		ADF4350_REG2_10BIT_R_CNT(r_cnt) |
		ADF4350_REG2_DOUBLE_BUFF_EN |
		(pdata->ref_doubler_en ? ADF4350_REG2_RMULT2_EN : 0) |
		(pdata->ref_div2_en ? ADF4350_REG2_RDIV2_EN : 0) |
		(pdata->r2_user_settings & (ADF4350_REG2_PD_POLARITY_POS |
		ADF4350_REG2_LDP_6ns | ADF4350_REG2_LDF_INT_N |
		ADF4350_REG2_CHARGE_PUMP_CURR_uA(5000) |
		ADF4350_REG2_MUXOUT(0x7) | ADF4350_REG2_NOISE_MODE(0x9)));

	st->regs[ADF4350_REG3] = pdata->r3_user_settings &
				 (ADF4350_REG3_12BIT_CLKDIV(0xFFF) |
				 ADF4350_REG3_12BIT_CLKDIV_MODE(0x3) |
				 ADF4350_REG3_12BIT_CSR_EN |
				 ADF4351_REG3_CHARGE_CANCELLATION_EN |
				 ADF4351_REG3_ANTI_BACKLASH_3ns_EN |
				 ADF4351_REG3_BAND_SEL_CLOCK_MODE_HIGH);

	st->regs[ADF4350_REG4] =
		ADF4350_REG4_FEEDBACK_FUND |
		ADF4350_REG4_RF_DIV_SEL(st->r4_rf_div_sel) |
		ADF4350_REG4_8BIT_BAND_SEL_CLKDIV(band_sel_div) |
		ADF4350_REG4_RF_OUT_EN |
		(pdata->r4_user_settings &
		(ADF4350_REG4_OUTPUT_PWR(0x3) |
		ADF4350_REG4_AUX_OUTPUT_PWR(0x3) |
		ADF4350_REG4_AUX_OUTPUT_EN |
		ADF4350_REG4_AUX_OUTPUT_FUND |
		ADF4350_REG4_MUTE_TILL_LOCK_EN));

	st->regs[ADF4350_REG5] = ADF4350_REG5_LD_PIN_MODE_DIGITAL;

	return adf4350_sync_config(st);
}

static ssize_t adf4350_write(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    const char *buf, size_t len)
{
	struct adf4350_state *st = iio_priv(indio_dev);
	unsigned long long readin;
	int ret;

	ret = kstrtoull(buf, 10, &readin);
	if (ret)
		return ret;

	mutex_lock(&indio_dev->mlock);
	switch ((u32)private) {
	case ADF4350_FREQ:
		ret = adf4350_set_freq(st, readin);
		break;
	case ADF4350_FREQ_REFIN:
		if (readin > ADF4350_MAX_FREQ_REFIN)
			ret = -EINVAL;
		else
			st->clkin = readin;
		break;
	case ADF4350_FREQ_RESOLUTION:
		if (readin == 0)
			ret = -EINVAL;
		else
			st->chspc = readin;
		break;
	case ADF4350_PWRDOWN:
		if (readin)
			st->regs[ADF4350_REG2] |= ADF4350_REG2_POWER_DOWN_EN;
		else
			st->regs[ADF4350_REG2] &= ~ADF4350_REG2_POWER_DOWN_EN;

		adf4350_sync_config(st);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}

static ssize_t adf4350_read(struct iio_dev *indio_dev,
				   uintptr_t private,
				   const struct iio_chan_spec *chan,
				   char *buf)
{
	struct adf4350_state *st = iio_priv(indio_dev);
	unsigned long long val;
	int ret = 0;

	mutex_lock(&indio_dev->mlock);
	switch ((u32)private) {
	case ADF4350_FREQ:
		val = (u64)((st->r0_int * st->r1_mod) + st->r0_fract) *
			(u64)st->fpfd;
		do_div(val, st->r1_mod * (1 << st->r4_rf_div_sel));
		/* PLL unlocked? return error */
		if (gpio_is_valid(st->pdata->gpio_lock_detect))
			if (!gpio_get_value(st->pdata->gpio_lock_detect)) {
				dev_dbg(&st->spi->dev, "PLL un-locked\n");
				ret = -EBUSY;
			}
		break;
	case ADF4350_FREQ_REFIN:
		val = st->clkin;
		break;
	case ADF4350_FREQ_RESOLUTION:
		val = st->chspc;
		break;
	case ADF4350_PWRDOWN:
		val = !!(st->regs[ADF4350_REG2] & ADF4350_REG2_POWER_DOWN_EN);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret < 0 ? ret : sprintf(buf, "%llu\n", val);
}

#define _ADF4350_EXT_INFO(_name, _ident) { \
	.name = _name, \
	.read = adf4350_read, \
	.write = adf4350_write, \
	.private = _ident, \
}

static const struct iio_chan_spec_ext_info adf4350_ext_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	_ADF4350_EXT_INFO("frequency", ADF4350_FREQ),
	_ADF4350_EXT_INFO("frequency_resolution", ADF4350_FREQ_RESOLUTION),
	_ADF4350_EXT_INFO("refin_frequency", ADF4350_FREQ_REFIN),
	_ADF4350_EXT_INFO("powerdown", ADF4350_PWRDOWN),
	{ },
};

static const struct iio_chan_spec adf4350_chan = {
	.type = IIO_ALTVOLTAGE,
	.indexed = 1,
	.output = 1,
	.ext_info = adf4350_ext_info,
};

static const struct iio_info adf4350_info = {
	.debugfs_reg_access = &adf4350_reg_access,
	.driver_module = THIS_MODULE,
};

static int __devinit adf4350_probe(struct spi_device *spi)
{
	struct adf4350_platform_data *pdata = spi->dev.platform_data;
	struct iio_dev *indio_dev;
	struct adf4350_state *st;
	int ret;

	if (!pdata) {
		dev_warn(&spi->dev, "no platform data? using default\n");

		pdata = &default_pdata;
	}

	indio_dev = iio_device_alloc(sizeof(*st));
	if (indio_dev == NULL)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->reg = regulator_get(&spi->dev, "vcc");
	if (!IS_ERR(st->reg)) {
		ret = regulator_enable(st->reg);
		if (ret)
			goto error_put_reg;
	}

	spi_set_drvdata(spi, indio_dev);
	st->spi = spi;
	st->pdata = pdata;

	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = (pdata->name[0] != 0) ? pdata->name :
		spi_get_device_id(spi)->name;

	indio_dev->info = &adf4350_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = &adf4350_chan;
	indio_dev->num_channels = 1;

	st->chspc = pdata->channel_spacing;
	st->clkin = pdata->clkin;

	st->min_out_freq = spi_get_device_id(spi)->driver_data == 4351 ?
		ADF4351_MIN_OUT_FREQ : ADF4350_MIN_OUT_FREQ;

	memset(st->regs_hw, 0xFF, sizeof(st->regs_hw));

	if (gpio_is_valid(pdata->gpio_lock_detect)) {
		ret = gpio_request(pdata->gpio_lock_detect, indio_dev->name);
		if (ret) {
			dev_err(&spi->dev, "fail to request lock detect GPIO-%d",
				pdata->gpio_lock_detect);
			goto error_disable_reg;
		}
		gpio_direction_input(pdata->gpio_lock_detect);
	}

	if (pdata->power_up_frequency) {
		ret = adf4350_set_freq(st, pdata->power_up_frequency);
		if (ret)
			goto error_free_gpio;
	}

	ret = iio_device_register(indio_dev);
	if (ret)
		goto error_free_gpio;

	return 0;

error_free_gpio:
	if (gpio_is_valid(pdata->gpio_lock_detect))
		gpio_free(pdata->gpio_lock_detect);

error_disable_reg:
	if (!IS_ERR(st->reg))
		regulator_disable(st->reg);
error_put_reg:
	if (!IS_ERR(st->reg))
		regulator_put(st->reg);

	iio_device_free(indio_dev);

	return ret;
}

static int __devexit adf4350_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct adf4350_state *st = iio_priv(indio_dev);
	struct regulator *reg = st->reg;

	st->regs[ADF4350_REG2] |= ADF4350_REG2_POWER_DOWN_EN;
	adf4350_sync_config(st);

	iio_device_unregister(indio_dev);

	if (!IS_ERR(reg)) {
		regulator_disable(reg);
		regulator_put(reg);
	}

	if (gpio_is_valid(st->pdata->gpio_lock_detect))
		gpio_free(st->pdata->gpio_lock_detect);

	iio_device_free(indio_dev);

	return 0;
}

static const struct spi_device_id adf4350_id[] = {
	{"adf4350", 4350},
	{"adf4351", 4351},
	{}
};

static struct spi_driver adf4350_driver = {
	.driver = {
		.name	= "adf4350",
		.owner	= THIS_MODULE,
	},
	.probe		= adf4350_probe,
	.remove		= __devexit_p(adf4350_remove),
	.id_table	= adf4350_id,
};
module_spi_driver(adf4350_driver);

MODULE_AUTHOR("Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("Analog Devices ADF4350/ADF4351 PLL");
MODULE_LICENSE("GPL v2");
