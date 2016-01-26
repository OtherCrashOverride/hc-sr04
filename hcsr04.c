#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/interrupt.h> 

#include <linux/amlogic/aml_gpio_consumer.h>
#include <linux/amlogic/gpio-amlogic.h>
#include <linux/wait.h>
#include <linux/sched.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Sergio Tanzilli, OtherCrashOverride");
MODULE_DESCRIPTION("Driver for HC-SR04 ultrasonic sensor");


// TODO: Refactor to allow multiple instances for using more
//       than once sensor.  This is complicated by the Amlogic
//       GPIO IRQ limit of (8) entries.  Rising and falling
//       each require a hardware entry thus limiting it to 
//       four (8 / 2 = 4).

typedef enum _HCSR04STATUS
{
	HCSR04STATUS_READY = 0,
	HCSR04STATUS_WAITING_FOR_ECHO_START,
	HCSR04STATUS_WAITING_FOR_ECHO_STOP,
	HCSR04STATUS_COMPLETE
} HCSR04STATUS;


static int rising_irq = -1;
static int falling_irq = -1;

volatile static HCSR04STATUS status = HCSR04STATUS_READY;
volatile static ktime_t echo_start;
volatile static ktime_t echo_end;

DECLARE_WAIT_QUEUE_HEAD(wq);

static const char* GPIO_OWNER = "hcsr04";


// Default GPIOs.  Use parameters during module load to override.
// TODO: add IRQ setting parameters
static int trigger_gpio = 104;	// J2 - Pin16
module_param(trigger_gpio, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(trigger_gpio, "Trigger GPIO pin number");

static int echo_gpio = 102;		// J2 - Pin18
module_param(echo_gpio, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(echo_gpio, "Echo GPIO pin number");


// This function is called when you write something on /sys/class/hcsr04/value
static ssize_t hcsr04_value_write(struct class *class, struct class_attribute *attr, const char *buf, size_t len)
{
	// Currently unused
	return len;
}

// This function is called when you read /sys/class/hcsr04/value
// Note: https://www.kernel.org/doc/Documentation/filesystems/sysfs.txt
//		 "sysfs allocates a buffer of size (PAGE_SIZE) and passes it to the
//		 method."
static ssize_t hcsr04_value_read(struct class *class, struct class_attribute *attr, char *buf)
{
	int waitResult;
	int returnValue;

	// Set the status first to ensure its valid when IRQ fires
	status = HCSR04STATUS_WAITING_FOR_ECHO_START;

	// Send a 10uS impulse to the TRIGGER line
	amlogic_set_value(trigger_gpio, 1, GPIO_OWNER);
	udelay(10);
	amlogic_set_value(trigger_gpio, 0, GPIO_OWNER);

	// Timemout is 38ms if no obstacle detected according to spec
	waitResult = wait_event_timeout(wq, status == HCSR04STATUS_COMPLETE, 38 * HZ / 1000);
	status = HCSR04STATUS_READY;

	if (waitResult == 0)
	{	// Timeout occured
		returnValue = sprintf(buf, "%d\n", -1);
	}
	else
	{
		returnValue = sprintf(buf, "%lld\n", ktime_to_us(ktime_sub(echo_end, echo_start)));
	}

	return returnValue;
}

// Sysfs definitions for hcsr04 class
static struct class_attribute hcsr04_class_attrs[] =
{
	__ATTR(value, S_IRUGO | S_IWUSR, hcsr04_value_read, hcsr04_value_write),
	__ATTR_NULL,
};

// Name of directory created in /sys/class
static struct class hcsr04_class =
{
	.name = "hcsr04",
	.owner = THIS_MODULE,
	.class_attrs = hcsr04_class_attrs,
};

// Interrupt handler on ECHO signal
static irqreturn_t gpio_isr_rising(int irq, void *data)
{
	// Rising edge
	if (status == HCSR04STATUS_WAITING_FOR_ECHO_START)
	{
		echo_start = ktime_get();
		echo_end = echo_start;

		status = HCSR04STATUS_WAITING_FOR_ECHO_STOP;
	}

	return IRQ_HANDLED;
}

static irqreturn_t gpio_isr_falling(int irq, void *data)
{
	// Falling edge
	if (status == HCSR04STATUS_WAITING_FOR_ECHO_STOP)
	{
		echo_end = ktime_get();
		status = HCSR04STATUS_COMPLETE;

		wake_up(&wq);
	}

	return IRQ_HANDLED;
}

static int hcsr04_init(void)
{
	int rtc;

	printk(KERN_INFO "HC-SR04 driver initializing.\n");
	printk(KERN_INFO "Trigger GPIO: %d.\n", trigger_gpio);
	printk(KERN_INFO "Echo GPIO: %d.\n", echo_gpio);

	if (class_register(&hcsr04_class) < 0)
	{
		goto fail_1;
	}


	// Setup TRIGGER gpio
	rtc = amlogic_gpio_request(trigger_gpio, GPIO_OWNER);
	if (rtc != 0)
	{
		printk(KERN_ERR "Trigger GPIO request failed.\n");
		goto fail_2;
	}

	rtc = amlogic_gpio_direction_output(trigger_gpio, 0, GPIO_OWNER);
	if (rtc != 0)
	{
		printk(KERN_ERR "Trigger GPIO direction set failed.");
		goto fail_3;
	}

	amlogic_set_value(trigger_gpio, 0, GPIO_OWNER);


	// Setup ECHO gpio
	rtc = amlogic_gpio_request(echo_gpio, GPIO_OWNER);
	if (rtc != 0) 
	{
		printk(KERN_ERR "Echo GPIO request failed.\n");
		goto fail_3;
	}

	rtc = amlogic_gpio_direction_input(echo_gpio, GPIO_OWNER);
	if (rtc != 0) 
	{
		printk(KERN_ERR "Echo GPIO direction set failed.\n");
		goto fail_4;
	}

	//amlogic_set_pull_up_down(echo_gpio, 1, ECHO_OWNER);
	amlogic_disable_pullup(echo_gpio, GPIO_OWNER);


	// http://forum.odroid.com/viewtopic.php?f=115&t=8958#p70802
	/*
		IRQ Trigger Select : (0 ~ 3)
			GPIO_IRQ_HIGH = 0, GPIO_IRQ_LOW = 1, GPIO_IRQ_RISING = 2, GPIO_IRQ_FALLING = 3

		IRQ Filter Select : 7
			Value 0 : No Filtering, Value 1 ~ 7 : value * 3 * 111nS(delay)
	*/

	// Set RISING irq
	rising_irq = (GPIO_IRQ0 + INT_GPIO_0);
	rtc = amlogic_gpio_to_irq(echo_gpio, GPIO_OWNER, AML_GPIO_IRQ(rising_irq, FILTER_NUM1, GPIO_IRQ_RISING));
	if (rtc < 0)
	{
		printk(KERN_ERR "Rising IRQ mapping failed.\n");
		goto fail_4;
	}

	rtc = request_irq(rising_irq, (irq_handler_t)gpio_isr_rising, IRQF_DISABLED, "hc-sr04", NULL);
	if (rtc)
	{
		printk(KERN_ERR "Rising IRQ:%d request failed. (Error=%d)\n", rising_irq, rtc);
		goto fail_4;
	}
	else
	{
		printk(KERN_ERR "Rising IRQ:%d\n", rising_irq);
	}


	// Set FALLING irq
	falling_irq = (GPIO_IRQ1 + INT_GPIO_0);
	rtc = amlogic_gpio_to_irq(echo_gpio, GPIO_OWNER, AML_GPIO_IRQ(falling_irq, FILTER_NUM1, GPIO_IRQ_FALLING));
	if (rtc < 0)
	{
		printk(KERN_ERR "Falling IRQ mapping failed.\n");
		goto fail_4;
	}

	rtc = request_irq(falling_irq, (irq_handler_t)gpio_isr_falling, IRQF_DISABLED, "hc-sr04", NULL);
	if (rtc)
	{
		printk(KERN_ERR "Falling IRQ:%d request failed. (Error=%d)\n", falling_irq, rtc);
		goto fail_5;
	}
	else
	{
		printk(KERN_ERR "Falling IRQ:%d\n", falling_irq);
	}


	printk(KERN_INFO "HC-SR04 driver installed.\n");
	return 0;

fail_5:
	if (rising_irq != -1)
	{
		free_irq(rising_irq, NULL);
	}

fail_4:
	amlogic_gpio_free(echo_gpio, GPIO_OWNER);

fail_3:
	amlogic_gpio_free(trigger_gpio, GPIO_OWNER);

fail_2:
	class_unregister(&hcsr04_class);

fail_1:
	return -1;

}

static void hcsr04_exit(void)
{
	if (falling_irq != -1)
	{
		free_irq(falling_irq, NULL);
	}

	if (rising_irq != -1)
	{
		free_irq(rising_irq, NULL);
	}

	amlogic_gpio_free(echo_gpio, GPIO_OWNER);
	amlogic_gpio_free(trigger_gpio, GPIO_OWNER);

	class_unregister(&hcsr04_class);

	printk(KERN_INFO "HC-SR04 driver removed.\n");
}

module_init(hcsr04_init);
module_exit(hcsr04_exit);
