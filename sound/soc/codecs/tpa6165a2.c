/*
 * Copyright (C) 2012-2013 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/miscdevice.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/tpa6165a2.h>
#include <sound/soc.h>
#include <sound/jack.h>

#include "tpa6165a2-core.h"


#define NAME "tpa6165a2"

#define I2C_RETRY_DELAY		5 /* ms */
#define I2C_RETRIES		5

static struct i2c_client *tpa6165_client;

struct tpa6165_data {
	struct regulator *vdd;
	struct regulator *micvdd;
	int gpio_irq;
	int gpio;
	struct snd_soc_jack *hs_jack;
	struct snd_soc_jack *button_jack;
	u8 dev_status_reg1;
	u8 dev_status_reg2;
	u8 dev_status_reg3;
	u8 hs_acc_reg;
	int hs_acc_type;
	int inserted;
	int button_pressed;
	int amp_state;
	int mic_state;
	int sleep_state;
	int special_hs;
	int alwayson_micb;
	struct mutex lock;
	struct wake_lock wake_lock;
	struct work_struct work;
};

static const struct tpa6165_regs tpa6165_reg_defaults[] = {
{
	.reg =  TPA6165_ENABLE_REG2,
	.value = 0x39,
},
{
	.reg = TPA6165_INT_MASK_REG1,
	.value = 0xc2,
},
{
	.reg = TPA6165_INT_MASK_REG2,
	.value = 0x04,
},
{
	.reg = TPA6165_HPH_VOL_CTL_REG1,
	.value = 0xb9,
},
{
	.reg = TPA6165_HPH_VOL_CTL_REG2,
	.value =  0x39,
},
{
	.reg = TPA6165_MICB_CTL_REG,
	.value = 0x04,
},
{
	.reg = TPA6165_RSVD_DEF_REG1,
	.value = 0x01,
},
{
	.reg =  TPA6165_RSVD_DEF_REG2,
	.value = 0x45,
},
{
	.reg =  TPA6165_KEYSCAN_DEBOUNCE_REG,
	.value = 0xff,
},
{
	.reg =  TPA6165_KEYSCAN_DELAY_REG,
	.value = 0xc7,
},
{
	.reg =  TPA6165_JACK_DETECT_TEST_HW1,
	.value = 0x80,
},
{
	.reg =  TPA6165_JACK_DETECT_TEST_HW2,
	.value = 0x95,
},
{
	.reg =  TPA6165_CLK_CTL,
	.value = 0x00,
},
{
	.reg = TPA6165_ENABLE_REG1,
	.value = 0xc0,
},
};

/* I2C Read/Write Functions */
static int tpa6165_i2c_read(struct tpa6165_data *tpa6165, u8 reg, u8 *value)
{
	int err;
	int tries = 0;

	struct i2c_msg msgs[] = {
		{
		 .addr = tpa6165_client->addr,
		 .flags = 0,
		 .len = 1,
		 .buf = &reg,
		 },
		{
		 .addr = tpa6165_client->addr,
		 .flags = I2C_M_RD,
		 .len = 1,
		 .buf = value,
		 },
	};

	do {
		err = i2c_transfer(tpa6165_client->adapter, msgs,
				ARRAY_SIZE(msgs));
		if (err != ARRAY_SIZE(msgs))
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err != ARRAY_SIZE(msgs)) && (++tries < I2C_RETRIES));

	if (err != ARRAY_SIZE(msgs)) {
		dev_err(&tpa6165_client->dev, "read transfer error %d\n", err);
		err = -EIO;
	} else {
		err = 0;
	}

	return err;
}

static int tpa6165_i2c_write(struct tpa6165_data *tpa6165, u8 reg, u8 value)
{
	int err;
	int tries = 0;
	u8 buf[2] = {0, 0};

	struct i2c_msg msgs[] = {
		{
		 .addr = tpa6165_client->addr,
		 .flags = tpa6165_client->flags & I2C_M_TEN,
		 .len = 2,
		 .buf = buf,
		 },
	};

	buf[0] = reg;
	buf[1] = value;

	do {
		err = i2c_transfer(tpa6165_client->adapter, msgs,
					ARRAY_SIZE(msgs));
		if (err != ARRAY_SIZE(msgs))
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err != ARRAY_SIZE(msgs)) && (++tries < I2C_RETRIES));

	if (err != ARRAY_SIZE(msgs)) {
		dev_err(&tpa6165_client->dev, "write transfer error\n");
		err = -EIO;
	} else {
		err = 0;
	}

	return err;
}


static int tpa6165_reg_read(struct tpa6165_data *tpa6165, u8 reg, u8 *value)
{
	return tpa6165_i2c_read(tpa6165, reg , value);
}

static int tpa6165_reg_write(struct tpa6165_data *tpa6165,
			u8 reg,
			u8 value,
			u8 mask)
{
	int retval;
	u8 old_value;

	value &= mask;

	retval = tpa6165_reg_read(tpa6165, reg , &old_value);

	if (retval != 0)
		goto error;

	old_value &= ~mask;
	value |= old_value;

	retval = tpa6165_i2c_write(tpa6165, reg, value);

error:
	return retval;
}

static void tpa6165_initialize_work(struct work_struct *work)
{
	int i;
	int retval;
	struct tpa6165_data *tpa6165 = i2c_get_clientdata(tpa6165_client);

	/* Initialize device registers */
	for (i = 0; i < ARRAY_SIZE(tpa6165_reg_defaults); i++) {
		retval = tpa6165_reg_write(tpa6165, tpa6165_reg_defaults[i].reg,
					tpa6165_reg_defaults[i].value, 0xff);
		if (retval != 0)
			goto error;
	}
	pr_info("tpa6165_initialize success\n");
error:
	return;
}

#ifdef CONFIG_DEBUG_FS

static struct dentry *tpa6165_dir;
static struct dentry *tpa6165_reg_file;
static struct dentry *tpa6165_set_reg_file;

static int tpa6165_registers_print(struct seq_file *s, void *p)
{
	struct tpa6165_data *tpa6165 = s->private;
	u8 reg;
	u8 value;
	pr_info("%s: print registers", __func__);
	seq_printf(s, "tpa6165 registers:\n");
	for (reg = 0; reg < TPA6165_MAX_REGISTER_VAL; reg++) {
		tpa6165_reg_read(tpa6165, reg, &value);
		seq_printf(s, "[0x%x]:  0x%x\n", reg, value);
	}

	return 0;
}

static int tpa6165_registers_open(struct inode *inode, struct file *file)
{
	return single_open(file, tpa6165_registers_print, inode->i_private);
}


static int tpa6165_set_reg_open_file(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t tpa6165_set_reg(struct file *file,
				  const char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	struct tpa6165_data *tpa6165 = file->private_data;
	char buf[32];
	ssize_t buf_size;
	unsigned int user_reg;
	unsigned int user_value;
	u8 reg_read_back;
	/* Get userspace string and assure termination */
	buf_size = min(count, (sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	if (sscanf(buf, "0x%02x  0x%02x", &user_reg, &user_value) != 2)
		return -EFAULT;

	if (user_reg > TPA6165_MAX_REGISTER_VAL) {
		pr_info("%s: val check failed", __func__);
		return -EINVAL;
	}

	pr_info("%s: set register 0x%02x value 0x%02x", __func__,
		user_reg, user_value);

	tpa6165_reg_write(tpa6165, user_reg & 0xFF, user_value & 0XFF, 0xFF);
	tpa6165_reg_read(tpa6165, user_reg, &reg_read_back);

	pr_info("%s: debug write reg[0x%02x] with 0x%02x after readback: 0x%02x\n",
				__func__, user_reg, user_value, reg_read_back);

	return buf_size;
}


static const struct file_operations tpa6165_registers_fops = {
	.open = tpa6165_registers_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

static const struct file_operations tpa6165_get_set_reg_fops = {
	.open = tpa6165_set_reg_open_file,
	.write = tpa6165_set_reg,
	.owner = THIS_MODULE,
};


static void tpa6165_setup_debugfs(struct tpa6165_data *tpa6165)
{
	if (!tpa6165)
		goto exit_no_debugfs;

	tpa6165_dir = debugfs_create_dir("tpa6165", NULL);
	if (!tpa6165_dir)
		goto exit_no_debugfs;

	tpa6165_reg_file = debugfs_create_file("registers",
				S_IRUGO, tpa6165_dir, tpa6165,
				&tpa6165_registers_fops);
	if (!tpa6165_reg_file)
		goto exit_destroy_dir;

	tpa6165_set_reg_file = debugfs_create_file("set_reg",
				S_IWUSR, tpa6165_dir, tpa6165,
				&tpa6165_get_set_reg_fops);
	if (!tpa6165_set_reg_file)
		goto exit_destroy_set_reg;

	return;

exit_destroy_set_reg:
	debugfs_remove(tpa6165_reg_file);
exit_destroy_dir:
	debugfs_remove(tpa6165_dir);
exit_no_debugfs:

	return;
}
static inline void tpa6165_remove_debugfs(void)
{
	debugfs_remove(tpa6165_set_reg_file);
	debugfs_remove(tpa6165_reg_file);
	debugfs_remove(tpa6165_dir);
}
#else
static inline void tpa6165_setup_debugfs(struct tpa6165_data *tpa6165)
{
}
static inline void tpa6165_remove_debugfs(void)
{
}
#endif


static void tpa6165_sleep(struct tpa6165_data *tpa6165, int sleep_state)
{
	switch (sleep_state) {
	case TPA6165_SPECIAL_SLEEP:
		/* if it is special headset or AOV, enables all blocks,
		 * and selectively disable L/R channels and mic amp
		 * and enable Mic bias for proper detection of the hs.
		 * The side effect is a bit of extra current drain for
		 * this use case.
		 */
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG1,
			~TPA6165_SLEEP_ENABLE, TPA6165_SLEEP_ENABLE);
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
			~TPA6165_AUTO_MODE, TPA6165_AUTO_MODE);
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
			(~TPA6165_LEFT_RIGHT_CH) & 0xff,
			TPA6165_LEFT_RIGHT_CH);
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
			(~TPA6165_MIC_AMP_EN) & 0xff,
			TPA6165_MIC_AMP_EN);
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
			(TPA6165_MIC_BIAS_EN), TPA6165_MIC_BIAS_EN);
		break;
	case TPA6165_SLEEP:
		/* set IC in sleep */
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG1,
			TPA6165_SLEEP_ENABLE, TPA6165_SLEEP_ENABLE);
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
			1, TPA6165_AUTO_MODE);
		break;
	case TPA6165_WAKEUP:
		/* take IC out of sleep */
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
			1, TPA6165_AUTO_MODE);
		tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG1,
			~TPA6165_SLEEP_ENABLE, TPA6165_SLEEP_ENABLE);
		break;
	}
	tpa6165->sleep_state = sleep_state;
}

void tpa6165_hp_event(int event)
{
	struct tpa6165_data *tpa6165;
	u8 en_reg2 = 0;
	/* make sure tpa6165 is probed */
	if (tpa6165_client == NULL)
		return;
	else {
		tpa6165 = i2c_get_clientdata(tpa6165_client);
		if (tpa6165 == NULL)
			return;
	}

	mutex_lock(&tpa6165->lock);
	if (event) {
		if (tpa6165->inserted) {
			tpa6165->amp_state = TPA6165_AMP_ENABLED;
			tpa6165_sleep(tpa6165, TPA6165_WAKEUP);
		}
	} else {
		tpa6165->amp_state = TPA6165_AMP_DISABLED;
		tpa6165_reg_read(tpa6165, TPA6165_ENABLE_REG2,
				&en_reg2);
		/* enable vol slew interrupt and disable left/rigt channels.
		 * IC will be put to sleep when vol slew complete interupt is
		 * received, which should happen in 10/20 ms. This step is
		 * needed to avoid loud pops when audio stops playing.
		 */
		if (en_reg2 & TPA6165_LEFT_RIGHT_CH &&
				tpa6165->sleep_state == TPA6165_WAKEUP &&
				tpa6165->mic_state == TPA6165_MIC_DISABLED) {
			tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG1,
				TPA6165_VOL_SLEW_INT, TPA6165_VOL_SLEW_INT);
			tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
				~TPA6165_AUTO_MODE, TPA6165_AUTO_MODE);
			tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
				(~TPA6165_LEFT_RIGHT_CH) & 0xff,
				TPA6165_LEFT_RIGHT_CH);
		}
	}
	mutex_unlock(&tpa6165->lock);

	return;
}
EXPORT_SYMBOL_GPL(tpa6165_hp_event);

void tpa6165_mic_event(int event)
{
	struct tpa6165_data *tpa6165;
	u8 en_reg2 = 0;

	if (tpa6165_client == NULL)
		return;
	else {
		tpa6165 = i2c_get_clientdata(tpa6165_client);
		if (tpa6165 == NULL)
			return;
	}

	mutex_lock(&tpa6165->lock);
	if (tpa6165->alwayson_micb || tpa6165->special_hs) {
		mutex_unlock(&tpa6165->lock);
		/* mic bias always on in this case nothing to do */
		return;
	}

	if (event) {
		if (tpa6165->inserted) {
			tpa6165->mic_state = TPA6165_MIC_ENABLED;
			tpa6165_sleep(tpa6165, TPA6165_WAKEUP);
		}
	} else {
		tpa6165->mic_state = TPA6165_MIC_DISABLED;
		tpa6165_reg_read(tpa6165, TPA6165_ENABLE_REG2,
				&en_reg2);
		/* enable vol slew interrupt and disable left/rigt channels.
		 * IC will be put to sleep when vol slew complete interupt is
		 * received, which should happen in 10/20 ms. This step is
		 * needed to avoid loud pops when audio stops playing.
		 */
		if (en_reg2 & TPA6165_LEFT_RIGHT_CH &&
				tpa6165->sleep_state == TPA6165_WAKEUP &&
				tpa6165->amp_state == TPA6165_AMP_DISABLED) {
			tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG1,
				TPA6165_VOL_SLEW_INT, TPA6165_VOL_SLEW_INT);
			tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
				~TPA6165_AUTO_MODE, TPA6165_AUTO_MODE);
			tpa6165_reg_write(tpa6165, TPA6165_ENABLE_REG2,
				(~TPA6165_LEFT_RIGHT_CH) & 0xff,
				TPA6165_LEFT_RIGHT_CH);
		}
	}
	mutex_unlock(&tpa6165->lock);

	return;
}
EXPORT_SYMBOL_GPL(tpa6165_mic_event);

static int tpa6165_update_device_status(struct tpa6165_data *tpa6165)
{
	tpa6165_reg_read(tpa6165, TPA6165_DEVICE_STATUS_REG1,
			&(tpa6165->dev_status_reg1));
	tpa6165_reg_read(tpa6165, TPA6165_DEVICE_STATUS_REG2,
			&(tpa6165->dev_status_reg2));
	tpa6165_reg_read(tpa6165, TPA6165_DEVICE_STATUS_REG3,
			&(tpa6165->dev_status_reg3));
	tpa6165_reg_read(tpa6165, TPA6165_ACC_STATE_REG,
			&(tpa6165->hs_acc_reg));
	pr_debug("%s: tpa6165 reg1:%d reg2: %d reg3:%d acc:%d\n", __func__,
		(tpa6165->dev_status_reg1),	(tpa6165->dev_status_reg2),
		(tpa6165->dev_status_reg3), tpa6165->hs_acc_reg);
	return 0;
}

static int tpa6165_get_hs_acc_type(struct tpa6165_data *tpa6165)
{
	int acc_type;

	switch (tpa6165->hs_acc_reg & (0x7f)) {
	case TPA6165_STEREO_MIC_0N_SLEEVE:
		/* This accessory type could be speacial headset type, if it
		 * is it will be detected as removed in < ~100ms incorrectly.
		 * This is due to the fact that if Micbias is off (state during
		 * sleep mode), special headset shows characteristics of
		 * resistive open, which triggers false removal detection.
		 */
		if (!tpa6165->special_hs) {
			/* disable interrupts, clear interrupt mask regs */
			tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG1,
				0, 0xff);
			tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG2,
				0, 0xff);
			msleep_interruptible(100);
			tpa6165_update_device_status(tpa6165);
			if (!(tpa6165->dev_status_reg1 & TPA6165_JACK_DETECT)) {
				tpa6165->special_hs = 1;
				/* set acc type as unsupported, will be updated
				 * when interrupt is received when device is
				 * out of sleep
				 */
				acc_type = SND_JACK_UNSUPPORTED;
				tpa6165_sleep(tpa6165, TPA6165_WAKEUP);
			} else {
				acc_type = SND_JACK_HEADSET;
				tpa6165->special_hs = 0;
			}
			/* enable interrupts */
			tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG1,
				0xc2, 0xff);
			tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG2,
				0x4, 0xff);
		} else
			acc_type = SND_JACK_HEADSET;

		break;
	case TPA6165_STEREO_MIC_0N_RING:
	case TPA6165_MONO_MIC_0N_SLEEVE1:
	case TPA6165_MONO_MIC_0N_SLEEVE2:
	case TPA6165_MONO_MIC_0N_SLEEVE3:
	case TPA6165_MONO_MIC_0N_RING1:
	case TPA6165_MONO_MIC_0N_RING2:
	case TPA6165_MONO_MIC_0N_RING3:
		tpa6165->special_hs = 0;
		acc_type = SND_JACK_HEADSET;
		break;
	case TPA6165_STEREO_HEADPHONE1:
		tpa6165->special_hs = 0;
		acc_type = SND_JACK_HEADPHONE;
		break;
	case TPA6165_STEREO_HEADPHONE2:
	case TPA6165_STEREO_HEADPHONE3:
		/* force high impedence stereo heaphone to low impedence type
		 * for proper detection.
		 */
		tpa6165_reg_write(tpa6165, TPA6165_ACC_STATE_REG,
			TPA6165_STEREO_HEADPHONE1, 0xff);
		tpa6165_reg_write(tpa6165, TPA6165_ACC_STATE_REG,
			TPA6165_FORCE_TYPE | TPA6165_STEREO_HEADPHONE1,
			0xff);
		tpa6165->special_hs = 0;
		acc_type = SND_JACK_HEADPHONE;
		break;
	case TPA6165_STEREO_LINEOUT1:
		tpa6165->special_hs = 0;
		acc_type = SND_JACK_HEADPHONE;
		break;
	case TPA6165_STEREO_LINEOUT2:
	case TPA6165_STEREO_LINEOUT3:
	case TPA6165_STEREO_LINEOUT4:
		/* For High Impedence lineout cases, need to force acc type
		 * to lineout1 type for proper detection.
		 */
		tpa6165_reg_write(tpa6165, TPA6165_ACC_STATE_REG,
			TPA6165_STEREO_LINEOUT1, 0xff);
		tpa6165_reg_write(tpa6165, TPA6165_ACC_STATE_REG,
			TPA6165_FORCE_TYPE | TPA6165_STEREO_LINEOUT1,
			0xff);
		tpa6165->special_hs = 0;
		acc_type = SND_JACK_HEADPHONE;
		break;
	default:
		/* no valid acc detected */
		tpa6165->special_hs = 0;
		acc_type = SND_JACK_UNSUPPORTED;
		break;
	}

	if (acc_type != SND_JACK_UNSUPPORTED &&
			tpa6165->amp_state != TPA6165_AMP_DISABLED)
		/* wake up if amp state is still enabled, from ASOC point of
		 * view. This might happen if the headset is pulled out and
		 * inserted back in quickly before the DAPM event to disable.
		 * if audio playback starts resumes immediatly IC will be wakeup
		 * state to playout audio and put to sleep when disable event.
		 * is received.
		 */
		tpa6165_sleep(tpa6165, TPA6165_WAKEUP);
	else if (acc_type == SND_JACK_HEADSET && (tpa6165->alwayson_micb ||
			tpa6165->special_hs))
		/* need to call this here to trigger special sleep state
		 * where all blocks are disabled but mic bias is enabled to
		 * support always on voice or special hs case.
		 */
		tpa6165_sleep(tpa6165, TPA6165_SPECIAL_SLEEP);

	return acc_type;
}

static int tpa6165_report_hs(struct tpa6165_data *tpa6165)
{
	mutex_lock(&tpa6165->lock);
	if ((tpa6165->button_jack == NULL) || (tpa6165->hs_jack == NULL)) {
		pr_err("tpa6165: Failed to report hs, jacks not initialized");
		mutex_unlock(&tpa6165->lock);
		return -EINVAL;
	}

	/* check jack detect bit, is set when valid acc type is inserted */
	if ((tpa6165->dev_status_reg1 & TPA6165_JACK_DETECT) &&
			!tpa6165->inserted) {
		/* insertion detected get hs accessory type */
		tpa6165->hs_acc_type = tpa6165_get_hs_acc_type(tpa6165);
		if (!(tpa6165->hs_acc_type == SND_JACK_UNSUPPORTED)) {
			tpa6165->inserted = 1;
			snd_soc_jack_report_no_dapm(tpa6165->hs_jack,
				tpa6165->hs_acc_type,
				tpa6165->hs_jack->jack->type);
		}
	} else if (!(tpa6165->dev_status_reg1 & TPA6165_JACK_DETECT) &&
			tpa6165->inserted) {
		/* removal detected */
		tpa6165->inserted = 0;
		snd_soc_jack_report_no_dapm(tpa6165->hs_jack, 0,
					tpa6165->hs_jack->jack->type);
		/* check if button pressed when jack removed */
		if (tpa6165->button_pressed) {
			/* report button released */
			tpa6165->button_pressed = 0;
			snd_soc_jack_report_no_dapm(tpa6165->button_jack,
				0, tpa6165->button_jack->jack->type);
		}
		/* clear flag and put the IC in regular sleep mode */
		tpa6165->special_hs = 0;
		tpa6165_sleep(tpa6165, TPA6165_SLEEP);
	} else if ((tpa6165->dev_status_reg1 & TPA6165_JACK_DETECT) &&
			tpa6165->inserted) {
		/* no change in jack detect state, check for Button press */
		if ((tpa6165->dev_status_reg1 & TPA6165_JACK_BUTTON)) {
			if (!tpa6165->button_pressed) {
				/* report button press */
				tpa6165->button_pressed = 1;
				pr_debug("%s:report button press", __func__);
				snd_soc_jack_report_no_dapm(
					tpa6165->button_jack,
					SND_JACK_BTN_0,
					tpa6165->button_jack->jack->type);
			} else {
				/* report button release */
				tpa6165->button_pressed = 0;
				pr_debug("%s:report button release", __func__);
				snd_soc_jack_report_no_dapm(
					tpa6165->button_jack,
					0, tpa6165->button_jack->jack->type);
			}
		} /* else nothing to report here */
	} else if (!(tpa6165->dev_status_reg1 & TPA6165_JACK_DETECT) &&
			!tpa6165->inserted) {
			/* no valid accessory detected on receiving interrupt,
			check if jack is broken or jack not properly inserted */
			if ((tpa6165->dev_status_reg1 & TPA6165_JACK_SENSE) &&
				(tpa6165->dev_status_reg1 & TPA6165_DET_DONE)) {
				tpa6165->hs_acc_type = SND_JACK_UNSUPPORTED;
				snd_soc_jack_report_no_dapm(tpa6165->hs_jack,
						tpa6165->hs_acc_type,
						tpa6165->hs_jack->jack->type);
			}
	}
	/* check vol slew status, this should be enabled only while
	 * putting the device the sleep to minimize pops.
	 */
	if (tpa6165->dev_status_reg1 & TPA6165_VOL_SLEW_DONE) {
		if (tpa6165->amp_state == TPA6165_AMP_DISABLED &&
				tpa6165->mic_state == TPA6165_MIC_DISABLED) {
			/* put the IC in sleep mode */
			if ((tpa6165->alwayson_micb || tpa6165->special_hs)
				&& tpa6165->inserted)
				tpa6165_sleep(tpa6165, TPA6165_SPECIAL_SLEEP);
			else
				tpa6165_sleep(tpa6165, TPA6165_SLEEP);
		}

		/* clear vol slew interrupt mask */
		tpa6165_reg_write(tpa6165, TPA6165_INT_MASK_REG1,
				~TPA6165_VOL_SLEW_DONE,
				TPA6165_VOL_SLEW_DONE);
	}
	mutex_unlock(&tpa6165->lock);
	return 0;
}

int tpa6165_hs_detect(struct snd_soc_jack *hs_jack,
					struct snd_soc_jack *button_jack)
{
	if (tpa6165_client) {
		struct tpa6165_data *tpa6165 =
					i2c_get_clientdata(tpa6165_client);

		if (tpa6165 == NULL)
			return -EINVAL;

		pr_debug("%s: hs and button jack", __func__);
		tpa6165->hs_jack = hs_jack;
		tpa6165->button_jack = button_jack;

		/* check device status registers for boot time detection */
		tpa6165_update_device_status(tpa6165);
		tpa6165_report_hs(tpa6165);
	} else
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(tpa6165_hs_detect);

static irqreturn_t tpa6165_det_thread(int irq, void *data)
{
	struct tpa6165_data *irq_data = data;

	pr_debug("%s: Enter", __func__);
	wake_lock_timeout(&irq_data->wake_lock, HZ);

	tpa6165_update_device_status(irq_data);
	tpa6165_report_hs(irq_data);

	return IRQ_HANDLED;
}

#ifdef CONFIG_OF
static struct tpa6165a2_platform_data *
tpa6165_of_init(struct i2c_client *client)
{
	struct tpa6165a2_platform_data *pdata;
	struct device_node *np = client->dev.of_node;

	pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(&client->dev, "pdata allocation failure\n");
		return NULL;
	}

	of_property_read_u32(np, "ti,tpa6165-always-on-micbias",
				&pdata->alwayson_micbias);
	pdata->irq_gpio = of_get_gpio(np, 0);

	return pdata;
}
#else
static inline struct tpa6165a2_platform_data *
tpa6165_of_init(struct i2c_client *client)
{
	return NULL;
}
#endif

static int __devinit tpa6165_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct tpa6165_data *tpa6165;
	struct tpa6165a2_platform_data *tpa6165_pdata;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "check_functionality failed\n");
		return -EIO;
	}

	if (client->dev.of_node)
			tpa6165_pdata = tpa6165_of_init(client);
	else
			tpa6165_pdata = client->dev.platform_data;

	/* Check platform data */
	if (tpa6165_pdata == NULL) {
		dev_err(&client->dev,
			"platform data is NULL\n");
		return -EINVAL;
	}

	tpa6165 = kzalloc(sizeof(*tpa6165), GFP_KERNEL);
	if (tpa6165 == NULL) {
		dev_err(&client->dev,
			"Failed to allocate memory for module data\n");
		return -ENOMEM;
	}

	err = gpio_request(tpa6165_pdata->irq_gpio, "hs irq");
	if (err) {
		dev_err(&client->dev, "tpa6165 irq gpio init failed\n");
		goto gpio_init_fail;
	}

	/* set global variables */
	tpa6165_client = client;
	i2c_set_clientdata(client, tpa6165);
	mutex_init(&tpa6165->lock);
	tpa6165->gpio = tpa6165_pdata->irq_gpio;
	tpa6165->inserted = 0;
	tpa6165->button_pressed = 0;
	tpa6165->button_jack = NULL;
	tpa6165->hs_jack = NULL;
	/* This flag is used to indicate that mic bias should be always
	 * on when headset is inserted, needed for always on voice
	 * operations and noise estimate.
	 */
	tpa6165->alwayson_micb = tpa6165_pdata->alwayson_micbias;

	/* enable regulators */
	tpa6165->vdd = regulator_get(&client->dev, "hs_det_vdd");
	if (IS_ERR(tpa6165->vdd)) {
		pr_err("%s: Error getting vdd regulator.\n", __func__);
		err = PTR_ERR(tpa6165->vdd);
		goto reg_get_vdd;
	}

	regulator_set_voltage(tpa6165->vdd, 1800000, 1800000);
	err = regulator_enable(tpa6165->vdd);
	if (err < 0) {
		pr_err("%s: Error enabling vdd regulator.\n", __func__);
		goto reg_enable_vdd_fail;
	}

	tpa6165->micvdd = regulator_get(&client->dev, "hs_det_micvdd");
	if (IS_ERR(tpa6165->micvdd)) {
		pr_err("%s: Error getting micvdd regulator.\n", __func__);
		err = PTR_ERR(tpa6165->micvdd);
		goto reg_get_micvdd;
	}

	regulator_set_voltage(tpa6165->micvdd, 2850000, 2850000);
	err = regulator_enable(tpa6165->micvdd);
	if (err < 0) {
		pr_err("%s: Error enabling micvdd regulator.\n", __func__);
		goto reg_enable_micvdd_fail;
	}

	wake_lock_init(&tpa6165->wake_lock, WAKE_LOCK_SUSPEND, "hs_det");

	/* Initialize device registers */
	INIT_WORK(&tpa6165->work,  tpa6165_initialize_work);
	schedule_work(&tpa6165->work);

	tpa6165->gpio_irq = gpio_to_irq(tpa6165->gpio);
	/* active low interrupt */
	err = request_threaded_irq(tpa6165->gpio_irq , NULL, tpa6165_det_thread,
			IRQF_TRIGGER_FALLING, "hs_det", tpa6165);
	if (err) {
		pr_err("%s: IRQ request failed: %d\n", __func__, err);
		goto irq_fail;
	}
	enable_irq_wake(tpa6165->gpio_irq);

	/* setup debug fs interfaces */
	tpa6165_setup_debugfs(tpa6165);

	pr_info("tpa6165a2 probed successfully\n");

	return 0;

irq_fail:
	wake_lock_destroy(&tpa6165->wake_lock);
reg_enable_micvdd_fail:
	regulator_put(tpa6165->micvdd);
reg_get_micvdd:
	regulator_disable(tpa6165->vdd);
reg_enable_vdd_fail:
	regulator_put(tpa6165->vdd);
reg_get_vdd:
	gpio_free(tpa6165->gpio);
gpio_init_fail:
	kfree(tpa6165);
	tpa6165_client = NULL;
	return err;
}

static int __devexit tpa6165_remove(struct i2c_client *client)
{
	struct tpa6165_data *tpa6165 = i2c_get_clientdata(client);
	regulator_disable(tpa6165->vdd);
	regulator_put(tpa6165->vdd);
	regulator_disable(tpa6165->micvdd);
	regulator_put(tpa6165->micvdd);
	gpio_free(tpa6165->gpio);
	wake_lock_destroy(&tpa6165->wake_lock);
	tpa6165_remove_debugfs();
	kfree(tpa6165);
	tpa6165_client = NULL;
	return 0;
}

static const struct i2c_device_id tpa6165_id[] = {
	{NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, tpa6165_id);

#ifdef CONFIG_OF
static struct of_device_id tpa6165_match_tbl[] = {
	{ .compatible = "ti,tpa6165" },
	{ },
};
MODULE_DEVICE_TABLE(of, tpa6165_match_tbl);
#endif

static struct i2c_driver tpa6165_driver = {
	.driver = {
			.name = NAME,
			.owner = THIS_MODULE,
			.of_match_table = of_match_ptr(tpa6165_match_tbl),
	},
	.probe = tpa6165_probe,
	.remove = __devexit_p(tpa6165_remove),
	.id_table = tpa6165_id,
};

static int __init tpa6165_init(void)
{
	return i2c_add_driver(&tpa6165_driver);
}

static void __exit tpa6165_exit(void)
{
	i2c_del_driver(&tpa6165_driver);
	return;
}

module_init(tpa6165_init);
module_exit(tpa6165_exit);

MODULE_DESCRIPTION("tpa6165 driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Motorola Mobility");
