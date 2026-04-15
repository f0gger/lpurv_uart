#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/clk.h>

/* Add your code here */
#define CR_RXE    9
#define CR_TXE    8
#define CR_UARTEN 0
#define FR_TXFF   5
#define FR_RXFE   4
#define IMSC_RXIM 4

#define UART01x_DR         0x00
#define UART01x_FR	   0x18	
#define UART011_IBRD       0x24
#define UART011_FBRD       0x28	
#define UART010_CR         0x30	
#define UART01x_IMSC       0x38

#define UART_BUFF_MAX_SIZE 256 
#define SERIAL_RESET_COUNTER 0
#define SERIAL_GET_COUNTER 1

//char circ_buf[UART_BUFF_MAX_SIZE];

struct serial_dev {
        void __iomem   *regs;
        struct miscdevice midev;
	int irq;
        struct clk *uart_clk;
        atomic_t char_cnt;
        wait_queue_head_t serial_wait;
        unsigned int head;
        unsigned int tail;
        char circ_buf[UART_BUFF_MAX_SIZE];
        spinlock_t buf_lock;
};


static unsigned int reg_read(struct serial_dev *dev, int offset)
{
	return readl(dev->regs + offset);
}

static void reg_write(struct serial_dev *dev, int value, int offset)
{
	writel(value, dev->regs + offset);
}

static irqreturn_t uart_interrupt_handler(int irq, void *dev_id)
{
        struct serial_dev *sdev = dev_id;
        unsigned long flags;

        pr_info("irq started\n");
        while (!(reg_read(sdev, UART01x_FR) & (1 << FR_RXFE))) 
        {
                spin_lock_irqsave(&sdev->buf_lock, flags);
                sdev->circ_buf[sdev->head] = reg_read(sdev, UART01x_DR);
                sdev->head = (sdev->head + 1) % UART_BUFF_MAX_SIZE;
                spin_unlock_irqrestore(&sdev->buf_lock, flags);
        }
        wake_up_interruptible(&sdev->serial_wait);

        return IRQ_HANDLED;
}

static ssize_t serial_read(struct file *file, char __user *buf, size_t sz, loff_t *ppos)
{
        int ret;
        int i;
        int status;
	struct serial_dev *sdev;
        char rx_buff[UART_BUFF_MAX_SIZE];
        unsigned long flags;

        sdev = container_of(file->private_data, struct serial_dev, midev);

        wait_event_interruptible(sdev->serial_wait, sdev->head != sdev->tail);

        i = 0;
        pr_info("Began printing to userspace\n");

        while (i < sz && sdev->tail != sdev->head) 
        {
                spin_lock_irqsave(&sdev->buf_lock, flags);
                rx_buff[i++] = sdev->circ_buf[sdev->tail];
                sdev->tail = (sdev->tail + 1) % UART_BUFF_MAX_SIZE;
                spin_unlock_irqrestore(&sdev->buf_lock, flags);
        }

        
        if (i > 0)
                pr_info("Curr buff: %s\n", rx_buff);

        ret = copy_to_user(buf, rx_buff, i);
        if (ret)
        {
                pr_err("epic ret fail\n");
                return -EFAULT;
        }

        return i;
}

static ssize_t serial_write(struct file *file, const char __user *buf, size_t sz, loff_t *ppos)
{
	struct serial_dev *sdev;
        char uart_buff[UART_BUFF_MAX_SIZE];
        unsigned long uart_buff_size;
        int ret;
        int i;
        
	sdev = container_of(file->private_data, struct serial_dev, midev);

        uart_buff_size = sz;
        if (uart_buff_size >= UART_BUFF_MAX_SIZE)
                uart_buff_size = UART_BUFF_MAX_SIZE - 1;

        ret = copy_from_user(uart_buff, buf, uart_buff_size);
        if (ret)
        {
                pr_err("Couldnt write to uart0\n");
                return -EFAULT;
        }

        pr_info("Stuff to send %s\n", uart_buff);
        
        *ppos += uart_buff_size;

        i = 0;
        while (i < uart_buff_size)
        {
                while (reg_read(sdev, UART01x_FR) & (1 << FR_TXFF))
                {
                        pr_info("TX fifo full\n");
                        cpu_relax();
                }

                reg_write(sdev, uart_buff[i], UART01x_DR);
                pr_info("Wrote to uart %c\n", uart_buff[i]);
                atomic_inc(&sdev->char_cnt);

                if (uart_buff[i] == '\n')
                {

                        reg_write(sdev, '\r', UART01x_DR);
                        atomic_inc(&sdev->char_cnt);
                }

                i++;
        }

        return uart_buff_size;
}


static long serial_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) 
{
	struct serial_dev *sdev;
        long ret;

        pr_info("Entered ioctl\n");
        sdev = container_of(filp->private_data, struct serial_dev, midev);
        ret = 0;

        switch (cmd)
        {
                case SERIAL_RESET_COUNTER:
                        atomic_set(&sdev->char_cnt, 0);
                        pr_info("Char cnt reset: %d\n", atomic_read(&sdev->char_cnt));
                        break;
                case SERIAL_GET_COUNTER:
                        unsigned int cnt = atomic_read(&sdev->char_cnt);
                        if (copy_to_user((unsigned int __user *)arg, &cnt, sizeof(cnt)))
                                return -EFAULT;
                        pr_info("Char cnt is %d\n", cnt);
                        break;
                default:
                        ret = -EFAULT;
                        break;
        }

        pr_info("retval = %ld\n", ret);
        return ret;
}

static const struct file_operations serial_fops = {
	.owner = THIS_MODULE,
	.write = serial_write,
	.read = serial_read,
        .unlocked_ioctl = serial_ioctl,
};

static int feserial_probe(struct platform_device *pdev)
{
        int ret;
        struct resource *res;
        struct serial_dev *sdev;
        struct clk *uart_clk;
        unsigned int baudrate;
        unsigned int baud_divisor;
        unsigned long uart_clk_rate;

	pr_info("Called feserial_probe\n");

        sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
        platform_set_drvdata(pdev, sdev);
        if (!sdev)
        {
                pr_err("Couldnt alloc mem for uart0\n");
                return -ENOMEM;
        }
        spin_lock_init(&sdev->buf_lock);
        pr_info("Did kzalloc\n");
        init_waitqueue_head(&sdev->serial_wait);
        pr_info("Made workqueue\n");

        res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        if (!res)
        {
                pr_err("Couldnt get base addr from uart0\n");
                return -EBUSY;
        }
        pr_info("Got reg res, start 0x%lx, size 0x%lx\n", (unsigned long)res->start, (unsigned long)resource_size(res));
        sdev->regs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
        if (IS_ERR(sdev->regs))
        {
                pr_err("Couldnt remap for regs\n");
                return -ENOMEM;
        }
        pr_info("Did ioremap %p\n", sdev->regs);
        sdev->irq = platform_get_irq(pdev, 0);
        if (sdev->irq < 0)
        {
                pr_err("Couldnt get uart0 irq\n");
                return -EBUSY;
        }
        pr_info("Got irq %d\n", sdev->irq);
        ret = request_irq(sdev->irq, uart_interrupt_handler, IRQF_SHARED, "uart0_irq", sdev);
        if (ret) 
        {
                pr_err("Could not request irq %d for uart0\n", sdev->irq);
                free_irq(sdev->irq, sdev);
                return ret;
        }
        pr_info("Requested irq\n");
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);
        pr_info("Enabled and synced pm\n");

        uart_clk = devm_clk_get(&pdev->dev, NULL);
        if (IS_ERR(uart_clk)) 
        {
                dev_err(&pdev->dev, "Could not get uart0 clock.\n");
                return -EBUSY;
        }
        pr_info("Got uart clk\n");

        ret = clk_prepare_enable(uart_clk);
        if (ret) 
        {
                dev_err(&pdev->dev, "Unable to enable uart0 clock\n");
                return ret;
        }
        sdev->uart_clk = uart_clk;
        pr_info("Enabled uartclk\n");

        uart_clk_rate = clk_get_rate(uart_clk);
        if (!uart_clk_rate)
        {
                pr_err("Couldnt get uart0 freq\n");
                return -EFAULT;
        }
        pr_info("Got clk rate %lu\n", uart_clk_rate);

        baudrate = 115200;
        baud_divisor = (uart_clk_rate * 4) / baudrate;

        pr_info("Attempting to fiddle with regs\n");
        reg_write(sdev, 0 << CR_UARTEN, UART010_CR);
        pr_info("Disabled uart0\n");
        reg_write(sdev, baud_divisor / 64, UART011_IBRD);
        reg_write(sdev, baud_divisor % 64, UART011_FBRD);
        pr_info("Baud div written to ibrd and fbrd %d\n", baud_divisor);

        reg_write(sdev, (1 << CR_UARTEN) | (1 << CR_TXE) | (1 << CR_RXE), UART010_CR);
        reg_write(sdev, (1 << IMSC_RXIM), UART01x_IMSC);
        pr_info("Inited uart\n");

        atomic_set(&sdev->char_cnt, 0);
        sdev->head = 0;
        sdev->tail = 0;

	sdev->midev.minor = MISC_DYNAMIC_MINOR;
        sdev->midev.name = "uart0";
        sdev->midev.fops = &serial_fops;
        ret = misc_register(&sdev->midev);
        if (ret)
        {
                pr_err("Couldnt create a uart misc device\n");
                return -ENOMEM;
        }

        pr_info("Trying to write t to uart0\n");
        reg_write(sdev, 't', UART01x_DR);

	return 0;
}

static int feserial_remove(struct platform_device *pdev)
{
	pr_info("Called feserial_remove\n");
        struct serial_dev *sdev;
	sdev = platform_get_drvdata(pdev);

        misc_deregister(&sdev->midev);

        clk_disable_unprepare(sdev->uart_clk);
        
        pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&(pdev->dev));


        free_irq(sdev->irq, sdev);

	pr_info("Removal successful\n");
        return 0;
}

static const struct of_device_id serial_dt_ids[] = {
    { .compatible = "rtrk,serial", },
    {}
};
MODULE_DEVICE_TABLE(of, serial_dt_ids);

static struct platform_driver feserial_driver = {
        .driver = {
                .name = "feserial",
                .owner = THIS_MODULE,
                .of_match_table = of_match_ptr(serial_dt_ids),
        },
        .probe = feserial_probe,
        .remove = feserial_remove,
};

module_platform_driver(feserial_driver);
MODULE_LICENSE("GPL");
