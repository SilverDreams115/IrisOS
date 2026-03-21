#include <iris/pci.h>
#include <iris/serial.h>
#include <stdint.h>

static struct pci_device pci_devices[PCI_MAX_DEVICES];
static uint32_t pci_count = 0;

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1UL << 31)
                  | ((uint32_t)bus    << 16)
                  | ((uint32_t)dev    << 11)
                  | ((uint32_t)func   <<  8)
                  | ((uint32_t)offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1UL << 31)
                  | ((uint32_t)bus    << 16)
                  | ((uint32_t)dev    << 11)
                  | ((uint32_t)func   <<  8)
                  | ((uint32_t)offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, val);
}

static const char *pci_class_name(uint8_t class_code) {
    switch (class_code) {
        case PCI_CLASS_STORAGE:    return "Storage";
        case PCI_CLASS_NETWORK:    return "Network";
        case PCI_CLASS_DISPLAY:    return "Display";
        case PCI_CLASS_MULTIMEDIA: return "Multimedia";
        case PCI_CLASS_BRIDGE:     return "Bridge";
        case PCI_CLASS_SERIAL:     return "Serial Bus";
        default:                   return "Unknown";
    }
}

static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t id = pci_read(bus, dev, func, PCI_VENDOR_ID);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)(id >> 16);

    if (vendor == 0xFFFF) return; /* no device */
    if (pci_count >= PCI_MAX_DEVICES) return;

    uint32_t class_info = pci_read(bus, dev, func, 0x08);
    uint8_t class_code  = (uint8_t)(class_info >> 24);
    uint8_t subclass    = (uint8_t)(class_info >> 16);
    uint8_t header_type = (uint8_t)(pci_read(bus, dev, func, PCI_HEADER_TYPE) & 0xFF);
    uint32_t bar0       = pci_read(bus, dev, func, PCI_BAR0);

    struct pci_device *d = &pci_devices[pci_count++];
    d->bus         = bus;
    d->device      = dev;
    d->function    = func;
    d->vendor_id   = vendor;
    d->device_id   = device;
    d->class_code  = class_code;
    d->subclass    = subclass;
    d->header_type = header_type;
    d->bar0        = bar0;

    serial_write("[PCI] ");
    serial_write_hex((uint32_t)bus);
    serial_write(":");
    serial_write_hex((uint32_t)dev);
    serial_write(".");
    serial_write_dec(func);
    serial_write("  vendor=");
    serial_write_hex(vendor);
    serial_write(" device=");
    serial_write_hex(device);
    serial_write(" class=");
    serial_write(pci_class_name(class_code));
    serial_write("\n");
}

static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint32_t id = pci_read(bus, dev, 0, PCI_VENDOR_ID);
    if ((id & 0xFFFF) == 0xFFFF) return;

    pci_scan_function(bus, dev, 0);

    uint8_t header = (uint8_t)(pci_read(bus, dev, 0, PCI_HEADER_TYPE) & 0xFF);
    if (header & 0x80) {
        /* multi-function device */
        for (uint8_t func = 1; func < 8; func++) {
            uint32_t fid = pci_read(bus, dev, func, PCI_VENDOR_ID);
            if ((fid & 0xFFFF) != 0xFFFF)
                pci_scan_function(bus, dev, func);
        }
    }
}

void pci_init(void) {
    pci_count = 0;
    serial_write("[IRIS][PCI] scanning buses...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            pci_scan_device((uint8_t)bus, dev);
        }
    }

    serial_write("[IRIS][PCI] found ");
    serial_write_dec(pci_count);
    serial_write(" device(s)\n");
}

uint32_t pci_device_count(void) { return pci_count; }

struct pci_device *pci_get_device(uint32_t index) {
    if (index >= pci_count) return (void *)0;
    return &pci_devices[index];
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < pci_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass   == subclass)
            return &pci_devices[i];
    }
    return (void *)0;
}
