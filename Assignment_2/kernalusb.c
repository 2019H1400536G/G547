#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>


#define PENDRIVE_VID  0x0781
#define PENDRIVE_PID  0x5567
#define READ_CAPACITY_LENGTH          0x08
#define LIBUSB_ENDPOINT_IN          0x80
#define LIBUSB_REQUEST_TYPE_CLASS    (0x01<<5)
#define LIBUSB_RECIPIENT_INTERFACE   0x01
#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])
#define BOMS_RESET                    0xFF
#define BOMS_RESET_REQ_TYPE           0x21
#define BOMS_GET_MAX_LUN              0xFE
#define BOMS_GET_MAX_LUN_REQ_TYPE     0xA1
#define REQUEST_DATA_LENGTH           0x12
#define LIBUSB_ERROR_PIPE          -9
#define LIBUSB_SUCCESS  0
#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])


// Section 5.1: Command Block Wrapper (CBW)
struct command_block_wrapper {
	uint8_t dCBWSignature[4];
	uint32_t dCBWTag;
	uint32_t dCBWDataTransferLength;
	uint8_t bmCBWFlags;
	uint8_t bCBWLUN;
	uint8_t bCBWCBLength;
	uint8_t CBWCB[16];
};

// Section 5.2: Command Status Wrapper (CSW)
struct command_status_wrapper {
	uint8_t dCSWSignature[4];
	uint32_t dCSWTag;
	uint32_t dCSWDataResidue;
	uint8_t bCSWStatus;
};

static uint8_t cdb_length[256] = {
//	 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  0
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  1
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  2
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  3
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  4
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  5
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  6
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  7
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  8
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  9
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  A
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  B
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  C
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  D
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  E
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  F
};

static void usbdev_disconnect(struct usb_interface *interface)
{
	printk(KERN_INFO "USBDEV Device is Removed\n");
	return;
}

static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(PENDRIVE_VID , PENDRIVE_PID)},
	{} /*terminating entry*/	
};

static int get_mass_storage_status(struct usb_device *device, uint8_t endpoint, uint32_t expected_tag)
{	struct command_status_wrapper *csw;
	int r,size;
	csw=(struct command_status_wrapper *)kmalloc(sizeof(struct command_status_wrapper),GFP_KERNEL);
	r=usb_bulk_msg(device,usb_rcvbulkpipe(device,endpoint),(void*)csw,13, &size, 1000);
	if(r<0)
		printk("ERROR IN RECIVING STATUS MESG %d",r);
	if (size != 13) {
		printk("   get_mass_storage_status: received %d bytes (expected 13)\n", size);
		return -1;
	}	
	if (csw->dCSWTag != expected_tag) {
		printk("   get_mass_storage_status: mismatched tags (expected %08X, received %08X)\n",
			expected_tag, csw->dCSWTag);
		return -1;
	}
	
	printk(KERN_INFO "Mass Storage Status: %02X (%s)\n", csw->bCSWStatus, csw->bCSWStatus?"FAILED":"Success");
    if (csw->dCSWTag != expected_tag)
		return -1;
	if (csw->bCSWStatus) {
		// REQUEST SENSE is appropriate only if bCSWStatus is 1, meaning that the
		// command failed somehow.  Larger values (2 in particular) mean that
		// the command couldn't be understood.
		if (csw->bCSWStatus == 1)
			return -2;	// request Get Sense
		else
			return -1;
	}	

	return 0;
}

static int send_mass_storage_command(struct usb_device *device, uint8_t endpoint, uint8_t lun,
	uint8_t *cdb, uint8_t direction, int data_length, uint32_t tag)
{

	uint8_t cdb_len;
	int a, size;
	struct command_block_wrapper *cbw;
	cbw=(struct command_block_wrapper *)kmalloc(sizeof(struct command_block_wrapper),GFP_KERNEL);
	

	if (cdb == NULL) {
		return -1;
	}

	if (endpoint & USB_DIR_IN) {
		printk(KERN_INFO "send_mass_storage_command: cannot send command on IN endpoint\n");
		return -1;
	}

	cdb_len = cdb_length[cdb[0]];
	if ((cdb_len == 0) || (cdb_len > sizeof(cbw->CBWCB))) {
		printk(KERN_INFO "send_mass_storage_command: don't know how to handle this command (%02X, length %d)\n",
			cdb[0], cdb_len);
		return -1;
	}

	memset(cbw, 0, sizeof(*cbw));
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';
	
	cbw->dCBWTag = tag;
	cbw->dCBWDataTransferLength = data_length;
	cbw->bmCBWFlags = direction;
	cbw->bCBWLUN = lun;
	// Subclass is 1 or 6 => cdb_len
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);

		// The transfer length must always be exactly 31 bytes.
		a = usb_bulk_msg(device, usb_sndbulkpipe(device,endpoint), (void*)cbw, 31, &size, 1000);
        if(a!=0)
		    printk("Failed command transfer %d",a);
	    else 	
	    	printk("read capacity command sent successfully");
	
	    printk(KERN_INFO"sent %d CDB bytes\n", cdb_len);
	    printk(KERN_INFO"sent %d bytes \n",size);

	return 0;
}

// Mass Storage device to test bulk transfers (non destructive test)
static int test_mass_storage (struct usb_device *device , uint8_t endpoint_in, uint8_t endpoint_out)
{
	int j=0, b=0,size,c=0;
	uint8_t *lun=(uint8_t *)kmalloc(sizeof(uint8_t),GFP_KERNEL);
	uint32_t expected_tag=1;
	uint32_t max_lba, block_size;
	long device_size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint8_t *buffer=(uint8_t *)kmalloc(64*sizeof(uint8_t),GFP_KERNEL);
	//char vid[9], pid[9], rev[5];
	//unsigned char *data;
	//FILE *fd;
    
    printk("Reset mass storage device");
	b = usb_control_msg(device,usb_sndctrlpipe(device,0),BOMS_RESET,BOMS_RESET_REQ_TYPE,0,0,NULL,0,1000);
	if(b<0)
		printk("error code: %d",b);
	else
		printk("successful Reset");

	printk(KERN_INFO "Reading Max LUN: %d\n",*lun);
	j = usb_control_msg(device,usb_sndctrlpipe(device,0), BOMS_GET_MAX_LUN ,BOMS_GET_MAX_LUN_REQ_TYPE,
	   0, 0, (void*)lun, 1, 1000);

	// Some devices send a STALL instead of the actual value.
	// In such cases we should set lun to 0.
	if (j == 0) {
		*lun = 0;
	} else if (j < 0) {
		printk(KERN_INFO "   Failed: %d\n", j);
	}
	//printk(KERN_INFO "Success: %d\n", j);
	printk(KERN_INFO "   Max LUN = %d, j= %d\n", *lun,j);
    

  

	// Read capacity
	printk(KERN_INFO "Reading Capacity:\n");
	memset(buffer, 0, sizeof(*buffer));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x25;	// Read Capacity

	send_mass_storage_command(device, endpoint_out, *lun, cdb, USB_DIR_IN, READ_CAPACITY_LENGTH, expected_tag);
	c = usb_bulk_msg(device, usb_rcvbulkpipe(device,endpoint_in), (void*)buffer, 64, &size, 10000);
	if(c<0)
		printk(KERN_INFO "status of c %d",c);
	printk(KERN_INFO "   received %d bytes\n", size);
	printk(KERN_INFO"value of &bufer[0] %d\n",buffer[0]);
	max_lba = be_to_int32(&buffer[0]);
	block_size = be_to_int32(&buffer[4]);
	device_size = ((long)(max_lba+1))*block_size/(1024*1024*1024);
	printk("max_lba: %x,block_size found: %x,Device size found : %ld GB \n",max_lba,block_size ,device_size);
	//printk(KERN_INFO "   Max LBA: %08X\n" /*Block Size: %08X (%.2f GB)\n*/, max_lba);
	get_mass_storage_status(device, endpoint_in, expected_tag);

	return 0;

}



static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{   
	
	struct usb_endpoint_descriptor *endpoint;
	unsigned char epAddr, epAttr;
	
	int t;
	int nb_ifaces;
	
	uint8_t endpoint_in = 0, endpoint_out = 0;
	// default IN and OUT endpoints
	struct usb_device *device;
	device=interface_to_usbdev(interface);
	//printf("Opening device %04X:%04X...\n", vid, pid);
    if(id->idProduct == PENDRIVE_PID && id->idVendor==PENDRIVE_VID)
	{
		printk(KERN_INFO "known USB drive detected \n");
	}
	printk(KERN_INFO "\nReading device descriptor:\n");
	printk(KERN_INFO "USB VID: %x\n",id->idVendor);
	printk(KERN_INFO "USB PID: %x\n",id->idProduct);
	printk(KERN_INFO "USB DEVICE CLASS : %x", interface->cur_altsetting->desc.bInterfaceClass);
	printk(KERN_INFO "USB DEVICE SUB CLASS : %x", interface->cur_altsetting->desc.bInterfaceSubClass);
	printk(KERN_INFO "USB DEVICE Protocol : %x", interface->cur_altsetting->desc.bInterfaceProtocol);

	nb_ifaces = device->config->desc.bNumInterfaces;
	printk(KERN_INFO "             nb interfaces: %d\n", nb_ifaces);
	

			
			// Check if the device is USB attaced SCSI type Mass storage class
			if ( (interface->cur_altsetting->desc.bInterfaceClass == 0x08)
			  && (interface->cur_altsetting->desc.bInterfaceSubClass == 0x06) 
			  && (interface->cur_altsetting->desc.bInterfaceProtocol == 0x50) ) 
			{   printk(KERN_INFO "valid SCSI device");
				// Mass storage devices that can use basic SCSI commands
				//test_mode = USE_SCSI;
			}
			printk(KERN_INFO "number of endpoints= %d\n",interface->cur_altsetting->desc.bNumEndpoints);
			

			for (t=0; t<interface->cur_altsetting->desc.bNumEndpoints; t++) 
			{
				endpoint = &interface->cur_altsetting->endpoint[t].desc;
				printk(KERN_INFO "       endpoint[%d].address: %02X\n", t, endpoint->bEndpointAddress);
				// Use the first interrupt or bulk IN/OUT endpoints as default for testing
				
			 	epAddr = endpoint->bEndpointAddress;//now after the endpoint is assigned to its discriptor now accesing bEndpointAddress member of that descriptor.
	        	epAttr = endpoint->bmAttributes;
	
		        if((epAttr & USB_ENDPOINT_XFERTYPE_MASK)==USB_ENDPOINT_XFER_BULK)
		        {
			       if(epAddr & 0x80)///masking d7 bit of bEndpointAddres and checkiing if its 1 then endpoint dir is in else out
				     {   
				     	 endpoint_in= endpoint->bEndpointAddress;
				         printk(KERN_INFO "EP %d is Bulk IN\n", t);
				     }
			       else
				      {   
				      	  endpoint_out= endpoint->bEndpointAddress;
    				      printk(KERN_INFO "EP %d is Bulk OUT\n", t);
				      }
	
		        } 
    
				printk(KERN_INFO "           max packet size: %04X\n", endpoint->wMaxPacketSize);
				printk(KERN_INFO "          polling interval: %02X\n", endpoint->bInterval);
				
			}

	            test_mass_storage(device, endpoint_in, endpoint_out);

	return 0;
}


/*Operations structure*/
static struct usb_driver usbdev_driver = {
	name: "usbdev",  //name of the device
	probe: usbdev_probe, // Whenever Device is plugged in
	disconnect: usbdev_disconnect, // When we remove a device
	id_table: usbdev_table, //  List of devices served by this driver
};


int device_init(void)
{
	usb_register(&usbdev_driver);
	printk(KERN_INFO "UAS READ CAPACITY DRIVER INSERTED");
	return 0;
}

void device_exit(void)
{
	usb_deregister(&usbdev_driver);
	printk(KERN_NOTICE "Leaving Kernel\n");
	//return 0;
}

module_init(device_init);
module_exit(device_exit);
MODULE_LICENSE("GPL");

