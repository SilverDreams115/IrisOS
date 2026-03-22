#include <stdint.h>
#include <iris/serial.h>
#include <iris/kernel.h>
#include <iris/boot_info.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/gdt.h>
#include <iris/idt.h>
#include <iris/pic.h>
#include <iris/scheduler.h>
#include <iris/task.h>
#include <iris/ipc.h>
#include <iris/tss.h>
#include <iris/syscall.h>
#include <iris/pci.h>
#include <iris/keyboard.h>
#include <iris/fb.h>
#include <iris/vfs.h>
#include <iris/irq_routing.h>
#include <iris/nameserver.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#define FB_ORANGE 0x00FF8800

#define COM1_PORT 0x3F8



static struct iris_boot_info saved_boot_info;

static int32_t ch_a_to_b = -1;
static int32_t ch_b_to_a = -1;

static void task_producer(void) {
    struct ipc_message msg;
    uint32_t counter = 0;
    for (;;) {
        counter++;
        msg.type        = IPC_MSG_DATA;
        msg.sender_id   = 1;
        msg.receiver_id = 2;
        msg.data_len    = 4;
        msg.data[0]     = (uint8_t)(counter & 0xFF);
        msg.data[1]     = (uint8_t)((counter >> 8) & 0xFF);
        msg.data[2]     = (uint8_t)((counter >> 16) & 0xFF);
        msg.data[3]     = (uint8_t)((counter >> 24) & 0xFF);

        int32_t r = ipc_send((uint32_t)ch_a_to_b, &msg);
        if (r == IPC_OK) {
            serial_write("[PRODUCER] sent #");
            serial_write_dec(counter);
            serial_write("\n");
        }

        struct ipc_message reply;
        ipc_recv((uint32_t)ch_b_to_a, &reply);
        serial_write("[PRODUCER] reply seq=");
        serial_write_dec(reply.seq);
        serial_write("\n");

        task_yield();
    }
}

static void task_consumer(void) {
    struct ipc_message msg;
    struct ipc_message reply;
    for (;;) {
        ipc_recv((uint32_t)ch_a_to_b, &msg);

        uint32_t value = (uint32_t)msg.data[0]
                       | ((uint32_t)msg.data[1] << 8)
                       | ((uint32_t)msg.data[2] << 16)
                       | ((uint32_t)msg.data[3] << 24);

        serial_write("[CONSUMER] value=");
        serial_write_dec(value);
        serial_write(" from=");
        serial_write_dec(msg.sender_id);
        serial_write("\n");

        reply.type        = IPC_MSG_REPLY;
        reply.sender_id   = 2;
        reply.receiver_id = msg.sender_id;
        reply.data_len    = 0;
        ipc_send((uint32_t)ch_b_to_a, &reply);

        task_yield();
    }
}

void iris_kernel_main(struct iris_boot_info *boot_info) {
    serial_init();

    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 13       \n");
    serial_write("====================================\n");
    serial_write("[IRIS][KERNEL] firmware services: OFF\n");

    if (!boot_info || boot_info->magic != IRIS_BOOTINFO_MAGIC) {
        serial_write("[IRIS][KERNEL] FATAL: invalid boot protocol\n");
        for (;;) __asm__ volatile ("hlt");
    }

    {
        uint64_t *src   = (uint64_t *)(uintptr_t)boot_info;
        uint64_t *dst   = (uint64_t *)(uintptr_t)&saved_boot_info;
        uint64_t  words = sizeof(struct iris_boot_info) / sizeof(uint64_t);
        for (uint64_t i = 0; i < words; i++) dst[i] = src[i];
    }

    serial_write("[IRIS][KERNEL] boot protocol OK (v");
    serial_write_dec(saved_boot_info.version);
    serial_write(")\n");

    serial_write("[IRIS][PMM] initializing...\n");
    pmm_init(&saved_boot_info);
    serial_write("[IRIS][PMM] free RAM: ");
    serial_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    serial_write(" MB\n");

    serial_write("[IRIS][PAGING] initializing...\n");
    paging_init(saved_boot_info.framebuffer.base, saved_boot_info.framebuffer.size);
    serial_write("[IRIS][PAGING] virtual memory active\n");

    serial_write("[IRIS][GDT] initializing...\n");
    gdt_init();
    serial_write("[IRIS][GDT] OK\n");

    serial_write("[IRIS][PIC] remapping IRQs...\n");
    pic_init();
    serial_write("[IRIS][PIT] timer at 100 Hz...\n");
    pit_init(100);

    serial_write("[IRIS][IDT] initializing...\n");
    idt_init();
    serial_write("[IRIS][IDT] OK\n");

    serial_write("[IRIS][IPC] initializing...\n");
    ipc_init();
    ch_a_to_b = ipc_channel_create(1);
    ch_b_to_a = ipc_channel_create(2);
    serial_write("[IRIS][IPC] channels: ");
    serial_write_dec((uint64_t)ch_a_to_b);
    serial_write(" and ");
    serial_write_dec((uint64_t)ch_b_to_a);
    serial_write("\n");

    serial_write("[IRIS][FB] initializing...\n");
    fb_init(&saved_boot_info);
    fb_fill(FB_BLACK);
    /* franjas de colores para validar framebuffer */
    uint32_t stripe = saved_boot_info.framebuffer.height / 7;
    fb_draw_rect(0, stripe * 0, saved_boot_info.framebuffer.width, stripe, FB_RED);
    fb_draw_rect(0, stripe * 1, saved_boot_info.framebuffer.width, stripe, FB_ORANGE);
    fb_draw_rect(0, stripe * 2, saved_boot_info.framebuffer.width, stripe, FB_YELLOW);
    fb_draw_rect(0, stripe * 3, saved_boot_info.framebuffer.width, stripe, FB_GREEN);
    fb_draw_rect(0, stripe * 4, saved_boot_info.framebuffer.width, stripe, FB_CYAN);
    fb_draw_rect(0, stripe * 5, saved_boot_info.framebuffer.width, stripe, FB_BLUE);
    fb_draw_rect(0, stripe * 6, saved_boot_info.framebuffer.width, stripe, FB_IRIS);
    serial_write("[IRIS][FB] framebuffer painted\n");

    serial_write("[IRIS][VFS] initializing...\n");
    vfs_init();
    vfs_mkdir("dev");
    serial_write("[IRIS][VFS] /dev created\n");

    /* crear archivo de prueba */
    int32_t fd = vfs_open("iris.txt", VFS_O_CREATE | VFS_O_WRITE);
    const char *msg = "Hello from IrisOS VFS!\n";
    uint32_t msglen = 0;
    while (msg[msglen]) msglen++;
    vfs_write(fd, msg, msglen);
    vfs_close(fd);

    /* leer de vuelta */
    char rdbuf[64];
    for (int i = 0; i < 64; i++) rdbuf[i] = 0;
    fd = vfs_open("iris.txt", VFS_O_READ);
    int32_t n = vfs_read(fd, rdbuf, 63);
    vfs_close(fd);

    serial_write("[IRIS][VFS] read iris.txt (");
    serial_write_dec((uint64_t)n);
    serial_write(" bytes): ");
    serial_write(rdbuf);

    uint32_t fsize = 0;
    vfs_stat("iris.txt", &fsize);
    serial_write("[IRIS][VFS] stat iris.txt size=");
    serial_write_dec(fsize);
    serial_write("\n");
    serial_write("[IRIS][VFS] OK\n");

    serial_write("[IRIS][PCI] initializing...\n");
    pci_init();

    serial_write("[IRIS][KBD] initializing...\n");
    kbd_init();

    serial_write("[IRIS][SYSCALL] initializing...\n");
    syscall_init();
    serial_write("[IRIS][SYSCALL] MSRs configured\n");

    serial_write("[IRIS][IRQ] initializing routing table...\n");
    irq_routing_init();

    serial_write("[IRIS][NS] initializing...\n");
    ns_init();
    serial_write("[IRIS][NS] bootstrap registry ready\n");

    serial_write("[IRIS][SCHED] initializing...\n");
    scheduler_init();
    scheduler_add_task(task_producer);
    scheduler_add_task(task_consumer);
    serial_write("[IRIS][SCHED] producer + consumer created\n");

    /* keyboard server: ring-3 task that receives IRQ1 via KChannel handle */
    {
        struct KChannel *kbd_ch = kchannel_alloc();
        if (kbd_ch) {
            irq_routing_register(1, kbd_ch);

            /* Spawn the server first so its handle table is ready */
            extern void kbd_server(void);
            /* Pass handle=0 temporarily; we'll fix it after inserting into table */
            struct task *ks = task_spawn_user((uint64_t)(uintptr_t)kbd_server, 0);
            if (ks) {
                /* Insert KChannel into kbd_server's handle table */
                handle_id_t h = handle_table_insert(&ks->process->handle_table,
                                                    &kbd_ch->base,
                                                    RIGHT_READ | RIGHT_WRITE);
                /* Patch arg0 on child's user stack using the physical address
                 * directly — ks->process->cr3 is not active here so paging_virt_to_phys
                 * would walk the wrong page table. ustack_phys is identity-mapped. */
                *(uint64_t *)(uintptr_t)(ks->ustack_phys + USER_STACK_SIZE - 8) = (uint64_t)h;

                iris_error_t nsr = ns_register("kbd", &kbd_ch->base,
                                               RIGHT_READ | RIGHT_WRITE);

                serial_write("[IRIS][KBD-SRV] keyboard server spawned, id=");
                serial_write_dec(ks->id);
                serial_write(", handle=");
                serial_write_dec((uint64_t)h);
                serial_write("\n");
                if (nsr == IRIS_OK) {
                    serial_write("[IRIS][NS] registered service 'kbd'\n");
                } else {
                    serial_write("[IRIS][NS] WARN: could not register 'kbd', err=");
                    serial_write_dec((uint64_t)(int64_t)nsr);
                    serial_write("\n");
                }
            } else {
                serial_write("[IRIS][KBD-SRV] WARN: could not spawn keyboard server\n");
            }
            /* Release our local reference — the table and routing table hold theirs */
            kobject_release(&kbd_ch->base);
        } else {
            serial_write("[IRIS][KBD-SRV] WARN: could not allocate KChannel\n");
        }
    }

    serial_write("[IRIS][USER] preparing init process...\n");

    /* user_init: first ring-3 bootstrap process */
    extern void user_init(void);
    struct task *ut = task_create_user((uint64_t)(uintptr_t)user_init);
    if (ut) {
        serial_write("[IRIS][USER] init task created, id=");
        serial_write_dec(ut->id);
        serial_write("\n");
    } else {
        serial_write("[IRIS][USER] WARN: could not create user task\n");
    }

    __asm__ volatile ("sti");
    serial_write("[IRIS][SCHED] IPC running\n");
    serial_write("====================================\n");

    /* yield a few times to let all tasks run including user_init */
    task_yield(); /* -> producer */
    task_yield(); /* -> consumer */
    task_yield(); /* -> user_init */
    task_yield(); /* -> back */

    for (;;) __asm__ volatile ("hlt");
}
