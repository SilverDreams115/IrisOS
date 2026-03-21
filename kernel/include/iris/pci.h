#ifndef IRIS_PCI_H
#define IRIS_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES  64

/* PCI config space offsets */
#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_CLASS_CODE     0x0B
#define PCI_SUBCLASS       0x0A
#define PCI_HEADER_TYPE    0x0E
#define PCI_BAR0           0x10

/* PCI IO ports */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

struct pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  header_type;
    uint32_t bar0;
};

void     pci_init(void);
uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

uint32_t              pci_device_count(void);
struct pci_device    *pci_get_device(uint32_t index);
struct pci_device    *pci_find_class(uint8_t class_code, uint8_t subclass);

/* class codes */
#define PCI_CLASS_STORAGE    0x01
#define PCI_CLASS_NETWORK    0x02
#define PCI_CLASS_DISPLAY    0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_BRIDGE     0x06
#define PCI_CLASS_SERIAL     0x0C

#endif
