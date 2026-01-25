# Croco

## Command

[https://github.com/shilga/croco-cartridge-webapp/blob/master/src/communication.js](https://github.com/shilga/croco-cartridge-webapp/blob/master/src/communication.js)

| ID (Hex) | Command | Description | Response Data |
| ---------- | --------- | ------------- | ---------------- |
| `0x01` | readRomUtilization | Global state of flash memory | `numRoms`, `usedBanks`, `maxBanks` |
| `0x02` | requestRomUpload | Prepare the cartridge to receive a new file | `status` (0 = OK) |
| `0x03` | sendRomChunk | Send a data packet to ROM (Flash) | `status` |
| `0x04` | readRomInfo | Get Metadata of a ROM | `romId`, `name`, `numRamBanks`, `mbc`, `numRomBanks` |
| `0x05` | deleteRom | Delete a ROM and is save file | `status` |
| `0x06` | requestSaveDownload | Prepare the export of a save file (.sav) | `status` |
| `0x07` | receiveSaveChunk | Get the data packet of SRAM | `bank`, `chunk`, `data` |
| `0x08` | requestSaveUpload | Prepare import of a save file (.sav) | `status` |
| `0x09` | sendSavegameChunk | Send a data packet from computer to SRAM | `status` |
| `0x0A` | fetchRtcData | Fetch Real Time Clock (RTC) | `rtcData` |
| `0x0B` | sendRtcData | Set Real Time Clock (RTC) of the cartridge | `status` |
| `0xFD` | readDeviceSerialId | Fetch UUID of the RP2040. | `serialId` (8 bytes) |
| `0xFE` | readDeviceInfoCommand | Version of the Firmware Feature Step. | `hwVersion`, `swVersion` (`major`, `minor`, `patch`, `buildType`, `gitShort`, `gitDirty`) |

## Bank counting method

Based on the real webapp [https://cartridge-web.croco-electronics.de/](https://cartridge-web.croco-electronics.de/) a bank is the `numbers of bytes / 256` ex: `48640 bytes / 256 = 190 banks`

CLI: ![48640 / 30723](assets/image2.png)

WebApp: ![190 banks used](assets/image.png)
