/**
 * Copyright (C) Fourier Semiconductor Inc. 2016-2020. All rights reserved.
 * 2018-10-22 File created.
 */

#if defined(CONFIG_FSM_CODEC)
#include "fsm_public.h"
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/miscdevice.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <linux/version.h>

struct fsm_codec {
	struct workqueue_struct *codec_wq;
	struct delayed_work codec_monitor;
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0))
#define snd_soc_codec              snd_soc_component
#define snd_soc_add_codec_controls snd_soc_add_component_controls
//#define snd_soc_register_codec     devm_snd_soc_register_component=>snd_soc_register_component
#define snd_soc_register_codec     snd_soc_register_component
#define snd_soc_unregister_codec   snd_soc_unregister_component
#define snd_soc_codec_driver       snd_soc_component_driver
#define remove_ret_type void
#define remove_ret_val
#else
#define remove_ret_type int
#define remove_ret_val (0)
#endif

/* Supported rates and data formats */
#define FSM_RATES SNDRV_PCM_RATE_8000_48000
#define FSM_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE \
					| SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S24_3LE)

static const unsigned int fsm_rates[] = { 8000, 16000, 32000, 44100, 48000 };
static const struct snd_pcm_hw_constraint_list fsm_constraints = {
	.list = fsm_rates,
	.count = ARRAY_SIZE(fsm_rates),
};

//+bug 537023, baoshulin@wingtech.com, 20200316,  update fs18xx add switch channel
int fsm_angle_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();

	if (!cfg) {
		ucontrol->value.integer.value[0] = -1;
		return 0;
	}
	pr_info("next_angle: %d", cfg->cur_angle);
	ucontrol->value.integer.value[0] = cfg->cur_angle;

	return 0;
}

int fsm_angle_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int next_angle = ucontrol->value.integer.value[0];
	pr_info("audiock next_angle: %d", next_angle);
	fsm_stereo_flip(next_angle);

	return 0;
}
//-bug 537023, baoshulin@wingtech.com, 20200316,  update fs18xx add switch channel
struct fsm_codec *g_fsm_codec = NULL;

static int fsm_get_scene_index(uint16_t scene)
{
	int index = 0;

	while (scene) {
		scene = (scene >> 1);
		if (scene == 0) {
			break;
		}
		index++;
	}

	return index;
}

int fsm_scene_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();
	int scene_index;

	if (!cfg) {
		ucontrol->value.integer.value[0] = -1;
		return 0;
	}
	scene_index = fsm_get_scene_index(cfg->next_scene);
	pr_info("next_scene: %04X, BIT(%d)", cfg->next_scene, scene_index);
	ucontrol->value.integer.value[0] = scene_index;

	return 0;
}

int fsm_scene_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int next_scene = ucontrol->value.integer.value[0];

	pr_info("next_scene: %d", next_scene);
	if (next_scene < 0 || next_scene >= FSM_SCENE_MAX) {
		next_scene = 0;
	}
	fsm_set_scene(next_scene);

	return 0;
}

int fsm_volume_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();
	int volume;

	volume = ((cfg != NULL) ? cfg->volume : FSM_VOLUME_MAX);
	ucontrol->value.integer.value[0] = volume;
	pr_info("volume: %ld", ucontrol->value.integer.value[0]);

	return 0;
}

int fsm_volume_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int volume = ucontrol->value.integer.value[0];

	pr_info("volume: %d", volume);
	fsm_set_volume(volume);

	return 0;
}

int fsm_stop_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();

	ucontrol->value.integer.value[0] = ((cfg != NULL) ? cfg->force_mute : 1);
	pr_info("stop: %ld", ucontrol->value.integer.value[0]);

	return 0;
}

int fsm_stop_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int stop = ucontrol->value.integer.value[0];

	pr_info("stop: %x", stop);

	return 0;
}

int fsm_rotation_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();

	ucontrol->value.integer.value[0] = ((cfg != NULL) ? cfg->cur_angle : 0);
	pr_info("angle: %ld", ucontrol->value.integer.value[0]);

	return 0;
}

int fsm_rotation_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int angle = ucontrol->value.integer.value[0];

	pr_info("angle: %x", angle);
	fsm_stereo_flip(angle);

	return 0;
}

int fsm_monitor_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();
	int status;

	if (cfg == NULL) {
		return -EINVAL;
	}
	status = (!cfg->skip_monitor && cfg->use_monitor);
	pr_info("status: %d", status);
	ucontrol->value.integer.value[0] = status;

	return 0;
}

int fsm_monitor_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	fsm_config_t *cfg = fsm_get_config();
	int monitor_on = ucontrol->value.integer.value[0];

	if (cfg == NULL) {
		return -EINVAL;
	}
	monitor_on = !!monitor_on;
	pr_info("set monitor: %s", monitor_on ? "On" : "Off");
	cfg->use_monitor = monitor_on;
	cfg->skip_monitor = !monitor_on;

	return 0;
}

static const struct snd_kcontrol_new fsm_snd_controls[] =
{
	SOC_SINGLE_EXT("FSM_Scene", SND_SOC_NOPM, 0, FSM_SCENE_MAX, 0,
			fsm_scene_get, fsm_scene_put),
	SOC_SINGLE_EXT("FSM_Volume", SND_SOC_NOPM, 0, FSM_VOLUME_MAX, 0,
			fsm_volume_get, fsm_volume_put),
	SOC_SINGLE_EXT("FSM_Stop", SND_SOC_NOPM, 0, 1, 0,
			fsm_stop_get, fsm_stop_put),
	SOC_SINGLE_EXT("FSM_Rotation", SND_SOC_NOPM, 0, 360, 0,
			fsm_rotation_get, fsm_rotation_put),
	SOC_SINGLE_EXT("FSM_Monitor", SND_SOC_NOPM, 0, 1, 0,
			fsm_monitor_get, fsm_monitor_put),
//bug 537023, baoshulin@wingtech.com, 20200316,  update fs18xx add switch channel
	SOC_SINGLE_EXT("FSM_Stereo_Switch", SND_SOC_NOPM, 0, FSM_ANGLE_MAX, 0,
			fsm_angle_get, fsm_angle_put),
};

static struct snd_soc_dapm_widget fsm_dapm_widgets_common[] =
{
	/* Stream widgets */
	SND_SOC_DAPM_AIF_IN("AIF IN", "AIF Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUTL"),
	SND_SOC_DAPM_INPUT("AEC Loopback"),
};

static const struct snd_soc_dapm_route fsm_dapm_routes_common[] =
{
	{ "OUTL", NULL, "AIF IN" },
	{ "AIF OUT", NULL, "AEC Loopback" },
};

static struct snd_soc_dapm_context *snd_soc_fsm_get_dapm(
		struct snd_soc_codec *codec)
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0))
	return &codec->dapm;
#elif (LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0))
	return snd_soc_codec_get_dapm(codec);
#else
	return snd_soc_component_get_dapm(codec);
#endif
}

static int fsm_add_widgets(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = snd_soc_fsm_get_dapm(codec);

	snd_soc_add_codec_controls(codec, fsm_snd_controls,
				ARRAY_SIZE(fsm_snd_controls));
	snd_soc_dapm_new_controls(dapm, fsm_dapm_widgets_common,
				ARRAY_SIZE(fsm_dapm_widgets_common));
	snd_soc_dapm_add_routes(dapm, fsm_dapm_routes_common,
				ARRAY_SIZE(fsm_dapm_routes_common));
	return 0;
}

//#if LINUX_VERSION_CODE < KERNEL_VERSION(3,16,0)
//static struct snd_soc_codec *snd_soc_kcontrol_codec(
//			struct snd_kcontrol *kcontrol)
//{
//	return snd_kcontrol_chip(kcontrol);
//}
//#endif

static int fsm_startup(struct snd_pcm_substream *substream,
			struct snd_soc_dai *dai)
{
	int ret;

	if (!substream->runtime) {
		return 0;
	}

	ret = snd_pcm_hw_constraint_mask64(substream->runtime, \
			SNDRV_PCM_HW_PARAM_FORMAT, FSM_FORMATS);
	if (ret < 0) {
		pr_err("set pcm param format fail:%d", ret);
		return ret;
	}

	ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE,
			&fsm_constraints);

	return ret;
}


static void fsm_shutdown(struct snd_pcm_substream *substream,
			struct snd_soc_dai *dai)
{
}

static int fsm_set_sysclk(struct snd_soc_dai *codec_dai,
			int clk_id, unsigned int freq, int dir)
{
	pr_info("freq:%d", freq);
	return 0;
}

static int fsm_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	int format = 0;
	int ret = 0;

	pr_debug("fmt: %X", fmt);
	/*switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
		case SND_SOC_DAIFMT_CBS_CFS:
			break;
		case SND_SOC_DAIFMT_CBM_CFM:
		default:
			// only supports Slave mode
			pr_err("invalid DAI master/slave interface");
			ret = -EINVAL;
			break;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
		case SND_SOC_DAIFMT_I2S:
			format = 3;
			break;
		default:
			pr_err("invalid dai format: %x", fmt);
			ret = -EINVAL;
			break;
	}*/
	pr_info("format:%d, ret:%d", format, ret);

	return ret;
}

static int fsm_hw_params(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params,
			struct snd_soc_dai *dai)
{
	int bclk;
	int srate;
	int format;
	int sample_size;
	int phy_size;

	format = params_format(params);
	pr_debug("format:%X", format);
	/*switch (format)
	{
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_3LE:
	case SNDRV_PCM_FORMAT_S32_LE:
		break;
	}*/

	srate = params_rate(params);
	sample_size = snd_pcm_format_width(format);
	phy_size = snd_pcm_format_physical_width(format);
	bclk = srate * phy_size * 2;
	fsm_set_i2s_clocks(srate, bclk);
	pr_info("bclk:%d, srate:%d, fmt:%d, phy:%d", bclk, srate,
			sample_size, phy_size);

	return 0;
}

int fsm_set_codec_monitor(struct fsm_codec *fsm_codec, bool enable)
{
	fsm_config_t *cfg = fsm_get_config();
	if (!cfg || !fsm_codec || !fsm_codec->codec_wq) {
		return -EINVAL;
	}
	if (!cfg->use_monitor) {
		return 0;
	}
	if (enable) {
		cfg->skip_monitor = false;
		queue_delayed_work(fsm_codec->codec_wq,
				&fsm_codec->codec_monitor, 5*HZ);
	}
	else {
		cfg->skip_monitor = true;
		if (delayed_work_pending(&fsm_codec->codec_monitor)) {
			cancel_delayed_work_sync(&fsm_codec->codec_monitor);
		}
	}

	return 0;
}

static void fsm_codec_monitor(struct work_struct *work)
{
	struct fsm_codec *fsm_codec = g_fsm_codec;
	fsm_config_t *cfg = fsm_get_config();

	if (!fsm_codec || !cfg) {
		return;
	}
	if (cfg->skip_monitor) {
		return;
	}
	fsm_batv_monitor();
	/* reschedule */
	queue_delayed_work(fsm_codec->codec_wq, &fsm_codec->codec_monitor,
			2*HZ);

}

static int fsm_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct fsm_codec *fsm_codec = g_fsm_codec;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK) {
		pr_info("captrue stream");
		return 0;
	}

	if (mute) {
		fsm_set_codec_monitor(fsm_codec, false);
		fsm_speaker_off();
	}
	else {
		fsm_speaker_onn();

//ExtbP200604-05364  537023, baoshulin@wingtech.com, 20200624,  close vbat monitor
		fsm_set_codec_monitor(fsm_codec, false);
	}

	return 0;
}

#ifdef FSM_UNUSED_CODE
static int fsm_digital_mute(struct snd_soc_dai *dai, int mute)
{
	return fsm_mute_stream(dai, mute, SNDRV_PCM_STREAM_PLAYBACK);
}

static int fsm_trigger(struct snd_pcm_substream *substream,
			int cmd, struct snd_soc_dai *dai)
{
	switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
			break;
		case SNDRV_PCM_TRIGGER_STOP:
			break;
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			break;
		default:
			return -EINVAL;
	}
	return 0;
}
#endif

static const struct snd_soc_dai_ops fsm_aif_dai_ops = {
	.startup      = fsm_startup,
	.set_fmt      = fsm_set_fmt,
	.set_sysclk   = fsm_set_sysclk,
	.hw_params    = fsm_hw_params,
	.mute_stream  = fsm_mute_stream,
	//.digital_mute = fsm_digital_mute,
	//.trigger      = fsm_trigger,
	.shutdown     = fsm_shutdown,
};

static struct snd_soc_dai_driver fsm_aif_dai[] = {
	{
		.name = "fs18xx-aif",
		.id = 1,
		.playback = {
			.stream_name = "AIF Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = FSM_RATES,
			.formats = FSM_FORMATS,
		},
		.ops = &fsm_aif_dai_ops,
		.symmetric_rates = 1,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
		.symmetric_channels = 1,
		.symmetric_samplebits = 1,
#endif
	},
};

static int fsm_codec_probe(struct snd_soc_codec *codec)
{
	fsm_config_t *cfg = fsm_get_config();
	struct fsm_codec *fsm_codec;
	int ret;
	pr_info("audiock fsm %s enter\n",__func__);
	dev_set_name(codec->dev, "%s", "0-0036");
	pr_info("dev_name: %s", dev_name(codec->dev));
	if (!cfg) {
		return -EINVAL;
	}
	if (fsm_set_pdev(codec->dev)) {
		ret = fsm_firmware_init(cfg->fw_name);
		if (ret) {
			// firmware init fail, it will try in speaker on again
			pr_err("firmware init fail:%d", ret);
		}
	}
	ret = fsm_add_widgets(codec);
	fsm_codec = devm_kzalloc(codec->dev, sizeof(struct fsm_codec), GFP_KERNEL);
	if (!fsm_codec) {
		pr_err("allocate memory fail");
		return -EINVAL;
	}
	fsm_codec->codec_wq = create_singlethread_workqueue("fs16xx");
	INIT_DELAYED_WORK(&fsm_codec->codec_monitor, fsm_codec_monitor);
	g_fsm_codec = fsm_codec;
	pr_info("codec registered");
	pr_info("audiock fsm %s end\n",__func__);
	FSM_FUNC_EXIT(ret);
	return ret;
}

static remove_ret_type fsm_codec_remove(struct snd_soc_codec *codec)
{
	struct fsm_codec *fsm_codec = g_fsm_codec;

	if (fsm_codec) {
		cancel_delayed_work_sync(&fsm_codec->codec_monitor);
		destroy_workqueue(fsm_codec->codec_wq);
		devm_kfree(codec->dev, fsm_codec);
	}
	if (codec && fsm_get_pdev() == codec->dev) {
		fsm_firmware_deinit();
	}
	return remove_ret_val;
}

static struct snd_soc_codec_driver soc_codec_dev_fsm = {
	.probe  = fsm_codec_probe,
	.remove = fsm_codec_remove,
};

int fsm_codec_register(struct device *dev, int id)
{
	fsm_config_t *cfg = fsm_get_config();
	int size = ARRAY_SIZE(fsm_aif_dai);
	int ret;
	pr_info("audiock fsm %s enter\n",__func__);
	if (!cfg || cfg->codec_inited >= size) {
		// not support codec or codec inited
		pr_info("audiock fsm %s not support codec or codec inited\n",__func__);
		return MODULE_INITED;
	}
	pr_info("audiock fsm id:%d, size:%d", id, size);
	if (id < 0 || id >= size) {
		pr_err("invalid id: %d", id);
		return -EINVAL;
	}
	dev_set_name(dev, "%s", "fs18xx");
	pr_info("dev_name: %s", dev_name(dev));
	ret = snd_soc_register_codec(dev, &soc_codec_dev_fsm,
			&fsm_aif_dai[id], 1);
	if (ret < 0) {
		dev_err(dev, "failed to register CODEC DAI: %d", ret);
		return ret;
	}
	cfg->codec_inited++;
	// fsm_set_codec_config(dev, &fsm_aif_dai[id], id);

	return ret;
}

void fsm_codec_unregister(struct device *dev)
{
	fsm_config_t *cfg = fsm_get_config();

	pr_info("enter");
	if (!cfg || !cfg->codec_inited) {
		return;
	}
	snd_soc_unregister_codec(dev);
	cfg->codec_inited--;
}
#endif
