/*
 *  pacific_arizona.c
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <sound/tlv.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/wakelock.h>

#include <linux/mfd/arizona/registers.h>
#include <linux/mfd/arizona/core.h>

#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#include <mach/exynos-audio.h>

#include "i2s.h"
#include "i2s-regs.h"
#include "../codecs/wm5102.h"
#include "../codecs/florida.h"
#include "../codecs/vegas.h"


#if defined(CONFIG_SND_SOC_MAX98504A)
#include "../codecs/max98504a.h"
#endif

#if defined(CONFIG_SND_SOC_MAXIM_DSM_A)
#include <sound/maxim_dsm_a.h>
#endif

/* PACIFIC use CLKOUT from AP */
#define PACIFIC_MCLK_FREQ	24000000
#define PACIFIC_AUD_PLL_FREQ	393216018

#define PACIFIC_DEFAULT_MCLK1	24000000
#define PACIFIC_DEFAULT_MCLK2	32768

#define PACIFIC_RUN_MAINMIC	2
#define PACIFIC_RUN_EARMIC	1

static DECLARE_TLV_DB_SCALE(digital_tlv, -6400, 50, 0);

enum {
	PACIFIC_PLAYBACK_DAI,
	PACIFIC_VOICECALL_DAI,
	PACIFIC_BT_SCO_DAI,
	PACIFIC_MULTIMEDIA_DAI,
};

enum {
	BYTELOGINFO,
	INTLOGINFO,
	AFTERBYTELOGINFO,
	AFTERINTLOGINFO,
};

struct arizona_machine_priv {
	int mic_bias_gpio;
	int sub_mic_bias_gpio;
	struct snd_soc_codec *codec;
	struct snd_soc_dai *aif[3];
	int voice_uevent;
	int seamless_voicewakeup;
	int voicetriginfo;
	struct input_dev *input;
	int aif2mode;
	int voice_key_event;
	struct clk *mclk;
	struct wake_lock wake_lock;

	unsigned int hp_impedance_step;
	bool ear_mic;

	int sysclk_rate;
	int asyncclk_rate;

	u32 aif_format[3];
	int (*external_amp)(int onoff);
};

static struct class *svoice_class;
static struct device *keyword_dev;
unsigned int keyword_type;

static struct arizona_machine_priv pacific_wm5102_priv = {
	.sysclk_rate = 49152000,
	.asyncclk_rate = 49152000,
};

static struct arizona_machine_priv pacific_wm1814_priv = {
	.sysclk_rate = 49152000,
	.asyncclk_rate = 49152000,
};

static struct arizona_machine_priv pacific_wm5110_priv = {
	.sysclk_rate = 147456000,
	.asyncclk_rate = 49152000,
};

const char *aif2_mode_text[] = {
	"Slave", "Master"
};

const char *voicecontrol_mode_text[] = {
	"Normal", "LPSD"
};

static const struct soc_enum aif2_mode_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(aif2_mode_text), aif2_mode_text),
};

static const struct soc_enum voicecontrol_mode_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(voicecontrol_mode_text),
			voicecontrol_mode_text),
};

static const struct snd_soc_component_driver pacific_cmpnt = {
	.name	= "pacific-audio",
};

static struct {
	int min;           /* Minimum impedance */
	int max;           /* Maximum impedance */
	unsigned int gain; /* Register value to set for this measurement */
} hp_gain_table[] = {
#if defined(CONFIG_SND_SAMSUNG_COMPENSATE_EXT_RES)
	{    0,      45, 0 },
	{   46,     100, 4 },
	{  101,     200, 8 },
	{  201,     450, 12 },
	{  451,    1000, 14 },
	{ 1001, INT_MAX, 0 },
#else
	{    0,      42, 0 },
	{   43,     100, 4 },
	{  101,     200, 8 },
	{  201,     450, 12 },
	{  451,    1000, 14 },
	{ 1001, INT_MAX, 0 },
#endif
};

#if defined(CONFIG_SND_SOC_MAXIM_DSM_A)
static struct {
	unsigned int start;	/* start address */
	unsigned int size;	/* word size */
} dsm_log_info[] = {
	{ 0x49007C, 44 },
	{ 0x4900AA, 44 },
	{ 0x4900DA, 80 },
	{ 0x49012A, 90 },
};

static struct {
	unsigned int start;	/* start address */
	unsigned int size;	/* word size */
} dsm_param_info[] = {
	{ 0x490052, 40 },
};
#endif

static struct snd_soc_codec *the_codec;

void pacific_arizona_hpdet_cb(unsigned int meas)
{
	int i;
	struct arizona_machine_priv *priv;

	WARN_ON(!the_codec);
	if (!the_codec)
		return;

	priv = the_codec->card->drvdata;

	for (i = 0; i < ARRAY_SIZE(hp_gain_table); i++) {
		if (meas < hp_gain_table[i].min || meas > hp_gain_table[i].max)
			continue;

		dev_info(the_codec->dev, "SET GAIN %d step for %d ohms\n",
			 hp_gain_table[i].gain, meas);
		priv->hp_impedance_step = hp_gain_table[i].gain;
	}
}

void pacific_arizona_micd_cb(bool mic)
{
	struct arizona_machine_priv *priv;

	WARN_ON(!the_codec);
	if (!the_codec)
		return;

	priv = the_codec->card->drvdata;

	priv->ear_mic = mic;
	dev_info(the_codec->dev, "%s: ear_mic = %d\n", __func__, priv->ear_mic);
}

void pacific_update_impedance_table(struct device_node *np)
{
	int len = ARRAY_SIZE(hp_gain_table);
	u32 data[len * 3];
	int i, shift;

	WARN_ON(!the_codec);
	if (!the_codec)
		return;

	if (!of_property_read_u32_array(np, "imp_table", data, (len * 3))) {
		dev_info(the_codec->dev, "%s: data from DT\n", __func__);

		for (i = 0; i < len; i++) {
			hp_gain_table[i].min = data[i * 3];
			hp_gain_table[i].max = data[(i * 3) + 1];
			hp_gain_table[i].gain = data[(i * 3) + 2];
			dev_info(the_codec->dev, "%s: min=%d,max=%d ==> gain=%d\n", __func__, 
				hp_gain_table[i].min,hp_gain_table[i].max,
				hp_gain_table[i].gain);
		}
	}

	if (!of_property_read_u32(np, "imp_shift", &shift)) {
		dev_info(the_codec->dev, "%s: shift = %d\n", __func__, shift);

		for (i = 0; i < len; i++) {
			if (hp_gain_table[i].min != 0)
				hp_gain_table[i].min += shift;
			if ((hp_gain_table[i].max + shift) < INT_MAX)
				hp_gain_table[i].max += shift;
		}
	}
}

static int arizona_put_impedance_volsw(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int shift = mc->shift;
	int max = mc->max;
	unsigned int mask = (1 << fls(max)) - 1;
	unsigned int invert = mc->invert;
	int err;
	unsigned int val, val_mask;

	val = (ucontrol->value.integer.value[0] & mask);
	val += priv->hp_impedance_step;
	dev_info(codec->dev,
			 "SET GAIN %d according to impedance, moved %d step\n",
			 val, priv->hp_impedance_step);

	if (invert)
		val = max - val;
	val_mask = mask << shift;
	val = val << shift;

	err = snd_soc_update_bits(codec, reg, val_mask, val);
	if (err < 0)
		return err;

	return err;
}


static int get_aif2_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;

	ucontrol->value.integer.value[0] = priv->aif2mode;
	return 0;
}

static int set_aif2_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;

	priv->aif2mode = ucontrol->value.integer.value[0];

	dev_info(codec->dev, "set aif2 mode: %s\n",
					 aif2_mode_text[priv->aif2mode]);
	return  0;
}

static int get_voicecontrol_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;

	if (priv->seamless_voicewakeup) {
		ucontrol->value.integer.value[0] = priv->voice_uevent;
	} else {
		if (priv->voice_key_event == KEY_VOICE_WAKEUP)
			ucontrol->value.integer.value[0] = 0;
		else
			ucontrol->value.integer.value[0] = 1;
	}

	return 0;
}

static int set_voicecontrol_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;
	int voicecontrol_mode;

	voicecontrol_mode = ucontrol->value.integer.value[0];

	if (priv->seamless_voicewakeup) {
		priv->voice_uevent = voicecontrol_mode;
		dev_info(codec->dev, "set voice control mode: %s\n",
			voicecontrol_mode_text[priv->voice_uevent]);
	} else {
		if (voicecontrol_mode == 0) {
			priv->voice_key_event = KEY_VOICE_WAKEUP;
		} else if (voicecontrol_mode == 1) {
			priv->voice_key_event = KEY_LPSD_WAKEUP;
		} else {
			dev_err(the_codec->card->dev, "Invalid voice control mode =%d",
					voicecontrol_mode);
			return 0;
		}

		dev_info(codec->dev, "set voice control mode: %s key_event = %d\n",
				voicecontrol_mode_text[voicecontrol_mode],
				priv->voice_key_event);
	}

	return  0;
}

static int get_voice_tracking_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned int reg = mc->reg;
	unsigned int reg2 = mc->reg + 2;
	unsigned int shift = mc->shift;
	int max = mc->max;
	unsigned int invert = mc->invert;
	unsigned int val1, val2;

	val1  = (snd_soc_read(codec, reg) >> shift) & 0xFFFF;
	val2  = (snd_soc_read(codec, reg2) >> shift) & 0xFFFF;

	ucontrol->value.enumerated.item[0] = ((val1 << 16) | val2);

	if (invert)
		ucontrol->value.enumerated.item[0] =
			max - ucontrol->value.enumerated.item[0];

	dev_info(codec->dev, "get voice tracking info- degree|energy :0x%08x\n",
			ucontrol->value.enumerated.item[0]);

	return 0;
}

static int set_voice_tracking_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	dev_info(codec->dev, "set voice tracking info\n");
	return  0;
}

static int get_voice_trigger_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;

	ucontrol->value.integer.value[0] = priv->voicetriginfo;
	return 0;
}

static int set_voice_trigger_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct arizona_machine_priv *priv = codec->card->drvdata;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	unsigned int val, val1, val2;

	priv->voicetriginfo = ucontrol->value.integer.value[0];

	if (priv->voicetriginfo == 0)
		val = priv->voicetriginfo/15;
	else if (priv->voicetriginfo > 0)
		val = priv->voicetriginfo/15 + 1;
	else
		val = 0xFFFFFF + priv->voicetriginfo/15;

	val1 = val & 0xFFFF;
	val2 = (val >> 16) & 0xFFFF;

	snd_soc_write(codec, reg + 1, val1);
	snd_soc_write(codec, reg, val2);

	dev_info(codec->dev, "set voice trigger info: %d, val=0x%x\n", priv->voicetriginfo, val);
	return  0;
}

#if defined(CONFIG_SND_SOC_MAXIM_DSM_A)
static void create_dsm_log_dump(struct snd_soc_codec *codec)
{
	int i;
	unsigned int reg, val1, val2;
	unsigned int byte_buf[dsm_log_info[BYTELOGINFO].size];
	unsigned int int_buf[dsm_log_info[INTLOGINFO].size];
	unsigned int after_byte_buf[dsm_log_info[AFTERBYTELOGINFO].size];
	unsigned int after_int_buf[dsm_log_info[AFTERINTLOGINFO].size];

	dev_info(codec->dev, "%s: ++\n", __func__);

	memset(byte_buf, 0, sizeof(byte_buf));
	memset(int_buf, 0, sizeof(int_buf));
	memset(after_byte_buf, 0, sizeof(after_byte_buf));
	memset(after_int_buf, 0, sizeof(after_int_buf));

	for (i = 0; i < dsm_log_info[BYTELOGINFO].size; i++) {
		reg = dsm_log_info[BYTELOGINFO].start + i*2;
		val1  = (snd_soc_read(codec, reg)) & 0xFFFF;
		val2  = (snd_soc_read(codec, reg + 1)) & 0xFFFF;
		byte_buf[i] = ((val1 << 16) | val2);
	}

	for (i = 0; i < dsm_log_info[INTLOGINFO].size; i++) {
		reg = dsm_log_info[INTLOGINFO].start + i*2;
		val1  = (snd_soc_read(codec, reg)) & 0xFFFF;
		val2  = (snd_soc_read(codec, reg + 1)) & 0xFFFF;
		int_buf[i] = ((val1 << 16) | val2);
	}

	for (i = 0; i < dsm_log_info[AFTERBYTELOGINFO].size; i++) {
		reg = dsm_log_info[AFTERBYTELOGINFO].start + i*2;
		val1  = (snd_soc_read(codec, reg)) & 0xFFFF;
		val2  = (snd_soc_read(codec, reg + 1)) & 0xFFFF;
		after_byte_buf[i] = ((val1 << 16) | val2);
	}

	for (i = 0; i < dsm_log_info[AFTERINTLOGINFO].size; i++) {
		reg = dsm_log_info[AFTERINTLOGINFO].start + i*2;
		val1  = (snd_soc_read(codec, reg)) & 0xFFFF;
		val2  = (snd_soc_read(codec, reg + 1)) & 0xFFFF;
		after_int_buf[i] = ((val1 << 16) | val2);
	}
	maxdsm_log_update(byte_buf, int_buf, after_byte_buf, after_int_buf);

	dev_info(codec->dev, "%s: --\n", __func__);
}

static int max98504_get_dump_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned int val1, val2;

	val1  = (snd_soc_read(codec, 0x49007c)) & 0xFFFF;
	val2  = (snd_soc_read(codec, 0x49007d)) & 0xFFFF;

	ucontrol->value.enumerated.item[0] = ((val1 << 16) | val2);

	dev_info(codec->dev, "%s: dsm dump status 0x%08x(0x%x,0x%x)\n",
			__func__, ucontrol->value.enumerated.item[0],
			val1, val2);

	return 0;
}

static int max98504_set_dump_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned int log_available =
		snd_soc_read(codec, ARIZONA_DSP4_CONTROL_1);

	dev_info(codec->dev, "%s: log_available = 0x%x\n",
			__func__, log_available);

	if (log_available & 0x3)
		create_dsm_log_dump(codec);

	return 0;
}

static void create_dsm_param_dump(struct snd_soc_codec *codec)
{
	int i;
	unsigned int reg, val1, val2;
	unsigned int param_buf[dsm_param_info[0].size];

	dev_info(codec->dev, "%s: ++\n", __func__);

	memset(param_buf, 0, sizeof(param_buf));

	for (i = 0; i < dsm_param_info[0].size; i++) {
		reg = dsm_param_info[0].start + i*2;
		val1 = (snd_soc_read(codec, reg)) & 0xFFFF;
		val2 = (snd_soc_read(codec, reg + 1)) & 0xFFFF;
		param_buf[i] = ((val1 << 16) | val2);
	}
	maxdsm_param_update(param_buf);

	dev_info(codec->dev, "%s: --\n", __func__);
}

static int max98504_get_param_dump_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int max98504_set_param_dump_status(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned int param_available =
		snd_soc_read(codec, ARIZONA_DSP4_CONTROL_1);

	if (param_available & 0x3)
		create_dsm_param_dump(codec);

	return  0;
}

#endif

static int pacific_external_amp(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct arizona_machine_priv *priv = card->drvdata;
	struct snd_soc_codec *codec = priv->codec;

	dev_dbg(codec->dev, "%s: %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (priv->external_amp)
			priv->external_amp(1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (priv->external_amp)
			priv->external_amp(0);
		break;
	}

	return 0;
}

static int pacific_ext_mainmicbias(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct arizona_machine_priv *priv = card->drvdata;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		gpio_set_value(priv->mic_bias_gpio,  1);
		break;
	case SND_SOC_DAPM_POST_PMD:
		gpio_set_value(priv->mic_bias_gpio,  0);
		break;
	}

	dev_err(w->dapm->dev, "Main Mic BIAS: %d\n", event);

	return 0;
}

#ifdef CONFIG_SND_USE_SUBMIC_BIAS
static int wm5102_ext_submicbias(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol,  int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct arizona_machine_priv *priv = card->drvdata;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		gpio_set_value(priv->sub_mic_bias_gpio,  1);
		break;
	case SND_SOC_DAPM_POST_PMD:
		gpio_set_value(priv->sub_mic_bias_gpio,  0);
		break;
	}

	dev_err(w->dapm->dev, "Sub Mic BIAS: %d mic_bias_gpio %d %d\n",
		event, priv->sub_mic_bias_gpio, gpio_get_value(priv->sub_mic_bias_gpio));

	return 0;
}
#endif

static int wm1814_ext_submicbias(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol,  int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct arizona_machine_priv *priv = card->drvdata;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		gpio_set_value(priv->sub_mic_bias_gpio,  1);
		break;
	case SND_SOC_DAPM_POST_PMD:
		gpio_set_value(priv->sub_mic_bias_gpio,  0);
		break;
	}

	dev_err(w->dapm->dev, "Sub Mic BIAS: %d mic_bias_gpio %d %d\n",
		event, priv->sub_mic_bias_gpio, gpio_get_value(priv->sub_mic_bias_gpio));

	return 0;
}

static const struct snd_kcontrol_new pacific_codec_controls[] = {
	SOC_ENUM_EXT("AIF2 Mode", aif2_mode_enum[0],
		get_aif2_mode, set_aif2_mode),

	SOC_SINGLE_EXT_TLV("HPOUT1L Impedance Volume",
		ARIZONA_DAC_DIGITAL_VOLUME_1L,
		ARIZONA_OUT1L_VOL_SHIFT, 0xbf, 0,
		snd_soc_get_volsw, arizona_put_impedance_volsw,
		digital_tlv),

	SOC_SINGLE_EXT_TLV("HPOUT1R Impedance Volume",
		ARIZONA_DAC_DIGITAL_VOLUME_1R,
		ARIZONA_OUT1L_VOL_SHIFT, 0xbf, 0,
		snd_soc_get_volsw, arizona_put_impedance_volsw,
		digital_tlv),

	SOC_ENUM_EXT("VoiceControl Mode", voicecontrol_mode_enum[0],
		get_voicecontrol_mode, set_voicecontrol_mode),

	SOC_SINGLE_EXT("VoiceTrackInfo",
		ARIZONA_DSP3_SCRATCH_1,
		0, 0xff, 0,
		get_voice_tracking_info, set_voice_tracking_info),

	SOC_SINGLE_EXT("VoiceTrigInfo",
		0x380006, 0, 0xff, 0,
		get_voice_trigger_info, set_voice_trigger_info),

#if defined(CONFIG_SND_SOC_MAXIM_DSM_A)
	SOC_SINGLE_EXT("DSM LOG",
		SND_SOC_NOPM,
		0, 0xff, 0,
		max98504_get_dump_status, max98504_set_dump_status),
	SOC_SINGLE_EXT("DSM PARAM",
		SND_SOC_NOPM,
		0, 0xff, 0,
		max98504_get_param_dump_status, max98504_set_param_dump_status),
#endif
};

static const struct snd_kcontrol_new pacific_controls[] = {
	SOC_DAPM_PIN_SWITCH("HP"),
	SOC_DAPM_PIN_SWITCH("SPK"),
	SOC_DAPM_PIN_SWITCH("RCV"),
	SOC_DAPM_PIN_SWITCH("VPS"),
	SOC_DAPM_PIN_SWITCH("HDMI"),
	SOC_DAPM_PIN_SWITCH("Main Mic"),
	SOC_DAPM_PIN_SWITCH("Sub Mic"),
	SOC_DAPM_PIN_SWITCH("Third Mic"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("VI SENSING"),
};

const struct snd_soc_dapm_widget wm5102_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("HDMIL"),
	SND_SOC_DAPM_OUTPUT("HDMIR"),
	SND_SOC_DAPM_HP("HP", NULL),
	SND_SOC_DAPM_SPK("SPK", pacific_external_amp),
	SND_SOC_DAPM_SPK("RCV", NULL),
	SND_SOC_DAPM_LINE("VPS", NULL),
	SND_SOC_DAPM_LINE("HDMI", NULL),

	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
 #ifdef CONFIG_SND_USE_SUBMIC_BIAS
	SND_SOC_DAPM_MIC("Sub Mic", wm5102_ext_submicbias),
 #else
	SND_SOC_DAPM_MIC("Sub Mic", NULL),
 #endif
	SND_SOC_DAPM_MIC("Third Mic", NULL),
	SND_SOC_DAPM_MIC("VI SENSING", NULL),
};

const struct snd_soc_dapm_widget pacific_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("HDMIL"),
	SND_SOC_DAPM_OUTPUT("HDMIR"),
	SND_SOC_DAPM_HP("HP", NULL),
	SND_SOC_DAPM_SPK("SPK", pacific_external_amp),
	SND_SOC_DAPM_SPK("RCV", NULL),
	SND_SOC_DAPM_LINE("VPS", NULL),
	SND_SOC_DAPM_LINE("HDMI", NULL),

	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Mic", pacific_ext_mainmicbias),
	SND_SOC_DAPM_MIC("Sub Mic", NULL),
	SND_SOC_DAPM_MIC("Third Mic", NULL),
	SND_SOC_DAPM_MIC("VI SENSING", NULL),
};

const struct snd_soc_dapm_widget wm1814_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("HDMIL"),
	SND_SOC_DAPM_OUTPUT("HDMIR"),
	SND_SOC_DAPM_HP("HP", NULL),
	SND_SOC_DAPM_SPK("SPK", NULL),
	SND_SOC_DAPM_SPK("RCV", NULL),
	SND_SOC_DAPM_LINE("VPS", NULL),
	SND_SOC_DAPM_LINE("HDMI", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Sub Mic", wm1814_ext_submicbias),
};

const struct snd_soc_dapm_route pacific_wm5102_dapm_routes[] = {
	{ "HDMIL", NULL, "AIF1RX1" },
	{ "HDMIR", NULL, "AIF1RX2" },
	{ "HDMI", NULL, "HDMIL" },
	{ "HDMI", NULL, "HDMIR" },

	{ "HP", NULL, "HPOUT1L" },
	{ "HP", NULL, "HPOUT1R" },

	{ "SPK", NULL, "SPKOUTLN" },
	{ "SPK", NULL, "SPKOUTLP" },
	{ "SPK", NULL, "SPKOUTRN" },
	{ "SPK", NULL, "SPKOUTRP" },

	{ "VPS", NULL, "HPOUT2L" },
	{ "VPS", NULL, "HPOUT2R" },

	{ "RCV", NULL, "EPOUTN" },
	{ "RCV", NULL, "EPOUTP" },

	/* SEL of main mic is connected to GND */
#ifdef CONFIG_SND_USE_ANALOG_MIC
	{ "IN1L", NULL, "Main Mic" },
#else
	{ "IN1R", NULL, "Main Mic" },
#endif
	{ "Main Mic", NULL, "MICBIAS2" },

	/* Headset mic is Analog Mic */
	{ "Headset Mic", NULL, "MICBIAS1" },
	{ "IN1R", NULL, "Headset Mic" },

	/* SEL of 2nd mic is connected to MICBIAS2 */
#if defined(CONFIG_SND_USE_ANALOG_MIC)
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN2L", NULL, "Sub Mic" },
#elif defined(CONFIG_SND_USE_3MICS)
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN3L", NULL, "Sub Mic" },
#else
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN3R", NULL, "Sub Mic" },
#endif
	/* SEL of 3rd mic is connected to GND */
	{ "Third Mic", NULL, "MICBIAS2" },
	{ "Third Mic", NULL, "MICBIAS3" },
	{ "IN3R", NULL, "Third Mic" },
};

const struct snd_soc_dapm_route pacific_wm5110_dapm_routes[] = {
	{ "HDMIL", NULL, "AIF1RX1" },
	{ "HDMIR", NULL, "AIF1RX2" },
	{ "HDMI", NULL, "HDMIL" },
	{ "HDMI", NULL, "HDMIR" },

	{ "HP", NULL, "HPOUT1L" },
	{ "HP", NULL, "HPOUT1R" },

#if !defined(CONFIG_SND_SOC_MAX98504) && !defined(CONFIG_SND_SOC_MAX98504A)
	{ "SPK", NULL, "SPKOUTLN" },
	{ "SPK", NULL, "SPKOUTLP" },
	{ "SPK", NULL, "SPKOUTRN" },
	{ "SPK", NULL, "SPKOUTRP" },
#else
	{ "SPK", NULL, "HPOUT2L" },
	{ "SPK", NULL, "HPOUT2R" },
#endif

	{ "VPS", NULL, "HPOUT2L" },
	{ "VPS", NULL, "HPOUT2R" },

	{ "RCV", NULL, "HPOUT3L" },
	{ "RCV", NULL, "HPOUT3R" },

	/* SEL of main mic is connected to GND */
#ifdef CONFIG_SND_USE_ANALOG_MIC
	{ "IN1L", NULL, "Main Mic" },
#else
	{ "IN1R", NULL, "Main Mic" },
#endif
	{ "Main Mic", NULL, "MICBIAS2" },

	/* Headset mic is Analog Mic */
	{ "Headset Mic", NULL, "MICBIAS1" },
	{ "IN2R", NULL, "Headset Mic" },

	/* SEL of 2nd mic is connected to MICBIAS2 */
#if defined(CONFIG_SND_USE_ANALOG_MIC)
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN3L", NULL, "Sub Mic" },
#elif defined(CONFIG_SND_USE_3MICS)
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN3L", NULL, "Sub Mic" },
#else
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN3R", NULL, "Sub Mic" },
#endif
	/* SEL of 3rd mic is connected to GND */
	{ "Third Mic", NULL, "MICBIAS2" },
	{ "Third Mic", NULL, "MICBIAS3" },
	{ "IN3R", NULL, "Third Mic" },

#if defined(CONFIG_SND_SOC_MAX98504) || defined(CONFIG_SND_SOC_MAX98504A)
	{ "VI SENSING", NULL, "MICVDD" },
	{ "IN4R", NULL, "VI SENSING" },
	{ "IN4L", NULL, "VI SENSING" },
#endif
};

const struct snd_soc_dapm_route pacific_wm1814_dapm_routes[] = {
	{ "HDMIL", NULL, "AIF1RX1" },
	{ "HDMIR", NULL, "AIF1RX2" },
	{ "HDMI", NULL, "HDMIL" },
	{ "HDMI", NULL, "HDMIR" },
	{ "HP", NULL, "HPOUTL" },
	{ "HP", NULL, "HPOUTR" },
	{ "RCV", NULL, "EPOUT" },
	{ "SPK", NULL, "SPKOUTLN" },
	{ "SPK", NULL, "SPKOUTLP" },
	{ "SPK", NULL, "SPKOUTRN" },
	{ "SPK", NULL, "SPKOUTRP" },
	{ "VPS", NULL, "LINEOUTL" },
	{ "VPS", NULL, "LINEOUTR" },
	/* SEL of main mic is connected to GND */
	{ "Main Mic", NULL, "MICBIAS2" },
	{ "IN1BL", NULL, "Main Mic" },
	/* Sub mic is Analog Mic */
	{ "Sub Mic", NULL, "MICBIAS3" },
	{ "IN1BR", NULL, "Sub Mic" },
	/* Headset mic is Analog Mic */
	{ "Headset Mic", NULL, "MICBIAS1" },
	{ "IN2B", NULL, "Headset Mic" },
};

int pacific_set_media_clocking(struct arizona_machine_priv *priv)
{
	struct snd_soc_codec *codec = priv->codec;
	struct snd_soc_card *card = codec->card;
	int ret;

	ret = snd_soc_codec_set_sysclk(codec,
				       ARIZONA_CLK_SYSCLK,
				       ARIZONA_CLK_SRC_FLL1,
				       priv->sysclk_rate,
				       SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(card->dev, "Failed to set SYSCLK to FLL1: %d\n", ret);

	ret = snd_soc_codec_set_sysclk(codec, ARIZONA_CLK_ASYNCCLK,
				       ARIZONA_CLK_SRC_FLL2,
				       priv->asyncclk_rate,
				       SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(card->dev,
				 "Unable to set ASYNCCLK to FLL2: %d\n", ret);

	/* AIF1 from SYSCLK, AIF2 and 3 from ASYNCCLK */
	ret = snd_soc_dai_set_sysclk(priv->aif[0], ARIZONA_CLK_SYSCLK, 0, 0);
	if (ret < 0)
		dev_err(card->dev, "Can't set AIF1 to SYSCLK: %d\n", ret);

	ret = snd_soc_dai_set_sysclk(priv->aif[1], ARIZONA_CLK_ASYNCCLK, 0, 0);
	if (ret < 0)
		dev_err(card->dev, "Can't set AIF2 to ASYNCCLK: %d\n", ret);

	ret = snd_soc_dai_set_sysclk(priv->aif[2], ARIZONA_CLK_SYSCLK_2, 0, 0);
	if (ret < 0)
		dev_err(card->dev, "Can't set AIF3 to ASYNCCLK: %d\n", ret);

	return 0;
}

static int pacific_aif_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
#if defined(CONFIG_SND_SOC_MAX98504) || defined(CONFIG_SND_SOC_MAX98504A)
	struct arizona_machine_priv *priv = card->drvdata;
#endif

	dev_info(card->dev, "%s-%d startup ++, playback=%d, capture=%d\n",
			rtd->dai_link->name, substream->stream,
			codec_dai->playback_active, codec_dai->capture_active);

#if defined(CONFIG_SND_SOC_MAX98504) || defined(CONFIG_SND_SOC_MAX98504A)
	if (priv->aif[1]->playback_active)
		return 0;

	if (priv->aif[0] == codec_dai) {
		if ((substream->stream == SNDRV_PCM_STREAM_PLAYBACK) &&
			(codec_dai->playback_active == 0)) {
			mutex_lock(&card->dapm_mutex);
			snd_soc_dapm_force_enable_pin(&card->dapm,
							"VI SENSING");
			mutex_unlock(&card->dapm_mutex);

			snd_soc_dapm_sync(&card->dapm);
			dev_info(card->dev, "%s: force enable VI SENSING widgets\n",
					__func__);
		}
	}
#endif
	return 0;
}

static void pacific_aif_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
#if defined(CONFIG_SND_SOC_MAX98504) || defined(CONFIG_SND_SOC_MAX98504A)
	struct arizona_machine_priv *priv = card->drvdata;
#endif
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	dev_info(card->dev, "%s-%d shutdown++ codec_dai[%s]:playback=%d\n",
			rtd->dai_link->name, substream->stream,
			codec_dai->name, codec_dai->playback_active);

#if defined(CONFIG_SND_SOC_MAX98504) || defined(CONFIG_SND_SOC_MAX98504A)
	if (priv->aif[0] == codec_dai) {
		if ((substream->stream == SNDRV_PCM_STREAM_PLAYBACK) &&
			(codec_dai->playback_active == 0)) {
			mutex_lock(&card->dapm_mutex);
			snd_soc_dapm_disable_pin(&card->dapm, "VI SENSING");
			mutex_unlock(&card->dapm_mutex);

			snd_soc_dapm_sync(&card->dapm);

			dev_info(card->dev, "%s: force disable VI SENSING widgets\n",
						__func__);
		}
	}
#endif
	return;
}

static int pacific_aif1_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct arizona_machine_priv *priv = card->drvdata;
	int ret;

	dev_info(card->dev, "%s-%d %dch, %dHz, %dbytes\n",
			rtd->dai_link->name, substream->stream,
			params_channels(params), params_rate(params),
			params_buffer_bytes(params));

	pacific_set_media_clocking(priv);

	/* Set Codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
					 | SND_SOC_DAIFMT_NB_NF
					 | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		dev_err(card->dev, "Failed to set aif1 codec fmt: %d\n", ret);
		return ret;
	}

	/* Set CPU DAI configuration */
	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S
					 | SND_SOC_DAIFMT_NB_NF
					 | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		dev_err(card->dev, "Failed to set aif1 cpu fmt: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_CDCLK,
					0, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(card->dev, "Failed to set SAMSUNG_I2S_CDCL: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_OPCLK,
					0, MOD_OPCLK_PCLK);
	if (ret < 0) {
		dev_err(card->dev, "Failed to set SAMSUNG_I2S_OPCL: %d\n", ret);
		return ret;
	}

	return ret;
}

static int pacific_aif1_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "%s-%d\n hw_free",
			rtd->dai_link->name, substream->stream);

	return 0;
}

static int pacific_aif1_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "%s-%d prepare\n",
			rtd->dai_link->name, substream->stream);

	return 0;
}

static int pacific_aif1_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "%s-%d trigger\n",
			rtd->dai_link->name, substream->stream);

	return 0;
}

static struct snd_soc_ops pacific_aif1_ops = {
	.startup = pacific_aif_startup,
	.shutdown = pacific_aif_shutdown,
	.hw_params = pacific_aif1_hw_params,
	.hw_free = pacific_aif1_hw_free,
	.prepare = pacific_aif1_prepare,
	.trigger = pacific_aif1_trigger,
};

static int pacific_aif2_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct arizona_machine_priv *priv = rtd->card->drvdata;
	unsigned int fmt;
	int ret;
	int prate, bclk;

	dev_info(card->dev, "%s-%d %dch, %dHz\n",
			rtd->dai_link->name, substream->stream,
			params_channels(params), params_rate(params));

	prate = params_rate(params);
	switch (prate) {
	case 8000:
		bclk = 256000;
		break;
	case 16000:
		bclk = 512000;
		break;
	default:
		dev_warn(card->dev,
			"Unsupported LRCLK %d, falling back to 8000Hz\n",
			(int)params_rate(params));
		bclk = 256000;
	}

	if (priv->aif_format[1] & 0x10) {
		/* tdm mode */
		snd_soc_dai_set_tdm_slot(codec_dai, 0x07, 0x07, 4, 16);
	}

	fmt = ((priv->aif_format[1] & 0x0F) + 1) | SND_SOC_DAIFMT_NB_NF;

	/* Set the codec DAI configuration, aif2_mode:0 is slave */
	if (priv->aif2mode == 0)
		ret = snd_soc_dai_set_fmt(codec_dai,
				fmt | SND_SOC_DAIFMT_CBS_CFS);
	else
		ret = snd_soc_dai_set_fmt(codec_dai,
				fmt | SND_SOC_DAIFMT_CBM_CFM);

	if (ret < 0) {
		dev_err(card->dev,
			"Failed to set audio format in codec: %d\n", ret);
		return ret;
	}

	if (priv->aif2mode  == 0) {
		ret = snd_soc_dai_set_pll(codec_dai, FLORIDA_FLL2_REFCLK,
					  ARIZONA_FLL_SRC_MCLK1,
					  PACIFIC_DEFAULT_MCLK1,
					  priv->asyncclk_rate);
		if (ret != 0) {
			dev_err(card->dev,
					"Failed to start FLL2 REF: %d\n", ret);
			return ret;
		}

		ret = snd_soc_dai_set_pll(codec_dai, FLORIDA_FLL2,
					  ARIZONA_FLL_SRC_AIF2BCLK,
					  bclk,
					  priv->asyncclk_rate);
		if (ret != 0) {
			dev_err(card->dev,
					 "Failed to start FLL2%d\n", ret);
			return ret;
		}
	} else {
		ret = snd_soc_dai_set_pll(codec_dai, FLORIDA_FLL2, 0, 0, 0);
		if (ret != 0)
			dev_err(card->dev,
					"Failed to stop FLL2: %d\n", ret);

		ret = snd_soc_dai_set_pll(codec_dai, WM5102_FLL2_REFCLK,
					  ARIZONA_FLL_SRC_NONE, 0, 0);
		if (ret != 0) {
			dev_err(card->dev,
				 "Failed to start FLL2 REF: %d\n", ret);
			return ret;
		}
		ret = snd_soc_dai_set_pll(codec_dai, FLORIDA_FLL2,
					  ARIZONA_CLK_SRC_MCLK1,
					  PACIFIC_DEFAULT_MCLK1,
					  priv->asyncclk_rate);
		if (ret != 0) {
			dev_err(card->dev,
					"Failed to start FLL2: %d\n", ret);
			return ret;
		}
	}

	/* Set Sample rate 1 as 48k */
	snd_soc_update_bits(priv->codec, ARIZONA_SAMPLE_RATE_1,
			    ARIZONA_SAMPLE_RATE_1_MASK, 0x3);

	return 0;
}

static struct snd_soc_ops pacific_aif2_ops = {
	.shutdown = pacific_aif_shutdown,
	.hw_params = pacific_aif2_hw_params,
};

static int pacific_aif3_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	dev_info(card->dev, "%s-%d %dch, %dHz\n",
			rtd->dai_link->name, substream->stream,
			params_channels(params), params_rate(params));

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
				| SND_SOC_DAIFMT_NB_NF
				| SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to set BT mode: %d\n", ret);
		return ret;
	}
	return 0;
}

static struct snd_soc_ops pacific_aif3_ops = {
	.shutdown = pacific_aif_shutdown,
	.hw_params = pacific_aif3_hw_params,
};


static struct snd_soc_dai_link pacific_wm1814_dai[] = {
	{ /* playback & recording */
		.name = "playback-pri",
		.stream_name = "playback-pri",
		.codec_dai_name = "vegas-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* voice call */
		.name = "baseband",
		.stream_name = "baseband",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "vegas-aif2",
		.ops = &pacific_aif2_ops,
		.ignore_suspend = 1,
	},
	{ /* bluetooth sco */
		.name = "bluetooth sco",
		.stream_name = "bluetooth sco",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "vegas-aif3",
		.ops = &pacific_aif3_ops,
		.ignore_suspend = 1,
	},
	{ /* deep buffer playback */
		.name = "playback-sec",
		.stream_name = "playback-sec",
		.cpu_dai_name = "samsung-i2s-sec",
		.platform_name = "samsung-i2s-sec",
		.codec_dai_name = "vegas-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax0 playback */
		.name = "playback-eax0",
		.stream_name = "playback-eax0",
		.cpu_dai_name = "samsung-eax.0",
		.platform_name = "samsung-eax.0",
		.codec_dai_name = "vegas-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax1 playback */
		.name = "playback-eax1",
		.stream_name = "playback-eax1",
		.cpu_dai_name = "samsung-eax.1",
		.platform_name = "samsung-eax.1",
		.codec_dai_name = "vegas-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax2 playback */
		.name = "playback-eax2",
		.stream_name = "playback-eax2",
		.cpu_dai_name = "samsung-eax.2",
		.platform_name = "samsung-eax.2",
		.codec_dai_name = "vegas-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax3playback */
		.name = "playback-eax3",
		.stream_name = "playback-eax3",
		.cpu_dai_name = "samsung-eax.3",
		.platform_name = "samsung-eax.3",
		.codec_dai_name = "vegas-aif1",
		.ops = &pacific_aif1_ops,
	},
};

static struct snd_soc_dai_link pacific_wm5102_dai[] = {
	{ /* playback & recording */
		.name = "playback-pri",
		.stream_name = "playback-pri",
		.codec_dai_name = "wm5102-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* voice call */
		.name = "baseband",
		.stream_name = "baseband",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "wm5102-aif2",
		.ops = &pacific_aif2_ops,
		.ignore_suspend = 1,
	},
	{ /* bluetooth sco */
		.name = "bluetooth sco",
		.stream_name = "bluetooth sco",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "wm5102-aif3",
		.ops = &pacific_aif3_ops,
		.ignore_suspend = 1,
	},
	{ /* deep buffer playback */
		.name = "playback-sec",
		.stream_name = "playback-sec",
		.cpu_dai_name = "samsung-i2s-sec",
		.platform_name = "samsung-i2s-sec",
		.codec_dai_name = "wm5102-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax0 playback */
		.name = "playback-eax0",
		.stream_name = "playback-eax0",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "wm5102-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax1 playback */
		.name = "playback-eax1",
		.stream_name = "playback-eax1",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "wm5102-aif1",
		.ops = &pacific_aif1_ops,
	},
};

static struct snd_soc_dai_link pacific_wm5110_dai[] = {
	{ /* playback & recording */
		.name = "playback-pri",
		.stream_name = "playback-pri",
		.codec_dai_name = "florida-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* voice call */
		.name = "baseband",
		.stream_name = "baseband",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "florida-aif2",
		.ops = &pacific_aif2_ops,
		.ignore_suspend = 1,
	},
	{ /* bluetooth sco */
		.name = "bluetooth sco",
		.stream_name = "bluetooth sco",
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.codec_dai_name = "florida-aif3",
		.ops = &pacific_aif3_ops,
		.ignore_suspend = 1,
	},
	{ /* deep buffer playback */
		.name = "playback-sec",
		.stream_name = "playback-sec",
		.cpu_dai_name = "samsung-i2s-sec",
		.platform_name = "samsung-i2s-sec",
		.codec_dai_name = "florida-aif1",
		.ops = &pacific_aif1_ops,
	},
	{
		.name = "CPU-DSP Voice Control",
		.stream_name = "CPU-DSP Voice Control",
		.cpu_dai_name = "florida-cpu-voicectrl",
		.platform_name = "florida-codec",
		.codec_dai_name = "florida-dsp-voicectrl",
	},
	{ /* pcm dump interface */
		.name = "CPU-DSP trace",
		.stream_name = "CPU-DSP trace",
		.cpu_dai_name = "florida-cpu-trace",
		.platform_name = "florida-codec",
		.codec_dai_name = "florida-dsp-trace",
	},
	{ /* eax0 playback */
		.name = "playback-eax0",
		.stream_name = "playback-eax0",
		.cpu_dai_name = "samsung-eax.0",
		.platform_name = "samsung-eax.0",
		.codec_dai_name = "florida-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax1 playback */
		.name = "playback-eax1",
		.stream_name = "playback-eax1",
		.cpu_dai_name = "samsung-eax.1",
		.platform_name = "samsung-eax.1",
		.codec_dai_name = "florida-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax2 playback */
		.name = "playback-eax2",
		.stream_name = "playback-eax2",
		.cpu_dai_name = "samsung-eax.2",
		.platform_name = "samsung-eax.2",
		.codec_dai_name = "florida-aif1",
		.ops = &pacific_aif1_ops,
	},
	{ /* eax3 playback */
		.name = "playback-eax3",
		.stream_name = "playback-eax3",
		.cpu_dai_name = "samsung-eax.3",
		.platform_name = "samsung-eax.3",
		.codec_dai_name = "florida-aif1",
		.ops = &pacific_aif1_ops,
	},
};

static int pacific_of_get_pdata(struct snd_soc_card *card)
{
	struct device_node *pdata_np;
	struct arizona_machine_priv *priv = card->drvdata;
	int ret;

	pdata_np = of_find_node_by_path("/audio_pdata");
	if (!pdata_np) {
		dev_err(card->dev,
			"Property 'samsung,audio-pdata' missing or invalid\n");
		return -EINVAL;
	}

	if (of_find_property(pdata_np, "mic_bias_gpio", NULL)) {
		priv->mic_bias_gpio = of_get_named_gpio(pdata_np, "mic_bias_gpio", 0);

		if (priv->mic_bias_gpio >= 0) {
			ret = gpio_request(priv->mic_bias_gpio, "MICBIAS_EN_AP");
			if (ret)
				dev_err(card->dev, "Failed to request gpio: %d\n", ret);

			gpio_direction_output(priv->mic_bias_gpio, 0);
		}
	}

	if (of_find_property(pdata_np, "sub_mic_bias_gpio", NULL)) {
		priv->sub_mic_bias_gpio = of_get_named_gpio(pdata_np, "sub_mic_bias_gpio", 0);
		if (priv->sub_mic_bias_gpio >= 0) {
			ret = gpio_request(priv->sub_mic_bias_gpio, "MICBIAS_EN_AP");
			if (ret)
				dev_err(card->dev, "Failed to request gpio: %d\n", ret);

			gpio_direction_output(priv->sub_mic_bias_gpio, 0);
		}
	}

	pacific_update_impedance_table(pdata_np);

	priv->seamless_voicewakeup =
		of_property_read_bool(pdata_np, "seamless_voicewakeup");

	of_property_read_u32_array(pdata_np, "aif_format",
			priv->aif_format, ARRAY_SIZE(priv->aif_format));

	return 0;
}
static void get_voicetrigger_dump(struct snd_soc_codec *codec)
{
	unsigned int val1, val2;
	unsigned int svScore, finalScore, noiseFloor;

	val1 = (snd_soc_read(codec, 0x390084)) & 0xFFFF;
	val2 = (snd_soc_read(codec, 0x390085)) & 0xFFFF;
	svScore = ((val1 << 16) | val2);

	val1 = (snd_soc_read(codec, 0x390086)) & 0xFFFF;
	val2 = (snd_soc_read(codec, 0x390087)) & 0xFFFF;
	finalScore = ((val1 << 16) | val2);

	val1 = (snd_soc_read(codec, 0x39008E)) & 0xFFFF;
	val2 = (snd_soc_read(codec, 0x39008F)) & 0xFFFF;
	noiseFloor = ((val1 << 16) | val2);

	dev_info(codec->dev,"svScore=0x%x, finalScore=0x%x, noiseFloor=0x%x\n",
				svScore, finalScore, noiseFloor);

}

static void ez2ctrl_voicewakeup_cb(void)
{
	struct arizona_machine_priv *priv;
	char keyword_buf[100];
	char *envp[2];

	memset(keyword_buf, 0, sizeof(keyword_buf));

	dev_info(the_codec->dev, "Voice Control Callback invoked!\n");

	WARN_ON(!the_codec);
	if (!the_codec)
		return;

	priv = the_codec->card->drvdata;
	if (!priv)
		return;

	if (priv->voice_uevent == 0) {
		keyword_type = snd_soc_read(the_codec, 0x39007d);
		snprintf(keyword_buf, sizeof(keyword_buf),
			"VOICE_WAKEUP_WORD_ID=%x", keyword_type);
	} else if (priv->voice_uevent == 1) {
		snprintf(keyword_buf, sizeof(keyword_buf),
			"VOICE_WAKEUP_WORD_ID=LPSD");
	} else {
		dev_err(the_codec->dev, "Invalid voice control mode =%d",
				priv->voice_uevent);
		return;
	}
	get_voicetrigger_dump(the_codec);

	envp[0] = keyword_buf;
	envp[1] = NULL;

	dev_info(the_codec->dev, "%s : raise the uevent, string = %s\n",
			__func__, keyword_buf);

	wake_lock_timeout(&priv->wake_lock, 5000);
	kobject_uevent_env(&(the_codec->dev->kobj), KOBJ_CHANGE, envp);

	return;
}

static ssize_t svoice_keyword_type_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int ret;
	ret = snprintf(buf, PAGE_SIZE, "%x\n", keyword_type);

	return ret;
}

static ssize_t svoice_keyword_type_set(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct snd_soc_codec *codec = dev_get_drvdata(dev);
	int ret;

	ret = kstrtoint(buf, 0, &keyword_type);
	if (ret)
		dev_err(codec->dev,
			"Failed to convert a string to an int: %d\n", ret);

	dev_info(codec->dev, "%s(): keyword_type = %x\n",
			__func__, keyword_type);

	return count;
}
static DEVICE_ATTR(keyword_type, S_IRUGO | S_IWUSR | S_IWGRP,
	svoice_keyword_type_show, svoice_keyword_type_set);

static void ez2ctrl_debug_cb(void)
{
	struct arizona_machine_priv *priv;

	pr_info("Voice Control Callback invoked!\n");

	WARN_ON(!the_codec);
	if (!the_codec)
		return;

	priv = the_codec->card->drvdata;
	if (!priv)
		return;

	input_report_key(priv->input, priv->voice_key_event, 1);
	input_sync(priv->input);

	usleep_range(10000, 20000);

	input_report_key(priv->input, priv->voice_key_event, 0);
	input_sync(priv->input);

	return;
}

static int pacific_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_codec *codec = card->rtd[0].codec;
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dai;
	struct snd_soc_dai *cpu_dai = card->rtd[0].cpu_dai;
	struct arizona_machine_priv *priv = card->drvdata;
	int i, ret;

	priv->codec = codec;
	the_codec = codec;

	pacific_of_get_pdata(card);

	for (i = 0; i < 3; i++)
		priv->aif[i] = card->rtd[i].codec_dai;

	codec_dai->driver->playback.channels_max =
				cpu_dai->driver->playback.channels_max;

	/* close codec device immediately when pcm is closed */
	codec->ignore_pmdown_time = true;

	ret = snd_soc_add_codec_controls(codec, pacific_codec_controls,
					ARRAY_SIZE(pacific_codec_controls));
	if (ret < 0) {
		dev_err(codec->dev,
				"Failed to add controls to codec: %d\n", ret);
		return ret;
	}

	snd_soc_dapm_ignore_suspend(&card->dapm, "Main Mic");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Sub Mic");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Third Mic");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(&card->dapm, "RCV");
	snd_soc_dapm_ignore_suspend(&card->dapm, "VPS");
	snd_soc_dapm_ignore_suspend(&card->dapm, "SPK");
	snd_soc_dapm_ignore_suspend(&card->dapm, "HP");
	snd_soc_dapm_sync(&card->dapm);

	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF2 Playback");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF2 Capture");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF3 Playback");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "AIF3 Capture");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "DSP Virtual Output");
	snd_soc_dapm_ignore_suspend(&codec->dapm, "DRC2 Signal Activity");
	snd_soc_dapm_sync(&codec->dapm);

	ret = snd_soc_codec_set_sysclk(codec,
				       ARIZONA_CLK_SYSCLK,
				       ARIZONA_CLK_SRC_FLL1,
				       48000 * 1024,
				       SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(card->dev, "Failed to set SYSCLK to FLL1: %d\n", ret);

	ret = snd_soc_codec_set_sysclk(codec, ARIZONA_CLK_ASYNCCLK,
				       ARIZONA_CLK_SRC_FLL2,
				       priv->asyncclk_rate,
				       SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(card->dev, "Failed to set ASYSCLK to FLL2 %d\n", ret);

	wake_lock_init(&priv->wake_lock, WAKE_LOCK_SUSPEND,
				"pacific-voicewakeup");
	if (priv->seamless_voicewakeup) {
		arizona_set_ez2ctrl_cb(codec, ez2ctrl_voicewakeup_cb);

		svoice_class = class_create(THIS_MODULE, "svoice");
		ret = IS_ERR(svoice_class);
		if (ret) {
			pr_err("Failed to create class(svoice)!\n");
			goto err_class_create;
		}

		keyword_dev = device_create(svoice_class,
					NULL, 0, NULL, "keyword");
		ret = IS_ERR(keyword_dev);
		if (ret) {
			pr_err("Failed to create device(keyword)!\n");
			goto err_device_create;
		}

		ret = device_create_file(keyword_dev, &dev_attr_keyword_type);
		if (ret < 0) {
			pr_err("Failed to create device file in sysfs entries(%s)!\n",
				dev_attr_keyword_type.attr.name);
			goto err_device_create_file;
		}
	} else {
		priv->input = devm_input_allocate_device(card->dev);
		if (!priv->input) {
			dev_err(card->dev, "Can't allocate input dev\n");
			ret = -ENOMEM;
			goto err_register;
		}

		priv->input->name = "voicecontrol";
		priv->input->dev.parent = card->dev;

		input_set_capability(priv->input, EV_KEY,
			KEY_VOICE_WAKEUP);
		input_set_capability(priv->input, EV_KEY,
			KEY_LPSD_WAKEUP);

		ret = input_register_device(priv->input);
		if (ret) {
			dev_err(card->dev,
				"Can't register input device: %d\n", ret);
			goto err_register;
		}

		arizona_set_ez2ctrl_cb(codec, ez2ctrl_debug_cb);
		priv->voice_key_event = KEY_VOICE_WAKEUP;
	}

	arizona_set_hpdet_cb(codec, pacific_arizona_hpdet_cb);
	arizona_set_micd_cb(codec, pacific_arizona_micd_cb);

#if defined(CONFIG_SND_SOC_MAX98504A)
	priv->external_amp = max98504_set_speaker_status;
#endif
	return 0;

err_device_create_file:
	device_destroy(svoice_class, 0);
err_device_create:
	class_destroy(svoice_class);
err_class_create:
err_register:
	wake_lock_destroy(&priv->wake_lock);
	return ret;
}

static int pacific_start_sysclk(struct snd_soc_card *card)
{
	struct arizona_machine_priv *priv = card->drvdata;
	struct snd_soc_codec *codec = priv->codec;
	int ret;

	if (priv->mclk) {
		clk_enable(priv->mclk);
		dev_info(card->dev, "mclk enabled\n");
	} else
		exynos5_audio_set_mclk(true, 0);

#ifdef CONFIG_MFD_FLORIDA
	ret = snd_soc_codec_set_pll(codec, FLORIDA_FLL1_REFCLK,
				    ARIZONA_FLL_SRC_NONE, 0, 0);
	if (ret != 0) {
		dev_err(card->dev, "Failed to start FLL1 REF: %d\n", ret);
		return ret;
	}
	ret = snd_soc_codec_set_pll(codec, FLORIDA_FLL1, ARIZONA_CLK_SRC_MCLK1,
				    PACIFIC_DEFAULT_MCLK1,
				    priv->sysclk_rate);
	if (ret != 0) {
		dev_err(card->dev, "Failed to start FLL1: %d\n", ret);
		return ret;
	}
#else
	ret = snd_soc_codec_set_pll(codec, WM5102_FLL1_REFCLK,
				    ARIZONA_FLL_SRC_NONE, 0, 0);
	if (ret != 0) {
		dev_err(card->dev, "Failed to start FLL1 REF: %d\n", ret);
		return ret;
	}
	ret = snd_soc_codec_set_pll(codec, WM5102_FLL1, ARIZONA_CLK_SRC_MCLK1,
				    PACIFIC_DEFAULT_MCLK1,
				    priv->sysclk_rate);
	if (ret != 0) {
		dev_err(card->dev, "Failed to start FLL1: %d\n", ret);
		return ret;
	}
#endif

	return ret;
}

static int pacific_change_sysclk(struct snd_soc_card *card, int source)
{
	struct arizona_machine_priv *priv = card->drvdata;
	struct snd_soc_codec *codec = priv->codec;
	int ret;

	dev_info(card->dev, "%s: source = %d\n", __func__, source);

	if (source) {
		/* uses MCLK1 when the source is 1 */
		if (priv->mclk) {
			clk_enable(priv->mclk);
			dev_info(card->dev, "mclk enabled\n");
		} else
			exynos5_audio_set_mclk(true, 0);

		ret = snd_soc_codec_set_pll(codec, FLORIDA_FLL1,
					ARIZONA_FLL_SRC_MCLK1,
					PACIFIC_DEFAULT_MCLK1,
					priv->sysclk_rate);
		if (ret != 0) {
			dev_err(card->dev, "Failed to start FLL1: %d\n", ret);
			return ret;
		}
	} else {
		/* uses MCLK2 when the source is not 1 */
		ret = snd_soc_codec_set_pll(codec, FLORIDA_FLL1,
					ARIZONA_FLL_SRC_MCLK2,
					PACIFIC_DEFAULT_MCLK2,
					priv->sysclk_rate);
		if (ret != 0) {
			dev_err(card->dev, "Failed to change FLL1: %d\n", ret);
			return ret;
		}

		if (priv->mclk) {
			clk_disable(priv->mclk);
			dev_info(card->dev, "mclk disbled\n");
		} else
			exynos5_audio_set_mclk(false, 0);
	}

	return ret;
}

static int pacific_stop_sysclk(struct snd_soc_card *card)
{
	struct arizona_machine_priv *priv = card->drvdata;
	int ret;

	ret = snd_soc_codec_set_pll(priv->codec, FLORIDA_FLL1, 0, 0, 0);
	if (ret != 0) {
		dev_err(card->dev, "Failed to stop FLL1: %d\n", ret);
		return ret;
	}

	ret = snd_soc_codec_set_pll(priv->codec, FLORIDA_FLL2, 0, 0, 0);
	if (ret != 0) {
		dev_err(card->dev, "Failed to stop FLL1: %d\n", ret);
		return ret;
	}

	if (priv->mclk) {
		clk_disable(priv->mclk);
		dev_info(card->dev, "mclk disbled\n");
	} else
		exynos5_audio_set_mclk(false, 0);

	return ret;
}

static int pacific_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level)
{
	struct arizona_machine_priv *priv = card->drvdata;

	if (!priv->codec || dapm != &priv->codec->dapm)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		if (card->dapm.bias_level == SND_SOC_BIAS_OFF)
			pacific_start_sysclk(card);
		break;
	case SND_SOC_BIAS_OFF:
		pacific_stop_sysclk(card);
		break;
	case SND_SOC_BIAS_PREPARE:
		break;
	default:
	break;
	}

	card->dapm.bias_level = level;
	dev_dbg(card->dev, "%s: %d\n", __func__, level);

	return 0;
}

static int pacific_set_bias_level_post(struct snd_soc_card *card,
				     struct snd_soc_dapm_context *dapm,
				     enum snd_soc_bias_level level)
{
	dev_dbg(card->dev, "%s: %d\n", __func__, level);

	return 0;
}

static int pacific_check_clock_conditions(struct snd_soc_card *card)
{
	struct snd_soc_codec *codec = card->rtd[0].codec;
	struct arizona_machine_priv *priv = card->drvdata;
	int mainmic_state = 0;

#ifdef CONFIG_MFD_FLORIDA
	/* Check status of the Main Mic for ez2control
	 * Because when the phone goes to suspend mode,
	 * Enabling case of Main mic is only ez2control mode */
	mainmic_state = snd_soc_dapm_get_pin_status(&card->dapm, "Main Mic");
#endif

	if (!codec->active && mainmic_state) {
		dev_info(card->dev, "MAIN_MIC is running without input stream\n");
		return PACIFIC_RUN_MAINMIC;
	}

	if (!codec->active && priv->ear_mic && !mainmic_state) {
		dev_info(card->dev, "EAR_MIC is running without input stream\n");
		return PACIFIC_RUN_EARMIC;
	}

	return 0;
}

static int pacific_suspend_post(struct snd_soc_card *card)
{
	struct arizona_machine_priv *priv = card->drvdata;
	int ret;

	/* When the card goes to suspend state, If codec is not active,
	 * the micbias of headset is enable and the ez2control is not running,
	 * The MCLK and the FLL1 should be disable to reduce the sleep current.
	 * In the other cases, these should keep previous status */
	ret = pacific_check_clock_conditions(card);

	if (ret == PACIFIC_RUN_EARMIC) {
		pacific_stop_sysclk(card);
		dev_info(card->dev, "%s: stop_sysclk\n", __func__);
	} else if (ret == PACIFIC_RUN_MAINMIC) {
		pacific_change_sysclk(card, 0);
		dev_info(card->dev, "%s: change_sysclk\n", __func__);
	}

	if (!priv->codec->active) {
		dev_info(card->dev, "%s : set AIF1 port slave\n", __func__);
		snd_soc_update_bits(priv->codec, ARIZONA_AIF1_BCLK_CTRL,
				ARIZONA_AIF1_BCLK_MSTR_MASK, 0);
		snd_soc_update_bits(priv->codec, ARIZONA_AIF1_TX_PIN_CTRL,
				ARIZONA_AIF1TX_LRCLK_MSTR_MASK, 0);
		snd_soc_update_bits(priv->codec, ARIZONA_AIF1_RX_PIN_CTRL,
				ARIZONA_AIF1RX_LRCLK_MSTR_MASK, 0);
	}

	return 0;
}

static int pacific_resume_pre(struct snd_soc_card *card)
{
	int ret;

	/* When the card goes to resume state, If codec is not active,
	 * the micbias of headset is enable and the ez2control is not running,
	 * The MCLK and the FLL1 should be enable.
	 * In the other cases, these should keep previous status */
	ret = pacific_check_clock_conditions(card);

	if (ret == PACIFIC_RUN_EARMIC) {
		pacific_start_sysclk(card);
		dev_info(card->dev, "%s: start_sysclk\n", __func__);
	} else if (ret == PACIFIC_RUN_MAINMIC) {
		pacific_change_sysclk(card, 1);
		dev_info(card->dev, "%s: change_sysclk\n", __func__);
	}

	return 0;
}

static struct snd_soc_card pacific_cards[] = {
	{
		.name = "Pacific WM5102 Sound",
		.owner = THIS_MODULE,

		.dai_link = pacific_wm5102_dai,
		.num_links = ARRAY_SIZE(pacific_wm5102_dai),

		.controls = pacific_controls,
		.num_controls = ARRAY_SIZE(pacific_controls),
		.dapm_widgets = wm5102_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(wm5102_dapm_widgets),
		.dapm_routes = pacific_wm5102_dapm_routes,
		.num_dapm_routes = ARRAY_SIZE(pacific_wm5102_dapm_routes),

		.late_probe = pacific_late_probe,

		.suspend_post = pacific_suspend_post,
		.resume_pre = pacific_resume_pre,

		.set_bias_level = pacific_set_bias_level,
		.set_bias_level_post = pacific_set_bias_level_post,

		.drvdata = &pacific_wm5102_priv,
	},
	{
		.name = "Pacific WM5110 Sound",
		.owner = THIS_MODULE,

		.dai_link = pacific_wm5110_dai,
		.num_links = ARRAY_SIZE(pacific_wm5110_dai),

		.controls = pacific_controls,
		.num_controls = ARRAY_SIZE(pacific_controls),
		.dapm_widgets = pacific_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(pacific_dapm_widgets),
		.dapm_routes = pacific_wm5110_dapm_routes,
		.num_dapm_routes = ARRAY_SIZE(pacific_wm5110_dapm_routes),

		.late_probe = pacific_late_probe,

		.suspend_post = pacific_suspend_post,
		.resume_pre = pacific_resume_pre,

		.set_bias_level = pacific_set_bias_level,
		.set_bias_level_post = pacific_set_bias_level_post,

		.drvdata = &pacific_wm5110_priv,
	},
	{
		.name = "Pacific WM1814 Sound",
		.owner = THIS_MODULE,

		.dai_link = pacific_wm1814_dai,
		.num_links = ARRAY_SIZE(pacific_wm1814_dai),

		.controls = pacific_controls,
		.num_controls = ARRAY_SIZE(pacific_controls),
		.dapm_widgets = wm1814_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(wm1814_dapm_widgets),
		.dapm_routes = pacific_wm1814_dapm_routes,
		.num_dapm_routes = ARRAY_SIZE(pacific_wm1814_dapm_routes),

		.late_probe = pacific_late_probe,

		.suspend_post = pacific_suspend_post,
		.resume_pre = pacific_resume_pre,

		.set_bias_level = pacific_set_bias_level,
		.set_bias_level_post = pacific_set_bias_level_post,

		.drvdata = &pacific_wm1814_priv,
	},
};

const char *card_ids[] = {
	"wm5102", "wm5110", "wm1814"
};

static int pacific_audio_probe(struct platform_device *pdev)
{
	int n, ret;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *codec_np, *cpu_np;
	struct snd_soc_card *card;
	struct snd_soc_dai_link *dai_link;
	struct arizona_machine_priv *priv;

	for (n = 0; n < ARRAY_SIZE(card_ids); n++) {
		if (NULL != of_find_node_by_name(NULL, card_ids[n])) {
			card = &pacific_cards[n];
			dai_link = card->dai_link;
			break;
		}
	}

	if (n == ARRAY_SIZE(card_ids)) {
		dev_err(&pdev->dev, "%s :couldn't find card\n", __func__);
		return -EINVAL;
	}

	card->dev = &pdev->dev;
	priv = card->drvdata;

	priv->mclk = devm_clk_get(card->dev, "mclk");
	if (IS_ERR(priv->mclk)) {
		dev_dbg(card->dev, "%s :Device tree node not found for mclk", __func__);
		priv->mclk = NULL;
	} else
		clk_prepare(priv->mclk);

	for (n = 0; n < card->num_links; n++) {
		if (!dai_link[n].cpu_name && !dai_link[n].cpu_dai_name) {
			cpu_np = of_parse_phandle(np, "samsung,audio-cpu", n);
			if (cpu_np) {
				dai_link[n].cpu_of_node = cpu_np;
				dai_link[n].platform_of_node = cpu_np;
			} else
				dev_err(&pdev->dev, "Property 'samsung,audio-cpu'"
						": dai_link[%d] missing or invalid\n",n);
		}
		if (!dai_link[n].codec_name) {
			codec_np = of_parse_phandle(np, "samsung,audio-codec", n);
			if (codec_np)
				dai_link[n].codec_of_node = codec_np;
			else
				dev_err(&pdev->dev, "Property 'samsung,audio-codec'"
						": dai_link[%d] missing or invalid\n", n);
		}
	}

	ret = snd_soc_register_card(card);
	if (ret)
		dev_err(&pdev->dev, "Failed to register card:%d\n", ret);

	return ret;

}

static int pacific_audio_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct arizona_machine_priv *priv = card->drvdata;

	device_remove_file(keyword_dev, &dev_attr_keyword_type);
	device_destroy(svoice_class, 0);
	class_destroy(svoice_class);

	snd_soc_unregister_component(card->dev);
	snd_soc_unregister_card(card);
	wake_lock_destroy(&priv->wake_lock);

	if (priv->mclk) {
		clk_unprepare(priv->mclk);
		devm_clk_put(card->dev, priv->mclk);
	}

	return 0;
}

static const struct of_device_id pacific_arizona_of_match[] = {
	{ .compatible = "samsung,pacific-arizona", },
	{ },
};
MODULE_DEVICE_TABLE(of, pacific_arizona_of_match);

static struct platform_driver pacific_audio_driver = {
	.driver	= {
		.name	= "pacific-audio",
		.owner	= THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = of_match_ptr(pacific_arizona_of_match),
	},
	.probe	= pacific_audio_probe,
	.remove	= pacific_audio_remove,
};

module_platform_driver(pacific_audio_driver);

MODULE_DESCRIPTION("ALSA SoC PACIFIC ARIZONA");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pacific-audio");

