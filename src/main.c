#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>

#define CROCO_VENDOR_ID  0x2e8a
#define CROCO_PRODUCT_ID 0x107F
#define TIMEOUT_MS 5000

typedef struct {
    libusb_device_handle *dev;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t out_ep;
    uint8_t in_ep;
    int if_num;
} CrocoDevice;

typedef struct {
    uint8_t rom_id;
    char name[18];
    uint8_t num_ram_banks;
    uint8_t mbc;
    uint16_t num_rom_banks;
} RomInfo;

int find_croco_device(CrocoDevice *device) {
    libusb_device **devs;
    libusb_device *found = NULL;
    ssize_t cnt = libusb_get_device_list(NULL, &devs);

    if (cnt < 0) {
        fprintf(stderr, "Error getting device list\n");
        return -1;
    }

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) == 0) {
            if (desc.idVendor == CROCO_VENDOR_ID && desc.idProduct == CROCO_PRODUCT_ID) {
                printf("Found device: %04x:%04x\n", desc.idVendor, desc.idProduct);
                found = devs[i];
                break;
            }
        }
    }

    if (!found) {
        fprintf(stderr, "Croco Cartridge not found\n");
        libusb_free_device_list(devs, 1);
        return -1;
    }

    if (libusb_open(found, &device->dev) != 0) {
        fprintf(stderr, "Failed to open device\n");
        libusb_free_device_list(devs, 1);
        return -1;
    }

    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(found, &desc);
    device->vendor_id = desc.idVendor;
    device->product_id = desc.idProduct;

    libusb_free_device_list(devs, 1);
    return 0;
}

int get_endpoints(CrocoDevice *device) {
    struct libusb_config_descriptor *config = NULL;
    const struct libusb_interface *iface = NULL;
    const struct libusb_interface_descriptor *iface_desc = NULL;
    int ret = 0;

    ret = libusb_get_active_config_descriptor(libusb_get_device(device->dev), &config);
    if (ret != 0) {
        fprintf(stderr, "Failed to get config descriptor: %s\n", libusb_error_name(ret));
        return -1;
    }

    // Find interface with class 0xFF (vendor specific)
    for (int i = 0; i < config->bNumInterfaces; i++) {
        iface = &config->interface[i];
        if (iface->num_altsetting > 0) {
            iface_desc = &iface->altsetting[0];

            if (iface_desc->bInterfaceClass == 0xFF) {
                device->if_num = iface_desc->bInterfaceNumber;

                for (int j = 0; j < iface_desc->bNumEndpoints; j++) {
                    const struct libusb_endpoint_descriptor *ep = &iface_desc->endpoint[j];

                    if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                        if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                            device->in_ep = ep->bEndpointAddress;
                        } else {
                            device->out_ep = ep->bEndpointAddress;
                        }
                    }
                }
                break;
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (device->out_ep == 0 || device->in_ep == 0) {
        fprintf(stderr, "Could not find bulk endpoints\n");
        return -1;
    }

    return 0;
}

int configure_device(CrocoDevice *device) {
    int ret;

    ret = libusb_kernel_driver_active(device->dev, 0);
    if (ret == 1) {
        ret = libusb_detach_kernel_driver(device->dev, 0);
        if (ret != 0 && ret != LIBUSB_ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "Failed to detach kernel driver: %s\n", libusb_error_name(ret));
            return -1;
        }
    }

    ret = libusb_claim_interface(device->dev, device->if_num);
    if (ret != 0) {
        fprintf(stderr, "Failed to claim interface: %s\n", libusb_error_name(ret));
        return -1;
    }

    ret = libusb_set_interface_alt_setting(device->dev, device->if_num, 0);
    if (ret != 0) {
        fprintf(stderr, "Failed to set alt setting: %s\n", libusb_error_name(ret));
        libusb_release_interface(device->dev, device->if_num);
        return -1;
    }

    // Control transfer - request 0x22, value 0x01 (CDC protocol setup)
    ret = libusb_control_transfer(
        device->dev,
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
        0x22,
        0x01,
        device->if_num,
        NULL,
        0,
        TIMEOUT_MS
    );

    if (ret != 0) {
        fprintf(stderr, "Control transfer failed: %s\n", libusb_error_name(ret));
        libusb_release_interface(device->dev, device->if_num);
        return -1;
    }

    return 0;
}

int send_command(CrocoDevice *device, uint8_t *cmd, int cmd_len) {
    int transferred = 0;
    int result = libusb_bulk_transfer(
        device->dev,
        device->out_ep,
        cmd,
        cmd_len,
        &transferred,
        TIMEOUT_MS
    );

    if (result != 0) {
        fprintf(stderr, "Failed to send command: %s\n", libusb_error_name(result));
        return -1;
    }

    return transferred;
}

int read_response(CrocoDevice *device, uint8_t *buffer, int max_len) {
    int transferred = 0;
    int result = libusb_bulk_transfer(
        device->dev,
        device->in_ep,
        buffer,
        max_len,
        &transferred,
        TIMEOUT_MS
    );

    if (result != 0 && result != LIBUSB_ERROR_TIMEOUT) {
        fprintf(stderr, "Failed to read response: %s\n", libusb_error_name(result));
        return -1;
    }

    return transferred;
}

int execute_command(CrocoDevice *device, uint8_t command, uint8_t *payload,
                    int payload_len, uint8_t *response, int response_len) {
    uint8_t cmd_buffer[65];
    int cmd_len = 1 + payload_len;

    if (cmd_len > 65) {
        fprintf(stderr, "Command too large\n");
        return -1;
    }

    cmd_buffer[0] = command;
    if (payload_len > 0 && payload != NULL) {
        memcpy(cmd_buffer + 1, payload, payload_len);
    }

    if (send_command(device, cmd_buffer, cmd_len) < 0) {
        return -1;
    }

    usleep(10000);  // 10ms delay

    uint8_t buffer[128];
    int bytes_read = read_response(device, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        return -1;
    }

    if (bytes_read < 1) {
        fprintf(stderr, "No response from device\n");
        return -1;
    }

    // First byte should echo the command
    if (buffer[0] != command) {
        fprintf(stderr, "Command echo mismatch: expected 0x%02x, got 0x%02x\n",
                command, buffer[0]);
        return -1;
    }

    // Copy response data (skip echo byte)
    int data_len = bytes_read - 1;
    if (data_len > response_len) {
        data_len = response_len;
    }
    memcpy(response, buffer + 1, data_len);

    return data_len;
}

int list_games(CrocoDevice *device) {
    printf("\nFetching ROM information...\n");

    uint8_t response[10];
    int bytes = execute_command(device, 0x01, NULL, 0, response, sizeof(response));

    if (bytes < 5) {
        fprintf(stderr, "Failed to get ROM utilization\n");
        return -1;
    }

    uint8_t num_roms = response[0];
    uint16_t used_banks = ((response[2] << 8) | response[1]) / 256;
    uint16_t max_banks = 888;

    // TODO: change ./README.md:3
    printf("Found %u game(s) using %u / %u banks (for more info: ./README.md:3)\n\n", num_roms, used_banks, max_banks);

    if (num_roms == 0) {
        printf("No ROMs found on cartridge\n");
        return 0;
    }

    // Fetch info for each ROM
    for (int i = 0; i < num_roms; i++) {
        uint8_t rom_id = i;
        uint8_t info_response[25];

        int info_bytes = execute_command(device, 0x04, &rom_id, 1,
                                         info_response, sizeof(info_response));

        if (info_bytes < 20) {
            fprintf(stderr, "Failed to get ROM %u info\n", i);
            continue;
        }

        char name[18];
        memcpy(name, info_response, 17);
        name[17] = '\0';

        uint8_t num_ram_banks = info_response[17];
        uint8_t mbc = (info_bytes > 18) ? info_response[18] : 0xFF;
        uint16_t num_rom_banks = 0;
        if (info_bytes > 20) {
            num_rom_banks = (info_response[20] << 8) | info_response[19];
        }

        printf("[%2u] %-22s | ROM: %5u x 32KB | RAM: %u x 8KB | MBC: 0x%02x\n",
               i + 1, name, num_rom_banks, num_ram_banks, mbc);

        usleep(10000); // safety delay
    }

    return 0;
}

int get_device_info(CrocoDevice *device) {
    printf("\nFetching device information...\n\n");

    uint8_t response[15];
    int bytes = execute_command(device, 0xFE, NULL, 0, response, sizeof(response));

    if (bytes < 11) {
        fprintf(stderr, "Failed to get device info\n");
        return -1;
    }

    printf("Device Information:\n");
    printf("  Feature Step: %u\n", response[0]);
    printf("  HW Version: %u\n", response[1]);
    printf("  SW Version: %u.%u.%u%c\n", response[2], response[3], response[4], response[5]);
    printf("  Git Short: 0x%08x\n",
           (response[6] << 24) | (response[7] << 16) | (response[8] << 8) | response[9]);
    printf("  Git Dirty: %s\n", response[10] ? "yes" : "no");

    // Get serial ID (command 0xFD)
    usleep(50000);
    uint8_t serial_response[10];
    int serial_bytes = execute_command(device, 0xFD, NULL, 0, serial_response, sizeof(serial_response));

    if (serial_bytes >= 8) {
        printf("  Serial ID: ");
        for (int i = 0; i < 8; i++) {
            printf("%02X", serial_response[i]);
        }
        printf("\n");
    }

    return 0;
}

void cleanup(CrocoDevice *device) {
    if (device->dev) {
        libusb_release_interface(device->dev, device->if_num);
        libusb_close(device->dev);
    }
}

void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -l, --list      List all games on cartridge\n");
    printf("  -i, --info      Get device information\n");
    printf("  -h, --help      Show this help message\n");
}

int main(int argc, char *argv[]) {
    CrocoDevice device = {0};
    int result = 0;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (libusb_init(NULL) != 0) {
        fprintf(stderr, "Failed to initialize libusb\n");
        return 1;
    }

    if (find_croco_device(&device) != 0) {
        libusb_exit(NULL);
        return 1;
    }

    printf("Croco Cartridge found!\n");
    printf("Vendor ID: 0x%04x, Product ID: 0x%04x\n\n",
           device.vendor_id, device.product_id);

    if (get_endpoints(&device) != 0) {
        cleanup(&device);
        libusb_exit(NULL);
        return 1;
    }

    if (configure_device(&device) != 0) {
        cleanup(&device);
        libusb_exit(NULL);
        return 1;
    }

    const char *arg = argv[1];
    if (strcmp(arg, "-l") == 0 || strcmp(arg, "--list") == 0) {
        result = list_games(&device);
    } else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--info") == 0) {
        result = get_device_info(&device);
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
        print_usage(argv[0]);
    } else {
        fprintf(stderr, "Unknown option: %s\n", arg);
        print_usage(argv[0]);
        result = 1;
    }

    cleanup(&device);
    libusb_exit(NULL);
    return result;
}