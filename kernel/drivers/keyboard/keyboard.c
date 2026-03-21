#include <iris/keyboard.h>
#include <iris/serial.h>
#include <stdint.h>

/* PS/2 scancode set 1 -> ASCII (lowercase, no modifiers) */
static const char scancode_map[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*',
    0,   ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,   0,
    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0
};

/* circular key buffer */
static char     kbd_buf[KBD_BUFFER_SIZE];
static uint32_t kbd_head = 0;
static uint32_t kbd_tail = 0;
static uint8_t  shift_pressed = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void kbd_buf_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

static int32_t kbd_buf_pop(void) {
    if (kbd_tail == kbd_head) return -1;
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return (int32_t)(uint8_t)c;
}

/* Called from IRQ1 handler */
void kbd_irq_handler(void) {
    uint8_t sc = inb(KBD_DATA_PORT);

    /* key release — bit 7 set */
    if (sc & 0x80) {
        uint8_t released = sc & 0x7F;
        if (released == 0x2A || released == 0x36)
            shift_pressed = 0;
        return;
    }

    /* shift press */
    if (sc == 0x2A || sc == 0x36) {
        shift_pressed = 1;
        return;
    }

    if (sc >= 128) return;

    char c = scancode_map[sc];
    if (c == 0) return;

    /* apply shift */
    if (shift_pressed) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        else if (c == '1') c = '!';
        else if (c == '2') c = '@';
        else if (c == '3') c = '#';
        else if (c == '4') c = '$';
        else if (c == '5') c = '%';
        else if (c == '6') c = '^';
        else if (c == '7') c = '&';
        else if (c == '8') c = '*';
        else if (c == '9') c = '(';
        else if (c == '0') c = ')';
        else if (c == '-') c = '_';
        else if (c == '=') c = '+';
    }

    kbd_buf_push(c);

    /* echo to serial for debug */
    serial_write("[KBD] key='");
    char tmp[2] = {c, '\0'};
    serial_write(tmp);
    serial_write("' sc=");
    serial_write_dec(sc);
    serial_write("\n");
}

void kbd_init(void) {
    /* flush any pending data */
    while (inb(KBD_STATUS_PORT) & 0x01)
        inb(KBD_DATA_PORT);
    serial_write("[IRIS][KBD] PS/2 keyboard ready\n");
}

int32_t kbd_trygetchar(void) {
    return kbd_buf_pop();
}

int32_t kbd_getchar(void) {
    int32_t c;
    while ((c = kbd_buf_pop()) == -1)
        __asm__ volatile ("hlt");
    return c;
}

uint8_t kbd_scancode(void) {
    return inb(KBD_DATA_PORT);
}
