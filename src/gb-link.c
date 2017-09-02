#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/cm3/nvic.h>

#include "usart.h"

#define MODE_SNIFF 's'
#define MODE_SLAVE 'b'
#define SLAVE_PRINTER 'p'


/* STM32F411-Nucleo at 96 MHz */
const struct rcc_clock_scale rcc_hse_8mhz_3v3_96mhz = {
	.pllm = 8,
	.plln = 384,
	.pllp = 4,
	.pllq = 8,
	.pllr = 0,
	.hpre = RCC_CFGR_HPRE_DIV_NONE,
	.ppre1 = RCC_CFGR_PPRE_DIV_2,
	.ppre2 = RCC_CFGR_PPRE_DIV_NONE,
	.power_save = 1,
	.flash_config = FLASH_ACR_ICEN | FLASH_ACR_DCEN |
		FLASH_ACR_LATENCY_3WS,
	.ahb_frequency  = 96000000,
	.apb1_frequency = 48000000,
	.apb2_frequency = 96000000,
};

static void
clock_setup(void)
{
	rcc_clock_setup_hse_3v3(&rcc_hse_8mhz_3v3_96mhz);
	//rcc_clock_setup_hse_3v3(&rcc_hse_8mhz_3v3[RCC_CLOCK_3V3_84MHZ]);

	/* Enable GPIOA clock for LED & USARTs. */
	rcc_periph_clock_enable(RCC_GPIOA);

	/* Enable GPIOC for game link pins. */
	rcc_periph_clock_enable(RCC_GPIOC);

	/* Enable clocks for USART2. */
	rcc_periph_clock_enable(RCC_USART2);

	/* Enable DMA1 clock */
	rcc_periph_clock_enable(RCC_DMA1);
}

static void
gpio_setup(void)
{
	/* Setup GPIO pin GPIO5 on GPIO port A for LED. */
	gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO5);

	/* Setup GPIO pins for USART2 transmit. */
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO2 | GPIO3);

	/* Setup USART2 TX/RX pin as alternate function. */
	gpio_set_af(GPIOA, GPIO_AF7, GPIO2 | GPIO3);
}

volatile int dma_sent = 0;

void
dma1_stream6_isr(void)
{
	if (dma_get_interrupt_flag(DMA1, DMA_STREAM6, DMA_TCIF)) {
	        // Clear Transfer Complete Interrupt Flag
		dma_clear_interrupt_flags(DMA1, DMA_STREAM6, DMA_TCIF);
		dma_sent = 1;
	}

	dma_disable_transfer_complete_interrupt(DMA1, DMA_STREAM6);
	usart_disable_tx_dma(USART2);
	dma_disable_stream(DMA1, DMA_STREAM6);
}

volatile int dma_recvd = 0;

void
dma1_stream5_isr(void)
{
	if (dma_get_interrupt_flag(DMA1, DMA_STREAM5, DMA_TCIF)) {
	        // Clear Transfer Complete Interrupt Flag
		dma_clear_interrupt_flags(DMA1, DMA_STREAM5, DMA_TCIF);
		dma_recvd = 1;
	}

	dma_disable_transfer_complete_interrupt(DMA1, DMA_STREAM5);
	usart_disable_rx_dma(USART2);
	dma_disable_stream(DMA1, DMA_STREAM5);
}

static inline void
delay_nop(unsigned int t)
{
	unsigned int i;
	for (i = 0; i < t; i++) { /* Wait a bit. */
		__asm__("nop");
	}
}

static void
gblink_sniff_gpio_setup(void)
{
	// PA0 -> SCK
	gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO0);
	// PC0 -> SIN
	gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO0);
	// PC1 -> SOUT
	gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO1);
	// PC2 -> SD
	gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO2);

	nvic_set_priority(NVIC_EXTI0_IRQ, 0);
	nvic_enable_irq(NVIC_EXTI0_IRQ);

	exti_select_source(EXTI0, GPIOA);
	//exti_set_trigger(EXTI0, EXTI_TRIGGER_FALLING);
	exti_set_trigger(EXTI0, EXTI_TRIGGER_RISING);
	exti_enable_request(EXTI0);
}

static void
gblink_slave_gpio_setup(void)
{
	// PA0 -> SCK
	gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO0);
	// PC0 -> SIN
	gpio_mode_setup(GPIOC, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLDOWN, GPIO0);
	gpio_set_output_options(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_100MHZ, GPIO0);
	gpio_clear(GPIOC, GPIO0);
	// PC1 -> SOUT
	gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO1);
	// PC2 -> SD
	//gpio_mode_setup(GPIOC, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLDOWN, GPIO2);
	//gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, GPIO2);

	//gpio_set(GPIOC, GPIO2);

	nvic_set_priority(NVIC_EXTI0_IRQ, 0);
	nvic_enable_irq(NVIC_EXTI0_IRQ);
	nvic_set_priority(NVIC_USART2_IRQ, 1);
	nvic_enable_irq(NVIC_USART2_IRQ);

	exti_select_source(EXTI0, GPIOA);
	exti_set_trigger(EXTI0, EXTI_TRIGGER_BOTH);
	exti_enable_request(EXTI0);

	usart_enable_rx_interrupt(USART2);
}

#define BUF_LEN 1024

struct circular_buf {
	uint8_t buf[BUF_LEN];
	uint32_t head;
	uint32_t tail;
	uint32_t len;
};

volatile uint8_t mode;
volatile uint8_t slave_mode;

volatile uint8_t gb_sin, gb_sout;
volatile uint8_t gb_bit;

struct circular_buf recv_buf;

//uint8_t recv_buf[RECV_BUF_LEN];
//volatile uint32_t recv_buf_head;
//volatile uint32_t recv_buf_tail;

static inline void
buf_push(struct circular_buf *buf, uint8_t b) {
	buf->buf[buf->head] = b;
	buf->head = (buf->head + 1) % BUF_LEN;
}

static inline uint8_t
buf_pop(struct circular_buf *buf)
{
	uint8_t b = buf->buf[buf->tail];
	buf->tail = (buf->tail + 1) % BUF_LEN;
	return b;
}

static inline void
buf_clear(struct circular_buf *buf)
{
	buf->head = 0;
	buf->tail = 0;
}

static inline int
buf_empty(struct circular_buf *buf)
{
	return (buf->tail == buf->head);
}

void
usart2_isr(void)
{
	uint8_t empty;
	/* Check if we were called because of RXNE. */
	if (((USART_CR1(USART2) & USART_CR1_RXNEIE) != 0) &&
	    ((USART_SR(USART2) & USART_SR_RXNE) != 0)) {
		empty = buf_empty(&recv_buf);

		buf_push(&recv_buf, usart_recv(USART2));

		if (empty && gb_bit == 0 ) {
			gb_sin = buf_pop(&recv_buf);
		}
	}
}

const char printer_magic[] = {0x88, 0x33};

enum printer_state {MAGIC0, MAGIC1, CMD, ARG0, LEN_LOW, LEN_HIGH, DATA, CHECKSUM0, CHECKSUM1, ACK, STATUS};
enum printer_state printer_state;
uint16_t printer_data_len;


static void
printer_update_state(uint8_t b)
{
	switch (printer_state) {
	case MAGIC0:
		if (b == printer_magic[0]) {
			printer_state = MAGIC1;
		}
		break;
	case MAGIC1:
		if (b == printer_magic[1]) {
			printer_state = CMD;
		} else {
			printer_state = MAGIC0;
		}
		break;
	case CMD:
		printer_state = ARG0;
		break;
	case ARG0:
		printer_state = LEN_LOW;
		break;
	case LEN_LOW:
		printer_data_len = b;
		printer_state = LEN_HIGH;
		break;
	case LEN_HIGH:
		printer_data_len |= b << 8;
		if (printer_data_len != 0) {
			printer_state = DATA;
		} else {
			printer_state = CHECKSUM0;
		}
		break;
	case DATA:
		printer_data_len--;
		printer_state = (printer_data_len == 0) ? CHECKSUM0 : DATA;
		break;
	case CHECKSUM0:
		printer_state = CHECKSUM1;
		break;
	case CHECKSUM1:
		buf_push(&recv_buf, 0x81);
		printer_state = ACK;
		break;
	case ACK:
		buf_push(&recv_buf, 0x00);
		printer_state = STATUS;
		break;
	case STATUS:
		printer_state = MAGIC0;
		break;
	}
}

static void
printer_reset_state(void)
{
	printer_data_len = 0;
	printer_state = MAGIC0;
}

inline static void
exti0_isr_sniff(void)
{
	//delay_nop(1000);
	gb_sin |= gpio_get(GPIOC, GPIO0) ? 1 : 0;
	gb_sout |= gpio_get(GPIOC, GPIO1) ? 1 : 0;
	gb_bit++;

	if (gb_bit == 8) {
		// Send gb_sin and gb_sout over USART2
		usart_send_blocking(USART2, gb_sin);
		usart_send_blocking(USART2, gb_sout);

		// Reset state
		gb_bit = 0;
		gb_sin = 0;
		gb_sout = 0;
	} else {
		gb_sin <<= 1;
		gb_sout <<= 1;
	}
}

inline static void
exti0_isr_slave(void)
{
	if (gpio_get(GPIOA, GPIO0) == 0) { // FALLING
		gb_sout |= gpio_get(GPIOC, GPIO1) ? 1 : 0;
		gb_bit++;

		if (gb_bit == 8) {
			// Send gb_sin and gb_sout over USART2
			usart_send_blocking(USART2, gb_sout);

			switch(slave_mode) {
			case SLAVE_PRINTER:
				printer_update_state(gb_sout);
				break;
			default:
				break;
			}

			// Reset state
			gb_bit = 0;
			gb_sout = 0;

			// Prepare next gb_sin
			if (buf_empty(&recv_buf)) {
				gb_sin = 0x00;
			} else {
				gb_sin = buf_pop(&recv_buf);
			}
		} else {
			gb_sin <<= 1;
			gb_sout <<= 1;
		}
	} else { // RISING
		(gb_sin & 0x80) ? gpio_set(GPIOC, GPIO0) : gpio_clear(GPIOC, GPIO0);
	}
}

void
exti0_isr(void)
{
	// NOTE: If this goes at the end of the function, things no longer work!
	exti_reset_request(EXTI0);
	switch (mode) {
	case 's':
		exti0_isr_sniff();
		break;
	case 'b':
		exti0_isr_slave();
		break;
	default:
		break;
	}

	//gpio_toggle(GPIOA, GPIO5); /* LED on/off */
}


int
main(void)
{
	uint8_t opt;

	mode = 'x';
	slave_mode = 'x';
	//recv_buf_head = 0;
	//recv_buf_tail = 0;
	buf_clear(&recv_buf);

	clock_setup();
	gpio_setup();
	//usart_setup(115200);
	//usart_setup(1152000);
	usart_setup(1000000);
	usart_send_dma_setup();
	usart_recv_dma_setup();

	usart_recv(USART2); // Clear initial garbage
	usart_send_srt_blocking("\nHELLO\n");

	while (1) {
		opt = usart_recv_blocking(USART2);
		switch (opt) {
		case MODE_SNIFF:
			mode = MODE_SNIFF;
			gblink_sniff_gpio_setup();
			while (1);
			break;
		case SLAVE_PRINTER:
			mode = MODE_SLAVE;
			slave_mode = SLAVE_PRINTER;
			printer_reset_state();
			gblink_slave_gpio_setup();
			while (1);
			break;
		case MODE_SLAVE:
			mode = MODE_SLAVE;
			gblink_slave_gpio_setup();
			while (1);
			break;
		default:
			break;
		}
	}

	while (1) {

	}

	return 0;
}
