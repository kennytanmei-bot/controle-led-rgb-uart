#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

#define PKT_LEN          6
#define PKT_ID           0xA5   
#define BYTE_TIMEOUT_MS  50     

#ifndef USE_CRC8
#define USE_CRC8 0
#endif

enum { CMD_ON = 1, CMD_OFF, CMD_TOGGLE, CMD_BLINK_1S, CMD_BLINK_2S };


static uint8_t checksum(const uint8_t *p, size_t len)
{
	uint8_t c = 0;

	for (size_t i = 0; i < len; i++) {
		c ^= p[i];
#if USE_CRC8
		for (int k = 0; k < 8; k++) {
			c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
		}
#endif
	}
	return c;
}

/* ---------- LED fisico (PWM ou GPIO) ---------- */

#if DT_NODE_EXISTS(DT_ALIAS(red_pwm_led)) && \
	DT_NODE_EXISTS(DT_ALIAS(green_pwm_led)) && \
	DT_NODE_EXISTS(DT_ALIAS(blue_pwm_led))

#define PWM_PERIOD PWM_MSEC(1)

static const struct pwm_dt_spec pin_r = PWM_DT_SPEC_GET(DT_ALIAS(red_pwm_led));
static const struct pwm_dt_spec pin_g = PWM_DT_SPEC_GET(DT_ALIAS(green_pwm_led));
static const struct pwm_dt_spec pin_b = PWM_DT_SPEC_GET(DT_ALIAS(blue_pwm_led));

static int led_init(void)
{
	return (pwm_is_ready_dt(&pin_r) && pwm_is_ready_dt(&pin_g) &&
		pwm_is_ready_dt(&pin_b)) ? 0 : -ENODEV;
}

static void pwm_level(const struct pwm_dt_spec *pin, uint8_t v)
{
	pwm_set_dt(pin, PWM_PERIOD, (uint32_t)((uint64_t)PWM_PERIOD * v / 255));
}

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
	pwm_level(&pin_r, r);
	pwm_level(&pin_g, g);
	pwm_level(&pin_b, b);
}

#else

static const struct gpio_dt_spec pin_r = GPIO_DT_SPEC_GET(DT_NODELABEL(red_led), gpios);
static const struct gpio_dt_spec pin_g = GPIO_DT_SPEC_GET(DT_NODELABEL(green_led), gpios);
static const struct gpio_dt_spec pin_b = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);

static int led_init(void)
{
	if (!gpio_is_ready_dt(&pin_r) || !gpio_is_ready_dt(&pin_g) ||
	    !gpio_is_ready_dt(&pin_b)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&pin_r, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&pin_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&pin_b, GPIO_OUTPUT_INACTIVE);
	return 0;
}

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
	gpio_pin_set_dt(&pin_r, r != 0);
	gpio_pin_set_dt(&pin_g, g != 0);
	gpio_pin_set_dt(&pin_b, b != 0);
}

#endif


static uint8_t cor_r, cor_g, cor_b;  
static bool aceso;                   
static uint32_t pisca_ms;            

static K_MUTEX_DEFINE(led_mutex);    
static struct k_work_delayable pisca_job;

static void led_refresh(void)        
{
	if (aceso) {
		led_write(cor_r, cor_g, cor_b);
	} else {
		led_write(0, 0, 0);
	}
}

static void pisca_tick(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&led_mutex, K_FOREVER);
	if (pisca_ms != 0) {  
		aceso = !aceso;
		led_refresh();
		k_work_reschedule(&pisca_job, K_MSEC(pisca_ms));
	}
	k_mutex_unlock(&led_mutex);
}

static void run_command(uint8_t cmd, uint8_t r, uint8_t g, uint8_t b)
{
	if (cmd < CMD_ON || cmd > CMD_BLINK_2S) {
		return;  
	}

	k_mutex_lock(&led_mutex, K_FOREVER);

	if (cmd != CMD_OFF) {
		cor_r = r;
		cor_g = g;
		cor_b = b;
	}

	switch (cmd) {
	case CMD_ON:
		aceso = true;
		pisca_ms = 0;
		break;
	case CMD_OFF:
		aceso = false;
		pisca_ms = 0;
		break;
	case CMD_TOGGLE:
		aceso = !aceso;
		pisca_ms = 0;
		break;
	default:  
		aceso = true;
		pisca_ms = (cmd == CMD_BLINK_1S) ? 1000 : 2000;
		break;
	}

	if (pisca_ms != 0) {
		k_work_reschedule(&pisca_job, K_MSEC(pisca_ms));
	} else {
		k_work_cancel_delayable(&pisca_job);
	}
	led_refresh();

	k_mutex_unlock(&led_mutex);
}



static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

K_MSGQ_DEFINE(rx_q, sizeof(uint8_t), 128, 1);

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t byte;

	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}
	uart_err_check(dev);  
	while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &byte, 1) == 1) {
		k_msgq_put(&rx_q, &byte, K_NO_WAIT);  
	}
}

int main(void)
{
	uint8_t pkt[PKT_LEN];
	size_t n = 0;  

	if (!device_is_ready(uart_dev) || led_init() != 0) {
		return 0;
	}

	k_work_init_delayable(&pisca_job, pisca_tick);
	led_write(0, 0, 0);

	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	for (;;) {
		uint8_t byte;
		k_timeout_t wait = (n == 0) ? K_FOREVER : K_MSEC(BYTE_TIMEOUT_MS);

		if (k_msgq_get(&rx_q, &byte, wait) != 0) {
			n = 0;  
			continue;
		}

		if (n == 0 && byte != PKT_ID) {
			continue;  
		}

		pkt[n++] = byte;
		if (n < PKT_LEN) {
			continue;
		}
		n = 0;

		if (pkt[5] == checksum(pkt, 5)) {
			run_command(pkt[4], pkt[1], pkt[2], pkt[3]);
		}
	}

	return 0;
}
