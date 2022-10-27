/*
 *                  QLOGIC LINUX SOFTWARE
 *
 * QLogic ISP2x00 device driver for Linux 2.6.x
 * Copyright (C) 2003-2004 QLogic Corporation
 * (www.qlogic.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */
#include "qla_def.h"

#include <linux/delay.h>
#include <scsi/scsi_transport_fc.h>

#include "qla_devtbl.h"

/* XXX(hch): this is ugly, but we don't want to pull in exioctl.h */
#ifndef EXT_IS_LUN_BIT_SET
#define EXT_IS_LUN_BIT_SET(P,L) \
    (((P)->mask[L/8] & (0x80 >> (L%8)))?1:0)
#define EXT_SET_LUN_BIT(P,L) \
    ((P)->mask[L/8] |= (0x80 >> (L%8)))
#endif

/*
*  QLogic ISP2x00 Hardware Support Function Prototypes.
*/
static int qla2x00_pci_config(scsi_qla_host_t *);
static int qla2x00_isp_firmware(scsi_qla_host_t *);
static void qla2x00_reset_chip(scsi_qla_host_t *);
static int qla2x00_chip_diag(scsi_qla_host_t *);
static void qla2x00_resize_request_q(scsi_qla_host_t *);
static int qla2x00_setup_chip(scsi_qla_host_t *);
static void qla2x00_init_response_q_entries(scsi_qla_host_t *);
static int qla2x00_init_rings(scsi_qla_host_t *);
static int qla2x00_fw_ready(scsi_qla_host_t *);
static int qla2x00_configure_hba(scsi_qla_host_t *);
static int qla2x00_nvram_config(scsi_qla_host_t *);
static int qla2x00_configure_loop(scsi_qla_host_t *);
static int qla2x00_configure_local_loop(scsi_qla_host_t *);
static void qla2x00_update_fcport(scsi_qla_host_t *, fc_port_t *);
static int qla2x00_configure_fabric(scsi_qla_host_t *);
static int qla2x00_find_all_fabric_devs(scsi_qla_host_t *, struct list_head *);
static int qla2x00_device_resync(scsi_qla_host_t *);
static int qla2x00_fabric_dev_login(scsi_qla_host_t *, fc_port_t *,
    uint16_t *);

static int qla2x00_restart_isp(scsi_qla_host_t *);
static void qla2x00_reset_adapter(scsi_qla_host_t *);

/****************************************************************************/
/*                QLogic ISP2x00 Hardware Support Functions.                */
/****************************************************************************/

/*
* qla2x00_initialize_adapter
*      Initialize board.
*
* Input:
*      ha = adapter block pointer.
*
* Returns:
*      0 = success
*/
int
qla2x00_initialize_adapter(scsi_qla_host_t *ha)
{
	int	rval;
	uint8_t	restart_risc = 0;
	uint8_t	retry;
	uint32_t wait_time;

	/* Clear adapter flags. */
	ha->flags.online = 0;
	ha->flags.reset_active = 0;
	atomic_set(&ha->loop_down_timer, LOOP_DOWN_TIME);
	atomic_set(&ha->loop_state, LOOP_DOWN);
	ha->device_flags = 0;
	ha->dpc_flags = 0;
	ha->flags.management_server_logged_in = 0;
	ha->marker_needed = 0;
	ha->mbx_flags = 0;
	ha->isp_abort_cnt = 0;
	ha->beacon_blink_led = 0;

	rval = qla2x00_pci_config(ha);
	if (rval) {
		DEBUG2(printk("scsi(%ld): Unable to configure PCI space=n",
		    ha->host_no));
		return (rval);
	}

	qla2x00_reset_chip(ha);

	qla_printk(KERN_INFO, ha, "Configure NVRAM parameters...\n");
	qla2x00_nvram_config(ha);

	qla_printk(KERN_INFO, ha, "Verifying loaded RISC code...\n");

	retry = 10;
	/*
	 * Try to configure the loop.
	 */
	do {
		restart_risc = 0;

		/* If firmware needs to be loaded */
		if (qla2x00_isp_firmware(ha) != QLA_SUCCESS) {
			if ((rval = qla2x00_chip_diag(ha)) == QLA_SUCCESS) {
				rval = qla2x00_setup_chip(ha);
			}
		}

		if (rval == QLA_SUCCESS &&
		    (rval = qla2x00_init_rings(ha)) == QLA_SUCCESS) {
check_fw_ready_again:
			/*
			 * Wait for a successful LIP up to a maximum 
			 * of (in seconds): RISC login timeout value,
			 * RISC retry count value, and port down retry
			 * value OR a minimum of 4 seconds OR If no 
			 * cable, only 5 seconds.
			 */
			rval = qla2x00_fw_ready(ha);
			if (rval == QLA_SUCCESS) {
				clear_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);

				/*
				 * Wait at most MAX_TARGET RSCNs for a stable
				 * link.
				 */
				wait_time = 256;
				do {
					clear_bit(LOOP_RESYNC_NEEDED,
					    &ha->dpc_flags);
					rval = qla2x00_configure_loop(ha);

					if (test_and_clear_bit(ISP_ABORT_NEEDED,
					    &ha->dpc_flags)) {
						restart_risc = 1;
						break;
					}

					/*
					 * If loop state change while we were
					 * discoverying devices then wait for
					 * LIP to complete
					 */

					if (atomic_read(&ha->loop_state) ==
					    LOOP_DOWN && retry--) {
						goto check_fw_ready_again;
					}
					wait_time--;
				} while (!atomic_read(&ha->loop_down_timer) &&
				    retry &&
				    wait_time &&
				    (test_bit(LOOP_RESYNC_NEEDED,
					&ha->dpc_flags)));

				if (wait_time == 0)
					rval = QLA_FUNCTION_FAILED;
			} else if (ha->device_flags & DFLG_NO_CABLE)
				/* If no cable, then all is good. */
				rval = QLA_SUCCESS;
		}
	} while (restart_risc && retry--);

	if (rval == QLA_SUCCESS) {
		clear_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);
		ha->marker_needed = 1;
		qla2x00_marker(ha, 0, 0, MK_SYNC_ALL);
		ha->marker_needed = 0;

		ha->flags.online = 1;
	} else {
		DEBUG2_3(printk("%s(): **** FAILED ****\n", __func__));
	}

	return (rval);
}

/**
 * qla2x00_pci_config() - Setup device PCI configuration registers.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static int
qla2x00_pci_config(scsi_qla_host_t *ha)
{
	uint16_t	w, mwi;
	unsigned long   flags = 0;
	uint32_t	cnt;

	qla_printk(KERN_INFO, ha, "Configuring PCI space...\n");

	/* 
	 * Turn on PCI master; for system BIOSes that don't turn it on by
	 * default.
	 */
	pci_set_master(ha->pdev);
	mwi = 0;
	if (pci_set_mwi(ha->pdev))
		mwi = PCI_COMMAND_INVALIDATE;
	pci_read_config_word(ha->pdev, PCI_REVISION_ID, &ha->revision);

	if (!ha->iobase)
		return (QLA_FUNCTION_FAILED);

	/*
	 * We want to respect framework's setting of PCI configuration space
	 * command register and also want to make sure that all bits of
	 * interest to us are properly set in command register.
	 */
	pci_read_config_word(ha->pdev, PCI_COMMAND, &w);
	w |= mwi | (PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

	/* Get PCI bus information. */
	spin_lock_irqsave(&ha->hardware_lock, flags);
	ha->pci_attr = RD_REG_WORD(&ha->iobase->ctrl_status);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	if (!IS_QLA2100(ha) && !IS_QLA2200(ha)) {
		pci_write_config_byte(ha->pdev, PCI_LATENCY_TIMER, 0x80);

		/* PCI Specification Revision 2.3 changes */
		if (IS_QLA2322(ha) || IS_QLA6322(ha))
			/* Command Register - Reset Interrupt Disable. */
			w &= ~PCI_COMMAND_INTX_DISABLE;

		/*
		 * If this is a 2300 card and not 2312, reset the
		 * COMMAND_INVALIDATE due to a bug in the 2300. Unfortunately,
		 * the 2310 also reports itself as a 2300 so we need to get the
		 * fb revision level -- a 6 indicates it really is a 2300 and
		 * not a 2310.
		 */
		if (IS_QLA2300(ha)) {
			spin_lock_irqsave(&ha->hardware_lock, flags);

			/* Pause RISC. */
			WRT_REG_WORD(&ha->iobase->hccr, HCCR_PAUSE_RISC);
			for (cnt = 0; cnt < 30000; cnt++) {
				if ((RD_REG_WORD(&ha->iobase->hccr) &
				    HCCR_RISC_PAUSE) != 0)
					break;
	
				udelay(10);
			}

			/* Select FPM registers. */
			WRT_REG_WORD(&ha->iobase->ctrl_status, 0x20);
			RD_REG_WORD(&ha->iobase->ctrl_status);

			/* Get the fb rev level */
			ha->fb_rev = RD_FB_CMD_REG(ha, ha->iobase);

			if (ha->fb_rev == FPM_2300)
				w &= ~PCI_COMMAND_INVALIDATE;

			/* Deselect FPM registers. */
			WRT_REG_WORD(&ha->iobase->ctrl_status, 0x0);
			RD_REG_WORD(&ha->iobase->ctrl_status);

			/* Release RISC module. */
			WRT_REG_WORD(&ha->iobase->hccr, HCCR_RELEASE_RISC);
			for (cnt = 0; cnt < 30000; cnt++) {
				if ((RD_REG_WORD(&ha->iobase->hccr) &
				    HCCR_RISC_PAUSE) == 0)
					break;
	
				udelay(10);
			}

			spin_unlock_irqrestore(&ha->hardware_lock, flags);
		}
	}

	pci_write_config_word(ha->pdev, PCI_COMMAND, w);

	/* Reset expansion ROM address decode enable */
	pci_read_config_word(ha->pdev, PCI_ROM_ADDRESS, &w);
	w &= ~PCI_ROM_ADDRESS_ENABLE;
	pci_write_config_word(ha->pdev, PCI_ROM_ADDRESS, w);

	return (QLA_SUCCESS);
}

/**
 * qla2x00_isp_firmware() - Choose firmware image.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static int
qla2x00_isp_firmware(scsi_qla_host_t *ha)
{
	int  rval;

	/* Assume loading risc code */
	rval = QLA_FUNCTION_FAILED; 

	if (ha->flags.disable_risc_code_load) {
		DEBUG2(printk("scsi(%ld): RISC CODE NOT loaded\n",
		    ha->host_no));
		qla_printk(KERN_INFO, ha, "RISC CODE NOT loaded\n");

		/* Verify checksum of loaded RISC code. */
		rval = qla2x00_verify_checksum(ha);
	}

	if (rval) {
		DEBUG2_3(printk("scsi(%ld): **** Load RISC code ****\n",
		    ha->host_no));
	}

	return (rval);
}

/**
 * qla2x00_reset_chip() - Reset ISP chip.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static void
qla2x00_reset_chip(scsi_qla_host_t *ha) 
{
	unsigned long   flags = 0;
	device_reg_t __iomem *reg = ha->iobase;
	uint32_t	cnt;
	unsigned long	mbx_flags = 0;
	uint16_t	cmd;

	/* Disable ISP interrupts. */
	qla2x00_disable_intrs(ha);

	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Turn off master enable */
	cmd = 0;
	pci_read_config_word(ha->pdev, PCI_COMMAND, &cmd);
	cmd &= ~PCI_COMMAND_MASTER;
	pci_write_config_word(ha->pdev, PCI_COMMAND, cmd);

	if (!IS_QLA2100(ha)) {
		/* Pause RISC. */
		WRT_REG_WORD(&reg->hccr, HCCR_PAUSE_RISC);
		if (IS_QLA2200(ha) || IS_QLA2300(ha)) {
			for (cnt = 0; cnt < 30000; cnt++) {
				if ((RD_REG_WORD(&reg->hccr) &
				    HCCR_RISC_PAUSE) != 0)
					break;
				udelay(100);
			}
		} else {
			RD_REG_WORD(&reg->hccr);	/* PCI Posting. */
			udelay(10);
		}

		/* Select FPM registers. */
		WRT_REG_WORD(&reg->ctrl_status, 0x20);
		RD_REG_WORD(&reg->ctrl_status);		/* PCI Posting. */

		/* FPM Soft Reset. */
		WRT_REG_WORD(&reg->fpm_diag_config, 0x100);
		RD_REG_WORD(&reg->fpm_diag_config);	/* PCI Posting. */

		/* Toggle Fpm Reset. */
		if (!IS_QLA2200(ha)) {
			WRT_REG_WORD(&reg->fpm_diag_config, 0x0);
			RD_REG_WORD(&reg->fpm_diag_config); /* PCI Posting. */
		}

		/* Select frame buffer registers. */
		WRT_REG_WORD(&reg->ctrl_status, 0x10);
		RD_REG_WORD(&reg->ctrl_status);		/* PCI Posting. */

		/* Reset frame buffer FIFOs. */
		if (IS_QLA2200(ha)) {
			WRT_FB_CMD_REG(ha, reg, 0xa000);
			RD_FB_CMD_REG(ha, reg);		/* PCI Posting. */
		} else {
			WRT_FB_CMD_REG(ha, reg, 0x00fc);

			/* Read back fb_cmd until zero or 3 seconds max */
			for (cnt = 0; cnt < 3000; cnt++) {
				if ((RD_FB_CMD_REG(ha, reg) & 0xff) == 0)
					break;
				udelay(100);
			}
		}

		/* Select RISC module registers. */
		WRT_REG_WORD(&reg->ctrl_status, 0);
		RD_REG_WORD(&reg->ctrl_status);		/* PCI Posting. */

		/* Reset RISC processor. */
		WRT_REG_WORD(&reg->hccr, HCCR_RESET_RISC);
		RD_REG_WORD(&reg->hccr);		/* PCI Posting. */

		/* Release RISC processor. */
		WRT_REG_WORD(&reg->hccr, HCCR_RELEASE_RISC);
		RD_REG_WORD(&reg->hccr);		/* PCI Posting. */
	}

	WRT_REG_WORD(&reg->hccr, HCCR_CLR_RISC_INT);
	WRT_REG_WORD(&reg->hccr, HCCR_CLR_HOST_INT);

	/* Reset ISP chip. */
	WRT_REG_WORD(&reg->ctrl_status, CSR_ISP_SOFT_RESET);

	/* Wait for RISC to recover from reset. */
	if (IS_QLA2100(ha) || IS_QLA2200(ha) || IS_QLA2300(ha)) {
		/*
		 * It is necessary to for a delay here since the card doesn't
		 * respond to PCI reads during a reset. On some architectures
		 * this will result in an MCA.
		 */
		udelay(20);
		for (cnt = 30000; cnt; cnt--) {
			if ((RD_REG_WORD(&reg->ctrl_status) &
			    CSR_ISP_SOFT_RESET) == 0)
				break;
			udelay(100);
		}
	} else
		udelay(10);

	/* Reset RISC processor. */
	WRT_REG_WORD(&reg->hccr, HCCR_RESET_RISC);

	WRT_REG_WORD(&reg->semaphore, 0);

	/* Release RISC processor. */
	WRT_REG_WORD(&reg->hccr, HCCR_RELEASE_RISC);
	RD_REG_WORD(&reg->hccr);			/* PCI Posting. */

	if (IS_QLA2100(ha) || IS_QLA2200(ha) || IS_QLA2300(ha)) {
		for (cnt = 0; cnt < 30000; cnt++) {
			if (!(test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags)))
				spin_lock_irqsave(&ha->mbx_reg_lock, mbx_flags);

			if (RD_MAILBOX_REG(ha, reg, 0) != MBS_BUSY) {
				if (!(test_bit(ABORT_ISP_ACTIVE,
				    &ha->dpc_flags)))
					spin_unlock_irqrestore(
					    &ha->mbx_reg_lock, mbx_flags);
				break;
			}

			if (!(test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags)))
				spin_unlock_irqrestore(&ha->mbx_reg_lock,
				    mbx_flags);

			udelay(100);
		}
	} else
		udelay(100);

	/* Turn on master enable */
	cmd |= PCI_COMMAND_MASTER;
	pci_write_config_word(ha->pdev, PCI_COMMAND, cmd);

	/* Disable RISC pause on FPM parity error. */
	if (!IS_QLA2100(ha)) {
		WRT_REG_WORD(&reg->hccr, HCCR_DISABLE_PARITY_PAUSE);
		RD_REG_WORD(&reg->hccr);		/* PCI Posting. */
	}

	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}

/**
 * qla2x00_chip_diag() - Test chip for proper operation.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static int
qla2x00_chip_diag(scsi_qla_host_t *ha)
{
	int		rval;
	device_reg_t __iomem *reg = ha->iobase;
	unsigned long	flags = 0;
	uint16_t	data;
	uint32_t	cnt;
	uint16_t	mb[5];

	/* Assume a failed state */
	rval = QLA_FUNCTION_FAILED;

	DEBUG3(printk("scsi(%ld): Testing device at %lx.\n",
	    ha->host_no, (u_long)&reg->flash_address));

	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Reset ISP chip. */
	WRT_REG_WORD(&reg->ctrl_status, CSR_ISP_SOFT_RESET);

	/*
	 * We need to have a delay here since the card will not respond while
	 * in reset causing an MCA on some architectures.
	 */
	udelay(20);
	data = qla2x00_debounce_register(&reg->ctrl_status);
	for (cnt = 6000000 ; cnt && (data & CSR_ISP_SOFT_RESET); cnt--) {
		udelay(5);
		data = RD_REG_WORD(&reg->ctrl_status);
		barrier();
	}

	if (!cnt)
		goto chip_diag_failed;

	DEBUG3(printk("scsi(%ld): Reset register cleared by chip reset\n",
	    ha->host_no));

	/* Reset RISC processor. */
	WRT_REG_WORD(&reg->hccr, HCCR_RESET_RISC);
	WRT_REG_WORD(&reg->hccr, HCCR_RELEASE_RISC);

	/* Workaround for QLA2312 PCI parity error */
	if (IS_QLA2100(ha) || IS_QLA2200(ha) || IS_QLA2300(ha)) {
		data = qla2x00_debounce_register(MAILBOX_REG(ha, reg, 0));
		for (cnt = 6000000; cnt && (data == MBS_BUSY); cnt--) {
			udelay(5);
			data = RD_MAILBOX_REG(ha, reg, 0);
			barrier(); 
		}
	} else
		udelay(10);

	if (!cnt)
		goto chip_diag_failed;

	/* Check product ID of chip */
	DEBUG3(printk("scsi(%ld): Checking product ID of chip\n", ha->host_no));

	mb[1] = RD_MAILBOX_REG(ha, reg, 1);
	mb[2] = RD_MAILBOX_REG(ha, reg, 2);
	mb[3] = RD_MAILBOX_REG(ha, reg, 3);
	mb[4] = qla2x00_debounce_register(MAILBOX_REG(ha, reg, 4));
	if (mb[1] != PROD_ID_1 || (mb[2] != PROD_ID_2 && mb[2] != PROD_ID_2a) ||
	    mb[3] != PROD_ID_3) {
		qla_printk(KERN_WARNING, ha,
		    "Wrong product ID = 0x%x,0x%x,0x%x\n", mb[1], mb[2], mb[3]);

		goto chip_diag_failed;
	}
	ha->product_id[0] = mb[1];
	ha->product_id[1] = mb[2];
	ha->product_id[2] = mb[3];
	ha->product_id[3] = mb[4];

	/* Adjust fw RISC transfer size */
	if (ha->request_q_length > 1024)
		ha->fw_transfer_size = REQUEST_ENTRY_SIZE * 1024;
	else
		ha->fw_transfer_size = REQUEST_ENTRY_SIZE *
		    ha->request_q_length;

	if (IS_QLA2200(ha) &&
	    RD_MAILBOX_REG(ha, reg, 7) == QLA2200A_RISC_ROM_VER) {
		/* Limit firmware transfer size with a 2200A */
		DEBUG3(printk("scsi(%ld): Found QLA2200A chip.\n",
		    ha->host_no));

		ha->fw_transfer_size = 128;
	}

	/* Wrap Incoming Mailboxes Test. */
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	DEBUG3(printk("scsi(%ld): Checking mailboxes.\n", ha->host_no));
	rval = qla2x00_mbx_reg_test(ha);
	if (rval) {
		DEBUG(printk("scsi(%ld): Failed mailbox send register test\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha,
		    "Failed mailbox send register test\n");
	}
	else {
		/* Flag a successful rval */
		rval = QLA_SUCCESS;
	}
	spin_lock_irqsave(&ha->hardware_lock, flags);

chip_diag_failed:
	if (rval)
		DEBUG2_3(printk("scsi(%ld): Chip diagnostics **** FAILED "
		    "****\n", ha->host_no));

	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	return (rval);
}

/**
 * qla2x00_resize_request_q() - Resize request queue given available ISP memory.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static void
qla2x00_resize_request_q(scsi_qla_host_t *ha)
{
	int rval;
	uint16_t fw_iocb_cnt = 0;
	uint16_t request_q_length = REQUEST_ENTRY_CNT_2XXX_EXT_MEM;
	dma_addr_t request_dma;
	request_t *request_ring;

	/* Valid only on recent ISPs. */
	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return;

	/* Retrieve IOCB counts available to the firmware. */
	rval = qla2x00_get_resource_cnts(ha, NULL, NULL, NULL, &fw_iocb_cnt);
	if (rval)
		return;
	/* No point in continuing if current settings are sufficient. */
	if (fw_iocb_cnt < 1024)
		return;
	if (ha->request_q_length >= request_q_length)
		return;

	/* Attempt to claim larger area for request queue. */
	request_ring = dma_alloc_coherent(&ha->pdev->dev,
	    (request_q_length + 1) * sizeof(request_t), &request_dma,
	    GFP_KERNEL);
	if (request_ring == NULL)
		return;

	/* Resize successful, report extensions. */
	qla_printk(KERN_INFO, ha, "Extended memory detected (%d KB)...\n",
	    (ha->fw_memory_size + 1) / 1024);
	qla_printk(KERN_INFO, ha, "Resizing request queue depth "
	    "(%d -> %d)...\n", ha->request_q_length, request_q_length);

	/* Clear old allocations. */
	dma_free_coherent(&ha->pdev->dev,
	    (ha->request_q_length + 1) * sizeof(request_t), ha->request_ring,
	    ha->request_dma);

	/* Begin using larger queue. */
	ha->request_q_length = request_q_length;
	ha->request_ring = request_ring;
	ha->request_dma = request_dma;
}

/**
 * qla2x00_setup_chip() - Load and start RISC firmware.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static int
qla2x00_setup_chip(scsi_qla_host_t *ha)
{
	int		rval;
	uint16_t	cnt;
	uint16_t	*risc_code;
	unsigned long	risc_address;
	unsigned long	risc_code_size;
	int		num;
	int		i;
	uint16_t	*req_ring;
	struct qla_fw_info *fw_iter;

	rval = QLA_SUCCESS;

	/* Load firmware sequences */
	fw_iter = ha->brd_info->fw_info;
	while (fw_iter->addressing != FW_INFO_ADDR_NOMORE) {
		risc_code = fw_iter->fwcode;
		risc_code_size = *fw_iter->fwlen;

		if (fw_iter->addressing == FW_INFO_ADDR_NORMAL) {
			risc_address = *fw_iter->fwstart;
		} else {
			/* Extended address */
			risc_address = *fw_iter->lfwstart;
		}

		num = 0;
		rval = 0;
		while (risc_code_size > 0 && !rval) {
			cnt = (uint16_t)(ha->fw_transfer_size >> 1);
			if (cnt > risc_code_size)
				cnt = risc_code_size;

			DEBUG7(printk("scsi(%ld): Loading risc segment@ "
			    "addr %p, number of bytes 0x%x, offset 0x%lx.\n",
			    ha->host_no, risc_code, cnt, risc_address));

			req_ring = (uint16_t *)ha->request_ring;
			for (i = 0; i < cnt; i++)
				req_ring[i] = cpu_to_le16(risc_code[i]);

			if (fw_iter->addressing == FW_INFO_ADDR_NORMAL) {
				rval = qla2x00_load_ram(ha,
				    ha->request_dma, risc_address, cnt);
			} else {
				rval = qla2x00_load_ram_ext(ha,
				    ha->request_dma, risc_address, cnt);
			}
			if (rval) {
				DEBUG(printk("scsi(%ld): [ERROR] Failed to "
				    "load segment %d of firmware\n",
				    ha->host_no, num));
				qla_printk(KERN_WARNING, ha,
				    "[ERROR] Failed to load "
				    "segment %d of firmware\n", num);

				qla2x00_dump_regs(ha);
				break;
			}

			risc_code += cnt;
			risc_address += cnt;
			risc_code_size -= cnt;
			num++;
		}

		/* Next firmware sequence */
		fw_iter++;
	}

	/* Verify checksum of loaded RISC code. */
	if (!rval) {
		DEBUG(printk("scsi(%ld): Verifying Checksum of loaded RISC "
		    "code.\n", ha->host_no));

		rval = qla2x00_verify_checksum(ha);
		if (rval == QLA_SUCCESS) {
			/* Start firmware execution. */
			DEBUG(printk("scsi(%ld): Checksum OK, start "
			    "firmware.\n", ha->host_no));

			rval = qla2x00_execute_fw(ha);
			/* Retrieve firmware information. */
			if (rval == QLA_SUCCESS && ha->fw_major_version == 0) {
				qla2x00_get_fw_version(ha,
				    &ha->fw_major_version,
				    &ha->fw_minor_version,
				    &ha->fw_subminor_version,
				    &ha->fw_attributes, &ha->fw_memory_size);
				qla2x00_resize_request_q(ha);
			}
		} else {
			DEBUG2(printk(KERN_INFO
			    "scsi(%ld): ISP Firmware failed checksum.\n",
			    ha->host_no));
		}
	}

	if (rval) {
		DEBUG2_3(printk("scsi(%ld): Setup chip **** FAILED ****.\n",
		    ha->host_no));
	}

	return (rval);
}

/**
 * qla2x00_init_response_q_entries() - Initializes response queue entries.
 * @ha: HA context
 *
 * Beginning of request ring has initialization control block already built
 * by nvram config routine.
 *
 * Returns 0 on success.
 */
static void
qla2x00_init_response_q_entries(scsi_qla_host_t *ha)
{
	uint16_t cnt;
	response_t *pkt;

	pkt = ha->response_ring_ptr;
	for (cnt = 0; cnt < ha->response_q_length; cnt++) {
		pkt->signature = RESPONSE_PROCESSED;
		pkt++;
	}

}

/**
 * qla2x00_update_fw_options() - Read and process firmware options.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static void
qla2x00_update_fw_options(scsi_qla_host_t *ha)
{
	uint16_t swing, emphasis, tx_sens, rx_sens;

	memset(ha->fw_options, 0, sizeof(ha->fw_options));
	qla2x00_get_fw_options(ha, ha->fw_options);

	if (IS_QLA2100(ha) || IS_QLA2200(ha))
		return;

	/* Serial Link options. */
	DEBUG3(printk("scsi(%ld): Serial link options:\n",
	    ha->host_no));
	DEBUG3(qla2x00_dump_buffer((uint8_t *)&ha->fw_seriallink_options,
	    sizeof(ha->fw_seriallink_options)));

	ha->fw_options[1] &= ~FO1_SET_EMPHASIS_SWING;
	if (ha->fw_seriallink_options[3] & BIT_2) {
		ha->fw_options[1] |= FO1_SET_EMPHASIS_SWING;

		/*  1G settings */
		swing = ha->fw_seriallink_options[2] & (BIT_2 | BIT_1 | BIT_0);
		emphasis = (ha->fw_seriallink_options[2] &
		    (BIT_4 | BIT_3)) >> 3;
		tx_sens = ha->fw_seriallink_options[0] &
		    (BIT_3 | BIT_2 | BIT_1 | BIT_0); 
		rx_sens = (ha->fw_seriallink_options[0] &
		    (BIT_7 | BIT_6 | BIT_5 | BIT_4)) >> 4;
		ha->fw_options[10] = (emphasis << 14) | (swing << 8);
		if (IS_QLA2300(ha) || IS_QLA2312(ha) || IS_QLA6312(ha)) {
			if (rx_sens == 0x0)
				rx_sens = 0x3;
			ha->fw_options[10] |= (tx_sens << 4) | rx_sens;
		} else if (IS_QLA2322(ha) || IS_QLA6322(ha))
			ha->fw_options[10] |= BIT_5 |
			    ((rx_sens & (BIT_1 | BIT_0)) << 2) |
			    (tx_sens & (BIT_1 | BIT_0));

		/*  2G settings */
		swing = (ha->fw_seriallink_options[2] &
		    (BIT_7 | BIT_6 | BIT_5)) >> 5;
		emphasis = ha->fw_seriallink_options[3] & (BIT_1 | BIT_0);
		tx_sens = ha->fw_seriallink_options[1] &
		    (BIT_3 | BIT_2 | BIT_1 | BIT_0); 
		rx_sens = (ha->fw_seriallink_options[1] &
		    (BIT_7 | BIT_6 | BIT_5 | BIT_4)) >> 4;
		ha->fw_options[11] = (emphasis << 14) | (swing << 8);
		if (IS_QLA2300(ha) || IS_QLA2312(ha) || IS_QLA6312(ha)) {
			if (rx_sens == 0x0)
				rx_sens = 0x3;
			ha->fw_options[11] |= (tx_sens << 4) | rx_sens;
		} else if (IS_QLA2322(ha) || IS_QLA6322(ha))
			ha->fw_options[11] |= BIT_5 |
			    ((rx_sens & (BIT_1 | BIT_0)) << 2) |
			    (tx_sens & (BIT_1 | BIT_0));
	}

	/* FCP2 options. */
	/*  Return command IOCBs without waiting for an ABTS to complete. */
	ha->fw_options[3] |= BIT_13;

	/* LED scheme. */
	if (ha->flags.enable_led_scheme)
		ha->fw_options[2] |= BIT_12;

	/* Update firmware options. */
	qla2x00_set_fw_options(ha, ha->fw_options);
}

/**
 * qla2x00_init_rings() - Initializes firmware.
 * @ha: HA context
 *
 * Beginning of request ring has initialization control block already built
 * by nvram config routine.
 *
 * Returns 0 on success.
 */
static int
qla2x00_init_rings(scsi_qla_host_t *ha)
{
	int	rval;
	unsigned long flags = 0;
	int cnt;
	device_reg_t __iomem *reg = ha->iobase;

	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Clear outstanding commands array. */
	for (cnt = 0; cnt < MAX_OUTSTANDING_COMMANDS; cnt++)
		ha->outstanding_cmds[cnt] = NULL;

	ha->current_outstanding_cmd = 0;

	/* Clear RSCN queue. */
	ha->rscn_in_ptr = 0;
	ha->rscn_out_ptr = 0;

	/* Initialize firmware. */
	ha->request_ring_ptr  = ha->request_ring;
	ha->req_ring_index    = 0;
	ha->req_q_cnt         = ha->request_q_length;
	ha->response_ring_ptr = ha->response_ring;
	ha->rsp_ring_index    = 0;

	/* Setup ring parameters in initialization control block. */
	ha->init_cb->request_q_outpointer = __constant_cpu_to_le16(0);
	ha->init_cb->response_q_inpointer = __constant_cpu_to_le16(0);
	ha->init_cb->request_q_length = cpu_to_le16(ha->request_q_length);
	ha->init_cb->response_q_length = cpu_to_le16(ha->response_q_length);
	ha->init_cb->request_q_address[0] = cpu_to_le32(LSD(ha->request_dma));
	ha->init_cb->request_q_address[1] = cpu_to_le32(MSD(ha->request_dma));
	ha->init_cb->response_q_address[0] = cpu_to_le32(LSD(ha->response_dma));
	ha->init_cb->response_q_address[1] = cpu_to_le32(MSD(ha->response_dma));

	/* Initialize response queue entries */
	qla2x00_init_response_q_entries(ha);

 	WRT_REG_WORD(ISP_REQ_Q_IN(ha, reg), 0);
 	WRT_REG_WORD(ISP_REQ_Q_OUT(ha, reg), 0);
 	WRT_REG_WORD(ISP_RSP_Q_IN(ha, reg), 0);
 	WRT_REG_WORD(ISP_RSP_Q_OUT(ha, reg), 0);
	RD_REG_WORD(ISP_RSP_Q_OUT(ha, reg));		/* PCI Posting. */

	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	/* Update any ISP specific firmware options before initialization. */
	qla2x00_update_fw_options(ha);

	DEBUG(printk("scsi(%ld): Issue init firmware.\n", ha->host_no));
	rval = qla2x00_init_firmware(ha, sizeof(init_cb_t));
	if (rval) {
		DEBUG2_3(printk("scsi(%ld): Init firmware **** FAILED ****.\n",
		    ha->host_no));
	} else {
		DEBUG3(printk("scsi(%ld): Init firmware -- success.\n",
		    ha->host_no));
	}

	return (rval);
}

/**
 * qla2x00_fw_ready() - Waits for firmware ready.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
static int
qla2x00_fw_ready(scsi_qla_host_t *ha)
{
	int		rval;
	unsigned long	wtime, mtime;
	uint16_t	min_wait;	/* Minimum wait time if loop is down */
	uint16_t	wait_time;	/* Wait time if loop is coming ready */
	uint16_t	fw_state;

	rval = QLA_SUCCESS;

	/* 20 seconds for loop down. */
	min_wait = 20;		

	/*
	 * Firmware should take at most one RATOV to login, plus 5 seconds for
	 * our own processing.
	 */
	if ((wait_time = (ha->retry_count*ha->login_timeout) + 5) < min_wait) {
		wait_time = min_wait;
	}

	/* Min wait time if loop down */
	mtime = jiffies + (min_wait * HZ);

	/* wait time before firmware ready */
	wtime = jiffies + (wait_time * HZ);

	/* Wait for ISP to finish LIP */
	if (!ha->flags.init_done)
 		qla_printk(KERN_INFO, ha, "Waiting for LIP to complete...\n");

	DEBUG3(printk("scsi(%ld): Waiting for LIP to complete...\n",
	    ha->host_no));

	do {
		rval = qla2x00_get_firmware_state(ha, &fw_state);
		if (rval == QLA_SUCCESS) {
			if (fw_state < FSTATE_LOSS_OF_SYNC) {
				ha->device_flags &= ~DFLG_NO_CABLE;
			}
			if (fw_state == FSTATE_READY) {
				DEBUG(printk("scsi(%ld): F/W Ready - OK \n",
				    ha->host_no));

				qla2x00_get_retry_cnt(ha, &ha->retry_count,
				    &ha->login_timeout, &ha->r_a_tov);

				rval = QLA_SUCCESS;
				break;
			}

			rval = QLA_FUNCTION_FAILED;

			if (atomic_read(&ha->loop_down_timer) &&
			    (fw_state >= FSTATE_LOSS_OF_SYNC ||
				fw_state == FSTATE_WAIT_AL_PA)) {
				/* Loop down. Timeout on min_wait for states
				 * other than Wait for Login. 
				 */	
				if (time_after_eq(jiffies, mtime)) {
					qla_printk(KERN_INFO, ha,
					    "Cable is unplugged...\n");

					ha->device_flags |= DFLG_NO_CABLE;
					break;
				}
			}
		} else {
			/* Mailbox cmd failed. Timeout on min_wait. */
			if (time_after_eq(jiffies, mtime))
				break;
		}

		if (time_after_eq(jiffies, wtime))
			break;

		/* Delay for a while */
		msleep(500);

		DEBUG3(printk("scsi(%ld): fw_state=%x curr time=%lx.\n",
		    ha->host_no, fw_state, jiffies));
	} while (1);

	DEBUG(printk("scsi(%ld): fw_state=%x curr time=%lx.\n",
	    ha->host_no, fw_state, jiffies));

	if (rval) {
		DEBUG2_3(printk("scsi(%ld): Firmware ready **** FAILED ****.\n",
		    ha->host_no));
	}

	return (rval);
}

/*
*  qla2x00_configure_hba
*      Setup adapter context.
*
* Input:
*      ha = adapter state pointer.
*
* Returns:
*      0 = success
*
* Context:
*      Kernel context.
*/
static int
qla2x00_configure_hba(scsi_qla_host_t *ha)
{
	int       rval;
	uint16_t      loop_id;
	uint16_t      topo;
	uint8_t       al_pa;
	uint8_t       area;
	uint8_t       domain;
	char		connect_type[22];

	/* Get host addresses. */
	rval = qla2x00_get_adapter_id(ha,
	    &loop_id, &al_pa, &area, &domain, &topo);
	if (rval != QLA_SUCCESS) {
		qla_printk(KERN_WARNING, ha,
		    "ERROR -- Unable to get host loop ID.\n");
		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		return (rval);
	}

	if (topo == 4) {
		qla_printk(KERN_INFO, ha,
			"Cannot get topology - retrying.\n");
		return (QLA_FUNCTION_FAILED);
	}

	ha->loop_id = loop_id;

	/* initialize */
	ha->min_external_loopid = SNS_FIRST_LOOP_ID;
	ha->operating_mode = LOOP;

	switch (topo) {
	case 0:
		DEBUG3(printk("scsi(%ld): HBA in NL topology.\n",
		    ha->host_no));
		ha->current_topology = ISP_CFG_NL;
		strcpy(connect_type, "(Loop)");
		break;

	case 1:
		DEBUG3(printk("scsi(%ld): HBA in FL topology.\n",
		    ha->host_no));
		ha->current_topology = ISP_CFG_FL;
		strcpy(connect_type, "(FL_Port)");
		break;

	case 2:
		DEBUG3(printk("scsi(%ld): HBA in N P2P topology.\n",
		    ha->host_no));
		ha->operating_mode = P2P;
		ha->current_topology = ISP_CFG_N;
		strcpy(connect_type, "(N_Port-to-N_Port)");
		break;

	case 3:
		DEBUG3(printk("scsi(%ld): HBA in F P2P topology.\n",
		    ha->host_no));
		ha->operating_mode = P2P;
		ha->current_topology = ISP_CFG_F;
		strcpy(connect_type, "(F_Port)");
		break;

	default:
		DEBUG3(printk("scsi(%ld): HBA in unknown topology %x. "
		    "Using NL.\n",
		    ha->host_no, topo));
		ha->current_topology = ISP_CFG_NL;
		strcpy(connect_type, "(Loop)");
		break;
	}

	/* Save Host port and loop ID. */
	/* byte order - Big Endian */
	ha->d_id.b.domain = domain;
	ha->d_id.b.area = area;
	ha->d_id.b.al_pa = al_pa;

	if (!ha->flags.init_done)
 		qla_printk(KERN_INFO, ha,
		    "Topology - %s, Host Loop address 0x%x\n",
 		    connect_type, ha->loop_id);

	if (rval) {
		DEBUG2_3(printk("scsi(%ld): FAILED.\n", ha->host_no));
	} else {
		DEBUG3(printk("scsi(%ld): exiting normally.\n", ha->host_no));
	}

	return(rval);
}

/*
* NVRAM configuration for ISP 2xxx
*
* Input:
*      ha                = adapter block pointer.
*
* Output:
*      initialization control block in response_ring
*      host adapters parameters in host adapter block
*
* Returns:
*      0 = success.
*/
static int
qla2x00_nvram_config(scsi_qla_host_t *ha)
{
	int   rval;
	uint8_t   chksum = 0;
	uint16_t  cnt;
	uint8_t   *dptr1, *dptr2;
	init_cb_t *icb   = ha->init_cb;
	nvram_t *nv    = (nvram_t *)ha->request_ring;
	uint16_t  *wptr  = (uint16_t *)ha->request_ring;
	device_reg_t __iomem *reg = ha->iobase;
	uint8_t  timer_mode;

	rval = QLA_SUCCESS;

	/* Determine NVRAM starting address. */
	ha->nvram_base = 0;
	if (!IS_QLA2100(ha) && !IS_QLA2200(ha) && !IS_QLA2300(ha))
		if ((RD_REG_WORD(&reg->ctrl_status) >> 14) == 1)
			ha->nvram_base = 0x80;

	/* Get NVRAM data and calculate checksum. */
	qla2x00_lock_nvram_access(ha);
	for (cnt = 0; cnt < sizeof(nvram_t)/2; cnt++) {
		*wptr = cpu_to_le16(qla2x00_get_nvram_word(ha,
		    (cnt+ha->nvram_base)));
		chksum += (uint8_t)*wptr;
		chksum += (uint8_t)(*wptr >> 8);
		wptr++;
	}
	qla2x00_unlock_nvram_access(ha);

	DEBUG5(printk("scsi(%ld): Contents of NVRAM\n", ha->host_no));
	DEBUG5(qla2x00_dump_buffer((uint8_t *)ha->request_ring,
	    sizeof(nvram_t)));

	/* Bad NVRAM data, set defaults parameters. */
	if (chksum || nv->id[0] != 'I' || nv->id[1] != 'S' ||
	    nv->id[2] != 'P' || nv->id[3] != ' ' || nv->nvram_version < 1) {
		/* Reset NVRAM data. */
		qla_printk(KERN_WARNING, ha, "Inconsistent NVRAM detected: "
		    "checksum=0x%x id=%c version=0x%x.\n", chksum, nv->id[0],
		    nv->nvram_version);
		qla_printk(KERN_WARNING, ha, "Falling back to functioning (yet "
		    "invalid -- WWPN) defaults.\n");

		/*
		 * Set default initialization control block.
		 */
		memset(nv, 0, sizeof(nvram_t));
		nv->parameter_block_version = ICB_VERSION;

		if (IS_QLA23XX(ha)) {
			nv->firmware_options[0] = BIT_2 | BIT_1;
			nv->firmware_options[1] = BIT_7 | BIT_5;
			nv->add_firmware_options[0] = BIT_5;
			nv->add_firmware_options[1] = BIT_5 | BIT_4;
			nv->frame_payload_size = __constant_cpu_to_le16(2048);
			nv->special_options[1] = BIT_7;
		} else if (IS_QLA2200(ha)) {
			nv->firmware_options[0] = BIT_2 | BIT_1;
			nv->firmware_options[1] = BIT_7 | BIT_5;
			nv->add_firmware_options[0] = BIT_5;
			nv->add_firmware_options[1] = BIT_5 | BIT_4;
			nv->frame_payload_size = __constant_cpu_to_le16(1024);
		} else if (IS_QLA2100(ha)) {
			nv->firmware_options[0] = BIT_3 | BIT_1;
			nv->firmware_options[1] = BIT_5;
			nv->frame_payload_size = __constant_cpu_to_le16(1024);
		}

		nv->max_iocb_allocation = __constant_cpu_to_le16(256);
		nv->execution_throttle = __constant_cpu_to_le16(16);
		nv->retry_count = 8;
		nv->retry_delay = 1;

		nv->port_name[0] = 33;
		nv->port_name[3] = 224;
		nv->port_name[4] = 139;

		nv->login_timeout = 4;

		/*
		 * Set default host adapter parameters
		 */
		nv->host_p[1] = BIT_2;
		nv->reset_delay = 5;
		nv->port_down_retry_count = 8;
		nv->max_luns_per_target = __constant_cpu_to_le16(8);
		nv->link_down_timeout = 60;

		rval = 1;
	}

#if defined(CONFIG_IA64_GENERIC) || defined(CONFIG_IA64_SGI_SN2)
	/*
	 * The SN2 does not provide BIOS emulation which means you can't change
	 * potentially bogus BIOS settings. Force the use of default settings
	 * for link rate and frame size.  Hope that the rest of the settings
	 * are valid.
	 */
	if (ia64_platform_is("sn2")) {
		nv->frame_payload_size = __constant_cpu_to_le16(2048);
		if (IS_QLA23XX(ha))
			nv->special_options[1] = BIT_7;
	}
#endif

	/* Reset Initialization control block */
	memset(icb, 0, sizeof(init_cb_t));

	/*
	 * Setup driver NVRAM options.
	 */
	nv->firmware_options[0] |= (BIT_6 | BIT_1);
	nv->firmware_options[0] &= ~(BIT_5 | BIT_4);
	nv->firmware_options[1] |= (BIT_5 | BIT_0);
	nv->firmware_options[1] &= ~BIT_4;

	if (IS_QLA23XX(ha)) {
		nv->firmware_options[0] |= BIT_2;
		nv->firmware_options[0] &= ~BIT_3;

		if (IS_QLA2300(ha)) {
			if (ha->fb_rev == FPM_2310) {
				strcpy(ha->model_number, "QLA2310");
			} else {
				strcpy(ha->model_number, "QLA2300");
			}
		} else {
			if (rval == 0 &&
			    memcmp(nv->model_number, BINZERO,
				    sizeof(nv->model_number)) != 0) {
				char *st, *en;

				strncpy(ha->model_number, nv->model_number,
				    sizeof(nv->model_number));
				st = en = ha->model_number;
				en += sizeof(nv->model_number) - 1;
				while (en > st) {
					if (*en != 0x20 && *en != 0x00)
						break;
					*en-- = '\0';
				}
			} else {
				uint16_t        index;

				index = (ha->pdev->subsystem_device & 0xff);
				if (index < QLA_MODEL_NAMES) {
					strcpy(ha->model_number,
					    qla2x00_model_name[index]);
					ha->model_desc =
					    qla2x00_model_desc[index];
				} else {
					strcpy(ha->model_number, "QLA23xx");
				}
			}
		}
	} else if (IS_QLA2200(ha)) {
		nv->firmware_options[0] |= BIT_2;
		/*
		 * 'Point-to-point preferred, else loop' is not a safe
		 * connection mode setting.
		 */
		if ((nv->add_firmware_options[0] & (BIT_6 | BIT_5 | BIT_4)) ==
		    (BIT_5 | BIT_4)) {
			/* Force 'loop preferred, else point-to-point'. */
			nv->add_firmware_options[0] &= ~(BIT_6 | BIT_5 | BIT_4);
			nv->add_firmware_options[0] |= BIT_5;
		}
		strcpy(ha->model_number, "QLA22xx");
	} else /*if (IS_QLA2100(ha))*/ {
		strcpy(ha->model_number, "QLA2100");
	}

	/*
	 * Copy over NVRAM RISC parameter block to initialization control block.
	 */
	dptr1 = (uint8_t *)icb;
	dptr2 = (uint8_t *)&nv->parameter_block_version;
	cnt = (uint8_t *)&icb->request_q_outpointer - (uint8_t *)&icb->version;
	while (cnt--)
		*dptr1++ = *dptr2++;

	/* Copy 2nd half. */
	dptr1 = (uint8_t *)icb->add_firmware_options;
	cnt = (uint8_t *)icb->reserved_3 - (uint8_t *)icb->add_firmware_options;
	while (cnt--)
		*dptr1++ = *dptr2++;

	/* Prepare nodename */
	if ((icb->firmware_options[1] & BIT_6) == 0) {
		/*
		 * Firmware will apply the following mask if the nodename was
		 * not provided.
		 */
		memcpy(icb->node_name, icb->port_name, WWN_SIZE);
		icb->node_name[0] &= 0xF0;
	}

	/*
	 * Set host adapter parameters.
	 */
	ha->nvram_version = nv->nvram_version;

	ha->flags.disable_risc_code_load = ((nv->host_p[0] & BIT_4) ? 1 : 0);
	/* Always load RISC code on non ISP2[12]00 chips. */
	if (!IS_QLA2100(ha) && !IS_QLA2200(ha))
		ha->flags.disable_risc_code_load = 0;
	ha->flags.enable_lip_reset = ((nv->host_p[1] & BIT_1) ? 1 : 0);
	ha->flags.enable_lip_full_login = ((nv->host_p[1] & BIT_2) ? 1 : 0);
	ha->flags.enable_target_reset = ((nv->host_p[1] & BIT_3) ? 1 : 0);
	ha->flags.enable_led_scheme = ((nv->efi_parameters & BIT_3) ? 1 : 0);

	ha->operating_mode =
	    (icb->add_firmware_options[0] & (BIT_6 | BIT_5 | BIT_4)) >> 4;

	memcpy(ha->fw_seriallink_options, nv->seriallink_options,
	    sizeof(ha->fw_seriallink_options));

	/* save HBA serial number */
	ha->serial0 = icb->port_name[5];
	ha->serial1 = icb->port_name[6];
	ha->serial2 = icb->port_name[7];
	memcpy(ha->node_name, icb->node_name, WWN_SIZE);

	icb->execution_throttle = __constant_cpu_to_le16(0xFFFF);

	ha->retry_count = nv->retry_count;

	/* Set minimum login_timeout to 4 seconds. */
	if (nv->login_timeout < ql2xlogintimeout)
		nv->login_timeout = ql2xlogintimeout;
	if (nv->login_timeout < 4)
		nv->login_timeout = 4;
	ha->login_timeout = nv->login_timeout;
	icb->login_timeout = nv->login_timeout;

	/* Set minimum RATOV to 200 tenths of a second. */
	ha->r_a_tov = 200;

	ha->loop_reset_delay = nv->reset_delay;

	/* Link Down Timeout = 0:
	 *
	 * 	When Port Down timer expires we will start returning
	 *	I/O's to OS with "DID_NO_CONNECT".
	 *
	 * Link Down Timeout != 0:
	 *
	 *	 The driver waits for the link to come up after link down
	 *	 before returning I/Os to OS with "DID_NO_CONNECT".
	 */						
	if (nv->link_down_timeout == 0) {
		ha->loop_down_abort_time =
		    (LOOP_DOWN_TIME - LOOP_DOWN_TIMEOUT);
	} else {
		ha->link_down_timeout =	 nv->link_down_timeout;
		ha->loop_down_abort_time =
		    (LOOP_DOWN_TIME - ha->link_down_timeout);
	} 

	/*
	 * Need enough time to try and get the port back.
	 */
	ha->port_down_retry_count = nv->port_down_retry_count;
	if (qlport_down_retry)
		ha->port_down_retry_count = qlport_down_retry;
	/* Set login_retry_count */
	ha->login_retry_count  = nv->retry_count;
	if (ha->port_down_retry_count == nv->port_down_retry_count &&
	    ha->port_down_retry_count > 3)
		ha->login_retry_count = ha->port_down_retry_count;
	else if (ha->port_down_retry_count > (int)ha->login_retry_count)
		ha->login_retry_count = ha->port_down_retry_count;
	if (ql2xloginretrycount)
		ha->login_retry_count = ql2xloginretrycount;

	icb->lun_enables = __constant_cpu_to_le16(0);
	icb->command_resource_count = 0;
	icb->immediate_notify_resource_count = 0;
	icb->timeout = __constant_cpu_to_le16(0);

	if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
		/* Enable RIO */
		icb->firmware_options[0] &= ~BIT_3;
		icb->add_firmware_options[0] &=
		    ~(BIT_3 | BIT_2 | BIT_1 | BIT_0);
		icb->add_firmware_options[0] |= BIT_2;
		icb->response_accumulation_timer = 3;
		icb->interrupt_delay_timer = 5;

		ha->flags.process_response_queue = 1;
	} else {
		/* Enable ZIO -- Support mode 5 only. */
		timer_mode = icb->add_firmware_options[0] &
		    (BIT_3 | BIT_2 | BIT_1 | BIT_0);
		icb->add_firmware_options[0] &=
		    ~(BIT_3 | BIT_2 | BIT_1 | BIT_0);
		if (ql2xenablezio)
			timer_mode = BIT_2 | BIT_0;
		if (timer_mode == (BIT_2 | BIT_0)) {
			DEBUG2(printk("scsi(%ld): ZIO enabled; timer delay "
			    "(%d).\n", ha->host_no, ql2xintrdelaytimer));
			qla_printk(KERN_INFO, ha,
			    "ZIO enabled; timer delay (%d).\n",
			    ql2xintrdelaytimer);

			icb->add_firmware_options[0] |= timer_mode;
			icb->interrupt_delay_timer = ql2xintrdelaytimer;
			ha->flags.process_response_queue = 1;
		}
	}

	if (rval) {
		DEBUG2_3(printk(KERN_WARNING
		    "scsi(%ld): NVRAM configuration failed!\n", ha->host_no));
	}
	return (rval);
}

/**
 * qla2x00_alloc_fcport() - Allocate a generic fcport.
 * @ha: HA context
 * @flags: allocation flags
 *
 * Returns a pointer to the allocated fcport, or NULL, if none available.
 */
fc_port_t *
qla2x00_alloc_fcport(scsi_qla_host_t *ha, int flags)
{
	fc_port_t *fcport;

	fcport = kmalloc(sizeof(fc_port_t), flags);
	if (fcport == NULL)
		return (fcport);

	/* Setup fcport template structure. */
	memset(fcport, 0, sizeof (fc_port_t));
	fcport->ha = ha;
	fcport->port_type = FCT_UNKNOWN;
	fcport->loop_id = FC_NO_LOOP_ID;
	fcport->iodesc_idx_sent = IODESC_INVALID_INDEX;
	atomic_set(&fcport->state, FCS_UNCONFIGURED);
	fcport->flags = FCF_RLC_SUPPORT;

	return (fcport);
}

/*
 * qla2x00_configure_loop
 *      Updates Fibre Channel Device Database with what is actually on loop.
 *
 * Input:
 *      ha                = adapter block pointer.
 *
 * Returns:
 *      0 = success.
 *      1 = error.
 *      2 = database was full and device was not configured.
 */
static int
qla2x00_configure_loop(scsi_qla_host_t *ha) 
{
	int  rval;
	unsigned long flags, save_flags;

	rval = QLA_SUCCESS;

	/* Get Initiator ID */
	if (test_bit(LOCAL_LOOP_UPDATE, &ha->dpc_flags)) {
		rval = qla2x00_configure_hba(ha);
		if (rval != QLA_SUCCESS) {
			DEBUG(printk("scsi(%ld): Unable to configure HBA.\n",
			    ha->host_no));
			return (rval);
		}
	}

	save_flags = flags = ha->dpc_flags;
	DEBUG(printk("scsi(%ld): Configure loop -- dpc flags =0x%lx\n",
	    ha->host_no, flags));

	/*
	 * If we have both an RSCN and PORT UPDATE pending then handle them
	 * both at the same time.
	 */
	clear_bit(LOCAL_LOOP_UPDATE, &ha->dpc_flags);
	clear_bit(RSCN_UPDATE, &ha->dpc_flags);

	/* Determine what we need to do */
	if (ha->current_topology == ISP_CFG_FL &&
	    (test_bit(LOCAL_LOOP_UPDATE, &flags))) {

		ha->flags.rscn_queue_overflow = 1;
		set_bit(RSCN_UPDATE, &flags);

	} else if (ha->current_topology == ISP_CFG_F &&
	    (test_bit(LOCAL_LOOP_UPDATE, &flags))) {

		ha->flags.rscn_queue_overflow = 1;
		set_bit(RSCN_UPDATE, &flags);
		clear_bit(LOCAL_LOOP_UPDATE, &flags);

	} else if (!ha->flags.online ||
	    (test_bit(ABORT_ISP_ACTIVE, &flags))) {

		ha->flags.rscn_queue_overflow = 1;
		set_bit(RSCN_UPDATE, &flags);
		set_bit(LOCAL_LOOP_UPDATE, &flags);
	}

	if (test_bit(LOCAL_LOOP_UPDATE, &flags)) {
		if (test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)) {
			rval = QLA_FUNCTION_FAILED;
		} else {
			rval = qla2x00_configure_local_loop(ha);
		}
	}

	if (rval == QLA_SUCCESS && test_bit(RSCN_UPDATE, &flags)) {
		if (test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)) {
			rval = QLA_FUNCTION_FAILED;
		} else {
			rval = qla2x00_configure_fabric(ha);
		}
	}

	if (rval == QLA_SUCCESS) {
		if (atomic_read(&ha->loop_down_timer) ||
		    test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)) {
			rval = QLA_FUNCTION_FAILED;
		} else {
			atomic_set(&ha->loop_state, LOOP_READY);

			DEBUG(printk("scsi(%ld): LOOP READY\n", ha->host_no));
		}
	}

	if (rval) {
		DEBUG2_3(printk("%s(%ld): *** FAILED ***\n",
		    __func__, ha->host_no));
	} else {
		DEBUG3(printk("%s: exiting normally\n", __func__));
	}

	/* Restore state if a resync event occured during processing */
	if (test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)) {
		if (test_bit(LOCAL_LOOP_UPDATE, &save_flags))
			set_bit(LOCAL_LOOP_UPDATE, &ha->dpc_flags);
		if (test_bit(RSCN_UPDATE, &save_flags))
			set_bit(RSCN_UPDATE, &ha->dpc_flags);
	}

	return (rval);
}



/*
 * qla2x00_configure_local_loop
 *	Updates Fibre Channel Device Database with local loop devices.
 *
 * Input:
 *	ha = adapter block pointer.
 *
 * Returns:
 *	0 = success.
 */
static int
qla2x00_configure_local_loop(scsi_qla_host_t *ha) 
{
	int		rval, rval2;
	int		found_devs;
	int		found;
	fc_port_t	*fcport, *new_fcport;

	uint16_t	index;
	uint16_t	entries;
	char		*id_iter;
	uint16_t	loop_id;
	uint8_t		domain, area, al_pa;

	found_devs = 0;
	new_fcport = NULL;
	entries = MAX_FIBRE_DEVICES;

	DEBUG3(printk("scsi(%ld): Getting FCAL position map\n", ha->host_no));
	DEBUG3(qla2x00_get_fcal_position_map(ha, NULL));

	/* Get list of logged in devices. */
	memset(ha->gid_list, 0, GID_LIST_SIZE);
	rval = qla2x00_get_id_list(ha, ha->gid_list, ha->gid_list_dma,
	    &entries);
	if (rval != QLA_SUCCESS)
		goto cleanup_allocation;

	DEBUG3(printk("scsi(%ld): Entries in ID list (%d)\n",
	    ha->host_no, entries));
	DEBUG3(qla2x00_dump_buffer((uint8_t *)ha->gid_list,
	    entries * sizeof(struct gid_list_info)));

	/* Allocate temporary fcport for any new fcports discovered. */
	new_fcport = qla2x00_alloc_fcport(ha, GFP_KERNEL);
	if (new_fcport == NULL) {
		rval = QLA_MEMORY_ALLOC_FAILED;
		goto cleanup_allocation;
	}
	new_fcport->flags &= ~FCF_FABRIC_DEVICE;

	/*
	 * Mark local devices that were present with FCF_DEVICE_LOST for now.
	 */
	list_for_each_entry(fcport, &ha->fcports, list) {
		if (atomic_read(&fcport->state) == FCS_ONLINE &&
		    fcport->port_type != FCT_BROADCAST &&
		    (fcport->flags & FCF_FABRIC_DEVICE) == 0) {

			DEBUG(printk("scsi(%ld): Marking port lost, "
			    "loop_id=0x%04x\n",
			    ha->host_no, fcport->loop_id));

			atomic_set(&fcport->state, FCS_DEVICE_LOST);
			fcport->flags &= ~FCF_FARP_DONE;
		}
	}

	/* Add devices to port list. */
	id_iter = (char *)ha->gid_list;
	for (index = 0; index < entries; index++) {
		domain = ((struct gid_list_info *)id_iter)->domain;
		area = ((struct gid_list_info *)id_iter)->area;
		al_pa = ((struct gid_list_info *)id_iter)->al_pa;
		if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
			loop_id = (uint16_t)
			    ((struct gid_list_info *)id_iter)->loop_id_2100;
			id_iter += 4;
		} else {
			loop_id = le16_to_cpu(
			    ((struct gid_list_info *)id_iter)->loop_id);
			id_iter += 6;
		}

		/* Bypass reserved domain fields. */
		if ((domain & 0xf0) == 0xf0)
			continue;

		/* Bypass if not same domain and area of adapter. */
		if (area != ha->d_id.b.area || domain != ha->d_id.b.domain)
			continue;

		/* Bypass invalid local loop ID. */
		if (loop_id > LAST_LOCAL_LOOP_ID)
			continue;

		/* Fill in member data. */
		new_fcport->d_id.b.domain = domain;
		new_fcport->d_id.b.area = area;
		new_fcport->d_id.b.al_pa = al_pa;
		new_fcport->loop_id = loop_id;
		rval2 = qla2x00_get_port_database(ha, new_fcport, 0);
		if (rval2 != QLA_SUCCESS) {
			DEBUG2(printk("scsi(%ld): Failed to retrieve fcport "
			    "information -- get_port_database=%x, "
			    "loop_id=0x%04x\n",
			    ha->host_no, rval2, new_fcport->loop_id));
			continue;
		}

		/* Check for matching device in port list. */
		found = 0;
		fcport = NULL;
		list_for_each_entry(fcport, &ha->fcports, list) {
			if (memcmp(new_fcport->port_name, fcport->port_name,
			    WWN_SIZE))
				continue;

			fcport->flags &= ~(FCF_FABRIC_DEVICE |
			    FCF_PERSISTENT_BOUND);
			fcport->loop_id = new_fcport->loop_id;
			fcport->port_type = new_fcport->port_type;
			fcport->d_id.b24 = new_fcport->d_id.b24;
			memcpy(fcport->node_name, new_fcport->node_name,
			    WWN_SIZE);

			found++;
			break;
		}

		if (!found) {
			/* New device, add to fcports list. */
			new_fcport->flags &= ~FCF_PERSISTENT_BOUND;
			list_add_tail(&new_fcport->list, &ha->fcports);

			/* Allocate a new replacement fcport. */
			fcport = new_fcport;
			new_fcport = qla2x00_alloc_fcport(ha, GFP_KERNEL);
			if (new_fcport == NULL) {
				rval = QLA_MEMORY_ALLOC_FAILED;
				goto cleanup_allocation;
			}
			new_fcport->flags &= ~FCF_FABRIC_DEVICE;
		}

		qla2x00_update_fcport(ha, fcport);

		found_devs++;
	}

cleanup_allocation:
	if (new_fcport)
		kfree(new_fcport);

	if (rval != QLA_SUCCESS) {
		DEBUG2(printk("scsi(%ld): Configure local loop error exit: "
		    "rval=%x\n", ha->host_no, rval));
	}

	if (found_devs) {
		ha->device_flags |= DFLG_LOCAL_DEVICES;
		ha->device_flags &= ~DFLG_RETRY_LOCAL_DEVICES;
	}

	return (rval);
}

static void
qla2x00_probe_for_all_luns(scsi_qla_host_t *ha) 
{
	fc_port_t	*fcport;

	qla2x00_mark_all_devices_lost(ha); 
 	list_for_each_entry(fcport, &ha->fcports, list) {
		if (fcport->port_type != FCT_TARGET)
			continue;

		qla2x00_update_fcport(ha, fcport); 
	}
}

/*
 * qla2x00_update_fcport
 *	Updates device on list.
 *
 * Input:
 *	ha = adapter block pointer.
 *	fcport = port structure pointer.
 *
 * Return:
 *	0  - Success
 *  BIT_0 - error
 *
 * Context:
 *	Kernel context.
 */
static void
qla2x00_update_fcport(scsi_qla_host_t *ha, fc_port_t *fcport)
{
	uint16_t	index;
	unsigned long flags;
	srb_t *sp;

	fcport->ha = ha;
	fcport->login_retry = 0;
	fcport->port_login_retry_count = ha->port_down_retry_count *
	    PORT_RETRY_TIME;
	atomic_set(&fcport->port_down_timer, ha->port_down_retry_count *
	    PORT_RETRY_TIME);
	fcport->flags &= ~FCF_LOGIN_NEEDED;

	/*
	 * Check for outstanding cmd on tape Bypass LUN discovery if active
	 * command on tape.
	 */
	if (fcport->flags & FCF_TAPE_PRESENT) {
		spin_lock_irqsave(&ha->hardware_lock, flags);
		for (index = 1; index < MAX_OUTSTANDING_COMMANDS; index++) {
			fc_port_t *sfcp;

			if ((sp = ha->outstanding_cmds[index]) != 0) {
				sfcp = sp->fcport;
				if (sfcp == fcport) {
					atomic_set(&fcport->state, FCS_ONLINE);
					spin_unlock_irqrestore(
					    &ha->hardware_lock, flags);
					return;
				}
			}
		}
		spin_unlock_irqrestore(&ha->hardware_lock, flags);
	}

	if (fcport->port_type == FCT_INITIATOR ||
	    fcport->port_type == FCT_BROADCAST)
		fcport->device_type = TYPE_PROCESSOR;

	atomic_set(&fcport->state, FCS_ONLINE);

	if (ha->flags.init_done)
		qla2x00_reg_remote_port(ha, fcport);
}

void
qla2x00_reg_remote_port(scsi_qla_host_t *ha, fc_port_t *fcport)
{
	struct fc_rport_identifiers rport_ids;
	struct fc_rport *rport;

	if (fcport->rport) {
		fc_remote_port_unblock(fcport->rport);
		return;
	}

	rport_ids.node_name = be64_to_cpu(*(uint64_t *)fcport->node_name);
	rport_ids.port_name = be64_to_cpu(*(uint64_t *)fcport->port_name);
	rport_ids.port_id = fcport->d_id.b.domain << 16 |
	    fcport->d_id.b.area << 8 | fcport->d_id.b.al_pa;
	rport_ids.roles = FC_RPORT_ROLE_UNKNOWN;
	if (fcport->port_type == FCT_INITIATOR)
		rport_ids.roles |= FC_RPORT_ROLE_FCP_INITIATOR;
	if (fcport->port_type == FCT_TARGET)
		rport_ids.roles |= FC_RPORT_ROLE_FCP_TARGET;

	fcport->rport = rport = fc_remote_port_add(ha->host, 0, &rport_ids);
	if (!rport)
		qla_printk(KERN_WARNING, ha,
		    "Unable to allocate fc remote port!\n");

	if (rport->scsi_target_id != -1 && rport->scsi_target_id < MAX_TARGETS)
		fcport->os_target_id = rport->scsi_target_id;

	rport->dd_data = fcport;
}

/*
 * qla2x00_configure_fabric
 *      Setup SNS devices with loop ID's.
 *
 * Input:
 *      ha = adapter block pointer.
 *
 * Returns:
 *      0 = success.
 *      BIT_0 = error
 */
static int
qla2x00_configure_fabric(scsi_qla_host_t *ha)
{
	int	rval, rval2;
	fc_port_t	*fcport, *fcptemp;
	uint16_t	next_loopid;
	uint16_t	mb[MAILBOX_REGISTER_COUNT];
	LIST_HEAD(new_fcports);

	/* If FL port exists, then SNS is present */
	rval = qla2x00_get_port_name(ha, SNS_FL_PORT, NULL, 0);
	if (rval != QLA_SUCCESS) {
		DEBUG2(printk("scsi(%ld): MBC_GET_PORT_NAME Failed, No FL "
		    "Port\n", ha->host_no));

		ha->device_flags &= ~SWITCH_FOUND;
		return (QLA_SUCCESS);
	}

	/* Mark devices that need re-synchronization. */
	rval2 = qla2x00_device_resync(ha);
	if (rval2 == QLA_RSCNS_HANDLED) {
		/* No point doing the scan, just continue. */
		return (QLA_SUCCESS);
	}
	do {
		/* Ensure we are logged into the SNS. */
		qla2x00_login_fabric(ha, SIMPLE_NAME_SERVER, 0xff, 0xff, 0xfc,
		    mb, BIT_1 | BIT_0);
		if (mb[0] != MBS_COMMAND_COMPLETE) {
			DEBUG2(qla_printk(KERN_INFO, ha,
			    "Failed SNS login: loop_id=%x mb[0]=%x mb[1]=%x "
			    "mb[2]=%x mb[6]=%x mb[7]=%x\n", SIMPLE_NAME_SERVER,
			    mb[0], mb[1], mb[2], mb[6], mb[7]));
			return (QLA_SUCCESS);
		}

		if (test_and_clear_bit(REGISTER_FC4_NEEDED, &ha->dpc_flags)) {
			if (qla2x00_rft_id(ha)) {
				/* EMPTY */
				DEBUG2(printk("scsi(%ld): Register FC-4 "
				    "TYPE failed.\n", ha->host_no));
			}
			if (qla2x00_rff_id(ha)) {
				/* EMPTY */
				DEBUG2(printk("scsi(%ld): Register FC-4 "
				    "Features failed.\n", ha->host_no));
			}
			if (qla2x00_rnn_id(ha)) {
				/* EMPTY */
				DEBUG2(printk("scsi(%ld): Register Node Name "
				    "failed.\n", ha->host_no));
			} else if (qla2x00_rsnn_nn(ha)) {
				/* EMPTY */
				DEBUG2(printk("scsi(%ld): Register Symbolic "
				    "Node Name failed.\n", ha->host_no));
			}
		}

		rval = qla2x00_find_all_fabric_devs(ha, &new_fcports);
		if (rval != QLA_SUCCESS)
			break;

		/*
		 * Logout all previous fabric devices marked lost, except
		 * tape devices.
		 */
		list_for_each_entry(fcport, &ha->fcports, list) {
			if (test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags))
				break;

			if ((fcport->flags & FCF_FABRIC_DEVICE) == 0)
				continue;

			if (atomic_read(&fcport->state) == FCS_DEVICE_LOST) {
				qla2x00_mark_device_lost(ha, fcport,
				    ql2xplogiabsentdevice);
				if (fcport->loop_id != FC_NO_LOOP_ID &&
				    (fcport->flags & FCF_TAPE_PRESENT) == 0 &&
				    fcport->port_type != FCT_INITIATOR &&
				    fcport->port_type != FCT_BROADCAST) {

					qla2x00_fabric_logout(ha,
					    fcport->loop_id);
					fcport->loop_id = FC_NO_LOOP_ID;
				}
			}
		}

		/* Starting free loop ID. */
		next_loopid = ha->min_external_loopid;

		/*
		 * Scan through our port list and login entries that need to be
		 * logged in.
		 */
		list_for_each_entry(fcport, &ha->fcports, list) {
			if (atomic_read(&ha->loop_down_timer) ||
			    test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags))
				break;

			if ((fcport->flags & FCF_FABRIC_DEVICE) == 0 ||
			    (fcport->flags & FCF_LOGIN_NEEDED) == 0)
				continue;

			if (fcport->loop_id == FC_NO_LOOP_ID) {
				fcport->loop_id = next_loopid;
				rval = qla2x00_find_new_loop_id(ha, fcport);
				if (rval != QLA_SUCCESS) {
					/* Ran out of IDs to use */
					break;
				}
			}

			/* Login and update database */
			qla2x00_fabric_dev_login(ha, fcport, &next_loopid);
		}

		/* Exit if out of loop IDs. */
		if (rval != QLA_SUCCESS) {
			break;
		}

		/*
		 * Login and add the new devices to our port list.
		 */
		list_for_each_entry_safe(fcport, fcptemp, &new_fcports, list) {
			if (atomic_read(&ha->loop_down_timer) ||
			    test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags))
				break;

			/* Find a new loop ID to use. */
			fcport->loop_id = next_loopid;
			rval = qla2x00_find_new_loop_id(ha, fcport);
			if (rval != QLA_SUCCESS) {
				/* Ran out of IDs to use */
				break;
			}

			/* Remove device from the new list and add it to DB */
			list_del(&fcport->list);
			list_add_tail(&fcport->list, &ha->fcports);

			/* Login and update database */
			qla2x00_fabric_dev_login(ha, fcport, &next_loopid);
		}
	} while (0);

	/* Free all new device structures not processed. */
	list_for_each_entry_safe(fcport, fcptemp, &new_fcports, list) {
		list_del(&fcport->list);
		kfree(fcport);
	}

	if (rval) {
		DEBUG2(printk("scsi(%ld): Configure fabric error exit: "
		    "rval=%d\n", ha->host_no, rval));
	}

	return (rval);
}


/*
 * qla2x00_find_all_fabric_devs
 *
 * Input:
 *	ha = adapter block pointer.
 *	dev = database device entry pointer.
 *
 * Returns:
 *	0 = success.
 *
 * Context:
 *	Kernel context.
 */
static int
qla2x00_find_all_fabric_devs(scsi_qla_host_t *ha, struct list_head *new_fcports)
{
	int		rval;
	uint16_t	loop_id;
	fc_port_t	*fcport, *new_fcport, *fcptemp;
	int		found;

	sw_info_t	*swl;
	int		swl_idx;
	int		first_dev, last_dev;
	port_id_t	wrap, nxt_d_id;

	rval = QLA_SUCCESS;

	/* Try GID_PT to get device list, else GAN. */
	swl = kmalloc(sizeof(sw_info_t) * MAX_FIBRE_DEVICES, GFP_ATOMIC);
	if (swl == NULL) {
		/*EMPTY*/
		DEBUG2(printk("scsi(%ld): GID_PT allocations failed, fallback "
		    "on GA_NXT\n", ha->host_no));
	} else {
		memset(swl, 0, sizeof(sw_info_t) * MAX_FIBRE_DEVICES);
		if (qla2x00_gid_pt(ha, swl) != QLA_SUCCESS) {
			kfree(swl);
			swl = NULL;
		} else if (qla2x00_gpn_id(ha, swl) != QLA_SUCCESS) {
			kfree(swl);
			swl = NULL;
		} else if (qla2x00_gnn_id(ha, swl) != QLA_SUCCESS) {
			kfree(swl);
			swl = NULL;
		}
	}
	swl_idx = 0;

	/* Allocate temporary fcport for any new fcports discovered. */
	new_fcport = qla2x00_alloc_fcport(ha, GFP_KERNEL);
	if (new_fcport == NULL) {
		if (swl)
			kfree(swl);
		return (QLA_MEMORY_ALLOC_FAILED);
	}
	new_fcport->flags |= (FCF_FABRIC_DEVICE | FCF_LOGIN_NEEDED);

	/* Set start port ID scan at adapter ID. */
	first_dev = 1;
	last_dev = 0;

	/* Starting free loop ID. */
	loop_id = ha->min_external_loopid;

	for (; loop_id <= ha->last_loop_id; loop_id++) {
		if (RESERVED_LOOP_ID(loop_id))
			continue;

		if (atomic_read(&ha->loop_down_timer) ||
		    test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags))
			break;

		if (swl != NULL) {
			if (last_dev) {
				wrap.b24 = new_fcport->d_id.b24;
			} else {
				new_fcport->d_id.b24 = swl[swl_idx].d_id.b24;
				memcpy(new_fcport->node_name,
				    swl[swl_idx].node_name, WWN_SIZE);
				memcpy(new_fcport->port_name,
				    swl[swl_idx].port_name, WWN_SIZE);

				if (swl[swl_idx].d_id.b.rsvd_1 != 0) {
					last_dev = 1;
				}
				swl_idx++;
			}
		} else {
			/* Send GA_NXT to the switch */
			rval = qla2x00_ga_nxt(ha, new_fcport);
			if (rval != QLA_SUCCESS) {
				qla_printk(KERN_WARNING, ha,
				    "SNS scan failed -- assuming zero-entry "
				    "result...\n");
				list_for_each_entry_safe(fcport, fcptemp,
				    new_fcports, list) {
					list_del(&fcport->list);
					kfree(fcport);
				}
				rval = QLA_SUCCESS;
				break;
			}
		}

		/* If wrap on switch device list, exit. */
		if (first_dev) {
			wrap.b24 = new_fcport->d_id.b24;
			first_dev = 0;
		} else if (new_fcport->d_id.b24 == wrap.b24) {
			DEBUG2(printk("scsi(%ld): device wrap (%02x%02x%02x)\n",
			    ha->host_no, new_fcport->d_id.b.domain,
			    new_fcport->d_id.b.area, new_fcport->d_id.b.al_pa));
			break;
		}

		/* Bypass if host adapter. */
		if (new_fcport->d_id.b24 == ha->d_id.b24)
			continue;

		/* Bypass reserved domain fields. */
		if ((new_fcport->d_id.b.domain & 0xf0) == 0xf0)
			continue;

		/* Locate matching device in database. */
		found = 0;
		list_for_each_entry(fcport, &ha->fcports, list) {
			if (memcmp(new_fcport->port_name, fcport->port_name,
			    WWN_SIZE))
				continue;

			found++;

			/*
			 * If address the same and state FCS_ONLINE, nothing
			 * changed.
			 */
			if (fcport->d_id.b24 == new_fcport->d_id.b24 &&
			    atomic_read(&fcport->state) == FCS_ONLINE) {
				break;
			}

			/*
			 * If device was not a fabric device before.
			 */
			if ((fcport->flags & FCF_FABRIC_DEVICE) == 0) {
				fcport->d_id.b24 = new_fcport->d_id.b24;
				fcport->loop_id = FC_NO_LOOP_ID;
				fcport->flags |= (FCF_FABRIC_DEVICE |
				    FCF_LOGIN_NEEDED);
				fcport->flags &= ~FCF_PERSISTENT_BOUND;
				break;
			}

			/*
			 * Port ID changed or device was marked to be updated;
			 * Log it out if still logged in and mark it for
			 * relogin later.
			 */
			fcport->d_id.b24 = new_fcport->d_id.b24;
			fcport->flags |= FCF_LOGIN_NEEDED;
			if (fcport->loop_id != FC_NO_LOOP_ID &&
			    (fcport->flags & FCF_TAPE_PRESENT) == 0 &&
			    fcport->port_type != FCT_INITIATOR &&
			    fcport->port_type != FCT_BROADCAST) {
				qla2x00_fabric_logout(ha, fcport->loop_id);
				fcport->loop_id = FC_NO_LOOP_ID;
			}

			break;
		}

		if (found)
			continue;

		/* If device was not in our fcports list, then add it. */
		list_add_tail(&new_fcport->list, new_fcports);

		/* Allocate a new replacement fcport. */
		nxt_d_id.b24 = new_fcport->d_id.b24;
		new_fcport = qla2x00_alloc_fcport(ha, GFP_KERNEL);
		if (new_fcport == NULL) {
			if (swl)
				kfree(swl);
			return (QLA_MEMORY_ALLOC_FAILED);
		}
		new_fcport->flags |= (FCF_FABRIC_DEVICE | FCF_LOGIN_NEEDED);
		new_fcport->d_id.b24 = nxt_d_id.b24;
	}

	if (swl)
		kfree(swl);

	if (new_fcport)
		kfree(new_fcport);

	if (!list_empty(new_fcports))
		ha->device_flags |= DFLG_FABRIC_DEVICES;

	return (rval);
}

/*
 * qla2x00_find_new_loop_id
 *	Scan through our port list and find a new usable loop ID.
 *
 * Input:
 *	ha:	adapter state pointer.
 *	dev:	port structure pointer.
 *
 * Returns:
 *	qla2x00 local function return status code.
 *
 * Context:
 *	Kernel context.
 */
int
qla2x00_find_new_loop_id(scsi_qla_host_t *ha, fc_port_t *dev)
{
	int	rval;
	int	found;
	fc_port_t *fcport;
	uint16_t first_loop_id;

	rval = QLA_SUCCESS;

	/* Save starting loop ID. */
	first_loop_id = dev->loop_id;

	for (;;) {
		/* Skip loop ID if already used by adapter. */
		if (dev->loop_id == ha->loop_id) {
			dev->loop_id++;
		}

		/* Skip reserved loop IDs. */
		while (RESERVED_LOOP_ID(dev->loop_id)) {
			dev->loop_id++;
		}

		/* Reset loop ID if passed the end. */
		if (dev->loop_id > ha->last_loop_id) {
			/* first loop ID. */
			dev->loop_id = ha->min_external_loopid;
		}

		/* Check for loop ID being already in use. */
		found = 0;
		fcport = NULL;
		list_for_each_entry(fcport, &ha->fcports, list) {
			if (fcport->loop_id == dev->loop_id && fcport != dev) {
				/* ID possibly in use */
				found++;
				break;
			}
		}

		/* If not in use then it is free to use. */
		if (!found) {
			break;
		}

		/* ID in use. Try next value. */
		dev->loop_id++;

		/* If wrap around. No free ID to use. */
		if (dev->loop_id == first_loop_id) {
			dev->loop_id = FC_NO_LOOP_ID;
			rval = QLA_FUNCTION_FAILED;
			break;
		}
	}

	return (rval);
}

/*
 * qla2x00_device_resync
 *	Marks devices in the database that needs resynchronization.
 *
 * Input:
 *	ha = adapter block pointer.
 *
 * Context:
 *	Kernel context.
 */
static int
qla2x00_device_resync(scsi_qla_host_t *ha) 
{
	int	rval;
	int	rval2;
	uint32_t mask;
	fc_port_t *fcport;
	uint32_t rscn_entry;
	uint8_t rscn_out_iter;
	uint8_t format;
	port_id_t d_id;

	rval = QLA_RSCNS_HANDLED;

	while (ha->rscn_out_ptr != ha->rscn_in_ptr ||
	    ha->flags.rscn_queue_overflow) {

		rscn_entry = ha->rscn_queue[ha->rscn_out_ptr];
		format = MSB(MSW(rscn_entry));
		d_id.b.domain = LSB(MSW(rscn_entry));
		d_id.b.area = MSB(LSW(rscn_entry));
		d_id.b.al_pa = LSB(LSW(rscn_entry));

		DEBUG(printk("scsi(%ld): RSCN queue entry[%d] = "
		    "[%02x/%02x%02x%02x].\n",
		    ha->host_no, ha->rscn_out_ptr, format, d_id.b.domain,
		    d_id.b.area, d_id.b.al_pa));

		ha->rscn_out_ptr++;
		if (ha->rscn_out_ptr == MAX_RSCN_COUNT)
			ha->rscn_out_ptr = 0;

		/* Skip duplicate entries. */
		for (rscn_out_iter = ha->rscn_out_ptr;
		    !ha->flags.rscn_queue_overflow &&
		    rscn_out_iter != ha->rscn_in_ptr;
		    rscn_out_iter = (rscn_out_iter ==
			(MAX_RSCN_COUNT - 1)) ? 0: rscn_out_iter + 1) {

			if (rscn_entry != ha->rscn_queue[rscn_out_iter])
				break;

			DEBUG(printk("scsi(%ld): Skipping duplicate RSCN queue "
			    "entry found at [%d].\n", ha->host_no,
			    rscn_out_iter));

			ha->rscn_out_ptr = rscn_out_iter;
		}

		/* Queue overflow, set switch default case. */
		if (ha->flags.rscn_queue_overflow) {
			DEBUG(printk("scsi(%ld): device_resync: rscn "
			    "overflow.\n", ha->host_no));

			format = 3;
			ha->flags.rscn_queue_overflow = 0;
		}

		switch (format) {
		case 0:
			if (!IS_QLA2100(ha) && !IS_QLA2200(ha) &&
			    !IS_QLA6312(ha) && !IS_QLA6322(ha) &&
			    ha->flags.init_done) {
				/* Handle port RSCN via asyncronous IOCBs */
				rval2 = qla2x00_handle_port_rscn(ha, rscn_entry,
				    NULL, 0);
				if (rval2 == QLA_SUCCESS)
					continue;
			}
			mask = 0xffffff;
			break;
		case 1:
			mask = 0xffff00;
			break;
		case 2:
			mask = 0xff0000;
			break;
		default:
			mask = 0x0;
			d_id.b24 = 0;
			ha->rscn_out_ptr = ha->rscn_in_ptr;
			break;
		}

		rval = QLA_SUCCESS;

		/* Abort any outstanding IO descriptors. */
		if (!IS_QLA2100(ha) && !IS_QLA2200(ha))
			qla2x00_cancel_io_descriptors(ha);

		list_for_each_entry(fcport, &ha->fcports, list) {
			if ((fcport->flags & FCF_FABRIC_DEVICE) == 0 ||
			    (fcport->d_id.b24 & mask) != d_id.b24 ||
			    fcport->port_type == FCT_BROADCAST)
				continue;

			if (atomic_read(&fcport->state) == FCS_ONLINE) {
				if (format != 3 ||
				    fcport->port_type != FCT_INITIATOR) {
					qla2x00_mark_device_lost(ha, fcport, 0);
				}
			}
			fcport->flags &= ~FCF_FARP_DONE;
		}
	}
	return (rval);
}

/*
 * qla2x00_fabric_dev_login
 *	Login fabric target device and update FC port database.
 *
 * Input:
 *	ha:		adapter state pointer.
 *	fcport:		port structure list pointer.
 *	next_loopid:	contains value of a new loop ID that can be used
 *			by the next login attempt.
 *
 * Returns:
 *	qla2x00 local function return status code.
 *
 * Context:
 *	Kernel context.
 */
static int
qla2x00_fabric_dev_login(scsi_qla_host_t *ha, fc_port_t *fcport,
    uint16_t *next_loopid)
{
	int	rval;
	int	retry;

	rval = QLA_SUCCESS;
	retry = 0;

	rval = qla2x00_fabric_login(ha, fcport, next_loopid);
	if (rval == QLA_SUCCESS) {
		rval = qla2x00_get_port_database(ha, fcport, 0);
		if (rval != QLA_SUCCESS) {
			qla2x00_fabric_logout(ha, fcport->loop_id);
		} else {
			qla2x00_update_fcport(ha, fcport);
		}
	}

	return (rval);
}

/*
 * qla2x00_fabric_login
 *	Issue fabric login command.
 *
 * Input:
 *	ha = adapter block pointer.
 *	device = pointer to FC device type structure.
 *
 * Returns:
 *      0 - Login successfully
 *      1 - Login failed
 *      2 - Initiator device
 *      3 - Fatal error
 */
int
qla2x00_fabric_login(scsi_qla_host_t *ha, fc_port_t *fcport,
    uint16_t *next_loopid)
{
	int	rval;
	int	retry;
	uint16_t tmp_loopid;
	uint16_t mb[MAILBOX_REGISTER_COUNT];

	retry = 0;
	tmp_loopid = 0;

	for (;;) {
		DEBUG(printk("scsi(%ld): Trying Fabric Login w/loop id 0x%04x "
 		    "for port %02x%02x%02x.\n",
 		    ha->host_no, fcport->loop_id, fcport->d_id.b.domain,
		    fcport->d_id.b.area, fcport->d_id.b.al_pa));

		/* Login fcport on switch. */
		qla2x00_login_fabric(ha, fcport->loop_id,
		    fcport->d_id.b.domain, fcport->d_id.b.area,
		    fcport->d_id.b.al_pa, mb, BIT_0);
		if (mb[0] == MBS_PORT_ID_USED) {
			/*
			 * Device has another loop ID.  The firmware team
			 * recommends us to perform an implicit login with the
			 * specified ID again. The ID we just used is save here
			 * so we return with an ID that can be tried by the
			 * next login.
			 */
			retry++;
			tmp_loopid = fcport->loop_id;
			fcport->loop_id = mb[1];

			DEBUG(printk("Fabric Login: port in use - next "
 			    "loop id=0x%04x, port Id=%02x%02x%02x.\n",
			    fcport->loop_id, fcport->d_id.b.domain,
			    fcport->d_id.b.area, fcport->d_id.b.al_pa));

		} else if (mb[0] == MBS_COMMAND_COMPLETE) {
			/*
			 * Login succeeded.
			 */
			if (retry) {
				/* A retry occurred before. */
				*next_loopid = tmp_loopid;
			} else {
				/*
				 * No retry occurred before. Just increment the
				 * ID value for next login.
				 */
				*next_loopid = (fcport->loop_id + 1);
			}

			if (mb[1] & BIT_0) {
				fcport->port_type = FCT_INITIATOR;
			} else {
				fcport->port_type = FCT_TARGET;
				if (mb[1] & BIT_1) {
					fcport->flags |= FCF_TAPE_PRESENT;
				}
			}

			rval = QLA_SUCCESS;
			break;
		} else if (mb[0] == MBS_LOOP_ID_USED) {
			/*
			 * Loop ID already used, try next loop ID.
			 */
			fcport->loop_id++;
			rval = qla2x00_find_new_loop_id(ha, fcport);
			if (rval != QLA_SUCCESS) {
				/* Ran out of loop IDs to use */
				break;
			}
		} else if (mb[0] == MBS_COMMAND_ERROR) {
			/*
			 * Firmware possibly timed out during login. If NO
			 * retries are left to do then the device is declared
			 * dead.
			 */
			*next_loopid = fcport->loop_id;
			qla2x00_fabric_logout(ha, fcport->loop_id);
			qla2x00_mark_device_lost(ha, fcport, 1);

			rval = 1;
			break;
		} else {
			/*
			 * unrecoverable / not handled error
			 */
			DEBUG2(printk("%s(%ld): failed=%x port_id=%02x%02x%02x "
 			    "loop_id=%x jiffies=%lx.\n", 
 			    __func__, ha->host_no, mb[0], 
			    fcport->d_id.b.domain, fcport->d_id.b.area,
			    fcport->d_id.b.al_pa, fcport->loop_id, jiffies));

			*next_loopid = fcport->loop_id;
			qla2x00_fabric_logout(ha, fcport->loop_id);
			fcport->loop_id = FC_NO_LOOP_ID;
			atomic_set(&fcport->state, FCS_DEVICE_DEAD);

			rval = 3;
			break;
		}
	}

	return (rval);
}

/*
 * qla2x00_local_device_login
 *	Issue local device login command.
 *
 * Input:
 *	ha = adapter block pointer.
 *	loop_id = loop id of device to login to.
 *
 * Returns (Where's the #define!!!!):
 *      0 - Login successfully
 *      1 - Login failed
 *      3 - Fatal error
 */
int
qla2x00_local_device_login(scsi_qla_host_t *ha, uint16_t loop_id)
{
	int		rval;
	uint16_t	mb[MAILBOX_REGISTER_COUNT];

	memset(mb, 0, sizeof(mb));
	rval = qla2x00_login_local_device(ha, loop_id, mb, BIT_0);
	if (rval == QLA_SUCCESS) {
		/* Interrogate mailbox registers for any errors */
		if (mb[0] == MBS_COMMAND_ERROR)
			rval = 1;
		else if (mb[0] == MBS_COMMAND_PARAMETER_ERROR)
			/* device not in PCB table */
			rval = 3;
	}

	return (rval);
}

/*
 *  qla2x00_loop_resync
 *      Resync with fibre channel devices.
 *
 * Input:
 *      ha = adapter block pointer.
 *
 * Returns:
 *      0 = success
 */
int
qla2x00_loop_resync(scsi_qla_host_t *ha) 
{
	int   rval;
	uint32_t wait_time;

	rval = QLA_SUCCESS;

	atomic_set(&ha->loop_state, LOOP_UPDATE);
	clear_bit(ISP_ABORT_RETRY, &ha->dpc_flags);
	if (ha->flags.online) {
		if (!(rval = qla2x00_fw_ready(ha))) {
			/* Wait at most MAX_TARGET RSCNs for a stable link. */
			wait_time = 256;
			do {
				/* v2.19.05b6 */
				atomic_set(&ha->loop_state, LOOP_UPDATE);

				/*
				 * Issue marker command only when we are going
				 * to start the I/O .
				 */
				ha->marker_needed = 1;

				/* Remap devices on Loop. */
				clear_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags);

				qla2x00_configure_loop(ha);
				wait_time--;
			} while (!atomic_read(&ha->loop_down_timer) &&
				!(test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags)) &&
				wait_time &&
				(test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)));
		}
	}

	if (test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags)) {
		return (QLA_FUNCTION_FAILED);
	}

	if (rval) {
		DEBUG2_3(printk("%s(): **** FAILED ****\n", __func__));
	}

	return (rval);
}

void
qla2x00_rescan_fcports(scsi_qla_host_t *ha)
{
	int rescan_done;
	fc_port_t *fcport;

	rescan_done = 0;
	list_for_each_entry(fcport, &ha->fcports, list) {
		if ((fcport->flags & FCF_RESCAN_NEEDED) == 0)
			continue;

		qla2x00_update_fcport(ha, fcport);
		fcport->flags &= ~FCF_RESCAN_NEEDED;

		rescan_done = 1;
	}
	qla2x00_probe_for_all_luns(ha); 
}

/*
*  qla2x00_abort_isp
*      Resets ISP and aborts all outstanding commands.
*
* Input:
*      ha           = adapter block pointer.
*
* Returns:
*      0 = success
*/
int
qla2x00_abort_isp(scsi_qla_host_t *ha)
{
	unsigned long flags = 0;
	uint16_t       cnt;
	srb_t          *sp;
	uint8_t        status = 0;

	if (ha->flags.online) {
		ha->flags.online = 0;
		clear_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);

		qla_printk(KERN_INFO, ha,
		    "Performing ISP error recovery - ha= %p.\n", ha);
		qla2x00_reset_chip(ha);

		atomic_set(&ha->loop_down_timer, LOOP_DOWN_TIME);
		if (atomic_read(&ha->loop_state) != LOOP_DOWN) {
			atomic_set(&ha->loop_state, LOOP_DOWN);
			qla2x00_mark_all_devices_lost(ha);
		} else {
			if (!atomic_read(&ha->loop_down_timer))
				atomic_set(&ha->loop_down_timer,
				    LOOP_DOWN_TIME);
		}

		spin_lock_irqsave(&ha->hardware_lock, flags);
		/* Requeue all commands in outstanding command list. */
		for (cnt = 1; cnt < MAX_OUTSTANDING_COMMANDS; cnt++) {
			sp = ha->outstanding_cmds[cnt];
			if (sp) {
				ha->outstanding_cmds[cnt] = NULL;
				sp->flags = 0;
				sp->cmd->result = DID_RESET << 16;
				sp->cmd->host_scribble = (unsigned char *)NULL;
				qla2x00_sp_compl(ha, sp);
			}
		}
		spin_unlock_irqrestore(&ha->hardware_lock, flags);

		qla2x00_nvram_config(ha);

		if (!qla2x00_restart_isp(ha)) {
			clear_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);

			if (!atomic_read(&ha->loop_down_timer)) {
				/*
				 * Issue marker command only when we are going
				 * to start the I/O .
				 */
				ha->marker_needed = 1;
			}

			ha->flags.online = 1;

			/* Enable ISP interrupts. */
			qla2x00_enable_intrs(ha);

			ha->isp_abort_cnt = 0; 
			clear_bit(ISP_ABORT_RETRY, &ha->dpc_flags);
		} else {	/* failed the ISP abort */
			ha->flags.online = 1;
			if (test_bit(ISP_ABORT_RETRY, &ha->dpc_flags)) {
				if (ha->isp_abort_cnt == 0) {
 					qla_printk(KERN_WARNING, ha,
					    "ISP error recovery failed - "
					    "board disabled\n");
					/* 
					 * The next call disables the board
					 * completely.
					 */
					qla2x00_reset_adapter(ha);
					ha->flags.online = 0;
					clear_bit(ISP_ABORT_RETRY,
					    &ha->dpc_flags);
					status = 0;
				} else { /* schedule another ISP abort */
					ha->isp_abort_cnt--;
					DEBUG(printk("qla%ld: ISP abort - "
					    "retry remainning %d\n",
					    ha->host_no, ha->isp_abort_cnt);)
					status = 1;
				}
			} else {
				ha->isp_abort_cnt = MAX_RETRIES_OF_ISP_ABORT;
				DEBUG(printk("qla2x00(%ld): ISP error recovery "
				    "- retrying (%d) more times\n",
				    ha->host_no, ha->isp_abort_cnt);)
				set_bit(ISP_ABORT_RETRY, &ha->dpc_flags);
				status = 1;
			}
		}
		       
	}

	if (status) {
		qla_printk(KERN_INFO, ha,
			"qla2x00_abort_isp: **** FAILED ****\n");
	} else {
		DEBUG(printk(KERN_INFO
				"qla2x00_abort_isp(%ld): exiting.\n",
				ha->host_no);)
	}

	return(status);
}

/*
*  qla2x00_restart_isp
*      restarts the ISP after a reset
*
* Input:
*      ha = adapter block pointer.
*
* Returns:
*      0 = success
*/
static int
qla2x00_restart_isp(scsi_qla_host_t *ha)
{
	uint8_t		status = 0;
	device_reg_t __iomem *reg = ha->iobase;
	unsigned long	flags = 0;
	uint32_t wait_time;

	/* If firmware needs to be loaded */
	if (qla2x00_isp_firmware(ha)) {
		ha->flags.online = 0;
		if (!(status = qla2x00_chip_diag(ha))) {
			if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
				status = qla2x00_setup_chip(ha);
				goto done;
			}

			reg = ha->iobase;

			spin_lock_irqsave(&ha->hardware_lock, flags);

			/* Disable SRAM, Instruction RAM and GP RAM parity. */
			WRT_REG_WORD(&reg->hccr, (HCCR_ENABLE_PARITY + 0x0));
			RD_REG_WORD(&reg->hccr);	/* PCI Posting. */

			spin_unlock_irqrestore(&ha->hardware_lock, flags);
	
			status = qla2x00_setup_chip(ha);

			spin_lock_irqsave(&ha->hardware_lock, flags);
 
 			/* Enable proper parity */
 			if (IS_QLA2300(ha))
 				/* SRAM parity */
 				WRT_REG_WORD(&reg->hccr,
 				    (HCCR_ENABLE_PARITY + 0x1));
 			else
 				/* SRAM, Instruction RAM and GP RAM parity */
 				WRT_REG_WORD(&reg->hccr,
 				    (HCCR_ENABLE_PARITY + 0x7));
			RD_REG_WORD(&reg->hccr);	/* PCI Posting. */

			spin_unlock_irqrestore(&ha->hardware_lock, flags);
		}
	}

 done:
	if (!status && !(status = qla2x00_init_rings(ha))) {
		clear_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);
		if (!(status = qla2x00_fw_ready(ha))) {
			DEBUG(printk("%s(): Start configure loop, "
					"status = %d\n",
					__func__,
					status);)
			ha->flags.online = 1;
			/* Wait at most MAX_TARGET RSCNs for a stable link. */
			wait_time = 256;
			do {
				clear_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags);
				qla2x00_configure_loop(ha);
				wait_time--;
			} while (!atomic_read(&ha->loop_down_timer) &&
				!(test_bit(ISP_ABORT_NEEDED, &ha->dpc_flags)) &&
				wait_time &&
				(test_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags)));
		}

		/* if no cable then assume it's good */
		if ((ha->device_flags & DFLG_NO_CABLE)) 
			status = 0;

		DEBUG(printk("%s(): Configure loop done, status = 0x%x\n",
				__func__,
				status);)
	}
	return (status);
}

/*
* qla2x00_reset_adapter
*      Reset adapter.
*
* Input:
*      ha = adapter block pointer.
*/
static void
qla2x00_reset_adapter(scsi_qla_host_t *ha)
{
	unsigned long flags = 0;
	device_reg_t __iomem *reg = ha->iobase;

	ha->flags.online = 0;
	qla2x00_disable_intrs(ha);

	/* Reset RISC processor. */
	spin_lock_irqsave(&ha->hardware_lock, flags);
	WRT_REG_WORD(&reg->hccr, HCCR_RESET_RISC);
	RD_REG_WORD(&reg->hccr);			/* PCI Posting. */
	WRT_REG_WORD(&reg->hccr, HCCR_RELEASE_RISC);
	RD_REG_WORD(&reg->hccr);			/* PCI Posting. */
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}
