/*
 *  Copyright 2010 Broadcom
 *  Copyright 2012 Simon Arlott, Chris Boot
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * Quirk 1: Shortcut interrupts don't set the bank 1/2 register pending bits
 *
 * If an interrupt fires on bank 1 that isn't in the shortcuts list, bit 8
 * on bank 0 is set to signify that an interrupt in bank 1 has fired, and
 * to look in the bank 1 status register for more information.
 *
 * If an interrupt fires on bank 1 that _is_ in the shortcuts list, its
 * shortcut bit in bank 0 is set as well as its interrupt bit in the bank 1
 * status register, but bank 0 bit 8 is _not_ set.
 *
 * Quirk 2: You can't mask the register 1/2 pending interrupts
 *
 * In a proper cascaded interrupt controller, the interrupt lines with
 * cascaded interrupt controllers on them are just normal interrupt lines.
 * You can mask the interrupts and get on with things. With this controller
 * you can't do that.
 *
 * Quirk 3: The shortcut interrupts can't be (un)masked in bank 0
 *
 * Those interrupts that have shortcuts can only be masked/unmasked in
 * their respective banks' enable/disable registers. Doing so in the bank 0
 * enable/disable registers has no effect.
 *
 *
 * Each bank is registered as a separate interrupt controller but the
 * interrupt handler only acts on the top level interrupt controller,
 * routing shortcut interrupts directly and reading interrupts from the
 * other banks only when required.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <asm/exception.h>

#include "irq.h"

#define IS_VALID_BANK(x) ((x > 0) && (x < 32))
#define IS_VALID_IRQ(x) (x < 32)

struct armctrl_ic {
	void __iomem *pending;
	void __iomem *enable;
	void __iomem *disable;
	struct irq_domain *domain;

	u32 valid_mask;
	u32 source_mask;
	u32 shortcut_mask;
	u32 bank_mask;

	struct armctrl_ic_shortcut {
		u32 bank;
		u32 irq;
		struct irq_domain *domain;
	} shortcuts[32];
	struct armctrl_ic *banks[32];
};

struct of_armctrl_ic {
	unsigned long base;
	u32 base_irq;
	u32 bank_id;

	struct armctrl_ic *ic;
};

static struct armctrl_ic *intc __read_mostly = NULL;

static void armctrl_mask_irq(struct irq_data *d)
{
	struct armctrl_ic *data = irq_get_chip_data(d->irq);
	writel_relaxed(BIT(d->hwirq), data->disable);
}

static void armctrl_unmask_irq(struct irq_data *d)
{
	struct armctrl_ic *data = irq_get_chip_data(d->irq);
	writel_relaxed(BIT(d->hwirq), data->enable);
}

static struct irq_chip armctrl_chip = {
	.name = "ARMCTRL-level",
	.irq_ack = armctrl_mask_irq,
	.irq_mask = armctrl_mask_irq,
	.irq_mask_ack = armctrl_mask_irq,
	.irq_unmask = armctrl_unmask_irq
};

void of_read_armctrl_shortcuts(struct device_node *node, int count)
{
	struct of_armctrl_ic *data = node->data;
	u32 smap[count * 2];
	int ret, i, j;

	/* The shortcut map (smap) is multiple pairs of u32 {bank_id, irq}
	 * in order of the shortcut_mask from the LSB to the MSB.
	 *
	 * This means that the bit used for the shortcut itself is implicit
	 * based on where it is in the list.
	 */
	ret = of_property_read_u32_array(node, "shortcut-map", smap, count * 2);
	if (ret != 0)
		panic("%s: invalid shortcut map (%d)\n", node->full_name, ret);

	for (i = 0, j = 0; i < 32; i++) {
		if (!(data->ic->shortcut_mask & BIT(i)))
			continue;

		if (!IS_VALID_BANK(smap[j]) || !IS_VALID_IRQ(smap[j + 1]))
			panic("%s: invalid vic shortcut %u: %u->%u\n",
				node->full_name, i, smap[j], smap[j + 1]);

		data->ic->shortcuts[i].bank = smap[j];
		data->ic->shortcuts[i].irq = smap[j + 1];
		j += 2;
	}
}

struct of_armctrl_ic __init *of_read_armctrl_ic(struct device_node *node)
{
	struct of_armctrl_ic *data = kmalloc(sizeof(*data), GFP_ATOMIC);
	struct resource res[3];
	int nr_shortcuts, i;
	int ret;

	BUG_ON(data == NULL);
	/* this is freed in of_node_release */
	node->data = data;

	data->ic = kzalloc(sizeof(*data->ic), GFP_ATOMIC);
	BUG_ON(data->ic == NULL);

	ret = of_address_to_resource(node, 0, &res[0]);
	ret |= of_address_to_resource(node, 1, &res[1]);
	ret |= of_address_to_resource(node, 2, &res[2]);

	if (ret)
		panic("%s: unable to find all vic cpu registers\n",
			node->full_name);

	data->base = (unsigned long)res[0].start;
	data->ic->pending = ioremap(res[0].start, resource_size(&res[0]));
	data->ic->enable = ioremap(res[1].start, resource_size(&res[1]));
	data->ic->disable = ioremap(res[2].start, resource_size(&res[2]));

	if (!data->ic->pending || !data->ic->enable || !data->ic->disable)
		panic("%s: unable to map all vic cpu registers\n",
			node->full_name);

	if (!request_region(res[0].start,
				resource_size(&res[0]), node->full_name)
			|| !request_region(res[1].start,
				resource_size(&res[1]), node->full_name)
			|| !request_region(res[2].start,
				resource_size(&res[2]), node->full_name))
		panic("%s: unable to request resources for all vic cpu registers\n",
			node->full_name);

	of_property_read_u32(node, "interrupt-base", &data->base_irq);
	of_property_read_u32(node, "bank-interrupt", &data->bank_id);

	if (of_property_read_u32(node, "source-mask", &data->ic->source_mask))
		data->ic->source_mask = ~0;
	of_property_read_u32(node, "bank-mask", &data->ic->bank_mask);
	of_property_read_u32(node, "shortcut-mask", &data->ic->shortcut_mask);
	data->ic->valid_mask = data->ic->source_mask;

	if ((data->ic->source_mask & data->ic->bank_mask)
			|| (data->ic->source_mask & data->ic->shortcut_mask)
			|| (data->ic->bank_mask & data->ic->shortcut_mask)) {
		panic("%s: vic mask overlap %08x,%08x,%08x\n", node->full_name,
			data->ic->source_mask, data->ic->shortcut_mask,
			data->ic->bank_mask);
	}

	nr_shortcuts = 0;
	for (i = 0; i < 32; i++) {
		if (data->ic->shortcut_mask & BIT(i))
			nr_shortcuts++;
	}

	if (nr_shortcuts > 0) {
		of_read_armctrl_shortcuts(node, nr_shortcuts);
	}

	return data;
}

void __init armctrl_of_link(const char *name, struct of_armctrl_ic *c,
	struct of_armctrl_ic *p)
{
	BUG_ON(c == NULL);
	BUG_ON(p == NULL);

	if (!c->bank_id)
		panic("%s: missing bank id for child vic\n", name);

	if (c->bank_id > 32 || !(p->ic->bank_mask & BIT(c->bank_id)))
		panic("%s: invalid vic bank %d\n", name, c->bank_id);

	if (p->ic->banks[c->bank_id])
		panic("%s: duplicate vic bank %d\n", name, c->bank_id);

	p->ic->banks[c->bank_id] = c->ic;
	p->ic->valid_mask |= BIT(c->bank_id);
}

void __init armctrl_of_link_shortcuts(struct of_armctrl_ic *c,
	struct of_armctrl_ic *p)
{
	int i;

	for (i = 0; i < 32; i++) {
		if (p->ic->shortcuts[i].bank == c->bank_id) {
			p->ic->valid_mask |= BIT(i);
			p->ic->shortcuts[i].domain = c->ic->domain;
		}
	}
}

int __init armctrl_of_init(struct device_node *node,
		struct device_node *parent)
{
	struct of_armctrl_ic *data = of_read_armctrl_ic(node);
	int nr_irqs, irq, i;

	if (parent != NULL) {
		armctrl_of_link(node->full_name, node->data, parent->data);
	} else if (intc != NULL) {
		panic("%s: multiple top level vics\n", node->full_name);
	} else {
		intc = data->ic;
	}

	nr_irqs = 0;
	for (i = 0, irq = data->base_irq; i < 32; i++, irq++) {
		if (!(data->ic->source_mask & BIT(i)))
			continue;

		irq_set_chip_and_handler(irq, &armctrl_chip, handle_level_irq);
		irq_set_chip_data(irq, data->ic);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
		nr_irqs++;
	}

	data->ic->domain = irq_domain_add_legacy(node, nr_irqs, data->base_irq,
		0, &irq_domain_simple_ops, NULL);
	if (!data->ic->domain)
		panic("%s: unable to create IRQ domain\n",
			node->full_name);

	if (parent != NULL) {
		armctrl_of_link_shortcuts(node->data, parent->data);

		printk(KERN_INFO "%s: VIC at %#lx (%d IRQs)\n",
			node->name, data->base, nr_irqs);
	} else {
		printk(KERN_INFO "%s: VIC at %#lx (%d IRQs)\n",
			node->name, data->base, nr_irqs);
	}

	return 0;
}

static struct of_device_id irq_of_match[] __initconst = {
	{ .compatible = "broadcom,bcm2708-armctrl-ic", .data = armctrl_of_init }
};

void __init bcm2708_init_irq(void)
{
	of_irq_init(irq_of_match);
}

/*
 * Handle each interrupt across the entire interrupt controller.  This reads the
 * status register before handling each interrupt, which is necessary given that
 * handle_IRQ may briefly re-enable interrupts for soft IRQ handling.
 */
static void handle_one_irq(struct pt_regs *regs, struct armctrl_ic *dev)
{
	u32 stat, irq;

	while ((stat = readl_relaxed(dev->pending) & dev->valid_mask)) {
		if (stat & dev->source_mask) {
			irq = ffs(stat & dev->source_mask) - 1;
			handle_IRQ(irq_find_mapping(dev->domain, irq), regs);
		} else if (stat & dev->shortcut_mask) {
			irq = ffs(stat & dev->shortcut_mask) - 1;
			handle_IRQ(irq_find_mapping(dev->shortcuts[irq].domain,
				dev->shortcuts[irq].irq), regs);
		} else if (stat & dev->bank_mask) {
			irq = ffs(stat & dev->bank_mask) - 1;
			handle_one_irq(regs, dev->banks[irq]);
		} else {
			BUG();
		}
	}
}

asmlinkage void __exception_irq_entry armctrl_handle_irq(struct pt_regs *regs)
{
	handle_one_irq(regs, intc);
}
