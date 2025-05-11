# MAKCM 2024
**Open Source ESP32-S3 MAKCU (Kmbox Alternative)**

![MAKCM 2024 Image](https://github.com/user-attachments/assets/02ddc456-4abc-4676-8121-7f3084370923)

Welcome to the MAKCU 2024 project! This is a new open-source implementation for remote injection of mouse control with passthrough, featuring dual ESP32-S3 microcontrollers based on RTOS and advanced task management over the traditional loop.

~~- To switch internal Phy to OTG disabling cdc/jtag on boot, please run the OTG bat file on both the left, then the right mcu port!~~  
~~- To flash hold down boot button, then insert USB Cable~~  
~~- The bat file will enable automatic install of python~~  
~~- automatic install of the espidf python toolset~~  
~~- Will then be asked to input com name ( please enter as COM??)~~  
~~- Tool will then start and ask you to type BURN in caps~~  
~~- Once mcu is switched you will not see further connections unless boot button is held down upon inserting, this is correct~~  
~~- To flash bin, just hold boot upon inserting cable, mcu will boot cdc as normal, enjoy!~~

**Update:**  
- Now included inside the firmware! Automatic PHY switch (EFUSE) on 1st boot!! Happens one time.


## Key Features

- **High-Speed Communication**: Supports 6Mbps com port communication speed for seamless performance.
- **Dual ESP32-S3 MCUs**: Utilizing two microcontrollers for enhanced processing power and efficiency.
- **RTOS-Based Task Management**: Using real-time operating system (RTOS) tasks over the conventional loop for better multitasking and system management.
- **Automatic HID Report Descriptor Parsing**: No need for custom firmware; the system automatically parses HID report descriptors.
- **Future-Proof**: The roadmap includes additional features such as controller support, eliminating the need for XIM.
- **Internal PHY in OTG Mode**: !Requires efuse burn!, ensuring the device no longer appears as anything other than device on boot.
- **Ample Storage**: 
  - 4MB Quad SPI Flash per MCU
  - 2MB PSRAM per MCU
- **JSON Streaming for USB Descriptors**: All USB descriptors are easily accessible and followable via JSON streaming.
- **Clean Codebase**: Built with a focus on maintainability and readability.
- ** Espressif API ** The largest community based MCU team in the world!

## Roadmap

- Add comprehensive controller support.
- Expand HID compatibility and features.
- Continue to optimize the RTOS tasks for more efficient use.
- Implement additional security measures.

## Updates

### Left MCU:
- Implemented 4-stage debug logging.
- Both MCUs now share a COM port for debug logging.
- Made bottom MCU COM port redundant on DIY dev kit.


### General Changes:
- Migrated to ISR Trigger for both UART0, UART1 (Hybrid Arduino/RTOS).
- Removed `vTaskDelay(1)` on UART0 and UART1, enabling full-speed logging using interrupt triggers.
- Verified and reduced task stack sizes (All tasks have a 500-byte high-water mark breathing space).
- Added `volatile bool` + Mutex on `km.move` commands (COM will always override mouse at any time for better mouse feel).
- Implemented button state logging:
  - Ensures that if a button is already up/down, any other sends will be ignored.
  - Results in better mouse feel and avoids random clicks.
  - added commands to adjust com port speed at any time for cheats that support (115k-5mpbs) 

### Code Improvements:
- Migrated tasks to separate `.cpp` file for a cleaner look.
- Tidied up `main.cpp`.
- General formatting improvements.
- Removed redundant code.

### Right MCU:

- Migrated ESPLOG to seperate log function to pass over uart1
- Set LED to use a task of its own (reducing usb task by 1024 bytes)
- Ongone: Fixed usb_host_transfer_free bug, sometimes API reponds invalid state (USB wont reconnect)
- All tasks verified with high-water mark (all tasks have 500 breathing space)
- general tidy up 

## Simple Debug log tool in python
- Run the tool follow the simple instuctions and get full debug log for any issues handling debive



## Community and Support

Join our [Discord community](https://discord.gg/6TJBVtdZbq) to engage with other developers, share your experiences, and get support from the community.

We are excited to have you on board and look forward to your contributions!

---

Feel free to contribute, raise issues, or suggest enhancements. Together, we can make MAKCM 2024 a robust and versatile tool for everyone.

## Contributing

Please refer to the [Contributing Guidelines](CONTRIBUTING.md) to get started. Contributions are always welcome!

## License

This project is licensed under the [MIT License](LICENSE).

