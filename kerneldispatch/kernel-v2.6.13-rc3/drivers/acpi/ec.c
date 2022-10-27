/*
 *  acpi_ec.c - ACPI Embedded Controller Driver ($Revision: 38 $)
 *
 *  Copyright (C) 2004 Luming Yu <luming.yu@intel.com>
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>
#include <acpi/actypes.h>

#define _COMPONENT		ACPI_EC_COMPONENT
ACPI_MODULE_NAME		("acpi_ec")

#define ACPI_EC_COMPONENT		0x00100000
#define ACPI_EC_CLASS			"embedded_controller"
#define ACPI_EC_HID			"PNP0C09"
#define ACPI_EC_DRIVER_NAME		"ACPI Embedded Controller Driver"
#define ACPI_EC_DEVICE_NAME		"Embedded Controller"
#define ACPI_EC_FILE_INFO		"info"


#define ACPI_EC_FLAG_OBF	0x01	/* Output buffer full */
#define ACPI_EC_FLAG_IBF	0x02	/* Input buffer full */
#define ACPI_EC_FLAG_BURST	0x10	/* burst mode */
#define ACPI_EC_FLAG_SCI	0x20	/* EC-SCI occurred */

#define ACPI_EC_EVENT_OBF	0x01	/* Output buffer full */
#define ACPI_EC_EVENT_IBE	0x02	/* Input buffer empty */

#define ACPI_EC_DELAY		50	/* Wait 50ms max. during EC ops */
#define ACPI_EC_UDELAY_GLK	1000	/* Wait 1ms max. to get global lock */

#define ACPI_EC_COMMAND_READ	0x80
#define ACPI_EC_COMMAND_WRITE	0x81
#define ACPI_EC_BURST_ENABLE	0x82
#define ACPI_EC_BURST_DISABLE	0x83
#define ACPI_EC_COMMAND_QUERY	0x84

static int acpi_ec_add (struct acpi_device *device);
static int acpi_ec_remove (struct acpi_device *device, int type);
static int acpi_ec_start (struct acpi_device *device);
static int acpi_ec_stop (struct acpi_device *device, int type);

static struct acpi_driver acpi_ec_driver = {
	.name =		ACPI_EC_DRIVER_NAME,
	.class =	ACPI_EC_CLASS,
	.ids =		ACPI_EC_HID,
	.ops =		{
				.add =		acpi_ec_add,
				.remove =	acpi_ec_remove,
				.start =	acpi_ec_start,
				.stop =		acpi_ec_stop,
			},
};

struct acpi_ec {
	acpi_handle			handle;
	unsigned long			uid;
	unsigned long			gpe_bit;
	struct acpi_generic_address	status_addr;
	struct acpi_generic_address	command_addr;
	struct acpi_generic_address	data_addr;
	unsigned long			global_lock;
	unsigned int			expect_event;
	atomic_t			leaving_burst; /* 0 : No, 1 : Yes, 2: abort*/
	atomic_t			pending_gpe;
	struct semaphore		sem;
	wait_queue_head_t		wait;
};

/* If we find an EC via the ECDT, we need to keep a ptr to its context */
static struct acpi_ec	*ec_ecdt;

/* External interfaces use first EC only, so remember */
static struct acpi_device *first_ec;

/* --------------------------------------------------------------------------
                             Transaction Management
   -------------------------------------------------------------------------- */

static inline u32 acpi_ec_read_status(struct acpi_ec *ec)
{
	u32	status = 0;

	acpi_hw_low_level_read(8, &status, &ec->status_addr);
	return status;
}

static int acpi_ec_wait(struct acpi_ec *ec, unsigned int event)
{
	int	result = 0;

	ACPI_FUNCTION_TRACE("acpi_ec_wait");

	ec->expect_event = event;
	smp_mb();

	result = wait_event_interruptible_timeout(ec->wait,
					!ec->expect_event,
					msecs_to_jiffies(ACPI_EC_DELAY));
	
	ec->expect_event = 0;
	smp_mb();

	if (result < 0){
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR," result  = %d ", result));
		return_VALUE(result);
	}

	/*
	 * Verify that the event in question has actually happened by
	 * querying EC status. Do the check even if operation timed-out
	 * to make sure that we did not miss interrupt.
	 */
	switch (event) {
	case ACPI_EC_EVENT_OBF:
		if (acpi_ec_read_status(ec) & ACPI_EC_FLAG_OBF)
			return_VALUE(0);
		break;

	case ACPI_EC_EVENT_IBE:
		if (~acpi_ec_read_status(ec) & ACPI_EC_FLAG_IBF)
			return_VALUE(0);
		break;
	}

	return_VALUE(-ETIME);
}



static int
acpi_ec_enter_burst_mode (
	struct acpi_ec		*ec)
{
	u32			tmp = 0;
	int			status = 0;

	ACPI_FUNCTION_TRACE("acpi_ec_enter_burst_mode");

	status = acpi_ec_read_status(ec);
	if (status != -EINVAL &&
		!(status & ACPI_EC_FLAG_BURST)){
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"entering burst mode \n"));
		acpi_hw_low_level_write(8, ACPI_EC_BURST_ENABLE, &ec->command_addr);
		status = acpi_ec_wait(ec, ACPI_EC_EVENT_OBF);
		if (status){
			acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
			ACPI_DEBUG_PRINT((ACPI_DB_ERROR," status = %d\n", status));
			return_VALUE(-EINVAL);
		}
		acpi_hw_low_level_read(8, &tmp, &ec->data_addr);
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		if(tmp != 0x90 ) {/* Burst ACK byte*/
			ACPI_DEBUG_PRINT((ACPI_DB_ERROR,"Ack failed \n"));
			return_VALUE(-EINVAL);
		}
	} else
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"already be in burst mode \n"));
	atomic_set(&ec->leaving_burst , 0);
	return_VALUE(0);
}

static int
acpi_ec_leave_burst_mode (
	struct acpi_ec		*ec)
{
	int			status =0;

	ACPI_FUNCTION_TRACE("acpi_ec_leave_burst_mode");

	atomic_set(&ec->leaving_burst , 1);
	status = acpi_ec_read_status(ec);
	if (status != -EINVAL &&
		(status & ACPI_EC_FLAG_BURST)){
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"leaving burst mode\n"));
		acpi_hw_low_level_write(8, ACPI_EC_BURST_DISABLE, &ec->command_addr);
		status = acpi_ec_wait(ec, ACPI_EC_FLAG_IBF);
		if (status){
			acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
			ACPI_DEBUG_PRINT((ACPI_DB_ERROR,"------->wait fail\n"));
			return_VALUE(-EINVAL);
		}
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		status = acpi_ec_read_status(ec);
		if (status != -EINVAL &&
			(status & ACPI_EC_FLAG_BURST)) {
			ACPI_DEBUG_PRINT((ACPI_DB_ERROR,"------->status fail\n"));
			return_VALUE(-EINVAL);
		}
	}else
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"already be in Non-burst mode \n"));
	ACPI_DEBUG_PRINT((ACPI_DB_INFO,"leaving burst mode\n"));

	return_VALUE(0);
}

static int
acpi_ec_read (
	struct acpi_ec		*ec,
	u8			address,
	u32			*data)
{
	int			status = 0;
	u32			glk;

	ACPI_FUNCTION_TRACE("acpi_ec_read");

	if (!ec || !data)
		return_VALUE(-EINVAL);

retry:
	*data = 0;

	if (ec->global_lock) {
		status = acpi_acquire_global_lock(ACPI_EC_UDELAY_GLK, &glk);
		if (ACPI_FAILURE(status))
			return_VALUE(-ENODEV);
	}

	WARN_ON(in_interrupt());
	down(&ec->sem);

	if(acpi_ec_enter_burst_mode(ec))
		goto end;

	acpi_hw_low_level_write(8, ACPI_EC_COMMAND_READ, &ec->command_addr);
	status = acpi_ec_wait(ec, ACPI_EC_EVENT_IBE);
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
	if (status) {
		goto end;
	}

	acpi_hw_low_level_write(8, address, &ec->data_addr);
	status= acpi_ec_wait(ec, ACPI_EC_EVENT_OBF);
	if (status){
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		goto end;
	}

	acpi_hw_low_level_read(8, data, &ec->data_addr);
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Read [%02x] from address [%02x]\n",
		*data, address));
	
end:
	acpi_ec_leave_burst_mode(ec);
	up(&ec->sem);

	if (ec->global_lock)
		acpi_release_global_lock(glk);

	if(atomic_read(&ec->leaving_burst) == 2){
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"aborted, retry ...\n"));
		while(atomic_read(&ec->pending_gpe)){
			msleep(1);	
		}
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		goto retry;
	}

	return_VALUE(status);
}


static int
acpi_ec_write (
	struct acpi_ec		*ec,
	u8			address,
	u8			data)
{
	int			status = 0;
	u32			glk;
	u32			tmp;

	ACPI_FUNCTION_TRACE("acpi_ec_write");

	if (!ec)
		return_VALUE(-EINVAL);
retry:
	if (ec->global_lock) {
		status = acpi_acquire_global_lock(ACPI_EC_UDELAY_GLK, &glk);
		if (ACPI_FAILURE(status))
			return_VALUE(-ENODEV);
	}

	WARN_ON(in_interrupt());
	down(&ec->sem);

	if(acpi_ec_enter_burst_mode(ec))
		goto end;

	status = acpi_ec_read_status(ec);
	if (status != -EINVAL &&
		!(status & ACPI_EC_FLAG_BURST)){
		acpi_hw_low_level_write(8, ACPI_EC_BURST_ENABLE, &ec->command_addr);
		status = acpi_ec_wait(ec, ACPI_EC_EVENT_OBF);
		if (status)
			goto end;
		acpi_hw_low_level_read(8, &tmp, &ec->data_addr);
		if(tmp != 0x90 ) /* Burst ACK byte*/
			goto end;
	}
	/*Now we are in burst mode*/

	acpi_hw_low_level_write(8, ACPI_EC_COMMAND_WRITE, &ec->command_addr);
	status = acpi_ec_wait(ec, ACPI_EC_EVENT_IBE);
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
	if (status){
		goto end;
	}

	acpi_hw_low_level_write(8, address, &ec->data_addr);
	status = acpi_ec_wait(ec, ACPI_EC_EVENT_IBE);
	if (status){
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		goto end;
	}

	acpi_hw_low_level_write(8, data, &ec->data_addr);
	status = acpi_ec_wait(ec, ACPI_EC_EVENT_IBE);
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
	if (status)
		goto end;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Wrote [%02x] to address [%02x]\n",
		data, address));

end:
	acpi_ec_leave_burst_mode(ec);
	up(&ec->sem);

	if (ec->global_lock)
		acpi_release_global_lock(glk);

	if(atomic_read(&ec->leaving_burst) == 2){
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"aborted, retry ...\n"));
		while(atomic_read(&ec->pending_gpe)){
			msleep(1);	
		}
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		goto retry;
	}

	return_VALUE(status);
}

/*
 * Externally callable EC access functions. For now, assume 1 EC only
 */
int
ec_read(u8 addr, u8 *val)
{
	struct acpi_ec *ec;
	int err;
	u32 temp_data;

	if (!first_ec)
		return -ENODEV;

	ec = acpi_driver_data(first_ec);

	err = acpi_ec_read(ec, addr, &temp_data);

	if (!err) {
		*val = temp_data;
		return 0;
	}
	else
		return err;
}
EXPORT_SYMBOL(ec_read);

int
ec_write(u8 addr, u8 val)
{
	struct acpi_ec *ec;
	int err;

	if (!first_ec)
		return -ENODEV;

	ec = acpi_driver_data(first_ec);

	err = acpi_ec_write(ec, addr, val);

	return err;
}
EXPORT_SYMBOL(ec_write);


static int
acpi_ec_query (
	struct acpi_ec		*ec,
	u32			*data)
{
	int			status = 0;
	u32			glk;

	ACPI_FUNCTION_TRACE("acpi_ec_query");

	if (!ec || !data)
		return_VALUE(-EINVAL);
	*data = 0;

	if (ec->global_lock) {
		status = acpi_acquire_global_lock(ACPI_EC_UDELAY_GLK, &glk);
		if (ACPI_FAILURE(status))
			return_VALUE(-ENODEV);
	}

	down(&ec->sem);
	if(acpi_ec_enter_burst_mode(ec))
		goto end;
	/*
	 * Query the EC to find out which _Qxx method we need to evaluate.
	 * Note that successful completion of the query causes the ACPI_EC_SCI
	 * bit to be cleared (and thus clearing the interrupt source).
	 */
	acpi_hw_low_level_write(8, ACPI_EC_COMMAND_QUERY, &ec->command_addr);
	status = acpi_ec_wait(ec, ACPI_EC_EVENT_OBF);
	if (status){
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		goto end;
	}

	acpi_hw_low_level_read(8, data, &ec->data_addr);
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
	if (!*data)
		status = -ENODATA;

end:
	acpi_ec_leave_burst_mode(ec);
	up(&ec->sem);

	if (ec->global_lock)
		acpi_release_global_lock(glk);

	if(atomic_read(&ec->leaving_burst) == 2){
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,"aborted, retry ...\n"));
		acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);
		status = -ENODATA;
	}
	return_VALUE(status);
}


/* --------------------------------------------------------------------------
                                Event Management
   -------------------------------------------------------------------------- */

struct acpi_ec_query_data {
	acpi_handle		handle;
	u8			data;
};

static void
acpi_ec_gpe_query (
	void			*ec_cxt)
{
	struct acpi_ec		*ec = (struct acpi_ec *) ec_cxt;
	u32			value;
	int			result = -ENODATA;
	static char		object_name[5] = {'_','Q','0','0','\0'};
	const char		hex[] = {'0','1','2','3','4','5','6','7',
				         '8','9','A','B','C','D','E','F'};

	ACPI_FUNCTION_TRACE("acpi_ec_gpe_query");

	if (acpi_ec_read_status(ec) & ACPI_EC_FLAG_SCI)
		result = acpi_ec_query(ec, &value);

	if (result)
		goto end;

	object_name[2] = hex[((value >> 4) & 0x0F)];
	object_name[3] = hex[(value & 0x0F)];

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Evaluating %s\n", object_name));

	acpi_evaluate_object(ec->handle, object_name, NULL, NULL);
end:	
	atomic_dec(&ec->pending_gpe);
	return;
}

static u32
acpi_ec_gpe_handler (
	void			*data)
{
	acpi_status		status = AE_OK;
	u32			value;
	struct acpi_ec		*ec = (struct acpi_ec *) data;

	if (!ec)
		return ACPI_INTERRUPT_NOT_HANDLED;

	acpi_disable_gpe(NULL, ec->gpe_bit, ACPI_ISR);

	value = acpi_ec_read_status(ec);

	if((value & ACPI_EC_FLAG_IBF) &&
		!(value & ACPI_EC_FLAG_BURST) &&
			(atomic_read(&ec->leaving_burst) == 0)) { 
	/*
	 * the embedded controller disables 
	 * burst mode for any reason other 
	 * than the burst disable command
	 * to process critical event.
	 */
		atomic_set(&ec->leaving_burst , 2); /* block current pending transaction
					and retry */
		wake_up(&ec->wait);
	}else {
		if ((ec->expect_event == ACPI_EC_EVENT_OBF &&
				(value & ACPI_EC_FLAG_OBF)) ||
	    			(ec->expect_event == ACPI_EC_EVENT_IBE &&
				!(value & ACPI_EC_FLAG_IBF))) {
			ec->expect_event = 0;
			wake_up(&ec->wait);
			return ACPI_INTERRUPT_HANDLED;
		}
	}

	if (value & ACPI_EC_FLAG_SCI){
		atomic_add(1, &ec->pending_gpe) ;
		status = acpi_os_queue_for_execution(OSD_PRIORITY_GPE,
						acpi_ec_gpe_query, ec);
		return status == AE_OK ?
		ACPI_INTERRUPT_HANDLED : ACPI_INTERRUPT_NOT_HANDLED;
	} 
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_ISR);
	return status == AE_OK ?
		ACPI_INTERRUPT_HANDLED : ACPI_INTERRUPT_NOT_HANDLED;
}

/* --------------------------------------------------------------------------
                             Address Space Management
   -------------------------------------------------------------------------- */

static acpi_status
acpi_ec_space_setup (
	acpi_handle		region_handle,
	u32			function,
	void			*handler_context,
	void			**return_context)
{
	/*
	 * The EC object is in the handler context and is needed
	 * when calling the acpi_ec_space_handler.
	 */
	*return_context  = (function != ACPI_REGION_DEACTIVATE) ?
						handler_context : NULL;

	return AE_OK;
}


static acpi_status
acpi_ec_space_handler (
	u32			function,
	acpi_physical_address	address,
	u32			bit_width,
	acpi_integer		*value,
	void			*handler_context,
	void			*region_context)
{
	int			result = 0;
	struct acpi_ec		*ec = NULL;
	u64			temp = *value;
	acpi_integer		f_v = 0;
	int 			i = 0;

	ACPI_FUNCTION_TRACE("acpi_ec_space_handler");

	if ((address > 0xFF) || !value || !handler_context)
		return_VALUE(AE_BAD_PARAMETER);

	if (bit_width != 8 && acpi_strict) {
		printk(KERN_WARNING PREFIX "acpi_ec_space_handler: bit_width should be 8\n");
		return_VALUE(AE_BAD_PARAMETER);
	}

	ec = (struct acpi_ec *) handler_context;

next_byte:
	switch (function) {
	case ACPI_READ:
		temp = 0;
		result = acpi_ec_read(ec, (u8) address, (u32 *)&temp);
		break;
	case ACPI_WRITE:
		result = acpi_ec_write(ec, (u8) address, (u8) temp);
		break;
	default:
		result = -EINVAL;
		goto out;
		break;
	}

	bit_width -= 8;
	if (bit_width) {
		if (function == ACPI_READ)
			f_v |= temp << 8 * i;
		if (function == ACPI_WRITE)
			temp >>= 8;
		i++;
		address++;
		goto next_byte;
	}

	if (function == ACPI_READ) {
		f_v |= temp << 8 * i;
		*value = f_v;
	}

		
out:
	switch (result) {
	case -EINVAL:
		return_VALUE(AE_BAD_PARAMETER);
		break;
	case -ENODEV:
		return_VALUE(AE_NOT_FOUND);
		break;
	case -ETIME:
		return_VALUE(AE_TIME);
		break;
	default:
		return_VALUE(AE_OK);
	}
}


/* --------------------------------------------------------------------------
                              FS Interface (/proc)
   -------------------------------------------------------------------------- */

static struct proc_dir_entry	*acpi_ec_dir;


static int
acpi_ec_read_info (struct seq_file *seq, void *offset)
{
	struct acpi_ec		*ec = (struct acpi_ec *) seq->private;

	ACPI_FUNCTION_TRACE("acpi_ec_read_info");

	if (!ec)
		goto end;

	seq_printf(seq, "gpe bit:                 0x%02x\n",
		(u32) ec->gpe_bit);
	seq_printf(seq, "ports:                   0x%02x, 0x%02x\n",
		(u32) ec->status_addr.address, (u32) ec->data_addr.address);
	seq_printf(seq, "use global lock:         %s\n",
		ec->global_lock?"yes":"no");
	acpi_enable_gpe(NULL, ec->gpe_bit, ACPI_NOT_ISR);

end:
	return_VALUE(0);
}

static int acpi_ec_info_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, acpi_ec_read_info, PDE(inode)->data);
}

static struct file_operations acpi_ec_info_ops = {
	.open		= acpi_ec_info_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.owner = THIS_MODULE,
};

static int
acpi_ec_add_fs (
	struct acpi_device	*device)
{
	struct proc_dir_entry	*entry;

	ACPI_FUNCTION_TRACE("acpi_ec_add_fs");

	if (!acpi_device_dir(device)) {
		acpi_device_dir(device) = proc_mkdir(acpi_device_bid(device),
			acpi_ec_dir);
		if (!acpi_device_dir(device))
			return_VALUE(-ENODEV);
	}

	entry = create_proc_entry(ACPI_EC_FILE_INFO, S_IRUGO,
		acpi_device_dir(device));
	if (!entry)
		ACPI_DEBUG_PRINT((ACPI_DB_WARN,
			"Unable to create '%s' fs entry\n",
			ACPI_EC_FILE_INFO));
	else {
		entry->proc_fops = &acpi_ec_info_ops;
		entry->data = acpi_driver_data(device);
		entry->owner = THIS_MODULE;
	}

	return_VALUE(0);
}


static int
acpi_ec_remove_fs (
	struct acpi_device	*device)
{
	ACPI_FUNCTION_TRACE("acpi_ec_remove_fs");

	if (acpi_device_dir(device)) {
		remove_proc_entry(ACPI_EC_FILE_INFO, acpi_device_dir(device));
		remove_proc_entry(acpi_device_bid(device), acpi_ec_dir);
		acpi_device_dir(device) = NULL;
	}

	return_VALUE(0);
}


/* --------------------------------------------------------------------------
                               Driver Interface
   -------------------------------------------------------------------------- */

static int
acpi_ec_add (
	struct acpi_device	*device)
{
	int			result;
	acpi_status		status;
	struct acpi_ec		*ec;
	unsigned long		uid;

	ACPI_FUNCTION_TRACE("acpi_ec_add");

	if (!device)
		return_VALUE(-EINVAL);

	ec = kmalloc(sizeof(struct acpi_ec), GFP_KERNEL);
	if (!ec)
		return_VALUE(-ENOMEM);
	memset(ec, 0, sizeof(struct acpi_ec));

	ec->handle = device->handle;
	ec->uid = -1;
 	atomic_set(&ec->pending_gpe, 0);
 	atomic_set(&ec->leaving_burst , 1);
 	init_MUTEX(&ec->sem);
 	init_waitqueue_head(&ec->wait);
	strcpy(acpi_device_name(device), ACPI_EC_DEVICE_NAME);
	strcpy(acpi_device_class(device), ACPI_EC_CLASS);
	acpi_driver_data(device) = ec;

	/* Use the global lock for all EC transactions? */
	acpi_evaluate_integer(ec->handle, "_GLK", NULL, &ec->global_lock);

	/* If our UID matches the UID for the ECDT-enumerated EC,
	   we now have the *real* EC info, so kill the makeshift one.*/
	acpi_evaluate_integer(ec->handle, "_UID", NULL, &uid);
	if (ec_ecdt && ec_ecdt->uid == uid) {
		acpi_remove_address_space_handler(ACPI_ROOT_OBJECT,
			ACPI_ADR_SPACE_EC, &acpi_ec_space_handler);

		acpi_remove_gpe_handler(NULL, ec_ecdt->gpe_bit, &acpi_ec_gpe_handler);

		kfree(ec_ecdt);
	}

	/* Get GPE bit assignment (EC events). */
	/* TODO: Add support for _GPE returning a package */
	status = acpi_evaluate_integer(ec->handle, "_GPE", NULL, &ec->gpe_bit);
	if (ACPI_FAILURE(status)) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR,
			"Error obtaining GPE bit assignment\n"));
		result = -ENODEV;
		goto end;
	}

	result = acpi_ec_add_fs(device);
	if (result)
		goto end;

	printk(KERN_INFO PREFIX "%s [%s] (gpe %d)\n",
		acpi_device_name(device), acpi_device_bid(device),
		(u32) ec->gpe_bit);

	if (!first_ec)
		first_ec = device;

end:
	if (result)
		kfree(ec);

	return_VALUE(result);
}


static int
acpi_ec_remove (
	struct acpi_device	*device,
	int			type)
{
	struct acpi_ec		*ec;

	ACPI_FUNCTION_TRACE("acpi_ec_remove");

	if (!device)
		return_VALUE(-EINVAL);

	ec = acpi_driver_data(device);

	acpi_ec_remove_fs(device);

	kfree(ec);

	return_VALUE(0);
}


static acpi_status
acpi_ec_io_ports (
	struct acpi_resource	*resource,
	void			*context)
{
	struct acpi_ec		*ec = (struct acpi_ec *) context;
	struct acpi_generic_address *addr;

	if (resource->id != ACPI_RSTYPE_IO) {
		return AE_OK;
	}

	/*
	 * The first address region returned is the data port, and
	 * the second address region returned is the status/command
	 * port.
	 */
	if (ec->data_addr.register_bit_width == 0) {
		addr = &ec->data_addr;
	} else if (ec->command_addr.register_bit_width == 0) {
		addr = &ec->command_addr;
	} else {
		return AE_CTRL_TERMINATE;
	}

	addr->address_space_id = ACPI_ADR_SPACE_SYSTEM_IO;
	addr->register_bit_width = 8;
	addr->register_bit_offset = 0;
	addr->address = resource->data.io.min_base_address;

	return AE_OK;
}


static int
acpi_ec_start (
	struct acpi_device	*device)
{
	acpi_status		status;
	struct acpi_ec		*ec;

	ACPI_FUNCTION_TRACE("acpi_ec_start");

	if (!device)
		return_VALUE(-EINVAL);

	ec = acpi_driver_data(device);

	if (!ec)
		return_VALUE(-EINVAL);

	/*
	 * Get I/O port addresses. Convert to GAS format.
	 */
	status = acpi_walk_resources(ec->handle, METHOD_NAME__CRS,
		acpi_ec_io_ports, ec);
	if (ACPI_FAILURE(status) || ec->command_addr.register_bit_width == 0) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "Error getting I/O port addresses"));
		return_VALUE(-ENODEV);
	}

	ec->status_addr = ec->command_addr;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "gpe=0x%02x, ports=0x%2x,0x%2x\n",
		(u32) ec->gpe_bit, (u32) ec->command_addr.address,
		(u32) ec->data_addr.address));

	/*
	 * Install GPE handler
	 */
	status = acpi_install_gpe_handler(NULL, ec->gpe_bit,
		ACPI_GPE_EDGE_TRIGGERED, &acpi_ec_gpe_handler, ec);
	if (ACPI_FAILURE(status)) {
		return_VALUE(-ENODEV);
	}
	acpi_set_gpe_type (NULL, ec->gpe_bit, ACPI_GPE_TYPE_RUNTIME);
	acpi_enable_gpe (NULL, ec->gpe_bit, ACPI_NOT_ISR);

	status = acpi_install_address_space_handler (ec->handle,
			ACPI_ADR_SPACE_EC, &acpi_ec_space_handler,
			&acpi_ec_space_setup, ec);
	if (ACPI_FAILURE(status)) {
		acpi_remove_gpe_handler(NULL, ec->gpe_bit, &acpi_ec_gpe_handler);
		return_VALUE(-ENODEV);
	}

	return_VALUE(AE_OK);
}


static int
acpi_ec_stop (
	struct acpi_device	*device,
	int			type)
{
	acpi_status		status;
	struct acpi_ec		*ec;

	ACPI_FUNCTION_TRACE("acpi_ec_stop");

	if (!device)
		return_VALUE(-EINVAL);

	ec = acpi_driver_data(device);

	status = acpi_remove_address_space_handler(ec->handle,
		ACPI_ADR_SPACE_EC, &acpi_ec_space_handler);
	if (ACPI_FAILURE(status))
		return_VALUE(-ENODEV);

	status = acpi_remove_gpe_handler(NULL, ec->gpe_bit, &acpi_ec_gpe_handler);
	if (ACPI_FAILURE(status))
		return_VALUE(-ENODEV);

	return_VALUE(0);
}

static acpi_status __init
acpi_fake_ecdt_callback (
	acpi_handle	handle,
	u32		Level,
	void		*context,
	void		**retval)
{
	acpi_status	status;

	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
		acpi_ec_io_ports, ec_ecdt);
	if (ACPI_FAILURE(status))
		return status;
	ec_ecdt->status_addr = ec_ecdt->command_addr;

	ec_ecdt->uid = -1;
	acpi_evaluate_integer(handle, "_UID", NULL, &ec_ecdt->uid);

	status = acpi_evaluate_integer(handle, "_GPE", NULL, &ec_ecdt->gpe_bit);
	if (ACPI_FAILURE(status))
		return status;
	ec_ecdt->global_lock = TRUE;
	ec_ecdt->handle = handle;

	printk(KERN_INFO PREFIX  "GPE=0x%02x, ports=0x%2x, 0x%2x\n",
		(u32) ec_ecdt->gpe_bit, (u32) ec_ecdt->command_addr.address,
		(u32) ec_ecdt->data_addr.address);

	return AE_CTRL_TERMINATE;
}

/*
 * Some BIOS (such as some from Gateway laptops) access EC region very early
 * such as in BAT0._INI or EC._INI before an EC device is found and
 * do not provide an ECDT. According to ACPI spec, ECDT isn't mandatorily
 * required, but if EC regison is accessed early, it is required.
 * The routine tries to workaround the BIOS bug by pre-scan EC device
 * It assumes that _CRS, _HID, _GPE, _UID methods of EC don't touch any
 * op region (since _REG isn't invoked yet). The assumption is true for
 * all systems found.
 */
static int __init
acpi_ec_fake_ecdt(void)
{
	acpi_status	status;
	int		ret = 0;

	printk(KERN_INFO PREFIX "Try to make an fake ECDT\n");

	ec_ecdt = kmalloc(sizeof(struct acpi_ec), GFP_KERNEL);
	if (!ec_ecdt) {
		ret = -ENOMEM;
		goto error;
	}
	memset(ec_ecdt, 0, sizeof(struct acpi_ec));

	status = acpi_get_devices (ACPI_EC_HID,
				acpi_fake_ecdt_callback,
				NULL,
				NULL);
	if (ACPI_FAILURE(status)) {
		kfree(ec_ecdt);
		ec_ecdt = NULL;
		ret = -ENODEV;
		goto error;
	}
	return 0;
error:
	printk(KERN_ERR PREFIX "Can't make an fake ECDT\n");
	return ret;
}

static int __init
acpi_ec_get_real_ecdt(void)
{
	acpi_status		status;
	struct acpi_table_ecdt 	*ecdt_ptr;

	status = acpi_get_firmware_table("ECDT", 1, ACPI_LOGICAL_ADDRESSING,
		(struct acpi_table_header **) &ecdt_ptr);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	printk(KERN_INFO PREFIX "Found ECDT\n");

	/*
	 * Generate a temporary ec context to use until the namespace is scanned
	 */
	ec_ecdt = kmalloc(sizeof(struct acpi_ec), GFP_KERNEL);
	if (!ec_ecdt)
		return -ENOMEM;
	memset(ec_ecdt, 0, sizeof(struct acpi_ec));

 	init_MUTEX(&ec_ecdt->sem);
 	init_waitqueue_head(&ec_ecdt->wait);
	ec_ecdt->command_addr = ecdt_ptr->ec_control;
	ec_ecdt->status_addr = ecdt_ptr->ec_control;
	ec_ecdt->data_addr = ecdt_ptr->ec_data;
	ec_ecdt->gpe_bit = ecdt_ptr->gpe_bit;
	/* use the GL just to be safe */
	ec_ecdt->global_lock = TRUE;
	ec_ecdt->uid = ecdt_ptr->uid;

	status = acpi_get_handle(NULL, ecdt_ptr->ec_id, &ec_ecdt->handle);
	if (ACPI_FAILURE(status)) {
		goto error;
	}

	return 0;
error:
	printk(KERN_ERR PREFIX "Could not use ECDT\n");
	kfree(ec_ecdt);
	ec_ecdt = NULL;

	return -ENODEV;
}

static int __initdata acpi_fake_ecdt_enabled;
int __init
acpi_ec_ecdt_probe (void)
{
	acpi_status		status;
	int			ret;

	ret = acpi_ec_get_real_ecdt();
	/* Try to make a fake ECDT */
	if (ret && acpi_fake_ecdt_enabled) {
		ret = acpi_ec_fake_ecdt();
	}

	if (ret)
		return 0;

	/*
	 * Install GPE handler
	 */
	status = acpi_install_gpe_handler(NULL, ec_ecdt->gpe_bit,
		ACPI_GPE_EDGE_TRIGGERED, &acpi_ec_gpe_handler,
		ec_ecdt);
	if (ACPI_FAILURE(status)) {
		goto error;
	}
	acpi_set_gpe_type (NULL, ec_ecdt->gpe_bit, ACPI_GPE_TYPE_RUNTIME);
	acpi_enable_gpe (NULL, ec_ecdt->gpe_bit, ACPI_NOT_ISR);

	status = acpi_install_address_space_handler (ACPI_ROOT_OBJECT,
			ACPI_ADR_SPACE_EC, &acpi_ec_space_handler,
			&acpi_ec_space_setup, ec_ecdt);
	if (ACPI_FAILURE(status)) {
		acpi_remove_gpe_handler(NULL, ec_ecdt->gpe_bit,
			&acpi_ec_gpe_handler);
		goto error;
	}

	return 0;

error:
	printk(KERN_ERR PREFIX "Could not use ECDT\n");
	kfree(ec_ecdt);
	ec_ecdt = NULL;

	return -ENODEV;
}


static int __init acpi_ec_init (void)
{
	int			result;

	ACPI_FUNCTION_TRACE("acpi_ec_init");

	if (acpi_disabled)
		return_VALUE(0);

	acpi_ec_dir = proc_mkdir(ACPI_EC_CLASS, acpi_root_dir);
	if (!acpi_ec_dir)
		return_VALUE(-ENODEV);

	/* Now register the driver for the EC */
	result = acpi_bus_register_driver(&acpi_ec_driver);
	if (result < 0) {
		remove_proc_entry(ACPI_EC_CLASS, acpi_root_dir);
		return_VALUE(-ENODEV);
	}

	return_VALUE(result);
}

subsys_initcall(acpi_ec_init);

/* EC driver currently not unloadable */
#if 0
static void __exit
acpi_ec_exit (void)
{
	ACPI_FUNCTION_TRACE("acpi_ec_exit");

	acpi_bus_unregister_driver(&acpi_ec_driver);

	remove_proc_entry(ACPI_EC_CLASS, acpi_root_dir);

	return_VOID;
}
#endif /* 0 */

static int __init acpi_fake_ecdt_setup(char *str)
{
	acpi_fake_ecdt_enabled = 1;
	return 0;
}
__setup("acpi_fake_ecdt", acpi_fake_ecdt_setup);
