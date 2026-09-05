#include <linux/init.h>
#include <linux/module.h>

static int __init hello_init(void)
{
    pr_info("rk3568_hello: module loaded\n");
    return 0;
}

static void __exit hello_exit(void)
{
    pr_info("rk3568_hello: module unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RK3568 minimal module");