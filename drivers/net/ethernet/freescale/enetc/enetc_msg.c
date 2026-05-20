// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/* Copyright 2017-2019 NXP */

#include "enetc_pf.h"

static void enetc_msg_disable_mr_int(struct enetc_pf *pf)
{
	struct enetc_hw *hw = &pf->si->hw;
	u32 psiier;

	psiier = enetc_rd(hw, ENETC_PSIIER) & ~ENETC_PSIMR_MASK(pf->num_vfs);

	/* disable MR int source(s) */
	enetc_wr(hw, ENETC_PSIIER, psiier);
}

static void enetc_msg_enable_mr_int(struct enetc_pf *pf)
{
	struct enetc_hw *hw = &pf->si->hw;
	u32 psiier;

	psiier = enetc_rd(hw, ENETC_PSIIER) | ENETC_PSIMR_MASK(pf->num_vfs);

	enetc_wr(hw, ENETC_PSIIER, psiier);
}

static irqreturn_t enetc_msg_psi_msix(int irq, void *data)
{
	struct enetc_si *si = (struct enetc_si *)data;
	struct enetc_pf *pf = enetc_si_priv(si);

	enetc_msg_disable_mr_int(pf);
	schedule_work(&pf->msg_task);

	return IRQ_HANDLED;
}

static void enetc_msg_task(struct work_struct *work)
{
	struct enetc_pf *pf = container_of(work, struct enetc_pf, msg_task);
	u32 mr_mask = ENETC_PSIMR_MASK(pf->num_vfs);
	struct enetc_hw *hw = &pf->si->hw;
	u32 mr_status;
	int i;

	mr_status = (enetc_rd(hw, ENETC_PSIMSGRR) & mr_mask) |
		    (enetc_rd(hw, ENETC_PSIIDR) & mr_mask);
	if (!mr_status)
		goto out;

	for (i = 0; i < pf->num_vfs; i++) {
		u32 psimsgrr;
		u16 msg_code;

		if (!(ENETC_PSIMR_BIT(i) & mr_status))
			continue;

		enetc_msg_handle_rxmsg(pf, i, &msg_code);

		/* w1c to clear the corresponding VF MR bit */
		enetc_wr(hw, ENETC_PSIIDR, ENETC_PSIMR_BIT(i));

		psimsgrr = ENETC_SIMSGSR_SET_MC(msg_code);
		psimsgrr |= ENETC_PSIMR_BIT(i); /* w1c */
		enetc_wr(hw, ENETC_PSIMSGRR, psimsgrr);
	}

out:
	enetc_msg_enable_mr_int(pf);
}

/* Init */
static int enetc_msg_alloc_mbx(struct enetc_si *si, int idx)
{
	struct enetc_pf *pf = enetc_si_priv(si);
	struct device *dev = &si->pdev->dev;
	struct enetc_hw *hw = &si->hw;
	struct enetc_msg_swbd *msg;
	u32 val;

	msg = &pf->rxmsg[idx];
	/* allocate and set receive buffer */
	msg->size = ENETC_DEFAULT_MSG_SIZE;

	msg->vaddr = dma_alloc_coherent(dev, msg->size, &msg->dma,
					GFP_KERNEL);
	if (!msg->vaddr) {
		dev_err(dev, "msg: fail to alloc dma buffer of size: %d\n",
			msg->size);
		return -ENOMEM;
	}

	/* set multiple of 32 bytes */
	val = lower_32_bits(msg->dma);
	enetc_wr(hw, ENETC_PSIVMSGRCVAR0(idx), val);
	val = upper_32_bits(msg->dma);
	enetc_wr(hw, ENETC_PSIVMSGRCVAR1(idx), val);

	return 0;
}

static void enetc_msg_free_mbx(struct enetc_si *si, int idx)
{
	struct enetc_pf *pf = enetc_si_priv(si);
	struct enetc_hw *hw = &si->hw;
	struct enetc_msg_swbd *msg;

	enetc_wr(hw, ENETC_PSIVMSGRCVAR0(idx), 0);
	enetc_wr(hw, ENETC_PSIVMSGRCVAR1(idx), 0);

	msg = &pf->rxmsg[idx];
	dma_free_coherent(&si->pdev->dev, msg->size, msg->vaddr, msg->dma);
	memset(msg, 0, sizeof(*msg));
}

int enetc_msg_psi_init(struct enetc_pf *pf)
{
	struct enetc_si *si = pf->si;
	int vector, i, err;

	for (i = 0; i < pf->num_vfs; i++) {
		err = enetc_msg_alloc_mbx(si, i);
		if (err)
			goto free_mbx;
	}

	/* initialize PSI mailbox */
	INIT_WORK(&pf->msg_task, enetc_msg_task);

	/* register message passing interrupt handler */
	snprintf(pf->msg_int_name, sizeof(pf->msg_int_name), "%s-vfmsg",
		 si->ndev->name);
	vector = pci_irq_vector(si->pdev, ENETC_SI_INT_IDX);
	err = request_irq(vector, enetc_msg_psi_msix, 0, pf->msg_int_name, si);
	if (err) {
		dev_err(&si->pdev->dev,
			"PSI messaging: request_irq() failed!\n");
		goto free_mbx;
	}

	/* set one IRQ entry for PSI message receive notification (SI int) */
	enetc_wr(&si->hw, ENETC_SIMSIVR, ENETC_SI_INT_IDX);

	/* enable MR interrupts */
	enetc_msg_enable_mr_int(pf);

	return 0;

free_mbx:
	for (i--; i >= 0; i--)
		enetc_msg_free_mbx(si, i);

	return err;
}

void enetc_msg_psi_free(struct enetc_pf *pf)
{
	struct enetc_si *si = pf->si;
	int i;

	/* disable MR interrupts */
	enetc_msg_disable_mr_int(pf);

	/* de-register message passing interrupt handler */
	free_irq(pci_irq_vector(si->pdev, ENETC_SI_INT_IDX), si);

	cancel_work_sync(&pf->msg_task);

	/* MR interrupts may be re-enabled by workqueue */
	enetc_msg_disable_mr_int(pf);

	for (i = 0; i < pf->num_vfs; i++)
		enetc_msg_free_mbx(si, i);
}
