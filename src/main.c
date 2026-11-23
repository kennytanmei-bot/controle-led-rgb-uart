/*
 * Controle de LED RGB pela UART - FRDM-KL25Z, Zephyr RTOS
 *
 * Protocolo (6 bytes):  ID | R | G | B | ACAO | CHECK
 *   ACAO: 0x01 LIGAR, 0x02 DESLIGAR, 0x03 TOGGLE, 0x04 PISCA 1s, 0x05 PISCA 2s
 *   CHECK: XOR dos 5 primeiros bytes (ou CRC-8 poly 0x07 se USE_CRC8=1)
 *
 * Arquitetura:
 *   ISR da UART  -> k_msgq (bytes)  -> thread main (parser com timeout)
 *   parser       -> led_execute()   -> estado do LED (protegido por mutex)
 *   pisca        -> k_work_delayable (workqueue do sistema)
 *
 * Se existirem os aliases red/green/blue-pwm-led no devicetree da placa,
 * R/G/B (0..255) controlam a intensidade via PWM. Caso contrário, cai para
 * GPIO (liga se o valor for > 0).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <string.h>

/* ---------- Protocolo ---------- */

#define PKT_LEN        6
#define PKT_ID         0xA5   /* ajuste para o ID definido pelo enunciado/professor */
#define RX_TIMEOUT_MS  50     /* byte parado por mais que isso => pacote truncado */

#define ACT_ON         0x01
#define ACT_OFF        0x02
#define ACT_TOGGLE     0x03
#define ACT_BLINK_1S   0x04
#define ACT_BLINK_2S   0x05

#ifndef USE_CRC8
#define USE_CRC8 0
#endif

static __maybe_unused uint8_t crc8(const uint8_t *d, size_t n)
{
	uint8_t crc = 0x00;

	for (size_t i = 0; i < n; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static uint8_t xor8(const uint8_t *d, size_t n)
{
	uint8_t x = 0;

	for (size_t i = 0; i < n; i++) {
		x ^= d[i];
	}
	return x;
}

static uint8_t checksum(const uint8_t *d, size_t n)
{
#if USE_CRC8
	return crc8(d, n);
#else
	return xor8(d, n);
#endif
}

/* ---------- LED RGB ---------- */
#if DT_NODE_EXISTS(DT_ALIAS(red_pwm_led)) && \
	DT_NODE_EXISTS(DT_ALIAS(green_pwm_led)) && \
	DT_NODE_EXISTS(DT_ALIAS(blue_pwm_led))
#define LED_USE_PWM 1
static const struct pwm_dt_spec pwm_r = PWM_DT_SPEC_GET(DT_ALIAS(red_pwm_led));
static const struct pwm_dt_spec pwm_g = PWM_DT_SPEC_GET(DT_ALIAS(green_pwm_led));
static const struct pwm_dt_spec pwm_b = PWM_DT_SPEC_GET(DT_ALIAS(blue_pwm_led));
#define PWM_PERIOD_NS PWM_MSEC(1)
#else
#define LED_USE_PWM 0
/* FRDM-KL25Z: red_led (PTB18), green_led (PTB19), blue_led (PTD1) */
static const struct gpio_dt_spec gpio_r = GPIO_DT_SPEC_GET(DT_NODELABEL(red_led), gpios);
static const struct gpio_dt_spec gpio_g = GPIO_DT_SPEC_GET(DT_NODELABEL(green_led), gpios);
static const struct gpio_dt_spec gpio_b = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
#endif

static int led_hw_init(void)
{
#if LED_USE_PWM
	if (!pwm_is_ready_dt(&pwm_r) || !pwm_is_ready_dt(&pwm_g) || !pwm_is_ready_dt(&pwm_b)) {
		return -ENODEV;
	}
	return 0;
#else
	if (!gpio_is_ready_dt(&gpio_r) || !gpio_is_ready_dt(&gpio_g) ||
	    !gpio_is_ready_dt(&gpio_b)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&gpio_r, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&gpio_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&gpio_b, GPIO_OUTPUT_INACTIVE);
	return 0;
#endif
}

static void led_hw_write(uint8_t r, uint8_t g, uint8_t b)
{
#if LED_USE_PWM
	pwm_set_dt(&pwm_r, PWM_PERIOD_NS, (uint32_t)((uint64_t)PWM_PERIOD_NS * r / 255));
	pwm_set_dt(&pwm_g, PWM_PERIOD_NS, (uint32_t)((uint64_t)PWM_PERIOD_NS * g / 255));
	pwm_set_dt(&pwm_b, PWM_PERIOD_NS, (uint32_t)((uint64_t)PWM_PERIOD_NS * b / 255));
#else
	gpio_pin_set_dt(&gpio_r, r > 0);
	gpio_pin_set_dt(&gpio_g, g > 0);
	gpio_pin_set_dt(&gpio_b, b > 0);
#endif
}

/* Estado lógico do LED */
static struct {
	uint8_t r, g, b;
	bool lit;             /* LED visível (aceso) agora */
	uint32_t blink_half;  /* meio período do pisca em ms; 0 = sem pisca */
} led;

static K_MUTEX_DEFINE(led_lock);
static struct k_work_delayable blink_work;

static void led_refresh(void)  /* chamar com led_lock */
{
	if (led.lit) {
		led_hw_write(led.r, led.g, led.b);
	} else {
		led_hw_write(0, 0, 0);
	}
}

static void blink_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&led_lock, K_FOREVER);
	if (led.blink_half != 0) {      /* pode ter sido cancelado enquanto esperava o mutex */
		led.lit = !led.lit;
		led_refresh();
		k_work_reschedule(&blink_work, K_MSEC(led.blink_half));
	}
	k_mutex_unlock(&led_lock);
}

/* Retorna false se a ação for desconhecida */
static bool led_execute(uint8_t r, uint8_t g, uint8_t b, uint8_t action)
{
	bool ok = true;

	k_mutex_lock(&led_lock, K_FOREVER);

	switch (action) {
	case ACT_ON:
		led.blink_half = 0;
		led.r = r; led.g = g; led.b = b;
		led.lit = true;
		break;
	case ACT_OFF:
		led.blink_half = 0;
		led.lit = false;
		break;
	case ACT_TOGGLE:
		led.blink_half = 0;
		led.r = r; led.g = g; led.b = b;
		led.lit = !led.lit;
		break;
	case ACT_BLINK_1S:   /* 1s aceso / 1s apagado */
	case ACT_BLINK_2S:   /* 2s aceso / 2s apagado */
		led.r = r; led.g = g; led.b = b;
		led.lit = true;
		led.blink_half = (action == ACT_BLINK_1S) ? 1000 : 2000;
		k_work_reschedule(&blink_work, K_MSEC(led.blink_half));
		break;
	default:
		ok = false;
		break;
	}

	if (ok) {
		if (led.blink_half == 0) {
			k_work_cancel_delayable(&blink_work);
		}
		led_refresh();
	}

	k_mutex_unlock(&led_lock);
	return ok;
}

/* ---------- UART RX ---------- */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

K_MSGQ_DEFINE(rx_q, sizeof(uint8_t), 128, 1);

static struct {
	uint32_t ok, bad_id, bad_check, bad_action, truncated, overflow;
} stats;

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[16];

	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}
	uart_err_check(dev);  /* limpa overrun/framing para a UART não travar */

	while (uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			if (k_msgq_put(&rx_q, &buf[i], K_NO_WAIT) != 0) {
				stats.overflow++;
			}
		}
	}
}

int main(void)
{
	uint8_t pkt[PKT_LEN];
	size_t n = 0;

	if (!device_is_ready(uart_dev) || led_hw_init() != 0) {
		return 0;
	}

	k_work_init_delayable(&blink_work, blink_handler);
	led_hw_write(0, 0, 0);

	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	for (;;) {
		uint8_t b;
		/* Sem bytes pendentes: espera para sempre. Com pacote parcial: timeout curto. */
		k_timeout_t to = (n == 0) ? K_FOREVER : K_MSEC(RX_TIMEOUT_MS);

		if (k_msgq_get(&rx_q, &b, to) != 0) {
			stats.truncated++;   /* pacote incompleto: descarta e volta a sincronizar */
			n = 0;
			continue;
		}

		pkt[n++] = b;

		/* Valida o ID já no primeiro byte: lixo antes do pacote é ignorado */
		if (n == 1 && pkt[0] != PKT_ID) {
			stats.bad_id++;
			n = 0;
			continue;
		}
		if (n < PKT_LEN) {
			continue;
		}

		if (pkt[5] != checksum(pkt, 5)) {
			stats.bad_check++;
			/* ressincroniza: procura outro ID dentro do que já foi lido */
			size_t i = 1;

			while (i < PKT_LEN && pkt[i] != PKT_ID) {
				i++;
			}
			n = PKT_LEN - i;
			if (n > 0) {
				memmove(pkt, &pkt[i], n);
			}
			continue;
		}

		if (led_execute(pkt[1], pkt[2], pkt[3], pkt[4])) {
			stats.ok++;
		} else {
			stats.bad_action++;
		}
		n = 0;
	}
	return 0;
}  