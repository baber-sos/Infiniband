#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

__visible struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xc6c01fa, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0x16abfc10, __VMLINUX_SYMBOL_STR(param_ops_int) },
	{ 0xcd44d0d0, __VMLINUX_SYMBOL_STR(ib_get_dma_mr) },
	{ 0xbbd78bd4, __VMLINUX_SYMBOL_STR(dma_ops) },
	{ 0x61948d53, __VMLINUX_SYMBOL_STR(ib_dealloc_pd) },
	{ 0x2c3ec1d7, __VMLINUX_SYMBOL_STR(ib_destroy_cq) },
	{ 0xdfa6307c, __VMLINUX_SYMBOL_STR(ib_destroy_qp) },
	{ 0xb44c5fa4, __VMLINUX_SYMBOL_STR(ib_dereg_mr) },
	{ 0xa70a9fb5, __VMLINUX_SYMBOL_STR(rdma_disconnect) },
	{ 0xf95f5447, __VMLINUX_SYMBOL_STR(rdma_connect) },
	{ 0xd2b09ce5, __VMLINUX_SYMBOL_STR(__kmalloc) },
	{ 0x4c9d28b0, __VMLINUX_SYMBOL_STR(phys_base) },
	{ 0x3156661e, __VMLINUX_SYMBOL_STR(ib_create_cq) },
	{ 0x4b10c823, __VMLINUX_SYMBOL_STR(ib_alloc_pd) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x192729e2, __VMLINUX_SYMBOL_STR(rdma_destroy_id) },
	{ 0xd845eff6, __VMLINUX_SYMBOL_STR(rdma_create_id) },
	{ 0xaccabc6a, __VMLINUX_SYMBOL_STR(in4_pton) },
	{ 0x9e88526, __VMLINUX_SYMBOL_STR(__init_waitqueue_head) },
	{ 0x81fcd7c8, __VMLINUX_SYMBOL_STR(kmem_cache_alloc_trace) },
	{ 0x92a94ad2, __VMLINUX_SYMBOL_STR(kmalloc_caches) },
	{ 0x4d0f09a6, __VMLINUX_SYMBOL_STR(rdma_create_qp) },
	{ 0xdb7305a1, __VMLINUX_SYMBOL_STR(__stack_chk_fail) },
	{ 0xf08242c2, __VMLINUX_SYMBOL_STR(finish_wait) },
	{ 0x2207a57f, __VMLINUX_SYMBOL_STR(prepare_to_wait_event) },
	{ 0x1000e51, __VMLINUX_SYMBOL_STR(schedule) },
	{ 0xa1c76e0a, __VMLINUX_SYMBOL_STR(_cond_resched) },
	{ 0x3517fbb6, __VMLINUX_SYMBOL_STR(rdma_resolve_addr) },
	{ 0xa6bbd805, __VMLINUX_SYMBOL_STR(__wake_up) },
	{ 0x39ce4d69, __VMLINUX_SYMBOL_STR(rdma_resolve_route) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0xbdfb6dbb, __VMLINUX_SYMBOL_STR(__fentry__) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=ib_core,rdma_cm";


MODULE_INFO(srcversion, "535CAD5C73D1CBE99D58777");
