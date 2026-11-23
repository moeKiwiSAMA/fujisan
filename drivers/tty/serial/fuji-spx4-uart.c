// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multi-instance serial console, earlycon, and Bluetooth HCI UART driver
 * for Fujisan SPX4 PL011-compatible UART cluster:
 *   - UART0 (console / ttyFUJI0): 0xFFF3A000 (GIC SPI 254)
 *   - UART1 (aux     / ttyFUJI1): 0xFFF39000 (GIC SPI 253)
 *   - UART2 (btuart  / ttyFUJI2): 0xFFF3B000 (GIC SPI 255, BCM43438A1 H4)
 *
 * Strictly accesses ONLY verified safe PL011-compatible registers:
 *   +0x00 UARTDR (RW4) Data register (RX read / TX write)
 *   +0x18 UARTFR (RO4) Flag register: bit 4 = RXFE, bit 5 = TXFF
 *
 * Never touches unmodeled PrimeCell ID (+0xFE0..0xFFC), guaranteeing clean
 * operation on both real SPX4-MF silicon and spx4-emulator.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define SPX4_UART_MAX_PORTS	3
#define SPX4_UART_DR		0x00
#define SPX4_UART_FR		0x18
#define SPX4_UART_FR_RXFE	BIT(4)
#define SPX4_UART_FR_TXFF	BIT(5)
#define SPX4_UART_TX_SPINS	10000

struct spx4_uart_dev {
	struct uart_port port;
	bool rx_enabled;
};

static struct spx4_uart_dev *spx4_ports[SPX4_UART_MAX_PORTS];

static void spx4_uart_putc_raw(void __iomem *base, unsigned char ch)
{
	int spins = SPX4_UART_TX_SPINS;

	while ((readl_relaxed(base + SPX4_UART_FR) & SPX4_UART_FR_TXFF) && --spins > 0)
		cpu_relax();
	writel_relaxed((u32)ch, base + SPX4_UART_DR);
}

/* ---- Early console (earlycon=fujispx4,0xfff3a000) ---- */

static void spx4_early_putc(struct uart_port *port, unsigned char c)
{
	spx4_uart_putc_raw(port->membase, c);
}

static void spx4_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, spx4_early_putc);
}

static int __init spx4_early_setup(struct earlycon_device *device, const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;
	device->con->write = spx4_early_write;
	return 0;
}

EARLYCON_DECLARE(fujispx4, spx4_early_setup);
OF_EARLYCON_DECLARE(fujispx4, "fujisan,spx4-uart", spx4_early_setup);

/* ---- Platform serial driver (ttyFUJI0..2) ---- */

static unsigned int spx4_tx_empty(struct uart_port *port)
{
	return (readl_relaxed(port->membase + SPX4_UART_FR) & SPX4_UART_FR_TXFF)
		? 0 : TIOCSER_TEMT;
}

static void spx4_set_mctrl(struct uart_port *port, unsigned int mctrl) { }
static unsigned int spx4_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}
static void spx4_stop_tx(struct uart_port *port) { }
static void spx4_stop_rx(struct uart_port *port) { }
static void spx4_break_ctl(struct uart_port *port, int ctl) { }

static irqreturn_t spx4_uart_irq(int irq, void *dev_id)
{
	struct spx4_uart_dev *udev = dev_id;
	struct uart_port *port = &udev->port;
	unsigned long flags;
	int budget = 64;

	if (!udev->rx_enabled)
		return IRQ_NONE;

	uart_port_lock_irqsave(port, &flags);
	while (budget-- > 0 && !(readl_relaxed(port->membase + SPX4_UART_FR) & SPX4_UART_FR_RXFE)) {
		u32 ch = readl_relaxed(port->membase + SPX4_UART_DR) & 0xFF;

		port->icount.rx++;
		uart_insert_char(port, 0, 0, ch, TTY_NORMAL);
	}
	uart_port_unlock_irqrestore(port, flags);

	tty_flip_buffer_push(&port->state->port);
	return IRQ_HANDLED;
}

static int spx4_startup(struct uart_port *port)
{
	struct spx4_uart_dev *udev = container_of(port, struct spx4_uart_dev, port);

	if (udev->rx_enabled && port->irq > 0)
		return request_irq(port->irq, spx4_uart_irq, IRQF_SHARED,
				   "fuji-spx4-uart", udev);
	return 0;
}

static void spx4_shutdown(struct uart_port *port)
{
	struct spx4_uart_dev *udev = container_of(port, struct spx4_uart_dev, port);

	if (udev->rx_enabled && port->irq > 0)
		free_irq(port->irq, udev);
}

static void spx4_set_termios(struct uart_port *port, struct ktermios *new,
			     const struct ktermios *old)
{
	unsigned int baud = uart_get_baud_rate(port, new, old, 9600, 1500000);

	uart_update_timeout(port, new->c_cflag, baud);
}

static const char *spx4_type(struct uart_port *port) { return "FUJI_SPX4"; }
static void spx4_release_port(struct uart_port *port) { }
static int spx4_request_port(struct uart_port *port) { return 0; }
static void spx4_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_16550A;
}

static void spx4_start_tx(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;

	while (!uart_circ_empty(xmit)) {
		spx4_uart_putc_raw(port->membase, xmit->buf[xmit->tail]);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
	}
	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static const struct uart_ops spx4_uart_ops = {
	.tx_empty	= spx4_tx_empty,
	.set_mctrl	= spx4_set_mctrl,
	.get_mctrl	= spx4_get_mctrl,
	.stop_tx	= spx4_stop_tx,
	.start_tx	= spx4_start_tx,
	.stop_rx	= spx4_stop_rx,
	.break_ctl	= spx4_break_ctl,
	.startup	= spx4_startup,
	.shutdown	= spx4_shutdown,
	.set_termios	= spx4_set_termios,
	.type		= spx4_type,
	.release_port	= spx4_release_port,
	.request_port	= spx4_request_port,
	.config_port	= spx4_config_port,
};

static void spx4_console_putchar(struct uart_port *port, unsigned char ch)
{
	spx4_uart_putc_raw(port->membase, ch);
}

static void spx4_console_write(struct console *co, const char *s, unsigned int count)
{
	struct spx4_uart_dev *udev;

	if (co->index < 0 || co->index >= SPX4_UART_MAX_PORTS)
		return;
	udev = spx4_ports[co->index];
	if (!udev || !udev->port.membase)
		return;
	uart_console_write(&udev->port, s, count, spx4_console_putchar);
}

static struct uart_driver spx4_uart_driver;

static struct console spx4_console = {
	.name	= "ttyFUJI",
	.write	= spx4_console_write,
	.device	= uart_console_device,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &spx4_uart_driver,
};

static struct uart_driver spx4_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "fuji_spx4_uart",
	.dev_name	= "ttyFUJI",
	.major		= 204,
	.minor		= 64,
	.nr		= SPX4_UART_MAX_PORTS,
	.cons		= &spx4_console,
};

static int spx4_uart_probe(struct platform_device *pdev)
{
	struct spx4_uart_dev *udev;
	struct resource *res;
	void __iomem *base;
	int line, irq, ret;
	u32 clk = 54000000;

	line = of_alias_get_id(pdev->dev.of_node, "serial");
	if (line < 0 || line >= SPX4_UART_MAX_PORTS)
		line = 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	udev = devm_kzalloc(&pdev->dev, sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	of_property_read_u32(pdev->dev.of_node, "clock-frequency", &clk);
	udev->rx_enabled = of_property_read_bool(pdev->dev.of_node, "fujisan,enable-rx");

	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0)
		irq = 0;

	udev->port.dev = &pdev->dev;
	udev->port.iotype = UPIO_MEM32;
	udev->port.mapbase = res->start;
	udev->port.membase = base;
	udev->port.ops = &spx4_uart_ops;
	udev->port.fifosize = 16;
	udev->port.line = line;
	udev->port.irq = irq;
	udev->port.uartclk = clk;
	udev->port.flags = UPF_BOOT_AUTOCONF;

	spx4_ports[line] = udev;
	platform_set_drvdata(pdev, udev);

	ret = uart_add_one_port(&spx4_uart_driver, &udev->port);
	if (ret) {
		spx4_ports[line] = NULL;
		return ret;
	}

	dev_info(&pdev->dev, "Fujisan SPX4 UART registered at %pa (ttyFUJI%d, rx=%d)\n",
		 &res->start, line, udev->rx_enabled);
	return 0;
}

static int spx4_uart_remove(struct platform_device *pdev)
{
	struct spx4_uart_dev *udev = platform_get_drvdata(pdev);

	if (udev) {
		uart_remove_one_port(&spx4_uart_driver, &udev->port);
		spx4_ports[udev->port.line] = NULL;
	}
	return 0;
}

static const struct of_device_id spx4_uart_of_match[] = {
	{ .compatible = "fujisan,spx4-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, spx4_uart_of_match);

static struct platform_driver spx4_uart_platform_driver = {
	.probe	= spx4_uart_probe,
	.remove	= spx4_uart_remove,
	.driver	= {
		.name		= "fuji-spx4-uart",
		.of_match_table	= spx4_uart_of_match,
	},
};

static int __init spx4_uart_init(void)
{
	int ret = uart_register_driver(&spx4_uart_driver);

	if (ret)
		return ret;
	ret = platform_driver_register(&spx4_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&spx4_uart_driver);
	return ret;
}
arch_initcall(spx4_uart_init);
