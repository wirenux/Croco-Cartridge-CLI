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
        printf("\x1b[1;33mTry with `sudo`\x1b[0m\n");
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

    usleep(5000);  // 5ms delay

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

int list_games(CrocoDevice *device, int mode) {
    printf("\n   \x1b[1;34m[>] Fetching Cartridge Memory...\x1b[0m\n");

    uint8_t response[10];
    int bytes = execute_command(device, 0x01, NULL, 0, response, sizeof(response));

    if (bytes < 5) {
        fprintf(stderr, "\x1b[1;31m[!] Error: Failed to retrieve ROM utilization\x1b[0m\n");
        return -1;
    }

    uint8_t num_roms = response[0];
    uint16_t used_banks = ((response[2] << 8) | response[1]) / 256;
    uint16_t max_banks = 888;
    float percent = ((float)used_banks / max_banks) * 100;
    
    if (mode != 1) {
        printf("   \x1b[1;33m+-------------------------------------------------------------+\x1b[0m\n");
        printf("     Storage: [\x1b[1;32m%u/%u Banks\x1b[0m] used (%.1f%% full)\n", used_banks, max_banks, percent);
        printf("     Capacity: %u Games Registered\n", num_roms);
        printf("   \x1b[1;33m+-------------------------------------------------------------+\x1b[0m\n\n");

        if (num_roms == 0) {
            printf("     \x1b[90m(No ROMs found on cartridge memory)\x1b[0m\n");
            return 0;
        }
    }

    printf(" \x1b[1;37m  ID   NAME                     | ROM SIZE   | RAM     | MBC \x1b[0m\n");
    printf(" \x1b[90m  ---- ------------------------ | ---------- | ------- | ----\x1b[0m\n");

    for (int i = 0; i < num_roms; i++) {
        uint8_t rom_id = i;
        uint8_t info_response[25];

        int info_bytes = execute_command(device, 0x04, &rom_id, 1, info_response, sizeof(info_response));

        if (info_bytes < 20) {
            fprintf(stderr, "  \x1b[31m[!] Error reading slot %u\x1b[0m\n", i);
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

        // Inside your loop, replace your existing printf with this:

        printf("   [\x1b[32m%2u\x1b[0m]  \x1b[1;36m%-23s\x1b[0m | \x1b[33m%3u Banks \x1b[0m | RAM: %2u | MBC: 0x%02X\n",
            i, 
            name, 
            num_rom_banks / 256,  // This replaces the size in KB
            num_ram_banks, 
            mbc);

        usleep(5000); 
    }
    printf(" \x1b[90m  -------------------------------------------------------------\x1b[0m\n");

    return 0;
}

int get_device_info(CrocoDevice *device) {
    printf("\n   \x1b[1;34m[>] Accessing Hardware Registers...\x1b[0m\n\n");

    uint8_t response[15];
    int bytes = execute_command(device, 0xFE, NULL, 0, response, sizeof(response));

    if (bytes < 11) {
        printf("   \x1b[1;31m[!] CRITICAL ERROR: Hardware communication timeout.\x1b[0m\n");
        return -1;
    }

    // Header for the Hardware Card
    printf("   \x1b[1;37mCROCO HARDWARE MANIFEST\x1b[0m\n");
    printf("   \x1b[90m=============================================================\x1b[0m\n");

    // Feature and Hardware version
    printf("    \x1b[1m%-15s\x1b[0m %u\n", "Feature Step:", response[0]);
    printf("    \x1b[1m%-15s\x1b[0m v%u\n", "HW Revision:", response[1]);

    // Software version with a nice color highlight
    printf("    \x1b[1m%-15s\x1b[0m \x1b[32m%u.%u.%u%c\x1b[0m\n", 
           "Firmware:", response[2], response[3], response[4], response[5]);

    // Git Hash
    uint32_t git_hash = (response[6] << 24) | (response[7] << 16) | (response[8] << 8) | response[9];
    printf("    \x1b[1m%-15s\x1b[0m \x1b[36m#%08x\x1b[0m\n", "Git Commit:", git_hash);

    // Git Dirty (Red if dirty, Green if clean)
    const char* dirty_label = response[10] ? "\x1b[31mYES (Modified)\x1b[0m" : "\x1b[32mNO (Clean)\x1b[0m";
    printf("    \x1b[1m%-15s\x1b[0m %s\n", "Git Dirty:", dirty_label);

    // Get serial ID (command 0xFD)
    usleep(5000); 
    uint8_t serial_response[10];
    int serial_bytes = execute_command(device, 0xFD, NULL, 0, serial_response, sizeof(serial_response));

    if (serial_bytes >= 8) {
        printf("    \x1b[1m%-15s\x1b[0m \x1b[1;33m", "Serial ID:");
        for (int i = 0; i < 8; i++) {
            printf("%02X", serial_response[i]);
        }
        printf("\x1b[0m\n");
    }

    printf("   \x1b[90m=============================================================\x1b[0m\n");

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
        printf("\x1b[1;31m[!] CRITICAL ERROR: Could not open ROM file: %s\x1b[0m\n", file_path);
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

    printf("\n\x1b[1;34m   [>] Initializing Data Stream...\x1b[0m\n");
    printf("       Target:  \x1b[1;36m%s\x1b[0m\n", rom_name);
    printf("       Size:    \x1b[1;33m%ld bytes\x1b[0m (%u banks)\n", file_size, total_banks);

    // Command 0x02: Request Upload
    uint8_t req_payload[21] = {0};
    uint16_t be_banks = htons(total_banks);
    memcpy(req_payload, &be_banks, 2);
    strncpy((char*)(req_payload + 2), rom_name, 17);
    uint16_t speed_switch = htons(0xFFFF);
    memcpy(req_payload + 19, &speed_switch, 2);

    uint8_t resp;
    if (execute_command(device, 0x02, req_payload, 21, &resp, 1) < 0 || resp != 0) {
        fprintf(stderr, "\x1b[1;31m[!] Upload request rejected by cartridge (Error: %d)\x1b[0m\n", resp);
        fclose(f);
        return -1;
    }
    printf("\n\x1b[1;32m   [+] Handshake successful. Uploading data...\x1b[0m\n\n");

    // Command 0x03: Send Chunks
    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        fclose(f);
        return -1;
    }
    fread(file_data, 1, file_size, f);
    fclose(f);

    for (uint16_t b = 0; b < total_banks; b++) {
        printf("\r       \x1b[1;33mWriting Bank:\x1b[0m [\x1b[1;32m%u\x1b[0m/\x1b[1;32m%u\x1b[0m] ... ", b + 1, total_banks);
        fflush(stdout);

        for (uint16_t c = 0; c < CHUNKS_PER_BANK; c++) {
            uint8_t chunk_payload[36] = {0};
            uint32_t offset = (b * BANK_SIZE) + (c * CHUNK_SIZE);

            uint16_t be_b = htons(b);
            uint16_t be_c = htons(c);
            memcpy(chunk_payload, &be_b, 2);
            memcpy(chunk_payload + 2, &be_c, 2);

            if (offset < file_size) {
                size_t to_copy = (file_size - offset < CHUNK_SIZE) ? (file_size - offset) : CHUNK_SIZE;
                memcpy(chunk_payload + 4, file_data + offset, to_copy);
            }

            if (execute_command(device, 0x03, chunk_payload, 36, &resp, 1) < 0 || resp != 0) {
                printf("\n\x1b[1;31m[!] WRITE ERROR at Bank %u, Chunk %u\x1b[0m\n", b, c);
                free(file_data);
                return -1;
            }
        }
    }

    printf("\n\n\x1b[1;32m   =================================================\x1b[0m\n");
    printf("\x1b[1;32m       SUCCESS: ROM flashed to cartridge memory!\x1b[0m\n");
    printf("\x1b[1;32m   =================================================\x1b[0m\n");

    free(file_data);
    return 0;
}

int delete_rom(CrocoDevice *device, uint8_t rom_id) {
    printf("      Attempting to delete ROM ID: %u...\n", rom_id);

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

    printf("      \x1b[1;32mSuccessfully deleted ROM %u and its save file.\x1b[0m\n", rom_id);
    return 0;
}

int download_save(CrocoDevice *device, uint8_t rom_id, const char *dest_path, uint8_t num_ram_banks) {
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        printf("\x1b[1;31m[!] ERROR: Could not create save file: %s\x1b[0m\n", dest_path);
        return -1;
    }

    const int SRAM_BANK_SIZE = 8192;
    const int CHUNK_SIZE = 32;
    const int CHUNKS_PER_BANK = SRAM_BANK_SIZE / CHUNK_SIZE;
    uint32_t total_size = num_ram_banks * SRAM_BANK_SIZE;

    printf("\n\x1b[1;34m   [>] Requesting Savegame Data...\x1b[0m\n");
    printf("       ROM ID:  \x1b[1;36m%u\x1b[0m\n", rom_id);
    printf("       Size:    \x1b[1;33m%u bytes\x1b[0m (%u RAM banks)\n", total_size, num_ram_banks);

    // Command 0x06: Request Save Download
    uint8_t resp;
    if (execute_command(device, 0x06, &rom_id, 1, &resp, 1) < 0 || resp != 0) {
        printf("\x1b[1;31m[!] Download request rejected (Code: %d)\x1b[0m\n", resp);
        fclose(f);
        return -1;
    }
    printf("\x1b[1;32m   [+] Handshake successful. Receiving chunks...\x1b[0m\n\n");

    // Command 0x07: Receive Chunks
    for (uint16_t b = 0; b < num_ram_banks; b++) {
        printf("\r       \x1b[1;33mReading Bank:\x1b[0m [\x1b[1;32m%u\x1b[0m/\x1b[1;32m%u\x1b[0m] ... ", b + 1, num_ram_banks);
        fflush(stdout);

        for (uint16_t c = 0; c < CHUNKS_PER_BANK; c++) {
            uint8_t chunk_resp[36]; // 2 (bank) + 2 (chunk) + 32 (data)

            if (execute_command(device, 0x07, NULL, 0, chunk_resp, 36) < 36) {
                printf("\n\x1b[1;31m[!] READ ERROR at Bank %u, Chunk %u\x1b[0m\n", b, c);
                fclose(f);
                return -1;
            }

            uint16_t received_b = (uint16_t)((chunk_resp[0] << 8) | chunk_resp[1]);
            uint16_t received_c = (uint16_t)((chunk_resp[2] << 8) | chunk_resp[3]);

            if (received_b != b || received_c != c) {
                printf("\n\x1b[1;31m[!] SYNCHRONIZATION ERROR!\x1b[0m\n");
                printf("    Expected: Bank %u, Chunk %u\n", b, c);
                printf("    Received: Bank %u, Chunk %u\n", received_b, received_c);
                printf("    \x1b[1;33mAdvice: Check USB connection or try a lower speed.\x1b[0m\n");
                fclose(f);
                return -1;
            }

            if (fwrite(chunk_resp + 4, 1, 32, f) != 32) {
                printf("\n\x1b[1;31m[!] DISK ERROR: Failed to write to save file.\x1b[0m\n");
                fclose(f);
                return -1;
            }
        }
    }

    printf("\n\n\x1b[1;32m   =================================================\x1b[0m\n");
    printf("\x1b[1;32m       SUCCESS: Savegame dumped to %s\x1b[0m\n", dest_path);
    printf("\x1b[1;32m   =================================================\x1b[0m\n");

    fclose(f);
    return 0;
}

int upload_save(CrocoDevice *device, uint8_t rom_id, const char *file_path, uint8_t num_ram_banks) {
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        printf("\x1b[1;31m[!] ERROR: Could not open save file: %s\x1b[0m\n", file_path);
        return -1;
    }

    const int SRAM_BANK_SIZE = 8192;
    const int CHUNK_SIZE = 32;
    const int CHUNKS_PER_BANK = SRAM_BANK_SIZE / CHUNK_SIZE;

    // check file if fit in RAM
    fseek(f, 0, SEEK_END);
    long actual_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t expected_size = num_ram_banks * SRAM_BANK_SIZE;
    if (actual_size < expected_size) {
        printf("\x1b[1;33m[!] WARNING: File is smaller than expected (%ld < %u bytes). Padding with zeros.\x1b[0m\n", actual_size, expected_size);
    }

    printf("\n\x1b[1;34m   [>] Initializing Save Upload...\x1b[0m\n");
    printf("       Target ROM ID: \x1b[1;36m%u\x1b[0m\n", rom_id);
    printf("       Total Upload:  \x1b[1;33m%u bytes\x1b[0m\n", expected_size);

    // Command 0x08: Request Save Upload
    uint8_t resp;
    if (execute_command(device, 0x08, &rom_id, 1, &resp, 1) < 0 || resp != 0) {
        printf("\x1b[1;31m[!] Upload request rejected by cartridge (Code: %d)\x1b[0m\n", resp);
        fclose(f);
        return -1;
    }
    printf("\x1b[1;32m   [+] Handshake successful. Sending SRAM data...\x1b[0m\n\n");

    // Command 0x09: Send Chunks
    for (uint16_t b = 0; b < num_ram_banks; b++) {
        printf("\r       \x1b[1;33mWriting Bank:\x1b[0m [\x1b[1;32m%u\x1b[0m/\x1b[1;32m%u\x1b[0m] ... ", b + 1, num_ram_banks);
        fflush(stdout);

        for (uint16_t c = 0; c < CHUNKS_PER_BANK; c++) {
            uint8_t chunk_payload[36] = {0};

            // Format: [Bank High, Bank Low, Chunk High, Chunk Low, ...Data...]
            uint16_t be_b = htons(b);
            uint16_t be_c = htons(c);
            memcpy(chunk_payload, &be_b, 2);
            memcpy(chunk_payload + 2, &be_c, 2);

            size_t read_bytes = fread(chunk_payload + 4, 1, CHUNK_SIZE, f);

            if (execute_command(device, 0x09, chunk_payload, 36, &resp, 1) < 0 || resp != 0) {
                printf("\n\x1b[1;31m[!] WRITE ERROR at Bank %u, Chunk %u\x1b[0m\n", b, c);
                fclose(f);
                return -1;
            }
        }
    }

    printf("\n\n\x1b[1;32m   =================================================\x1b[0m\n");
    printf("\x1b[1;32m       SUCCESS: Savegame uploaded to cartridge!\x1b[0m\n");
    printf("\x1b[1;32m   =================================================\x1b[0m\n");

    fclose(f);
    return 0;
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

    printf("\033[H\033[J"); // clear
    printf(
        "    █████████                                           █████████  █████       █████\n"
        "  ███░░░░░███                                         ███░░░░░███░░███        ░░███ \n"
        " ███     ░░░  ████████   ██████   ██████   ██████     ███     ░░░  ░███         ░███ \n"
        "░███          ░░███░░███ ███░░███ ███░░███ ███░░███   ░███          ░███         ░███ \n"
        "░███           ░███ ░░░ ░███ ░███░███ ░░░ ░███ ░███   ░███          ░███         ░███ \n"
        "░░███     ███  ░███     ░███ ░███░███  ███░███ ░███   ░░███     ███ ░███      █  ░███ \n"
        " ░░█████████   █████    ░░██████ ░░██████ ░░██████     ░░█████████  ███████████ █████\n"
        "  ░░░░░░░░░   ░░░░░      ░░░░░░   ░░░░░░   ░░░░░░       ░░░░░░░░░  ░░░░░░░░░░░ ░░░░░ \n"
    );
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
        printf("\n  \x1b[1mMAIN INTERFACE\x1b[0m\n");
        printf("  \x1b[34m[l]\x1b[0m List Library\n");
        printf("  \x1b[32m[a]\x1b[0m Flash New ROM\n");
        printf("  \x1b[32m[s]\x1b[0m Backup Savegame\n");
        printf("  \x1b[32m[u]\x1b[0m Upload Savegame\n");
        printf("  \x1b[31m[d]\x1b[0m Wipe ROM\n");
        printf("  \x1b[34m[i]\x1b[0m Hardware Info\n");
        printf("  \x1b[90m[q]\x1b[0m Disconnect\n");
        printf("\n  \x1b[1;34m[>] \x1b[0m");
        fflush(stdout);
        if (scanf(" %c", &choice) != 1) break;

        if (choice == 'q') {
            printf("\x1b[34mDisconnecting safely...\x1b[0m\n");
            break;
        }

        switch (choice) {
            case 'l':
                list_games(&device, 0);
                break;
            case 'a': {
                    printf("\n\x1b[1;34m   [?]\x1b[0m \x1b[1mEnter path to ROM file (or 'EXIT'): \x1b[0m");
                    fflush(stdout); 
                    if (scanf("%s", path) != 1) break;

                    if (strcasecmp(path, "EXIT") == 0) {
                        printf("    \x1b[1;34mUpload cancelled.\x1b[0m\n");
                        break;
                    }

                    printf("\x1b[1;34m   [?]\x1b[0m \x1b[1mEnter display name (max 17 chars): \x1b[0m");
                    fflush(stdout);
                    if (scanf("%s", name) != 1) break;

                    if (strcasecmp(name, "EXIT") == 0) { // case sensitive
                        printf("    \x1b[1;34mUpload cancelled.\x1b[0m\n");
                        break;
                    }

                    upload_rom(&device, path, name);
                }
                break;
            case 's': {
                    char input[16];
                    char save_path[256];
                    list_games(&device, 1); // mode 1 = no header

                    printf("\n\x1b[1;34m   [?] Enter ROM ID to download save (or 'EXIT'): \x1b[0m");
                    fflush(stdout);
                    if (scanf("%s", input) != 1) break;
                    if (strcasecmp(input, "EXIT") == 0) break;

                    uint8_t target_id = (uint8_t)atoi(input);

                    // Fetch ROM info first to know how many RAM banks to download
                    uint8_t info_resp[25];
                    int info_bytes = execute_command(&device, 0x04, &target_id, 1, info_resp, sizeof(info_resp));

                    if (info_bytes < 18) {
                        printf("\x1b[1;31m   [!] Error: Could not retrieve info for ID %u\x1b[0m\n", target_id);
                        break;
                    }

                    uint8_t ram_banks = info_resp[17];
                    if (ram_banks == 0) {
                        printf("\x1b[1;33m   [!] This game has no RAM banks (No save to download).\x1b[0m\n");
                        break;
                    }

                    printf("\x1b[1;34m   [?] Enter destination path (e.g., backup.sav): \x1b[0m");
                    fflush(stdout);
                    if (scanf("%s", save_path) != 1) break;

                    download_save(&device, target_id, save_path, ram_banks);
                }
                break;
            case 'u': {
                    char input[16];
                    char save_path[256];
                    list_games(&device, 1); 

                    printf("\n\x1b[1;34m   [?] Enter ROM ID to upload save to (or 'EXIT'): \x1b[0m");
                    fflush(stdout);
                    if (scanf("%s", input) != 1) break;
                    if (strcasecmp(input, "EXIT") == 0) break;

                    uint8_t target_id = (uint8_t)atoi(input);

                    // Get Info to check RAM capacity
                    uint8_t info_resp[25];
                    int info_bytes = execute_command(&device, 0x04, &target_id, 1, info_resp, sizeof(info_resp));

                    if (info_bytes < 18) {
                        printf("\x1b[1;31m   [!] Error: Could not retrieve info for ID %u\x1b[0m\n", target_id);
                        break;
                    }

                    uint8_t ram_banks = info_resp[17];
                    if (ram_banks == 0) {
                        printf("\x1b[1;33m   [!] This game has no RAM. It doesn't need a save file.\x1b[0m\n");
                        break;
                    }

                    printf("\x1b[1;34m   [?] Enter path to .sav file to upload: \x1b[0m");
                    fflush(stdout);
                    if (scanf("%s", save_path) != 1) break;

                    upload_save(&device, target_id, save_path, ram_banks);
                }
                break;
            case 'd': {
                    char input[16];
                    list_games(&device, 1); // mode 1 = no header
                    printf("\n");
                    printf("   \x1b[1;31m[!] DANGER ZONE\x1b[0m\n");
                    printf("    \x1b[1;31m[-] \x1b[0m\x1b[1mEnter ROM ID to wipe (or type 'EXIT'): \x1b[0m");
                    fflush(stdout);

                    if (scanf("%s", input) == 1) {
                        if (strcasecmp(input, "EXIT") == 0) { // case sensitive
                            printf("    \x1b[1;34mReturning to main menu...\x1b[0m\n");
                            break; 
                        }

                        char *endptr;
                        long val = strtol(input, &endptr, 10);

                        if (*endptr == '\0') {
                            printf("\x1b[1;33m        Processing request for ID %ld...\x1b[0m\n", val);
                            delete_rom(&device, (uint8_t)val);
                        } else {
                            printf("\x1b[1;31m      Invalid input. Please enter a number or 'EXIT'.\x1b[0m\n");
                        }
                    } else {
                        printf("\x1b[1;31m      Invalid ID format.\x1b[0m\n");
                        while(getchar() != '\n'); 
                    }
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
