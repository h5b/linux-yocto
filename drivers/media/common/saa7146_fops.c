#include <media/saa7146_vv.h>

#define BOARD_CAN_DO_VBI(dev)   (dev->revision != 0 && dev->vv_data->vbi_minor != -1)

/****************************************************************************/
/* resource management functions, shamelessly stolen from saa7134 driver */

int saa7146_res_get(struct saa7146_fh *fh, unsigned int bit)
{
	struct saa7146_dev *dev = fh->dev;
	struct saa7146_vv *vv = dev->vv_data;

	if (fh->resources & bit) {
		DEB_D(("already allocated! want: 0x%02x, cur:0x%02x\n",bit,vv->resources));
		/* have it already allocated */
		return 1;
	}

	/* is it free? */
	down(&dev->lock);
	if (vv->resources & bit) {
		DEB_D(("locked! vv->resources:0x%02x, we want:0x%02x\n",vv->resources,bit));
		/* no, someone else uses it */
		up(&dev->lock);
		return 0;
	}
	/* it's free, grab it */
	fh->resources  |= bit;
	vv->resources |= bit;
	DEB_D(("res: get 0x%02x, cur:0x%02x\n",bit,vv->resources));
	up(&dev->lock);
	return 1;
}

void saa7146_res_free(struct saa7146_fh *fh, unsigned int bits)
{
	struct saa7146_dev *dev = fh->dev;
	struct saa7146_vv *vv = dev->vv_data;

	if ((fh->resources & bits) != bits)
		BUG();

	down(&dev->lock);
	fh->resources  &= ~bits;
	vv->resources &= ~bits;
	DEB_D(("res: put 0x%02x, cur:0x%02x\n",bits,vv->resources));
	up(&dev->lock);
}


/********************************************************************************/
/* common dma functions */

void saa7146_dma_free(struct saa7146_dev *dev,struct saa7146_buf *buf)
{
	DEB_EE(("dev:%p, buf:%p\n",dev,buf));

	if (in_interrupt())
		BUG();

	videobuf_waiton(&buf->vb,0,0);
	videobuf_dma_pci_unmap(dev->pci, &buf->vb.dma);
	videobuf_dma_free(&buf->vb.dma);
	buf->vb.state = STATE_NEEDS_INIT;
}


/********************************************************************************/
/* common buffer functions */

int saa7146_buffer_queue(struct saa7146_dev *dev,
			 struct saa7146_dmaqueue *q,
			 struct saa7146_buf *buf)
{
	assert_spin_locked(&dev->slock);
	DEB_EE(("dev:%p, dmaq:%p, buf:%p\n", dev, q, buf));

	BUG_ON(!q);

	if (NULL == q->curr) {
		q->curr = buf;
		DEB_D(("immediately activating buffer %p\n", buf));
		buf->activate(dev,buf,NULL);
	} else {
		list_add_tail(&buf->vb.queue,&q->queue);
		buf->vb.state = STATE_QUEUED;
		DEB_D(("adding buffer %p to queue. (active buffer present)\n", buf));
	}
	return 0;
}

void saa7146_buffer_finish(struct saa7146_dev *dev,
			   struct saa7146_dmaqueue *q,
			   int state)
{
	assert_spin_locked(&dev->slock);
	DEB_EE(("dev:%p, dmaq:%p, state:%d\n", dev, q, state));
	DEB_EE(("q->curr:%p\n",q->curr));

	BUG_ON(!q->curr);

	/* finish current buffer */
	if (NULL == q->curr) {
		DEB_D(("aiii. no current buffer\n"));
		return;
	}

	q->curr->vb.state = state;
	do_gettimeofday(&q->curr->vb.ts);
	wake_up(&q->curr->vb.done);

	q->curr = NULL;
}

void saa7146_buffer_next(struct saa7146_dev *dev,
			 struct saa7146_dmaqueue *q, int vbi)
{
	struct saa7146_buf *buf,*next = NULL;

	BUG_ON(!q);

	DEB_INT(("dev:%p, dmaq:%p, vbi:%d\n", dev, q, vbi));

	assert_spin_locked(&dev->slock);
	if (!list_empty(&q->queue)) {
		/* activate next one from queue */
		buf = list_entry(q->queue.next,struct saa7146_buf,vb.queue);
		list_del(&buf->vb.queue);
		if (!list_empty(&q->queue))
			next = list_entry(q->queue.next,struct saa7146_buf, vb.queue);
		q->curr = buf;
		DEB_INT(("next buffer: buf:%p, prev:%p, next:%p\n", buf, q->queue.prev,q->queue.next));
		buf->activate(dev,buf,next);
	} else {
		DEB_INT(("no next buffer. stopping.\n"));
		if( 0 != vbi ) {
			/* turn off video-dma3 */
			saa7146_write(dev,MC1, MASK_20);
		} else {
			/* nothing to do -- just prevent next video-dma1 transfer
			   by lowering the protection address */

			// fixme: fix this for vflip != 0

			saa7146_write(dev, PROT_ADDR1, 0);
			saa7146_write(dev, MC2, (MASK_02|MASK_18));

			/* write the address of the rps-program */
			saa7146_write(dev, RPS_ADDR0, dev->d_rps0.dma_handle);
			/* turn on rps */
			saa7146_write(dev, MC1, (MASK_12 | MASK_28));

/*
			printk("vdma%d.base_even:     0x%08x\n", 1,saa7146_read(dev,BASE_EVEN1));
			printk("vdma%d.base_odd:      0x%08x\n", 1,saa7146_read(dev,BASE_ODD1));
			printk("vdma%d.prot_addr:     0x%08x\n", 1,saa7146_read(dev,PROT_ADDR1));
			printk("vdma%d.base_page:     0x%08x\n", 1,saa7146_read(dev,BASE_PAGE1));
			printk("vdma%d.pitch:         0x%08x\n", 1,saa7146_read(dev,PITCH1));
			printk("vdma%d.num_line_byte: 0x%08x\n", 1,saa7146_read(dev,NUM_LINE_BYTE1));
*/
		}
		del_timer(&q->timeout);
	}
}

void saa7146_buffer_timeout(unsigned long data)
{
	struct saa7146_dmaqueue *q = (struct saa7146_dmaqueue*)data;
	struct saa7146_dev *dev = q->dev;
	unsigned long flags;

	DEB_EE(("dev:%p, dmaq:%p\n", dev, q));

	spin_lock_irqsave(&dev->slock,flags);
	if (q->curr) {
		DEB_D(("timeout on %p\n", q->curr));
		saa7146_buffer_finish(dev,q,STATE_ERROR);
	}

	/* we don't restart the transfer here like other drivers do. when
	   a streaming capture is disabled, the timeout function will be
	   called for the current buffer. if we activate the next buffer now,
	   we mess up our capture logic. if a timeout occurs on another buffer,
	   then something is seriously broken before, so no need to buffer the
	   next capture IMHO... */
/*
	saa7146_buffer_next(dev,q);
*/
	spin_unlock_irqrestore(&dev->slock,flags);
}

/********************************************************************************/
/* file operations */

static int fops_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct saa7146_dev *h = NULL, *dev = NULL;
	struct list_head *list;
	struct saa7146_fh *fh = NULL;
	int result = 0;

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	DEB_EE(("inode:%p, file:%p, minor:%d\n",inode,file,minor));

	if (down_interruptible(&saa7146_devices_lock))
		return -ERESTARTSYS;

	list_for_each(list,&saa7146_devices) {
		h = list_entry(list, struct saa7146_dev, item);
		if( NULL == h->vv_data ) {
			DEB_D(("device %p has not registered video devices.\n",h));
			continue;
		}
		DEB_D(("trying: %p @ major %d,%d\n",h,h->vv_data->video_minor,h->vv_data->vbi_minor));

		if (h->vv_data->video_minor == minor) {
			dev = h;
		}
		if (h->vv_data->vbi_minor == minor) {
			type = V4L2_BUF_TYPE_VBI_CAPTURE;
			dev = h;
		}
	}
	if (NULL == dev) {
		DEB_S(("no such video device.\n"));
		result = -ENODEV;
		goto out;
	}

	DEB_D(("using: %p\n",dev));

	/* check if an extension is registered */
	if( NULL == dev->ext ) {
		DEB_S(("no extension registered for this device.\n"));
		result = -ENODEV;
		goto out;
	}

	/* allocate per open data */
	fh = kmalloc(sizeof(*fh),GFP_KERNEL);
	if (NULL == fh) {
		DEB_S(("cannot allocate memory for per open data.\n"));
		result = -ENOMEM;
		goto out;
	}
	memset(fh,0,sizeof(*fh));

	file->private_data = fh;
	fh->dev = dev;
	fh->type = type;

	if( fh->type == V4L2_BUF_TYPE_VBI_CAPTURE) {
		DEB_S(("initializing vbi...\n"));
		if (dev->ext_vv_data->capabilities & V4L2_CAP_VBI_CAPTURE)
			result = saa7146_vbi_uops.open(dev,file);
		if (dev->ext_vv_data->vbi_fops.open)
			dev->ext_vv_data->vbi_fops.open(inode, file);
	} else {
		DEB_S(("initializing video...\n"));
		result = saa7146_video_uops.open(dev,file);
	}

	if (0 != result) {
		goto out;
	}

	if( 0 == try_module_get(dev->ext->module)) {
		result = -EINVAL;
		goto out;
	}

	result = 0;
out:
	if( fh != 0 && result != 0 ) {
		kfree(fh);
		file->private_data = NULL;
	}
	up(&saa7146_devices_lock);
	return result;
}

static int fops_release(struct inode *inode, struct file *file)
{
	struct saa7146_fh  *fh  = file->private_data;
	struct saa7146_dev *dev = fh->dev;

	DEB_EE(("inode:%p, file:%p\n",inode,file));

	if (down_interruptible(&saa7146_devices_lock))
		return -ERESTARTSYS;

	if( fh->type == V4L2_BUF_TYPE_VBI_CAPTURE) {
		if (dev->ext_vv_data->capabilities & V4L2_CAP_VBI_CAPTURE)
			saa7146_vbi_uops.release(dev,file);
		if (dev->ext_vv_data->vbi_fops.release)
			dev->ext_vv_data->vbi_fops.release(inode, file);
	} else {
		saa7146_video_uops.release(dev,file);
	}

	module_put(dev->ext->module);
	file->private_data = NULL;
	kfree(fh);

	up(&saa7146_devices_lock);

	return 0;
}

int saa7146_video_do_ioctl(struct inode *inode, struct file *file, unsigned int cmd, void *arg);
static int fops_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
/*
	DEB_EE(("inode:%p, file:%p, cmd:%d, arg:%li\n",inode, file, cmd, arg));
*/
	return video_usercopy(inode, file, cmd, arg, saa7146_video_do_ioctl);
}

static int fops_mmap(struct file *file, struct vm_area_struct * vma)
{
	struct saa7146_fh *fh = file->private_data;
	struct videobuf_queue *q;

	switch (fh->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE: {
		DEB_EE(("V4L2_BUF_TYPE_VIDEO_CAPTURE: file:%p, vma:%p\n",file, vma));
		q = &fh->video_q;
		break;
		}
	case V4L2_BUF_TYPE_VBI_CAPTURE: {
		DEB_EE(("V4L2_BUF_TYPE_VBI_CAPTURE: file:%p, vma:%p\n",file, vma));
		q = &fh->vbi_q;
		break;
		}
	default:
		BUG();
		return 0;
	}

	return videobuf_mmap_mapper(q,vma);
}

static unsigned int fops_poll(struct file *file, struct poll_table_struct *wait)
{
	struct saa7146_fh *fh = file->private_data;
	struct videobuf_buffer *buf = NULL;
	struct videobuf_queue *q;

	DEB_EE(("file:%p, poll:%p\n",file, wait));

	if (V4L2_BUF_TYPE_VBI_CAPTURE == fh->type) {
		if( 0 == fh->vbi_q.streaming )
			return videobuf_poll_stream(file, &fh->vbi_q, wait);
		q = &fh->vbi_q;
	} else {
		DEB_D(("using video queue.\n"));
		q = &fh->video_q;
	}

	if (!list_empty(&q->stream))
		buf = list_entry(q->stream.next, struct videobuf_buffer, stream);

	if (!buf) {
		DEB_D(("buf == NULL!\n"));
		return POLLERR;
	}

	poll_wait(file, &buf->done, wait);
	if (buf->state == STATE_DONE || buf->state == STATE_ERROR) {
		DEB_D(("poll succeeded!\n"));
		return POLLIN|POLLRDNORM;
	}

	DEB_D(("nothing to poll for, buf->state:%d\n",buf->state));
	return 0;
}

static ssize_t fops_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
	struct saa7146_fh *fh = file->private_data;

	switch (fh->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE: {
//		DEB_EE(("V4L2_BUF_TYPE_VIDEO_CAPTURE: file:%p, data:%p, count:%lun", file, data, (unsigned long)count));
		return saa7146_video_uops.read(file,data,count,ppos);
		}
	case V4L2_BUF_TYPE_VBI_CAPTURE: {
//		DEB_EE(("V4L2_BUF_TYPE_VBI_CAPTURE: file:%p, data:%p, count:%lu\n", file, data, (unsigned long)count));
		if (fh->dev->ext_vv_data->capabilities & V4L2_CAP_VBI_CAPTURE)
			return saa7146_vbi_uops.read(file,data,count,ppos);
		else
			return -EINVAL;
		}
		break;
	default:
		BUG();
		return 0;
	}
}

static ssize_t fops_write(struct file *file, const char __user *data, size_t count, loff_t *ppos)
{
	struct saa7146_fh *fh = file->private_data;

	switch (fh->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return -EINVAL;
	case V4L2_BUF_TYPE_VBI_CAPTURE:
		if (fh->dev->ext_vv_data->vbi_fops.write)
			return fh->dev->ext_vv_data->vbi_fops.write(file, data, count, ppos);
		else
			return -EINVAL;
	default:
		BUG();
		return -EINVAL;
	}
}

static struct file_operations video_fops =
{
	.owner		= THIS_MODULE,
	.open		= fops_open,
	.release	= fops_release,
	.read		= fops_read,
	.write		= fops_write,
	.poll		= fops_poll,
	.mmap		= fops_mmap,
	.ioctl		= fops_ioctl,
	.llseek		= no_llseek,
};

static void vv_callback(struct saa7146_dev *dev, unsigned long status)
{
	u32 isr = status;

	DEB_INT(("dev:%p, isr:0x%08x\n",dev,(u32)status));

	if (0 != (isr & (MASK_27))) {
		DEB_INT(("irq: RPS0 (0x%08x).\n",isr));
		saa7146_video_uops.irq_done(dev,isr);
	}

	if (0 != (isr & (MASK_28))) {
		u32 mc2 = saa7146_read(dev, MC2);
		if( 0 != (mc2 & MASK_15)) {
			DEB_INT(("irq: RPS1 vbi workaround (0x%08x).\n",isr));
			wake_up(&dev->vv_data->vbi_wq);
			saa7146_write(dev,MC2, MASK_31);
			return;
		}
		DEB_INT(("irq: RPS1 (0x%08x).\n",isr));
		saa7146_vbi_uops.irq_done(dev,isr);
	}
}

static struct video_device device_template =
{
	.hardware	= VID_HARDWARE_SAA7146,
	.fops		= &video_fops,
	.minor		= -1,
};

int saa7146_vv_init(struct saa7146_dev* dev, struct saa7146_ext_vv *ext_vv)
{
	struct saa7146_vv *vv = kmalloc (sizeof(struct saa7146_vv),GFP_KERNEL);
	if( NULL == vv ) {
		ERR(("out of memory. aborting.\n"));
		return -1;
	}
	memset(vv, 0x0, sizeof(*vv));

	DEB_EE(("dev:%p\n",dev));

	/* set default values for video parts of the saa7146 */
	saa7146_write(dev, BCS_CTRL, 0x80400040);

	/* enable video-port pins */
	saa7146_write(dev, MC1, (MASK_10 | MASK_26));

	/* save per-device extension data (one extension can
	   handle different devices that might need different
	   configuration data) */
	dev->ext_vv_data = ext_vv;

	vv->video_minor = -1;
	vv->vbi_minor = -1;

	vv->d_clipping.cpu_addr = pci_alloc_consistent(dev->pci, SAA7146_CLIPPING_MEM, &vv->d_clipping.dma_handle);
	if( NULL == vv->d_clipping.cpu_addr ) {
		ERR(("out of memory. aborting.\n"));
		kfree(vv);
		return -1;
	}
	memset(vv->d_clipping.cpu_addr, 0x0, SAA7146_CLIPPING_MEM);

	saa7146_video_uops.init(dev,vv);
	if (dev->ext_vv_data->capabilities & V4L2_CAP_VBI_CAPTURE)
		saa7146_vbi_uops.init(dev,vv);

	dev->vv_data = vv;
	dev->vv_callback = &vv_callback;

	return 0;
}

int saa7146_vv_release(struct saa7146_dev* dev)
{
	struct saa7146_vv *vv = dev->vv_data;

	DEB_EE(("dev:%p\n",dev));

	pci_free_consistent(dev->pci, SAA7146_RPS_MEM, vv->d_clipping.cpu_addr, vv->d_clipping.dma_handle);
	kfree(vv);
	dev->vv_data = NULL;
	dev->vv_callback = NULL;

	return 0;
}

int saa7146_register_device(struct video_device **vid, struct saa7146_dev* dev,
			    char *name, int type)
{
	struct saa7146_vv *vv = dev->vv_data;
	struct video_device *vfd;

	DEB_EE(("dev:%p, name:'%s', type:%d\n",dev,name,type));

	// released by vfd->release
	vfd = video_device_alloc();
	if (vfd == NULL)
		return -ENOMEM;

	memcpy(vfd, &device_template, sizeof(struct video_device));
	strlcpy(vfd->name, name, sizeof(vfd->name));
	vfd->release = video_device_release;
	vfd->priv = dev;

	// fixme: -1 should be an insmod parameter *for the extension* (like "video_nr");
	if (video_register_device(vfd, type, -1) < 0) {
		ERR(("cannot register v4l2 device. skipping.\n"));
		return -1;
	}

	if( VFL_TYPE_GRABBER == type ) {
		vv->video_minor = vfd->minor;
		INFO(("%s: registered device video%d [v4l2]\n",
			dev->name, vfd->minor & 0x1f));
	} else {
		vv->vbi_minor = vfd->minor;
		INFO(("%s: registered device vbi%d [v4l2]\n",
			dev->name, vfd->minor & 0x1f));
	}

	*vid = vfd;
	return 0;
}

int saa7146_unregister_device(struct video_device **vid, struct saa7146_dev* dev)
{
	struct saa7146_vv *vv = dev->vv_data;

	DEB_EE(("dev:%p\n",dev));

	if( VFL_TYPE_GRABBER == (*vid)->type ) {
		vv->video_minor = -1;
	} else {
		vv->vbi_minor = -1;
	}

	video_unregister_device(*vid);
	*vid = NULL;

	return 0;
}

static int __init saa7146_vv_init_module(void)
{
	return 0;
}


static void __exit saa7146_vv_cleanup_module(void)
{
}

module_init(saa7146_vv_init_module);
module_exit(saa7146_vv_cleanup_module);

MODULE_AUTHOR("Michael Hunold <michael@mihu.de>");
MODULE_DESCRIPTION("video4linux driver for saa7146-based hardware");
MODULE_LICENSE("GPL");
