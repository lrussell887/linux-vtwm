// SPDX-License-Identifier: GPL-2.0-only
/*
 * WonderMedia WM8505 KBDC Driver
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serio.h>
#include <linux/slab.h>

/* Hardware Register Offsets */
#define WM8505_KBDC_DATA	0x0
#define WM8505_KBDC_CMD_STAT	0x4

/* Status Register Bits */
#define KBDC_STR_OBF		BIT(0)
#define KBDC_STR_IBF		BIT(1)
#define KBDC_STR_AUXDATA	BIT(5)

/* 8042 Command Bytes */
#define KBDC_CMD_READ_CTR	0x20
#define KBDC_CMD_WRITE_CTR	0x60
#define KBDC_CMD_SELF_TEST	0xaa
#define KBDC_CMD_AUX_SEND	0xd4

/* 8042 Expected Responses */
#define KBDC_RET_SELF_TEST	0x55

/* 8042 Control Register (CTR) Bits */
#define KBDC_CTR_KBDINT		BIT(0)
#define KBDC_CTR_AUXINT		BIT(1)
#define KBDC_CTR_KBDDIS		BIT(4)
#define KBDC_CTR_AUXDIS		BIT(5)
#define KBDC_CTR_XLATE		BIT(6)

struct wm8505_kbdc {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct serio *kbd_port;
	struct serio *aux_port;
	spinlock_t lock;
	u8 ctr; /* Cached control register */
};

/**
 * wm8505_kbdc_wait_ibf - Wait for input buffer empty
 */
static int wm8505_kbdc_wait_ibf(struct wm8505_kbdc *kbdc)
{
	u32 val;

	return readl_poll_timeout_atomic(kbdc->base + WM8505_KBDC_CMD_STAT, val,
					 !(val & KBDC_STR_IBF), 50, 100000);
}

/**
 * wm8505_kbdc_wait_obf - Wait for output buffer full
 */
static int wm8505_kbdc_wait_obf(struct wm8505_kbdc *kbdc)
{
	u32 val;

	return readl_poll_timeout_atomic(kbdc->base + WM8505_KBDC_CMD_STAT, val,
					 (val & KBDC_STR_OBF), 50, 100000);
}

/**
 * wm8505_kbdc_flush - Flush the data register
 */
static void wm8505_kbdc_flush(struct wm8505_kbdc *kbdc)
{
	int i;

	for (i = 0; i < 16; i++) {
		if (!(readl(kbdc->base + WM8505_KBDC_CMD_STAT) & KBDC_STR_OBF))
			break;
		readl(kbdc->base + WM8505_KBDC_DATA);
		udelay(50);
	}
}

/**
 * wm8505_kbdc_write_ctr - Write to the control register
 */
static int wm8505_kbdc_write_ctr(struct wm8505_kbdc *kbdc, u8 ctr)
{
	if (wm8505_kbdc_wait_ibf(kbdc))
		return -ETIMEDOUT;
	writeb(KBDC_CMD_WRITE_CTR, kbdc->base + WM8505_KBDC_CMD_STAT);

	if (wm8505_kbdc_wait_ibf(kbdc))
		return -ETIMEDOUT;
	writeb(ctr, kbdc->base + WM8505_KBDC_DATA);

	kbdc->ctr = ctr;
	return 0;
}

/**
 * wm8505_kbdc_hw_init - Initialize hardware state
 */
static int wm8505_kbdc_hw_init(struct wm8505_kbdc *kbdc)
{
	wm8505_kbdc_flush(kbdc);

	/* Controller self-test */
	if (wm8505_kbdc_wait_ibf(kbdc))
		return -ETIMEDOUT;
	writeb(KBDC_CMD_SELF_TEST, kbdc->base + WM8505_KBDC_CMD_STAT);

	if (wm8505_kbdc_wait_obf(kbdc))
		return -ETIMEDOUT;

	if ((readl(kbdc->base + WM8505_KBDC_DATA) & 0xff) != KBDC_RET_SELF_TEST)
		dev_warn(kbdc->dev, "controller selftest failed\n");

	/* Read current control register state from hardware */
	if (wm8505_kbdc_wait_ibf(kbdc))
		return -ETIMEDOUT;
	writeb(KBDC_CMD_READ_CTR, kbdc->base + WM8505_KBDC_CMD_STAT);

	if (wm8505_kbdc_wait_obf(kbdc))
		return -ETIMEDOUT;
	kbdc->ctr = readl(kbdc->base + WM8505_KBDC_DATA) & 0xff;

	/* Turn off interrupts initially and disable both ports */
	kbdc->ctr |= KBDC_CTR_KBDDIS | KBDC_CTR_AUXDIS | KBDC_CTR_XLATE;
	kbdc->ctr &= ~(KBDC_CTR_KBDINT | KBDC_CTR_AUXINT);

	return wm8505_kbdc_write_ctr(kbdc, kbdc->ctr);
}

/* Forward declaration for interrupt kick */
static irqreturn_t wm8505_kbdc_interrupt(int irq, void *dev_id);

/**
 * wm8505_kbdc_open - Open a serio port
 */
static int wm8505_kbdc_open(struct serio *port)
{
	struct wm8505_kbdc *kbdc = port->port_data;
	unsigned long flags;
	u8 ctr;
	int ret;

	spin_lock_irqsave(&kbdc->lock, flags);
	wm8505_kbdc_flush(kbdc);

	/* Enable port and hardware interrupts in the CTR */
	ctr = kbdc->ctr;
	if (port == kbdc->kbd_port) {
		ctr &= ~KBDC_CTR_KBDDIS;
		ctr |= KBDC_CTR_KBDINT;
	} else {
		ctr &= ~KBDC_CTR_AUXDIS;
		ctr |= KBDC_CTR_AUXINT;
	}

	ret = wm8505_kbdc_write_ctr(kbdc, ctr);
	spin_unlock_irqrestore(&kbdc->lock, flags);

	/* Kick the interrupt handler to pop any pending bytes out of the FIFO */
	if (ret == 0)
		wm8505_kbdc_interrupt(0, kbdc);

	return ret;
}

/**
 * wm8505_kbdc_close - Close a serio port
 */
static void wm8505_kbdc_close(struct serio *port)
{
	struct wm8505_kbdc *kbdc = port->port_data;
	unsigned long flags;
	u8 ctr;

	spin_lock_irqsave(&kbdc->lock, flags);

	/* Disable port and hardware interrupts */
	ctr = kbdc->ctr;
	if (port == kbdc->kbd_port) {
		ctr |= KBDC_CTR_KBDDIS;
		ctr &= ~KBDC_CTR_KBDINT;
	} else {
		ctr |= KBDC_CTR_AUXDIS;
		ctr &= ~KBDC_CTR_AUXINT;
	}

	wm8505_kbdc_write_ctr(kbdc, ctr);
	spin_unlock_irqrestore(&kbdc->lock, flags);
}

/**
 * wm8505_kbdc_kbd_write - Write to the keyboard port
 */
static int wm8505_kbdc_kbd_write(struct serio *port, unsigned char c)
{
	struct wm8505_kbdc *kbdc = port->port_data;
	unsigned long flags;
	int ret = -ETIMEDOUT;

	spin_lock_irqsave(&kbdc->lock, flags);
	if (wm8505_kbdc_wait_ibf(kbdc) == 0) {
		writeb(c, kbdc->base + WM8505_KBDC_DATA);
		ret = 0;
	}
	spin_unlock_irqrestore(&kbdc->lock, flags);

	return ret;
}

/**
 * wm8505_kbdc_aux_write - Write to the auxiliary port
 */
static int wm8505_kbdc_aux_write(struct serio *port, unsigned char c)
{
	struct wm8505_kbdc *kbdc = port->port_data;
	unsigned long flags;
	int ret = -ETIMEDOUT;

	spin_lock_irqsave(&kbdc->lock, flags);
	if (wm8505_kbdc_wait_ibf(kbdc) == 0) {
		writeb(KBDC_CMD_AUX_SEND, kbdc->base + WM8505_KBDC_CMD_STAT);
		if (wm8505_kbdc_wait_ibf(kbdc) == 0) {
			writeb(c, kbdc->base + WM8505_KBDC_DATA);
			ret = 0;
		}
	}
	spin_unlock_irqrestore(&kbdc->lock, flags);

	return ret;
}

/**
 * wm8505_kbdc_interrupt - Interrupt handler
 */
static irqreturn_t wm8505_kbdc_interrupt(int irq, void *dev_id)
{
	struct wm8505_kbdc *kbdc = dev_id;
	unsigned long flags;
	u32 status;
	u8 data;

	spin_lock_irqsave(&kbdc->lock, flags);
	status = readl(kbdc->base + WM8505_KBDC_CMD_STAT);

	if (!(status & KBDC_STR_OBF)) {
		spin_unlock_irqrestore(&kbdc->lock, flags);
		return IRQ_NONE;
	}

	data = readl(kbdc->base + WM8505_KBDC_DATA) & 0xff;
	spin_unlock_irqrestore(&kbdc->lock, flags);

	/* Ignore spurious error flags to prevent valid packet loss */
	if (status & KBDC_STR_AUXDATA)
		serio_interrupt(kbdc->aux_port, data, 0);
	else
		serio_interrupt(kbdc->kbd_port, data, 0);

	return IRQ_HANDLED;
}

/**
 * wm8505_kbdc_allocate_port - Allocate and configure a serio port
 */
static struct serio *wm8505_kbdc_allocate_port(struct wm8505_kbdc *kbdc,
					       const char *name,
					       const char *phys,
					       int type,
					       int (*write_fn)(struct serio *, unsigned char))
{
	struct serio *port;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return NULL;

	port->id.type = type;
	port->write = write_fn;
	port->open = wm8505_kbdc_open;
	port->close = wm8505_kbdc_close;
	port->port_data = kbdc;
	port->dev.parent = kbdc->dev;
	strscpy(port->name, name, sizeof(port->name));
	strscpy(port->phys, phys, sizeof(port->phys));

	return port;
}

/**
 * wm8505_kbdc_probe - Probe platform device
 */
static int wm8505_kbdc_probe(struct platform_device *pdev)
{
	struct wm8505_kbdc *kbdc;
	int kbd_irq, aux_irq;
	int error;

	kbdc = devm_kzalloc(&pdev->dev, sizeof(*kbdc), GFP_KERNEL);
	if (!kbdc)
		return -ENOMEM;

	spin_lock_init(&kbdc->lock);
	kbdc->dev = &pdev->dev;

	kbdc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(kbdc->base))
		return PTR_ERR(kbdc->base);

	kbd_irq = platform_get_irq_byname(pdev, "kbd");
	if (kbd_irq < 0)
		return kbd_irq;

	aux_irq = platform_get_irq_byname(pdev, "aux");
	if (aux_irq < 0)
		return aux_irq;

	kbdc->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(kbdc->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(kbdc->clk), "Failed to get clock\n");

	/* Reset the 8042 state machine and CTR */
	error = wm8505_kbdc_hw_init(kbdc);
	if (error)
		return dev_err_probe(&pdev->dev, error, "Hardware initialization failed\n");

	/* KBD port uses SERIO_8042_XL for hardware scancode translation */
	kbdc->kbd_port = wm8505_kbdc_allocate_port(kbdc, "WM8505 KBD Port", "wm8505/serio0",
						   SERIO_8042_XL, wm8505_kbdc_kbd_write);
	kbdc->aux_port = wm8505_kbdc_allocate_port(kbdc, "WM8505 AUX Port", "wm8505/serio1",
						   SERIO_8042, wm8505_kbdc_aux_write);

	if (!kbdc->kbd_port || !kbdc->aux_port) {
		kfree(kbdc->kbd_port);
		kfree(kbdc->aux_port);
		return -ENOMEM;
	}

	/* Register handler for the Keyboard IRQ */
	error = devm_request_irq(&pdev->dev, kbd_irq, wm8505_kbdc_interrupt,
				 IRQF_SHARED, "wm8505-kbd", kbdc);
	if (error) {
		kfree(kbdc->kbd_port);
		kfree(kbdc->aux_port);
		return dev_err_probe(&pdev->dev, error, "Failed to request KBD IRQ\n");
	}

	/* Register handler for the Auxiliary (Touchpad) IRQ */
	error = devm_request_irq(&pdev->dev, aux_irq, wm8505_kbdc_interrupt,
				 IRQF_SHARED, "wm8505-aux", kbdc);
	if (error) {
		kfree(kbdc->kbd_port);
		kfree(kbdc->aux_port);
		return dev_err_probe(&pdev->dev, error, "Failed to request AUX IRQ\n");
	}

	serio_register_port(kbdc->kbd_port);
	serio_register_port(kbdc->aux_port);

	platform_set_drvdata(pdev, kbdc);
	return 0;
}

/**
 * wm8505_kbdc_remove - Remove platform device
 */
static void wm8505_kbdc_remove(struct platform_device *pdev)
{
	struct wm8505_kbdc *kbdc = platform_get_drvdata(pdev);

	/* Mask hardware interrupts before tearing down */
	wm8505_kbdc_write_ctr(kbdc, kbdc->ctr | KBDC_CTR_KBDDIS | KBDC_CTR_AUXDIS);

	serio_unregister_port(kbdc->kbd_port);
	serio_unregister_port(kbdc->aux_port);
}

static const struct of_device_id wm8505_kbdc_of_match[] = {
	{ .compatible = "wm,wm8505-kbdc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wm8505_kbdc_of_match);

static struct platform_driver wm8505_kbdc_driver = {
	.probe		= wm8505_kbdc_probe,
	.remove		= wm8505_kbdc_remove,
	.driver		= {
		.name		= "wm8505-kbdc",
		.of_match_table = wm8505_kbdc_of_match,
	},
};
module_platform_driver(wm8505_kbdc_driver);

MODULE_DESCRIPTION("WonderMedia WM8505 KBDC Driver");
MODULE_AUTHOR("Logan Russell <me@lrussell.net>");
MODULE_LICENSE("GPL v2");
