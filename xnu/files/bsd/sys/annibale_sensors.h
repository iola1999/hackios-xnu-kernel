#ifndef _SYS_ANNIBALE_SENSORS_H_
#define _SYS_ANNIBALE_SENSORS_H_

#include <sys/ioccom.h>
#include <sys/types.h>
#include <stdint.h>

#define ANNIBALE_SENSORS_DEVICE          "/dev/annibale-sensors"

/* Apple Vendor HID usage page (0xFF00) usages for ALS and Proximity */
#define HID_PAGE_APPLE_VENDOR           0xff00
#define HID_USAGE_APPLE_ALS             0x0004
#define HID_USAGE_APPLE_PROXIMITY       0x0005

/* Sensor sample source flags */
#define ANNIBALE_SENSOR_SRC_DEFAULT      (1u << 0)
#define ANNIBALE_SENSOR_SRC_HW_I2C       (1u << 1)
#define ANNIBALE_SENSOR_SRC_ADSP_SSC     (1u << 2)
#define ANNIBALE_SENSOR_SRC_INJECTED     (1u << 3)
#define ANNIBALE_SENSOR_SRC_FRONT_OK     (1u << 4)
#define ANNIBALE_SENSOR_SRC_BACK_OK      (1u << 5)

/* K90 (annibale, SM8750) factory calibration defaults from persist/sensors */
#define ANNIBALE_GLS6151_I2C_ADDR       0x31
#define ANNIBALE_GLS6151_IRQ_GPIO       163
#define ANNIBALE_GLS6151_PROX_OFFSET    585
#define ANNIBALE_GLS6151_PROX_NEAR      1041
#define ANNIBALE_GLS6151_PROX_FAR       747

#define ANNIBALE_STK6A2X_I2C_ADDR       0x47
#define ANNIBALE_STK6A2X_IRQ_GPIO       35

struct annibale_sensor_sample {
	uint64_t timestamp;             /* mach_absolute_time() of latest update */
	uint32_t als_lux;               /* calibrated ambient light illuminance in lux */
	uint32_t clear_raw;             /* front ALS clear channel raw count */
	uint32_t ir_raw;                /* front ALS IR channel raw count */
	uint32_t prox_raw;              /* proximity raw ADC count (crosstalk baseline = 585) */
	uint16_t prox_distance_mm;      /* estimated distance in mm (0 = near, 50 = far) */
	uint8_t  prox_near;             /* 1 = near (detected), 0 = far */
	uint8_t  source_flags;          /* ANNIBALE_SENSOR_SRC_* bitmask */
};

struct annibale_sensors_state {
	struct annibale_sensor_sample sample;
	uint32_t back_als_lux;          /* rear ALS (stk6a2x) illuminance in lux */
	uint32_t prox_crosstalk;        /* proximity crosstalk baseline offset (default 585) */
	uint32_t prox_near_thresh;      /* proximity near threshold (default 1041) */
	uint32_t prox_far_thresh;       /* proximity far threshold (default 747) */
	uint32_t als_scale_milli;       /* ALS calibration multiplier in milli-units (1000 = 1.0x) */
	uint32_t front_irq_gpio;        /* TLMM GPIO 163 pin state */
	uint32_t back_irq_gpio;         /* TLMM GPIO 35 pin state */
	uint32_t poll_count;            /* cumulative sensor poll/update count */
};

#define ANNIBALE_SENSORS_GET_STATE       _IOR('S', 1, struct annibale_sensors_state)
#define ANNIBALE_SENSORS_SET_STATE       _IOW('S', 2, struct annibale_sensor_sample)
#define ANNIBALE_SENSORS_CALIBRATE       _IOWR('S', 3, struct annibale_sensors_state)

#ifdef KERNEL
void annibale_sensors_init(void);
void annibale_sensors_report(uint32_t als_lux, uint32_t clear_raw, uint32_t ir_raw,
    uint32_t prox_raw, uint8_t prox_near, uint8_t source_flags);
#endif

#endif /* _SYS_ANNIBALE_SENSORS_H_ */
