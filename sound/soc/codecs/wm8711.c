/*
 * wm8711.c  --  WM8711 ALSA SoC Audio driver
 *
 * Copyright 2006 Wolfson Microelectronics
 *
 * Author: Mike Arthur <linux@wolfsonmicro.com>
 *
 * Based on wm8731.c by Richard Purdie
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

#include "wm8711.h"

#define AUDIO_NAME "wm8711"
#define WM8711_VERSION "0.3"

/* codec private data */
struct wm8711_priv {
	unsigned int sysclk;
};

/*
 * wm8711 register cache
 * We can't read the WM8711 register space when we are
 * using 2 wire for device control, so we cache them instead.
 * There is no point in caching the reset register
 */
static const u16 wm8711_reg[WM8711_CACHEREGNUM] = {
	0x0079, 0x0079, 0x000a, 0x0008,
	0x009f, 0x000a, 0x0000, 0x0000
};

/*
 * read wm8711 register cache
 */
static inline unsigned int wm8711_read_reg_cache(struct snd_soc_codec *codec,
	unsigned int reg)
{
	u16 *cache = codec->reg_cache;
	if (reg == WM8711_RESET)
		return 0;
	if (reg >= WM8711_CACHEREGNUM)
		return -1;
	return cache[reg];
}

/*
 * write wm8711 register cache
 */
static inline void wm8711_write_reg_cache(struct snd_soc_codec *codec,
	u16 reg, unsigned int value)
{
	u16 *cache = codec->reg_cache;
	if (reg >= WM8711_CACHEREGNUM)
		return;
	cache[reg] = value;
}

/*
 * write to the WM8711 register space
 */
static int wm8711_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	u8 data[2];

	/* data is
	 *   D15..D9 WM8753 register offset
	 *   D8...D0 register data
	 */
	data[0] = (reg << 1) | ((value >> 8) & 0x0001);
	data[1] = value & 0x00ff;

	wm8711_write_reg_cache(codec, reg, value);
	if (codec->hw_write(codec->control_data, data, 2) == 2)
		return 0;
	else
		return -EIO;
}

#define wm8711_reset(c)	wm8711_write(c, WM8711_RESET, 0)

static const struct snd_kcontrol_new wm8711_snd_controls[] = {

SOC_DOUBLE_R("Master Playback Volume", WM8711_LOUT1V, WM8711_ROUT1V,
	0, 127, 0),
SOC_DOUBLE_R("Master Playback ZC Switch", WM8711_LOUT1V, WM8711_ROUT1V,
	7, 1, 0),

};

/* add non dapm controls */
static int wm8711_add_controls(struct snd_soc_codec *codec)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(wm8711_snd_controls); i++) {
		err = snd_ctl_add(codec->card,
				snd_soc_cnew(&wm8711_snd_controls[i], codec,
					NULL));
		if (err < 0)
			return err;
	}

	return 0;
}

/* Output Mixer */
static const struct snd_kcontrol_new wm8711_output_mixer_controls[] = {
SOC_DAPM_SINGLE("Line Bypass Switch", WM8711_APANA, 3, 1, 0),
SOC_DAPM_SINGLE("HiFi Playback Switch", WM8711_APANA, 4, 1, 0),
};

static const struct snd_soc_dapm_widget wm8711_dapm_widgets[] = {
SND_SOC_DAPM_MIXER("Output Mixer", WM8711_PWR, 4, 1,
	&wm8711_output_mixer_controls[0],
	ARRAY_SIZE(wm8711_output_mixer_controls)),
SND_SOC_DAPM_DAC("DAC", "HiFi Playback", WM8711_PWR, 3, 1),
SND_SOC_DAPM_OUTPUT("LOUT"),
SND_SOC_DAPM_OUTPUT("LHPOUT"),
SND_SOC_DAPM_OUTPUT("ROUT"),
SND_SOC_DAPM_OUTPUT("RHPOUT"),
};

static const struct snd_soc_dapm_route intercon[] = {
	/* output mixer */
	{"Output Mixer", "Line Bypass Switch", "Line Input"},
	{"Output Mixer", "HiFi Playback Switch", "DAC"},

	/* outputs */
	{"RHPOUT", NULL, "Output Mixer"},
	{"ROUT", NULL, "Output Mixer"},
	{"LHPOUT", NULL, "Output Mixer"},
	{"LOUT", NULL, "Output Mixer"},
};

static int wm8711_add_widgets(struct snd_soc_codec *codec)
{
	snd_soc_dapm_new_controls(codec, wm8711_dapm_widgets,
				  ARRAY_SIZE(wm8711_dapm_widgets));

	snd_soc_dapm_add_routes(codec, intercon, ARRAY_SIZE(intercon));

	snd_soc_dapm_new_widgets(codec);
	return 0;
}

struct _coeff_div {
	u32 mclk;
	u32 rate;
	u16 fs;
	u8 sr:4;
	u8 bosr:1;
	u8 usb:1;
};

/* codec mclk clock divider coefficients */
static const struct _coeff_div coeff_div[] = {
	/* 48k */
	{12288000, 48000, 256, 0x0, 0x0, 0x0},
	{18432000, 48000, 384, 0x0, 0x1, 0x0},
	{12000000, 48000, 250, 0x0, 0x0, 0x1},

	/* 32k */
	{12288000, 32000, 384, 0x6, 0x0, 0x0},
	{18432000, 32000, 576, 0x6, 0x1, 0x0},
	{12000000, 32000, 375, 0x6, 0x0, 0x1},

	/* 8k */
	{12288000, 8000, 1536, 0x3, 0x0, 0x0},
	{18432000, 8000, 2304, 0x3, 0x1, 0x0},
	{11289600, 8000, 1408, 0xb, 0x0, 0x0},
	{16934400, 8000, 2112, 0xb, 0x1, 0x0},
	{12000000, 8000, 1500, 0x3, 0x0, 0x1},

	/* 96k */
	{12288000, 96000, 128, 0x7, 0x0, 0x0},
	{18432000, 96000, 192, 0x7, 0x1, 0x0},
	{12000000, 96000, 125, 0x7, 0x0, 0x1},

	/* 44.1k */
	{11289600, 44100, 256, 0x8, 0x0, 0x0},
	{16934400, 44100, 384, 0x8, 0x1, 0x0},
	{12000000, 44100, 272, 0x8, 0x1, 0x1},

	/* 88.2k */
	{11289600, 88200, 128, 0xf, 0x0, 0x0},
	{16934400, 88200, 192, 0xf, 0x1, 0x0},
	{12000000, 88200, 136, 0xf, 0x1, 0x1},
};

static inline int get_coeff(int mclk, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(coeff_div); i++) {
		if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
			return i;
	}
	return 0;
}

static int wm8711_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params,
	struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_device *socdev = rtd->socdev;
	struct snd_soc_codec *codec = socdev->codec;
	struct wm8711_priv *wm8711 = codec->private_data;
	u16 iface = wm8711_read_reg_cache(codec, WM8711_IFACE) & 0xfffc;
	int i = get_coeff(wm8711->sysclk, params_rate(params));
	u16 srate = (coeff_div[i].sr << 2) |
		(coeff_div[i].bosr << 1) | coeff_div[i].usb;

	wm8711_write(codec, WM8711_SRATE, srate);

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

	wm8711_write(codec, WM8711_IFACE, iface);
	return 0;
}

static int wm8711_pcm_prepare(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_device *socdev = rtd->socdev;
	struct snd_soc_codec *codec = socdev->codec;

	/* set active */
	wm8711_write(codec, WM8711_ACTIVE, 0x0001);

	return 0;
}

static void wm8711_shutdown(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_device *socdev = rtd->socdev;
	struct snd_soc_codec *codec = socdev->codec;

	/* deactivate */
	if (!codec->active) {
		udelay(50);
		wm8711_write(codec, WM8711_ACTIVE, 0x0);
	}
}

static int wm8711_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	u16 mute_reg = wm8711_read_reg_cache(codec, WM8711_APDIGI) & 0xfff7;

	if (mute)
		wm8711_write(codec, WM8711_APDIGI, mute_reg | 0x8);
	else
		wm8711_write(codec, WM8711_APDIGI, mute_reg);

	return 0;
}

static int wm8711_set_dai_sysclk(struct snd_soc_dai *codec_dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8711_priv *wm8711 = codec->private_data;

	switch (freq) {
	case 11289600:
	case 12000000:
	case 12288000:
	case 16934400:
	case 18432000:
		wm8711->sysclk = freq;
		return 0;
	}
	return -EINVAL;
}

static int wm8711_set_dai_fmt(struct snd_soc_dai *codec_dai,
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
	wm8711_write(codec, WM8711_IFACE, iface);
	return 0;
}


static int wm8711_set_bias_level(struct snd_soc_codec *codec,
	enum snd_soc_bias_level level)
{
	u16 reg = wm8711_read_reg_cache(codec, WM8711_PWR) & 0xff7f;

	switch (level) {
	case SND_SOC_BIAS_ON:
		wm8711_write(codec, WM8711_PWR, reg);
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	case SND_SOC_BIAS_STANDBY:
		wm8711_write(codec, WM8711_PWR, reg | 0x0040);
		break;
	case SND_SOC_BIAS_OFF:
		wm8711_write(codec, WM8711_ACTIVE, 0x0);
		wm8711_write(codec, WM8711_PWR, 0xffff);
		break;
	}
	codec->bias_level = level;
	return 0;
}

#define WM8711_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |\
		SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |\
		SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |\
		SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |\
		SNDRV_PCM_RATE_96000)

#define WM8711_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
	SNDRV_PCM_FMTBIT_S24_LE)

struct snd_soc_dai wm8711_dai = {
	.name = "WM8711",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = WM8711_RATES,
		.formats = WM8711_FORMATS,},
	.ops = {
		.prepare = wm8711_pcm_prepare,
		.hw_params = wm8711_hw_params,
		.shutdown = wm8711_shutdown,
		.digital_mute = wm8711_mute,
		.set_sysclk = wm8711_set_dai_sysclk,
		.set_fmt = wm8711_set_dai_fmt,
	},
};
EXPORT_SYMBOL_GPL(wm8711_dai);

static int wm8711_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->codec;

	wm8711_write(codec, WM8711_ACTIVE, 0x0);
	wm8711_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

static int wm8711_resume(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->codec;
	int i;
	u8 data[2];
	u16 *cache = codec->reg_cache;

	/* Sync reg_cache with the hardware */
	for (i = 0; i < ARRAY_SIZE(wm8711_reg); i++) {
		data[0] = (i << 1) | ((cache[i] >> 8) & 0x0001);
		data[1] = cache[i] & 0x00ff;
		codec->hw_write(codec->control_data, data, 2);
	}
	wm8711_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
	wm8711_set_bias_level(codec, codec->suspend_bias_level);
	return 0;
}

/*
 * initialise the WM8711 driver
 * register the mixer and dsp interfaces with the kernel
 */
static int wm8711_init(struct snd_soc_device *socdev)
{
	struct snd_soc_codec *codec = socdev->codec;
	int reg, ret = 0;

	codec->name = "WM8711";
	codec->owner = THIS_MODULE;
	codec->read = wm8711_read_reg_cache;
	codec->write = wm8711_write;
	codec->set_bias_level = wm8711_set_bias_level;
	codec->dai = &wm8711_dai;
	codec->num_dai = 1;
	codec->reg_cache_size = ARRAY_SIZE(wm8711_reg);
	codec->reg_cache = kmemdup(wm8711_reg, sizeof(wm8711_reg), GFP_KERNEL);

	if (codec->reg_cache == NULL)
		return -ENOMEM;

	wm8711_reset(codec);

	/* register pcms */
	ret = snd_soc_new_pcms(socdev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1);
	if (ret < 0) {
		printk(KERN_ERR "wm8711: failed to create pcms\n");
		goto pcm_err;
	}

	/* power on device */
	wm8711_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	/* set the update bits */
	reg = wm8711_read_reg_cache(codec, WM8711_LOUT1V);
	wm8711_write(codec, WM8711_LOUT1V, reg | 0x0100);
	reg = wm8711_read_reg_cache(codec, WM8711_ROUT1V);
	wm8711_write(codec, WM8711_ROUT1V, reg | 0x0100);

	wm8711_add_controls(codec);
	wm8711_add_widgets(codec);
	ret = snd_soc_init_card(socdev);
	if (ret < 0) {
		printk(KERN_ERR "wm8711: failed to register card\n");
		goto card_err;
	}
	return ret;

card_err:
	snd_soc_free_pcms(socdev);
	snd_soc_dapm_free(socdev);
pcm_err:
	kfree(codec->reg_cache);
	return ret;
}

static struct snd_soc_device *wm8711_socdev;

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)

/*
 * WM8711 2 wire address is determined by GPIO5
 * state during powerup.
 *    low  = 0x1a
 *    high = 0x1b
 */
#define I2C_DRIVERID_WM8711 0xfefe /* liam -  need a proper id */

static unsigned short normal_i2c[] = { 0, I2C_CLIENT_END };

/* Magic definition of all other variables and things */
I2C_CLIENT_INSMOD;

static struct i2c_driver wm8711_i2c_driver;
static struct i2c_client client_template;

/* If the i2c layer weren't so broken, we could pass this kind of data
   around */

static int wm8711_codec_probe(struct i2c_adapter *adap, int addr, int kind)
{
	struct snd_soc_device *socdev = wm8711_socdev;
	struct wm8711_setup_data *setup = socdev->codec_data;
	struct snd_soc_codec *codec = socdev->codec;
	struct i2c_client *i2c;
	int ret;

	if (addr != setup->i2c_address)
		return -ENODEV;

	client_template.adapter = adap;
	client_template.addr = addr;

	i2c = kmemdup(&client_template, sizeof(client_template), GFP_KERNEL);
	if (i2c == NULL) {
		kfree(codec);
		return -ENOMEM;
	}

	i2c_set_clientdata(i2c, codec);

	codec->control_data = i2c;

	ret = i2c_attach_client(i2c);
	if (ret < 0) {
		pr_err("failed to attach codec at addr %x\n", addr);
		goto err;
	}

	ret = wm8711_init(socdev);
	if (ret < 0) {
		pr_err("failed to initialise WM8711\n");
		goto err;
	}
	return ret;

err:
	kfree(codec);
	kfree(i2c);
	return ret;
}

static int wm8711_i2c_detach(struct i2c_client *client)
{
	struct snd_soc_codec *codec = i2c_get_clientdata(client);

	i2c_detach_client(client);
	kfree(codec->reg_cache);
	kfree(client);
	return 0;
}

static int wm8711_i2c_attach(struct i2c_adapter *adap)
{
	return i2c_probe(adap, &addr_data, wm8711_codec_probe);
}

/* corgi i2c codec control layer */
static struct i2c_driver wm8711_i2c_driver = {
	.driver = {
		.name = "WM8711 I2C Codec",
		.owner = THIS_MODULE,
	},
	.id =             I2C_DRIVERID_WM8711,
	.attach_adapter = wm8711_i2c_attach,
	.detach_client =  wm8711_i2c_detach,
	.command =        NULL,
};

static struct i2c_client client_template = {
	.name =   "WM8711",
	.driver = &wm8711_i2c_driver,
};
#endif

static int wm8711_probe(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct wm8711_setup_data *setup;
	struct snd_soc_codec *codec;
	struct wm8711_priv *wm8711;
	int ret = 0;

	pr_info("WM8711 Audio Codec %s", WM8711_VERSION);

	setup = socdev->codec_data;
	codec = kzalloc(sizeof(struct snd_soc_codec), GFP_KERNEL);
	if (codec == NULL)
		return -ENOMEM;

	wm8711 = kzalloc(sizeof(struct wm8711_priv), GFP_KERNEL);
	if (wm8711 == NULL) {
		kfree(codec);
		return -ENOMEM;
	}

	codec->private_data = wm8711;
	socdev->codec = codec;
	mutex_init(&codec->mutex);
	INIT_LIST_HEAD(&codec->dapm_widgets);
	INIT_LIST_HEAD(&codec->dapm_paths);

	wm8711_socdev = socdev;
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	if (setup->i2c_address) {
		normal_i2c[0] = setup->i2c_address;
		codec->hw_write = (hw_write_t)i2c_master_send;
		ret = i2c_add_driver(&wm8711_i2c_driver);
		if (ret != 0)
			printk(KERN_ERR "can't add i2c driver");
	}
#else
	/* Add other interfaces here */
#endif
	return ret;
}

/* power down chip */
static int wm8711_remove(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->codec;

	if (codec->control_data)
		wm8711_set_bias_level(codec, SND_SOC_BIAS_OFF);

	snd_soc_free_pcms(socdev);
	snd_soc_dapm_free(socdev);
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
	i2c_del_driver(&wm8711_i2c_driver);
#endif
	kfree(codec->private_data);
	kfree(codec);

	return 0;
}

struct snd_soc_codec_device soc_codec_dev_wm8711 = {
	.probe = 	wm8711_probe,
	.remove = 	wm8711_remove,
	.suspend = 	wm8711_suspend,
	.resume =	wm8711_resume,
};
EXPORT_SYMBOL_GPL(soc_codec_dev_wm8711);

static int __init wm8711_modinit(void)
{
	return snd_soc_register_dai(&wm8711_dai);
}
module_init(wm8711_modinit);

static void __exit wm8711_exit(void)
{
	snd_soc_unregister_dai(&wm8711_dai);
}
module_exit(wm8711_exit);

MODULE_DESCRIPTION("ASoC WM8711 driver");
MODULE_AUTHOR("Mike Arthur");
MODULE_LICENSE("GPL");
