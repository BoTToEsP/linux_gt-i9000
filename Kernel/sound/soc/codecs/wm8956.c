/*
 * wm8956.c  --  WM8956 ALSA SoC Audio driver
 *
 * Author: Liam Girdwood
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>

#include "wm8956.h"

struct snd_soc_codec_device soc_codec_dev_wm8956;

/*
 * wm8956 register cache
 * We can't read the WM8956 register space when we are
 * using 2 wire for device control, so we cache them instead.
 */
static const u16 wm8956_reg[WM8956_CACHEREGNUM] = {
	0x0097, 0x0097, 0x0000, 0x0000,
	0x0000, 0x0008, 0x0000, 0x000a,
	0x01c0, 0x0000, 0x00ff, 0x00ff,
	0x0000, 0x0000, 0x0000, 0x0000, /* r15 */
	0x0000, 0x007b, 0x0100, 0x0032,
	0x0000, 0x00c3, 0x00c3, 0x01c0,
	0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, /* r31 */
	0x0100, 0x0100, 0x0050, 0x0050,
	0x0050, 0x0050, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0040, 0x0000,
	0x0000, 0x0050, 0x0050, 0x0000, /* r47 */
	0x0002, 0x0037, 0x004d, 0x0080,
	0x0008, 0x0031, 0x0026, 0x00e9,
};

struct wm8956_priv {
	struct snd_soc_codec codec;
	u16 reg_cache[WM8956_CACHEREGNUM];
};

static struct snd_soc_codec *wm8956_codec;

/*
 * read wm8956 register cache
 */
static inline unsigned int wm8956_read_reg_cache(struct snd_soc_codec *codec,
	unsigned int reg)
{
	u16 *cache = codec->reg_cache;
	if (reg == WM8956_RESET)
		return 0;
	if (reg >= WM8956_CACHEREGNUM)
		return -1;
	return cache[reg];
}

/*
 * write wm8956 register cache
 */
static inline void wm8956_write_reg_cache(struct snd_soc_codec *codec,
	u16 reg, unsigned int value)
{
	u16 *cache = codec->reg_cache;
	if (reg >= WM8956_CACHEREGNUM)
		return;
	cache[reg] = value;
}

/*
 * write to the WM8956 register space
 */
static int wm8956_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	u8 data[2];

	/* data is
	 *   D15..D9 WM8956 register offset
	 *   D8...D0 register data
	 */
	data[0] = (reg << 1) | ((value >> 8) & 0x0001);
	data[1] = value & 0x00ff;

	wm8956_write_reg_cache (codec, reg, value);
	if (codec->hw_write(codec->control_data, data, 2) == 2)
		return 0;
	else
		return -EIO;
}

#define wm8956_reset(c)	wm8956_write(c, WM8956_RESET, 0)

/* todo - complete enumerated controls */
static const char *wm8956_deemph[] = {"None", "32Khz", "44.1Khz", "48Khz"};

static const struct soc_enum wm8956_enum[] = {
	SOC_ENUM_SINGLE(WM8956_DACCTL1, 1, 4, wm8956_deemph),
};

/* to complete */
static const struct snd_kcontrol_new wm8956_snd_controls[] = {

SOC_DOUBLE_R("Headphone Playback Volume", WM8956_LOUT1, WM8956_ROUT1,
	0, 127, 0),
SOC_DOUBLE_R("Headphone Playback ZC Switch", WM8956_LOUT1, WM8956_ROUT1,
	7, 1, 0),
SOC_DOUBLE_R("PCM Volume", WM8956_LDAC, WM8956_RDAC,
	0, 127, 0),

SOC_ENUM("Playback De-emphasis", wm8956_enum[0]),
};

/* Left Output Mixer */
static const struct snd_kcontrol_new wm8956_loutput_mixer_controls[] = {
SOC_DAPM_SINGLE("Left PCM Playback Switch", WM8956_LOUTMIX1, 8, 1, 0),
};

/* Right Output Mixer */
static const struct snd_kcontrol_new wm8956_routput_mixer_controls[] = {
SOC_DAPM_SINGLE("Right PCM Playback Switch", WM8956_ROUTMIX2, 8, 1, 0),
};

static const struct snd_soc_dapm_widget wm8956_dapm_widgets[] = {
SND_SOC_DAPM_MIXER("Left Mixer", SND_SOC_NOPM, 0, 0,
	&wm8956_loutput_mixer_controls[0],
	ARRAY_SIZE(wm8956_loutput_mixer_controls)),
SND_SOC_DAPM_MIXER("Right Mixer", SND_SOC_NOPM, 0, 0,
	&wm8956_loutput_mixer_controls[0],
	ARRAY_SIZE(wm8956_routput_mixer_controls)),
};

static int wm8956_add_widgets(struct snd_soc_codec *codec)
{
	snd_soc_dapm_new_controls(codec, wm8956_dapm_widgets,
				  ARRAY_SIZE(wm8956_dapm_widgets));

	snd_soc_dapm_new_widgets(codec);
	return 0;
}

static int wm8956_set_dai_fmt(struct snd_soc_dai *codec_dai,
		unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u16 iface = 0;

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		iface |= 0x0040;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		return -EINVAL;
	}

	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		iface |= 0x0002;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface |= 0x0001;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface |= 0x0003;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface |= 0x0013;
		break;
	default:
		return -EINVAL;
	}

	/* clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_IF:
		iface |= 0x0090;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		iface |= 0x0080;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		iface |= 0x0010;
		break;
	default:
		return -EINVAL;
	}

	/* set iface */
	wm8956_write(codec, WM8956_IFACE1, iface);
	return 0;
}

static int wm8956_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;

	u16 iface = wm8956_read_reg_cache(codec, WM8956_IFACE1) & 0xfff3;

	/* bit size */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		iface |= 0x0004;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		iface |= 0x0008;
		break;
	}

	/* set iface */
	wm8956_write(codec, WM8956_IFACE1, iface);
	return 0;
}

static int wm8956_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	u16 mute_reg = wm8956_read_reg_cache(codec, WM8956_DACCTL1) & 0xfff7;

	if (mute)
		wm8956_write(codec, WM8956_DACCTL1, mute_reg | 0x8);
	else
		wm8956_write(codec, WM8956_DACCTL1, mute_reg);
	return 0;
}

static int wm8956_set_bias_level(struct snd_soc_codec *codec,
				 enum snd_soc_bias_level event)
{
#if 0
	switch (event) {
	case SND_SOC_BIAS_ON:
	case SND_SOC_BIAS_PREPARE:
	case SND_SOC_BIAS_STANDBY:
	case SND_SOC_BIAS_OFF:
		break;
	}
#endif
	// tmp
	wm8956_write(codec, WM8956_POWER1, 0xfffe);
	wm8956_write(codec, WM8956_POWER2, 0xffff);
	wm8956_write(codec, WM8956_POWER3, 0xffff);
	codec->bias_level = event;
	return 0;
}

/* PLL divisors */
struct _pll_div {
	u32 pre_div:1;
	u32 n:4;
	u32 k:24;
};

static struct _pll_div pll_div;

/* The size in bits of the pll divide multiplied by 10
 * to allow rounding later */
#define FIXED_PLL_SIZE ((1 << 24) * 10)

static void pll_factors(unsigned int target, unsigned int source)
{
	unsigned long long Kpart;
	unsigned int K, Ndiv, Nmod;

	Ndiv = target / source;
	if (Ndiv < 6) {
		source >>= 1;
		pll_div.pre_div = 1;
		Ndiv = target / source;
	} else
		pll_div.pre_div = 0;

	if ((Ndiv < 6) || (Ndiv > 12))
		printk(KERN_WARNING
			"WM8956 N value outwith recommended range! N = %d\n",Ndiv);

	pll_div.n = Ndiv;
	Nmod = target % source;
	Kpart = FIXED_PLL_SIZE * (long long)Nmod;

	do_div(Kpart, source);

	K = Kpart & 0xFFFFFFFF;

	/* Check if we need to round */
	if ((K % 10) >= 5)
		K += 5;

	/* Move down to proper range now rounding is done */
	K /= 10;

	pll_div.k = K;
}

static int wm8956_set_dai_pll(struct snd_soc_dai *codec_dai,
	int pll_id, int src, unsigned int freq_in, unsigned int freq_out)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u16 reg;
	int found = 0;
#if 0
	if (freq_in == 0 || freq_out == 0) {
		/* disable the pll */
		/* turn PLL power off */
	}
#endif

	pll_factors(freq_out * 8, freq_in);

	if (!found)
		return -EINVAL;

	reg = wm8956_read_reg_cache(codec, WM8956_PLLN) & 0x1e0;
	wm8956_write(codec, WM8956_PLLN, reg | (1<<5) | (pll_div.pre_div << 4)
		| pll_div.n);
	wm8956_write(codec, WM8956_PLLK1, pll_div.k >> 16 );
	wm8956_write(codec, WM8956_PLLK2, (pll_div.k >> 8) & 0xff);
	wm8956_write(codec, WM8956_PLLK3, pll_div.k &0xff);
	wm8956_write(codec, WM8956_CLOCK1, 4);

	return 0;
}

static int wm8956_set_dai_clkdiv(struct snd_soc_dai *codec_dai,
		int div_id, int div)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u16 reg;

	switch (div_id) {
	case WM8956_SYSCLKSEL:
		reg = wm8956_read_reg_cache(codec, WM8956_CLOCK1) & 0x1fe;
		wm8956_write(codec, WM8956_CLOCK1, reg | div);
		break;
	case WM8956_SYSCLKDIV:
		reg = wm8956_read_reg_cache(codec, WM8956_CLOCK1) & 0x1f9;
		wm8956_write(codec, WM8956_CLOCK1, reg | div);
		break;
	case WM8956_DACDIV:
		reg = wm8956_read_reg_cache(codec, WM8956_CLOCK1) & 0x1c7;
		wm8956_write(codec, WM8956_CLOCK1, reg | div);
		break;
	case WM8956_OPCLKDIV:
		reg = wm8956_read_reg_cache(codec, WM8956_PLLN) & 0x03f;
		wm8956_write(codec, WM8956_PLLN, reg | div);
		break;
	case WM8956_DCLKDIV:
		reg = wm8956_read_reg_cache(codec, WM8956_CLOCK2) & 0x03f;
		wm8956_write(codec, WM8956_CLOCK2, reg | div);
		break;
	case WM8956_TOCLKSEL:
		reg = wm8956_read_reg_cache(codec, WM8956_ADDCTL1) & 0x1fd;
		wm8956_write(codec, WM8956_ADDCTL1, reg | div);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

#define WM8956_RATES SNDRV_PCM_RATE_8000_96000

#define WM8956_FORMATS \
	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
	SNDRV_PCM_FMTBIT_S24_LE)

static struct snd_soc_dai_ops wm8956_dai_ops = {
	.hw_params = wm8956_hw_params,
	.digital_mute = wm8956_mute,
	.set_fmt = wm8956_set_dai_fmt,
	.set_clkdiv = wm8956_set_dai_clkdiv,
	.set_pll = wm8956_set_dai_pll,
};

struct snd_soc_dai wm8956_dai = {
	.name = "WM8956",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = WM8956_RATES,
		.formats = WM8956_FORMATS,},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = WM8956_RATES,
		.formats = WM8956_FORMATS,},
	.ops = &wm8956_dai_ops,
};
EXPORT_SYMBOL_GPL(wm8956_dai);


/* To complete PM */
static int wm8956_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->card->codec;

	wm8956_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int wm8956_resume(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->card->codec;
	int i;
	u8 data[2];
	u16 *cache = codec->reg_cache;

	/* Sync reg_cache with the hardware */
	for (i = 0; i < ARRAY_SIZE(wm8956_reg); i++) {
		data[0] = (i << 1) | ((cache[i] >> 8) & 0x0001);
		data[1] = cache[i] & 0x00ff;
		codec->hw_write(codec->control_data, data, 2);
	}
	wm8956_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	wm8956_set_bias_level(codec, codec->suspend_bias_level);
	return 0;
}

static int wm8956_probe(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec;
	int ret = 0;

	if (wm8956_codec == NULL) {
		dev_err(&pdev->dev, "Codec device not registered\n");
		return -ENODEV;
	}

	socdev->card->codec = wm8956_codec;
	codec = wm8956_codec;

	/* register pcms */
	ret = snd_soc_new_pcms(socdev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1);
	if (ret < 0) {
		dev_err(codec->dev, "failed to create pcms: %d\n", ret);
		goto pcm_err;
	}

	snd_soc_add_controls(codec, wm8956_snd_controls,
			     ARRAY_SIZE(wm8956_snd_controls));
	wm8956_add_widgets(codec);

	return 0;

pcm_err:
	return ret;
}

/* power down chip */
static int wm8956_remove(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);

	snd_soc_free_pcms(socdev);
	snd_soc_dapm_free(socdev);

	return 0;
}

struct snd_soc_codec_device soc_codec_dev_wm8956 = {
	.probe = 	wm8956_probe,
	.remove = 	wm8956_remove,
	.suspend = 	wm8956_suspend,
	.resume =	wm8956_resume,
};
EXPORT_SYMBOL_GPL(soc_codec_dev_wm8956);

static int wm8956_register(struct wm8956_priv *wm8956)
{
	int ret;
	struct snd_soc_codec *codec = &wm8956->codec;

	if (wm8956_codec) {
		dev_err(codec->dev, "Another WM8956 is registered\n");
		return -EINVAL;
	}

	mutex_init(&codec->mutex);
	INIT_LIST_HEAD(&codec->dapm_widgets);
	INIT_LIST_HEAD(&codec->dapm_paths);

	codec->private_data = wm8956;
	codec->name = "WM8956";
	codec->owner = THIS_MODULE;
	codec->read = wm8956_read_reg_cache;
	codec->write = wm8956_write;
	codec->bias_level = SND_SOC_BIAS_OFF;
	codec->set_bias_level = wm8956_set_bias_level;
	codec->dai = &wm8956_dai;
	codec->num_dai = 1;
	codec->reg_cache_size = WM8956_CACHEREGNUM;
	codec->reg_cache = &wm8956->reg_cache;

	memcpy(codec->reg_cache, wm8956_reg, sizeof(wm8956_reg));

	ret = wm8956_reset(codec);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to issue reset\n");
		return ret;
	}

	wm8956_dai.dev = codec->dev;

	wm8956_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	wm8956_codec = codec;

	ret = snd_soc_register_codec(codec);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to register codec: %d\n", ret);
		return ret;
	}

	ret = snd_soc_register_dai(&wm8956_dai);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to register DAI: %d\n", ret);
		snd_soc_unregister_codec(codec);
		return ret;
	}

	return 0;
}

static void wm8956_unregister(struct wm8956_priv *wm8956)
{
	wm8956_set_bias_level(&wm8956->codec, SND_SOC_BIAS_OFF);
	snd_soc_unregister_dai(&wm8956_dai);
	snd_soc_unregister_codec(&wm8956->codec);
	kfree(wm8956);
	wm8956_codec = NULL;
}

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
static __devinit int wm8956_i2c_probe(struct i2c_client *i2c,
				      const struct i2c_device_id *id)
{
	struct wm8956_priv *wm8956;
	struct snd_soc_codec *codec;

	wm8956 = kzalloc(sizeof(struct wm8956_priv), GFP_KERNEL);
	if (wm8956 == NULL)
		return -ENOMEM;

	codec = &wm8956->codec;
	codec->hw_write = (hw_write_t)i2c_master_send;

	i2c_set_clientdata(i2c, wm8956);
	codec->control_data = i2c;

	codec->dev = &i2c->dev;

	return wm8956_register(wm8956);
}

static __devexit int wm8956_i2c_remove(struct i2c_client *client)
{
	struct wm8956_priv *wm8956 = i2c_get_clientdata(client);
	wm8956_unregister(wm8956);
	return 0;
}

static const struct i2c_device_id wm8956_i2c_id[] = {
	{ "wm8956", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wm8956_i2c_id);

static struct i2c_driver wm8956_i2c_driver = {
	.driver = {
		.name = "WM8956 I2C Codec",
		.owner = THIS_MODULE,
	},
	.probe =    wm8956_i2c_probe,
	.remove =   __devexit_p(wm8956_i2c_remove),
	.id_table = wm8956_i2c_id,
};
#endif

static int __init wm8956_modinit(void)
{
	int ret;
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	ret = i2c_add_driver(&wm8956_i2c_driver);
	if (ret != 0) {
		printk(KERN_ERR "Failed to register WM8956 I2C driver: %d\n",
		       ret);
	}
#endif
	return 0;
}
module_init(wm8956_modinit);

static void __exit wm8956_exit(void)
{
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	i2c_del_driver(&wm8956_i2c_driver);
#endif
}
module_exit(wm8956_exit);

MODULE_DESCRIPTION("ASoC WM8956 driver");
MODULE_AUTHOR("Liam Girdwood");
MODULE_LICENSE("GPL");
