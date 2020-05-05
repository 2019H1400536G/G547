#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/module.h>
#include<linux/slab.h>
#include<linux/usb.h>
#include<linux/blkdev.h>
#include<linux/genhd.h>
#include<linux/spinlock.h>
//#include <asm/highmem.h>
#include<linux/bio.h>
#include<linux/fs.h>
#include<linux/interrupt.h>
#include<linux/workqueue.h>
#include<linux/sched.h>

#define DEVICE_NAME "myusbDriver"
#define NR_OF_SECTORS 15633408
#define SECTOR_SIZE 512
#define CARD_CAPACITY  (NR_OF_SECTORS*SECTOR_SIZE)
#define bio_iovec_idx(bio, idx)	(&((bio)->bi_io_vec[(idx)]))
#define __bio_kmap_atomic(bio, idx, kmtype)				\
	(kmap_atomic(bio_iovec_idx((bio), (idx))->bv_page) +	\
		bio_iovec_idx((bio), (idx))->bv_offset)


#define __bio_kunmap_atomic(addr, kmtype) kunmap_atomic(addr)


#define PENDRIVE_VID  0x0781
#define PENDRIVE_PID  0x5567

#define BOMS_RESET                    0xFF
#define BOMS_RESET_REQ_TYPE           0x21
#define BOMS_GET_MAX_LUN              0xFE
#define BOMS_GET_MAX_LUN_REQ_TYPE     0xA1
#define READ_CAPACITY_LENGTH	      0x08
#define REQUEST_DATA_LENGTH           0x12
#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])


struct gendisk *usb_disk = NULL;
struct command_block_wrapper {
	uint8_t dCBWSignature[4];
	uint32_t dCBWTag;
	uint32_t dCBWDataTransferLength;
	uint8_t bmCBWFlags;
	uint8_t bCBWLUN;
	uint8_t bCBWCBLength;
	uint8_t CBWCB[16];
};

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


static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(PENDRIVE_VID,PENDRIVE_PID)},
	{} /*terminating entry*/	
};

struct usb_device *udev;
uint8_t endpoint_in , endpoint_out ;
//uint8_t *lun;
//lun=(uint8_t *)kmalloc(sizeof(uint8_t),GFP_KERNEL);
struct blkdev_private{
        int size;                       /* Device size in sectors */
        u8 *data;                       /* The data array */
        short users;                    /* How many users */
        short media_change;             /* Flag a media change? */
        spinlock_t lock;                /* For mutual exclusion */
        struct request_queue *queue;    /* The device request queue */
        struct gendisk *gd;             /* The gendisk structure */
       // struct timer_list timer;        /* For simulated media changes */
};	


struct request *req;
static struct blkdev_private *p_blkdev = NULL;
struct dev_work{  
	struct work_struct work; // kernel specific struct
	struct request *req;
 };


static void usbdev_disconnect(struct usb_interface *interface);
static int get_mass_storage_status(struct usb_device *udev, uint8_t endpoint, uint32_t expected_tag);
static int send_command(struct usb_device *udev,uint8_t endpoint,
                         uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag);
static int read_disk(sector_t initial_sector,sector_t nr_sect,char *page_address);
static int write_disk(sector_t initial_sector,sector_t nr_sect,char *page_address);
static void transfer_to_disk(sector_t sect,sector_t nsect, char *buff, int write);
static int transfer_each_req(struct request *req);
static void bottomhalf_work_function(struct work_struct *work);
void usb_request(struct request_queue *q);

static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id);

static void usbdev_disconnect(struct usb_interface *interface)
{
	struct gendisk *usb_disk = p_blkdev->gd;
        printk(KERN_INFO "USBDEV Device Removed\n");
	del_gendisk(usb_disk);
	blk_cleanup_queue(p_blkdev->queue);
	kfree(p_blkdev);
	return;
}

static int get_mass_storage_status(struct usb_device *udev, uint8_t endpoint, uint32_t expected_tag)
{	
	int r;
	int size;	
	
	struct command_status_wrapper *csw;
	csw=(struct command_status_wrapper *)kmalloc(sizeof(struct command_status_wrapper),GFP_KERNEL);
	r=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint),(void*)csw,13, &size, 1000);
	if(r<0)
		printk("error in status");
	
	if (csw->dCSWTag != expected_tag) {
		printk("   get_mass_storage_status: mismatched tags (expected %08X, received %08X)\n",
			expected_tag, csw->dCSWTag);
		return -1;
	}
	if (size != 13) {
		printk("   get_mass_storage_status: received %d bytes (expected 13)\n", size);
		return -1;
	}	
	printk(KERN_INFO "Mass Storage Status: %02X (%s)\n", csw->bCSWStatus, csw->bCSWStatus?"FAILED":"Success");
	return 0;
}  

static int send_command(struct usb_device *udev,uint8_t endpoint,
                         uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag)
{
	
	uint32_t tag = 1;
	int r;
	int size;
	uint8_t cdb_len;
	struct command_block_wrapper *cbw;
	cbw=(struct command_block_wrapper *)kmalloc(sizeof(struct command_block_wrapper),GFP_KERNEL);
	
	if (cdb == NULL) {
		return -1;
	}
	if (endpoint & USB_DIR_IN) {
		printk("send_mass_storage_command: cannot send command on IN endpoint\n");
		return -1;
	}	
	cdb_len = cdb_length[cdb[0]];
	if ((cdb_len == 0) || (cdb_len > sizeof(cbw->CBWCB))) {
		printk("Invalid command\n");
		return -1;
	}	

	memset(cbw, 0, sizeof(*cbw));
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';
	*ret_tag = tag;
	cbw->dCBWTag = tag++;
	cbw->dCBWDataTransferLength = data_length;
	cbw->bmCBWFlags = direction;
	cbw->bCBWLUN =0;
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);
	

	r = usb_bulk_msg(udev,usb_sndbulkpipe(udev,endpoint),(void*)cbw,31, &size,1000);
	if(r!=0)
		printk("Failed command transfer %d",r);
	//else 	
	//	printk("command sent successfully");
	
	//printk("sent %d CDB bytes\n", size);
	return 0;
} 

static int read_disk(sector_t initial_sector,sector_t nr_sect,char *page_address)
{
int result;
unsigned int size;
uint8_t cdb[16];	// SCSI Command Descriptor block
uint32_t expected_tag;
size=0;
memset(cdb,0,sizeof(cdb));
cdb[0] = 0x28;	// Read(10)
cdb[2]=(initial_sector>>24) & 0xFF;
cdb[3]=(initial_sector>>16) & 0xFF;
cdb[4]=(initial_sector>>8) & 0xFF;
cdb[5]=(initial_sector>>0) & 0xFF;
cdb[7]=(nr_sect>>8) & 0xFF;
cdb[8]=(nr_sect>>0) & 0xFF;	// 1 block

//printk("endpoint_out : %x",endpoint_out);
send_command(udev,endpoint_out,cdb,USB_DIR_IN,(nr_sect*512),&expected_tag);
result=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint_in),(void*)(page_address),(nr_sect*512),&size, 5000);
//printk("result: %d",result);
//printk("size of data sent: %d",size);
get_mass_storage_status(udev, endpoint_in, expected_tag);  
return 0;
}

 static int write_disk(sector_t initial_sector,sector_t nr_sect,char *page_address)
{   //int i;
	int result;
	unsigned int size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint32_t expected_tag;
	//printk(KERN_INFO "write command------>");
	memset(cdb,0,sizeof(cdb));
	cdb[0]=0x2A;
	cdb[2]=(initial_sector>>24)&0xFF;
	cdb[3]=(initial_sector>>16)&0xFF;
	cdb[4]=(initial_sector>>8)&0xFF;
	cdb[5]=(initial_sector>>0)&0xFF;
	cdb[7]=(nr_sect>>8)&0xFF;
	cdb[8]=(nr_sect>>0)&0xFF;	// 1 block
	cdb[8]=0x01;
	send_command(udev,endpoint_out,cdb,USB_DIR_OUT,nr_sect*512,&expected_tag);
	result=usb_bulk_msg(udev,usb_sndbulkpipe(udev,endpoint_out),(void*)page_address,nr_sect*512,&size, 1000);
	//printk("return msg of write %d",result);
	//printk("WRITE: sent %d bytes\n", size);
	get_mass_storage_status(udev, endpoint_in, expected_tag); 
	return 0;

}  

static void transfer_to_disk(sector_t sect,sector_t nsect, char *buff, int write)
{
    unsigned long offset = sect*512;
    unsigned long nbytes = nsect*512;

    if ((offset + nbytes) > (NR_OF_SECTORS*512)) {
        printk (KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
        return;
    }
     if (write)
        write_disk(sect,nsect,buff);
    else
        read_disk(sect,nsect,buff);
    return; 
}  

static int transfer_each_req(struct request *req)
{
    int i;
    sector_t addrs;
    int dir = rq_data_dir(req);
    struct req_iterator iter;
    struct bio_vec bvec;
    //struct req_iterator iter;
    sector_t sector_offset;
    unsigned int sectors;
    
    char *buffer = __bio_kmap_atomic(req->bio, i, KM_USER0);
    sector_t start_sector = blk_rq_pos(req);
    //unsigned int sector_cnt = blk_rq_sectors(req);
    //sector_t sector = req->bio->bi_iter.bi_sector;
    sector_offset =0;
    
    rq_for_each_segment(bvec,req,iter){
    	sectors = bvec.bv_len / 512;
    	addrs = start_sector+sector_offset;

    	
    	transfer_to_disk(addrs, sectors ,buffer, dir==WRITE);
    	sector_offset += sectors;
    	//sector += bio_cur_sectors(bio);
    	__bio_kunmap_atomic(req->bio, KM_USER0);
    	printk(KERN_DEBUG "STARTING SECTOR : %llu, sector offset: %llu;\
    		buffer: %p; length: %u sectors\n",\
    		(unsigned long long)(start_sector),(unsigned long long)\
    		(sector_offset), buffer , sectors);
    }
    return 0; // Always "succeed" 
}  

static struct workqueue_struct *myqueue=NULL;



static void bottomhalf_work_function(struct work_struct *work)
{
	struct dev_work *usb_work=container_of(work,struct dev_work,work);
	//printk("inside my delay function");
	transfer_each_req(usb_work->req);
	__blk_end_request_cur(usb_work->req,0);
	kfree(usb_work);
	
	return;
}

void usb_request(struct request_queue *q)  // request fn 
{
	struct request *req;  // local req struct,that will get next req
	//int sectors_xferred;
	struct dev_work *usb_work=NULL;
  
	//req = blk_fetch_request(q); // to fetch next req from queue

	while((req=blk_fetch_request(q)) != NULL)
	{
		if(req == NULL && !blk_rq_is_passthrough(req)) // check if file sys req, for DRiver to handle 
		{
			printk(KERN_INFO "non FS request");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		
		usb_work=(struct dev_work *)kmalloc(sizeof(struct dev_work),GFP_ATOMIC);
		if(usb_work==NULL){

			printk("Memory Allocation to deferred work failed");
			__blk_end_request_all(req, 0);
			continue;
		}

		usb_work->req=req;
		INIT_WORK(&usb_work->work,bottomhalf_work_function);
		queue_work(myqueue,&usb_work->work);
		//printk("Hanged in req fn");
	}	
} 

static int blkdev_open(struct block_device *bdev, fmode_t mode)
{
	//printk("inside blk_dev open fn");
    struct blkdev_private *dev = bdev->bd_disk->private_data;
    spin_lock(&dev->lock);
    if (! dev->users) 
        check_disk_change(bdev);	
    dev->users++;
    spin_unlock(&dev->lock);
    return 0;
}

static void blkdev_release(struct gendisk *gdisk, fmode_t mode)
{
	//printk("inside blk_dev release fn");
    struct blkdev_private *dev = gdisk->private_data;
    spin_lock(&dev->lock);
    dev->users--;
    spin_unlock(&dev->lock);

    //return 0;
}

static struct block_device_operations blkdev_ops =
{
	.owner= THIS_MODULE,
	.open=blkdev_open,
	.release=blkdev_release
};


static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{	int i, type;
	int usb_majnum=0;
	unsigned char epAddr;
	struct usb_endpoint_descriptor *ep_desc;
	if(id->idProduct == PENDRIVE_PID && id->idVendor==PENDRIVE_VID)
	{
		printk(KERN_INFO "USB drive detected\n");
	}

	udev=interface_to_usbdev(interface);

	printk(KERN_INFO "USB INTERFACE SUB CLASS : %x", interface->cur_altsetting->desc.bInterfaceSubClass);
	printk(KERN_INFO "USB INTERFACE Protocol : %x", interface->cur_altsetting->desc.bInterfaceProtocol);
	printk(KERN_INFO "No. of Endpoints = %d \n", interface->cur_altsetting->desc.bNumEndpoints);
	printk("VID  %#06x\n",udev->descriptor.idVendor);
	printk("PID  %#06x\n",udev->descriptor.idProduct);
	printk(KERN_INFO "USB DEVICE CLASS: %x", interface->cur_altsetting->desc.bInterfaceClass);

	for(i=0;i<interface->cur_altsetting->desc.bNumEndpoints;i++)
	{
		ep_desc = &interface->cur_altsetting->endpoint[i].desc;
		epAddr = ep_desc->bEndpointAddress;
		type=usb_endpoint_type(ep_desc);
		if(type==2){
		if(epAddr & 0x80)
		{		
			printk(KERN_INFO "EP %d is Bulk IN\n", i);
			endpoint_in=ep_desc->bEndpointAddress;
			printk("endpoint_in : %x",endpoint_in);
			
		}
		else
		{	
			endpoint_out=ep_desc->bEndpointAddress;
			printk(KERN_INFO "EP %d is Bulk OUT\n", i); 
			printk("endpoint_out : %x",endpoint_out);
		}
		}
		if(type==3)
		{
		if(epAddr && 0x80)
			printk(KERN_INFO "EP %d is Interrupt IN\n", i);
		else
			printk(KERN_INFO "EP %d is Interrupt OUT\n", i);
		}
		}
		if ((interface->cur_altsetting->desc.bInterfaceClass == 8)
			  && (interface->cur_altsetting->desc.bInterfaceSubClass == 6) 
			  && (interface->cur_altsetting->desc.bInterfaceProtocol == 80) ) 
			{
				// Mass storage devices that can use basic SCSI commands
				printk(KERN_INFO "Detected device is a valid SCSI mass storage device.\n \n");
			}

		else{
		     	printk(KERN_INFO "Detected device is not a valid SCSI mass storage device.\n");
	     	}

	
	usb_majnum = register_blkdev(0, "USB DISK");  // to reg the device, to have a device name, and assign a major no
	if (usb_majnum < 0) 
		printk(KERN_WARNING "Usb disk major number registration failed\n");
	
	p_blkdev = kmalloc(sizeof(struct blkdev_private),GFP_KERNEL); // private structre m/m allocation 
	
	if(!p_blkdev)
	{
		printk("ENOMEM  at %d\n",__LINE__);
		return 0;
	}
	memset(p_blkdev, 0, sizeof(struct blkdev_private)); // for initializing all p_blkdev var as 0

	spin_lock_init(&p_blkdev->lock);  // for initiaizing spin lock,holding the lock during initializing and manipulating req queue
	
	// every time usb_request function will be called when there is new request in the queue
	p_blkdev->queue = blk_init_queue(usb_request, &p_blkdev->lock);  // performing queue op , 
   // above fn called on every driver req : usb_request
	usb_disk = p_blkdev->gd = alloc_disk(2); // (2 is minor no) to get the gendisk structure, also assigning to local var mmc_disk
	if(!usb_disk)
	{
		kfree(p_blkdev);
		printk(KERN_INFO "disk allocation failed\n");
		return 0;
	}
	// below all are part of gendisk structure
	usb_disk->major =usb_majnum;
	usb_disk->first_minor = 0;
	usb_disk->fops = &blkdev_ops;
	usb_disk->queue = p_blkdev->queue;
	usb_disk->private_data = p_blkdev;
	strcpy(usb_disk->disk_name, DEVICE_NAME);
	set_capacity(usb_disk,NR_OF_SECTORS); // defined 
	add_disk(usb_disk);  // do add disk now

return 0;
}

static struct usb_driver usbdev_driver = {
	name: "my_usb_device",  //name of the device
	probe: usbdev_probe, // Whenever Device is plugged in
	disconnect: usbdev_disconnect, // When we remove a device
	id_table: usbdev_table, //  List of devices served by this driver
};

int block_init(void)
{
	usb_register(&usbdev_driver);
	printk(KERN_NOTICE "UAS READ Capacity Driver Inserted\n");
	printk(KERN_INFO "Registered disk\n"); 
	printk(KERN_ALERT "Work Queue defined\n");
	myqueue=create_workqueue("myqueue");  // my worker thread name
	return 0;	
}

void block_exit(void)
{ 
	usb_deregister(&usbdev_driver);
	printk("Device driver unregister");
	flush_workqueue(myqueue);  // to exit the work done
	destroy_workqueue(myqueue);
	return;
}


module_init(block_init);
module_exit(block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BITS GOA");
MODULE_DESCRIPTION("DISK");
