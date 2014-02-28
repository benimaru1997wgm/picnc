/*    Copyright (C) 2014 GP Orcullo
 *
 *    Portions of this code is based on stepgen.c
 *    by John Kasunich, Copyright (C) 2003-2007
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "rtapi.h"
#include "rtapi_app.h"
#include "hal.h"

#include <math.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "picnc_grbl.h"

#if !defined(BUILD_SYS_USER_DSO)
#error "This driver is for usermode threads only"
#endif

#define MODNAME "picnc_grbl"
#define PREFIX "picnc"

MODULE_AUTHOR("GP Orcullo");
MODULE_DESCRIPTION("Driver for GRBL pinout compatible Raspberry Pi PICnc board");
MODULE_LICENSE("GPL v2");

static int stepwidth = 1;
RTAPI_MP_INT(stepwidth, "Step width in 1/BASEFREQ");

static long pwmfreq = 1000;
RTAPI_MP_LONG(pwmfreq, "PWM frequency in Hz");

typedef struct {
	hal_float_t *position_cmd[NUMAXES],
	            *position_fb[NUMAXES],
	            *pwm_duty;
	hal_bit_t   *motor_enable,
		    *spindle_enable,
		    *coolant_enable,
	            *lim_x,
		    *lim_y,
		    *lim_z,
		    *abort,
		    *hold,
		    *resume,
	            *ready,
		    *spi_fault;
	hal_float_t scale[NUMAXES],
	            maxaccel[NUMAXES],
	            pwm_scale;
	hal_u32_t   *test;
} data_t;

static data_t *data;

static int comp_id;
static const char *modname = MODNAME;
static const char *prefix = PREFIX;

volatile unsigned *gpio, *spi;

volatile int32_t txBuf[BUFSIZE], rxBuf[BUFSIZE];
static u32 pwm_period = 0;

static double dt = 0,				/* update_freq period in seconds */
	recip_dt = 0,				/* reciprocal of period, avoids divides */
	scale_inv[NUMAXES] = { 1.0 },		/* inverse of scale */
	old_vel[NUMAXES] = { 0 },
	old_pos[NUMAXES] = { 0 },
	old_scale[NUMAXES] = { 0 },
	max_vel;
static long old_dtns = 0;			/* update_freq funct period in nsec */
static s32 accum_diff = 0,
	old_count[NUMAXES] = { 0 };
static s64 accum[NUMAXES] = { 0 };		/* 64 bit DDS accumulator */

static void read_spi(void *arg, long period);
static void write_spi(void *arg, long period);
static void update(void *arg, long period);
static void update_outputs(data_t *dat);
static void update_inputs(data_t *dat);
static void read_buf();
static void write_buf();
static int map_gpio();
static void setup_gpio();
static void restore_gpio();

int rtapi_app_main(void)
{
	char name[HAL_NAME_LEN + 1];
	int n, retval;

	/* initialise driver */
	comp_id = hal_init(modname);
	if (comp_id < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: hal_init() failed\n",
		        modname);
		return -1;
	}

	/* allocate shared memory */
	data = hal_malloc(sizeof(data_t));
	if (data == 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: hal_malloc() failed\n",
		        modname);
		hal_exit(comp_id);
		return -1;
	}

	/* configure board */
	retval = map_gpio();
	if (retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		        "%s: ERROR: cannot map GPIO memory\n", modname);
		return retval;
	}

	setup_gpio();

	pwm_period = (SYS_FREQ/pwmfreq) - 1;	/* PeripheralClock/pwmfreq - 1 */

	txBuf[0] = 0x4746433E;			/* this is config data (>CFG) */
	txBuf[1] = stepwidth;
	txBuf[2] = pwm_period;
	write_buf();			/* send config data */

	max_vel = BASEFREQ/(4.0 * stepwidth);	/* calculate velocity limit */

	/* export pins and parameters */
	for (n=0; n<NUMAXES; n++) {
		retval = hal_pin_float_newf(HAL_IN, &(data->position_cmd[n]),
		        comp_id, "%s.axis.%01d.position-cmd", prefix, n);
		if (retval < 0) goto error;
		*(data->position_cmd[n]) = 0.0;

		retval = hal_pin_float_newf(HAL_OUT, &(data->position_fb[n]),
		        comp_id, "%s.axis.%01d.position-fb", prefix, n);
		if (retval < 0) goto error;
		*(data->position_fb[n]) = 0.0;

		retval = hal_param_float_newf(HAL_RW, &(data->scale[n]),
		        comp_id, "%s.axis.%01d.scale", prefix, n);
		if (retval < 0) goto error;
		data->scale[n] = 1.0;

		retval = hal_param_float_newf(HAL_RW, &(data->maxaccel[n]),
		        comp_id, "%s.axis.%01d.maxaccel", prefix, n);
		if (retval < 0) goto error;
		data->maxaccel[n] = 1.0;
	}

	retval = hal_pin_bit_newf(HAL_OUT, &(data->lim_x), comp_id,
	        "%s.axis.0.limit", prefix);
	if (retval < 0) goto error;
	*(data->lim_x) = 0;

	retval = hal_pin_bit_newf(HAL_OUT, &(data->lim_y), comp_id,
	        "%s.axis.1.limit", prefix);
	if (retval < 0) goto error;
	*(data->lim_y) = 0;

	retval = hal_pin_bit_newf(HAL_OUT, &(data->lim_z), comp_id,
	        "%s.axis.2.limit", prefix);
	if (retval < 0) goto error;
	*(data->lim_z) = 0;

	retval = hal_pin_bit_newf(HAL_OUT, &(data->abort), comp_id,
	        "%s.in.abort", prefix);
	if (retval < 0) goto error;
	*(data->abort) = 0;

	retval = hal_pin_bit_newf(HAL_OUT, &(data->hold), comp_id,
	        "%s.in.hold", prefix);
	if (retval < 0) goto error;
	*(data->hold) = 0;

	retval = hal_pin_bit_newf(HAL_OUT, &(data->resume), comp_id,
	        "%s.in.resume", prefix);
	if (retval < 0) goto error;
	*(data->resume) = 0;

	retval = hal_pin_bit_newf(HAL_IN, &(data->motor_enable), comp_id,
	        "%s.motor.enable", prefix);
	if (retval < 0) goto error;
	*(data->motor_enable) = 0;

	retval = hal_pin_bit_newf(HAL_IN, &(data->coolant_enable), comp_id,
	        "%s.coolant.enable", prefix);
	if (retval < 0) goto error;
	*(data->coolant_enable) = 0;

	retval = hal_pin_bit_newf(HAL_IN, &(data->spindle_enable), comp_id,
	        "%s.spindle.enable", prefix);
	if (retval < 0) goto error;
	*(data->spindle_enable) = 0;

	retval = hal_pin_float_newf(HAL_IN, &(data->pwm_duty),
		comp_id, "%s.spindle_pwm.duty", prefix, n);
	if (retval < 0) goto error;
	*(data->pwm_duty) = 0.0;

	retval = hal_param_float_newf(HAL_RW, &(data->pwm_scale),
		comp_id,"%s.spindle_pwm.scale", prefix);
	if (retval < 0) goto error;
	data->pwm_scale = 1.0;


	retval = hal_pin_bit_newf(HAL_OUT, &(data->ready), comp_id,
	        "%s.ready", prefix);
	if (retval < 0) goto error;
	*(data->ready) = 0;

	retval = hal_pin_bit_newf(HAL_IO, &(data->spi_fault), comp_id,
	        "%s.spi_fault", prefix);
	if (retval < 0) goto error;
	*(data->spi_fault) = 0;

	retval = hal_pin_u32_newf(HAL_IN, &(data->test), comp_id,
	        "%s.test", prefix);
	if (retval < 0) goto error;
	*(data->test) = 0;
error:
	if (retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		        "%s: ERROR: pin export failed with err=%i\n",
		        modname, retval);
		hal_exit(comp_id);
		return -1;
	}

	/* export functions */
	rtapi_snprintf(name, sizeof(name), "%s.read", prefix);
	retval = hal_export_funct(name, read_spi, data, 1, 0, comp_id);
	if (retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		        "%s: ERROR: read function export failed\n", modname);
		hal_exit(comp_id);
		return -1;
	}
	rtapi_snprintf(name, sizeof(name), "%s.write", prefix);
	/* no FP operations */
	retval = hal_export_funct(name, write_spi, data, 0, 0, comp_id);
	if (retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		        "%s: ERROR: write function export failed\n", modname);
		hal_exit(comp_id);
		return -1;
	}
	rtapi_snprintf(name, sizeof(name), "%s.update", prefix);
	retval = hal_export_funct(name, update, data, 1, 0, comp_id);
	if (retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
		        "%s: ERROR: update function export failed\n", modname);
		hal_exit(comp_id);
		return -1;
	}

	rtapi_print_msg(RTAPI_MSG_INFO, "%s: installed driver\n", modname);
	hal_ready(comp_id);
	return 0;
}

void rtapi_app_exit(void)
{
	restore_gpio();
	munmap((void *)gpio,BLOCK_SIZE);
	munmap((void *)spi,BLOCK_SIZE);
	hal_exit(comp_id);
}

void read_spi(void *arg, long period)
{
	int i;
	static int startup = 0;
	data_t *dat = (data_t *)arg;

	read_buf();
	/* update input status */
	update_inputs(dat);
	*(dat->test) = rxBuf[2];

	/* command >CM2 */
	txBuf[0] = 0x324D433E;
	update_outputs(dat);

	write_buf();

	/* check for change in period */
	if (period != old_dtns) {
		old_dtns = period;
		dt = period * 0.000000001;
		recip_dt = 1.0 / dt;
	}

	/* check for scale change */
	for (i = 0; i < NUMAXES; i++) {
		if (dat->scale[i] != old_scale[i]) {
			old_scale[i] = dat->scale[i];
			/* scale must not be 0 */
			if ((dat->scale[i] < 1e-20) && (dat->scale[i] > -1e-20))
				dat->scale[i] = 1.0;
			scale_inv[i] = (1.0 / STEP_MASK) / dat->scale[i];
		}
	}

	read_buf();

	/* sanity check */
	if (rxBuf[0] == (0x314D433E ^ ~0)) {
		*(dat->ready) = 1;
	} else {
		*(dat->ready) = 0;
		if (!startup)
			startup = 1;
		else
			*(dat->spi_fault) = 1;
	}

	/* update outputs */
	for (i = 0; i < NUMAXES; i++) {
		/* the DDS uses 32 bit counter, this code converts
		   that counter into 64 bits */
		accum_diff = get_position(i) - old_count[i];
		old_count[i] = get_position(i);
		accum[i] += accum_diff;

		*(dat->position_fb[i]) = (float)(accum[i]) * scale_inv[i];
	}
}

void write_spi(void *arg, long period)
{
	write_buf();
}

void update(void *arg, long period)
{
	int i;
	data_t *dat = (data_t *)arg;
	double max_accl, vel_cmd, dv, new_vel,
	       dp, pos_cmd, curr_pos, match_accl, match_time, avg_v,
	       est_out, est_cmd, est_err;

	for (i = 0; i < NUMAXES; i++) {
		/* set internal accel limit to its absolute max, which is
		   zero to full speed in one thread period */
		max_accl = max_vel * recip_dt;

		/* check for user specified accel limit parameter */
		if (dat->maxaccel[i] <= 0.0) {
			/* set to zero if negative */
			dat->maxaccel[i] = 0.0;
		} else {
			/* parameter is non-zero, compare to max_accl */
			if ((dat->maxaccel[i] * fabs(dat->scale[i])) > max_accl) {
				/* parameter is too high, lower it */
				dat->maxaccel[i] = max_accl / fabs(dat->scale[i]);
			} else {
				/* lower limit to match parameter */
				max_accl = dat->maxaccel[i] * fabs(dat->scale[i]);
			}
		}

		/* calculate position command in counts */
		pos_cmd = *(dat->position_cmd[i]) * dat->scale[i];
		/* calculate velocity command in counts/sec */
		vel_cmd = (pos_cmd - old_pos[i]) * recip_dt;
		old_pos[i] = pos_cmd;

		/* apply frequency limit */
		if (vel_cmd > max_vel) {
			vel_cmd = max_vel;
		} else if (vel_cmd < -max_vel) {
			vel_cmd = -max_vel;
		}

		/* determine which way we need to ramp to match velocity */
		if (vel_cmd > old_vel[i])
			match_accl = max_accl;
		else
			match_accl = -max_accl;

		/* determine how long the match would take */
		match_time = (vel_cmd - old_vel[i]) / match_accl;
		/* calc output position at the end of the match */
		avg_v = (vel_cmd + old_vel[i]) * 0.5;
		curr_pos = (double)(accum[i]) * (1.0 / STEP_MASK);
		est_out = curr_pos + avg_v * match_time;
		/* calculate the expected command position at that time */
		est_cmd = pos_cmd + vel_cmd * (match_time - 1.5 * dt);
		/* calculate error at that time */
		est_err = est_out - est_cmd;

		if (match_time < dt) {
			/* we can match velocity in one period */
			if (fabs(est_err) < 0.0001) {
				/* after match the position error will be acceptable */
				/* so we just do the velocity match */
				new_vel = vel_cmd;
			} else {
				/* try to correct position error */
				new_vel = vel_cmd - 0.5 * est_err * recip_dt;
				/* apply accel limits */
				if (new_vel > (old_vel[i] + max_accl * dt)) {
					new_vel = old_vel[i] + max_accl * dt;
				} else if (new_vel < (old_vel[i] - max_accl * dt)) {
					new_vel = old_vel[i] - max_accl * dt;
				}
			}
		} else {
			/* calculate change in final position if we ramp in the
			opposite direction for one period */
			dv = -2.0 * match_accl * dt;
			dp = dv * match_time;
			/* decide which way to ramp */
			if (fabs(est_err + dp * 2.0) < fabs(est_err)) {
				match_accl = -match_accl;
			}
			/* and do it */
			new_vel = old_vel[i] + match_accl * dt;
		}

		/* apply frequency limit */
		if (new_vel > max_vel) {
			new_vel = max_vel;
		} else if (new_vel < -max_vel) {
			new_vel = -max_vel;
		}

		old_vel[i] = new_vel;
		/* calculate new velocity cmd */
		update_velocity(i, (new_vel * VELSCALE));
	}

	/* this is a command (>CM1) */
	txBuf[0] = 0x314D433E;
}

void update_outputs(data_t *dat)
{
	float duty;
	int i;

	/* update pic32 output */
	txBuf[1]  = (*(dat->motor_enable) ? 1l : 0) << 0;
	txBuf[1] |= (*(dat->spindle_enable) ? 1l : 0) << 1;
	txBuf[1] |= (*(dat->coolant_enable) ? 1l : 0) << 2;

	/* update pwm */
	duty = *(dat->pwm_duty) * dat->pwm_scale * 0.01;
	if (duty < 0.0) duty = 0.0;
	if (duty > 1.0) duty = 1.0;

	txBuf[2] = (duty * (1.0 + pwm_period));
}

static s32 debounce(s32 A)
{
	static s32 B = 0;
	static s32 C = 0;
	static s32 Z = 0;

	Z = (Z & (A | B | C)) | (A & B & C);
	C = B;
	B = A;

	return Z;
}

void update_inputs(data_t *dat)
{
	int i;
	s32 x;

	x = debounce(rxBuf[1]);

	*(dat->abort)  = (x & 0b0000001) ? 1 : 0;
	*(dat->hold)   = (x & 0b0000010) ? 1 : 0;
	*(dat->resume) = (x & 0b0000100) ? 1 : 0;
	*(dat->lim_x)  = (x & 0b0001000) ? 1 : 0;
	*(dat->lim_y)  = (x & 0b0010000) ? 1 : 0;
	*(dat->lim_z)  = (x & 0b0100000) ? 1 : 0;
}

void read_buf()
{
	char *buf;
	int i;

	/* wait until transfer is finished */
	while ((SPI2_CTL & SPI_CTL_XCH));

	/* read buffer */
	buf = (char *)rxBuf;
	for (i=0; i<SPIBUFSIZE; i++) {
		*buf++ = SPI2_RXDATA;
	}
}

void write_buf()
{
	char *buf;
	int i;

	/* send txBuf */
	buf = (char *)txBuf;
	for (i=0; i<SPIBUFSIZE; i++) {
		SPI2_TXDATA = *buf++;
	}

	/* update transmit len */
	SPI2_BC = SPIBUFSIZE;
	SPI2_TC = SPIBUFSIZE;

	/* start transmit */
	SPI2_CTL |= SPI_CTL_XCH;
}

int map_gpio()
{
	int fd;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,"%s: can't open /dev/mem \n",modname);
		return -1;
	}

	/* mmap GPIO */
	gpio = mmap(
	        NULL,
	        BLOCK_SIZE,
	        PROT_READ|PROT_WRITE,
	        MAP_SHARED,
	        fd,
	        SUNXI_PIO_BASE);

	if (gpio == MAP_FAILED) {
		rtapi_print_msg(RTAPI_MSG_ERR,"%s: can't map gpio\n",modname);
		close(fd);
		return -1;
	}

	/* mmap SPI */
	spi = mmap(
	        NULL,
	        BLOCK_SIZE,
	        PROT_READ|PROT_WRITE,
	        MAP_SHARED,
	        fd,
	        SUNXI_SPI2_BASE);

	close(fd);

	if (spi == MAP_FAILED) {
		rtapi_print_msg(RTAPI_MSG_ERR,"%s: can't map spi\n",modname);
		return -1;
	}

	return 0;
}

/*    GPIO USAGE
 *
 *	GPIO	Dir	Signal		Note
 *
 *	9	IN	MISO		SPI
 *	10	OUT	MOSI		SPI
 *	11	OUT	SCLK		SPI
 *
 */

void setup_gpio()
{
	u32 x;

	/* set pins to SPI, PE0 - PE3 */
	x = SUNXI_PE_CFG0;
	x &= ~(0xffff);
	x |= 0x4444;
	SUNXI_PE_CFG0 = x;

	/* setup CCM clock and enable gating */
	SUNXI_CCM_SPI2_CLK_CFG = 0x82000003;	/* AHB_CLK = 102MHz */
	SUNXI_CCMU_AHB_GATE0 |= (1<<22);

	/* reset SPI module */
	SPI2_CTL = 0;
	SPI2_INT_CTL = 0;
	SPI2_STATUS = ~0;
	SPI2_DMA_CTL = 0;
	SPI2_WAIT = 0;
	SPI2_BC = 0;
	SPI2_TC = 0;

	/* clear fifos */
	SPI2_CTL |= (SPI_CTL_RST_TXFIFO | SPI_CTL_RST_RXFIFO);

	/* set SPI clk */
	SPI2_CLK_RATE = SPICLKRATE;
	
	/* pause when RX full, SSCTL, SSPOL, 0=POL, 0=PHA, MASTER 
	   enable SPI */
	SPI2_CTL = SPI_CTL_T_PAUSE_EN | SPI_CTL_SSCTL | SPI_CTL_SSPOL | 
		SPI_CTL_FUNC_MODE | SPI_CTL_EN;
}

void restore_gpio()
{
	u32 x;

	/* set PE0 - PE3 pins to inputs */
	x = SUNXI_PE_CFG0;
	x &= ~(0xffff);
	SUNXI_PE_CFG0 = x;
}

