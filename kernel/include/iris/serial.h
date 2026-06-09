#ifndef IRIS_SERIAL_H
#define IRIS_SERIAL_H

/* Early-boot serial: ring-0, boot-phase and fatal/panic paths only.
 * Normal output uses klog → SYS_KLOG_DRAIN → ring-3 console service. */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

#endif
