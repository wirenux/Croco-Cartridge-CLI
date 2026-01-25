#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb.h>
#include <arpa/inet.h>

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
        printf("\x1b[1;33mTry with `sudo`\n");
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
            fprintf(stderr, "\x1b[1;31mCRITICAL: Access denied. \x1b[1;33mTry running with `sudo` or close the WebApp.\n");
            fprintf(stderr, "\x1b[1;31mFailed to detach kernel driver: %s\n", libusb_error_name(ret));
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
        printf("\x1b[1;33mTry (in the same order): disconnect / reconnect, close the WebApp, or use `sudo`.\x1b[0m\n");
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
               i, name, num_rom_banks, num_ram_banks, mbc);

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

int upload_rom(CrocoDevice *device, const char *file_path, const char *rom_name) {
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        perror("Failed to open ROM file");
        return -1;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const int BANK_SIZE = 16384;
    const int CHUNK_SIZE = 32;
    const int CHUNKS_PER_BANK = 512;
    uint16_t total_banks = (uint16_t)((file_size + BANK_SIZE - 1) / BANK_SIZE);

    printf("Uploading: %s (%ld bytes, %u banks)\n", file_path, file_size, total_banks);

    // Command 0x02: Request Upload
    uint8_t req_payload[21] = {0};
    uint16_t be_banks = htons(total_banks);
    memcpy(req_payload, &be_banks, 2);

    // Copy name (max 15-17 chars based on your JS logic)
    strncpy((char*)(req_payload + 2), rom_name, 17);

    // Speed switch bank (65535 / 0xFFFF)
    uint16_t speed_switch = htons(0xFFFF);
    memcpy(req_payload + 19, &speed_switch, 2);

    uint8_t resp;
    if (execute_command(device, 0x02, req_payload, 21, &resp, 1) < 0 || resp != 0) {
        fprintf(stderr, "Upload request rejected by cartridge (Error: %d)\n", resp);
        fclose(f);
        return -1;
    }
    printf("\x1b[1;32mUpload request accepted.\x1b[0m\n");

    // Command 0x03: Send Chunks
    uint8_t *file_data = malloc(file_size);
    fread(file_data, 1, file_size, f);
    fclose(f);

    for (uint16_t b = 0; b < total_banks; b++) {
        printf("Writing Bank %u/%u...\r", b + 1, total_banks);
        fflush(stdout);

        for (uint16_t c = 0; c < CHUNKS_PER_BANK; c++) {
            uint8_t chunk_payload[36] = {0};
            uint32_t offset = (b * BANK_SIZE) + (c * CHUNK_SIZE);

            // Prepare Header: Bank (2 bytes) + Chunk (2 bytes) in Big Endian
            uint16_t be_b = htons(b);
            uint16_t be_c = htons(c);
            memcpy(chunk_payload, &be_b, 2);
            memcpy(chunk_payload + 2, &be_c, 2);

            // Copy Data (handle end of file padding)
            if (offset < file_size) {
                size_t to_copy = (file_size - offset < CHUNK_SIZE) ? (file_size - offset) : CHUNK_SIZE;
                memcpy(chunk_payload + 4, file_data + offset, to_copy);
            }

            if (execute_command(device, 0x03, chunk_payload, 36, &resp, 1) < 0 || resp != 0) {
                fprintf(stderr, "\nError at Bank %u, Chunk %u\n", b, c);
                free(file_data);
                return -1;
            }
        }
    }

    printf("\x1b[1;32m\n == Upload Finished Successfully! ==\x1b[0m\n");
    free(file_data);
    return 0;
}

int delete_rom(CrocoDevice *device, uint8_t rom_id) {
    printf("Attempting to delete ROM ID: %u...\n", rom_id);

    uint8_t payload = rom_id;
    uint8_t response[2];

    // Command 0x05: deleteRom
    int bytes = execute_command(device, 0x05, &payload, 1, response, sizeof(response));

    if (bytes < 1) {
        fprintf(stderr, "Error: No response from cartridge during delete.\n");
        return -1;
    }

    if (response[0] != 0) {
        fprintf(stderr, "\x1b[1;31mDelete failed! Cartridge rejected command with code: %d\x1b[0m\n", response[0]);
        return -1;
    }

    printf("\x1b[1;32mSuccessfully deleted ROM %u and its save file.\x1b[0m\n", rom_id);
    return 0;
}

void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -l, --list            List all games on cartridge\n");
    printf("  -i, --info            Get device information\n");
    printf("  -w <file> <name>      Write/Upload a ROM to the cartridge\n");
    printf("  -h, --help            Show this help message\n");
    printf("  -d <id>               Delete a ROM by its ID\n");
}

int main(int argc, char *argv[]) {
    CrocoDevice device = {0};
    int result = 0;

    if (libusb_init(NULL) != 0) {
        fprintf(stderr, "Failed to initialize libusb\n");
        return 1;
    }

    if (find_croco_device(&device) != 0) {
        libusb_exit(NULL);
        return 1;
    }

    printf("\x1b[1;32mCroco Cartridge found and connected!\x1b[0m\n");

    if (get_endpoints(&device) != 0 || configure_device(&device) != 0) {
        cleanup(&device);
        libusb_exit(NULL);
        return 1;
    }

    char choice;
    char path[256];
    char name[20];
    int rom_id;

    // loop to keep the cartridge alive
    while (1) {
        printf("\n--- Croco Menu ---\n");
        printf("l) List Games\n");
        printf("a) Add Game (Upload ROM)\n");
        printf("d) Delete a Game\n");
        printf("i) Device Info\n");
        printf("q) Quit\n");
        printf("Choice: ");

        if (scanf(" %c", &choice) != 1) break;

        if (choice == 'q') break;

        switch (choice) {
            case 'l':
                list_games(&device);
                break;
            case 'a':
                printf("Enter path to ROM file: ");
                scanf("%s", path);
                printf("Enter display name (max 17 chars): ");
                scanf("%s", name);
                upload_rom(&device, path, name);
                break;
            case 'd':
                printf("Enter ROM ID to delete: ");
                if (scanf("%d", &rom_id) == 1) {
                    delete_rom(&device, (uint8_t)rom_id);
                }
                break;
            case 'i':
                get_device_info(&device);
                break;
            default:
                printf("Unknown option.\n");
        }
    }

    cleanup(&device);
    libusb_exit(NULL);
    return 0;
}