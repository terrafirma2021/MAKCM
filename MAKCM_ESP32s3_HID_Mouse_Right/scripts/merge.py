import os
import sys  
import csv
import subprocess
import time  
from os.path import join, exists
from colorama import Fore, Style

env = DefaultEnvironment()
platform = env.PioPlatform()
sys.path.append(join(platform.get_package_dir("tool-esptoolpy"))) 
import esptool

def find_partition_file(partitions_dir):
    for file in os.listdir(partitions_dir):
        if file.endswith(".csv"):
            return join(partitions_dir, file)
    return None

def esp32_create_combined_bin(source, target, env):

    bootloader_offset = 0x0000
    partitions_offset = 0x8000
    app_offset = 0x10000  

    partitions_dir = join(env.subst("$PROJECT_DIR"), "partitions")
    partition_file = find_partition_file(partitions_dir)
    
    if not partition_file:
        print(Fore.RED + "Partition CSV file not found!" + Style.RESET_ALL)
        return
    
    with open(partition_file) as csv_file:
        print(Fore.MAGENTA + "Read partitions from " + partition_file + Style.RESET_ALL)
        csv_reader = csv.reader(csv_file, delimiter=',')
        for row in csv_reader:
            if row[0] == 'app0':
                app_offset = int(row[3], base=16)

    bootloader_bin = env.subst("$BUILD_DIR/bootloader.bin")
    partitions_bin = env.subst("$BUILD_DIR/partitions.bin")
    firmware_bin = env.subst("$BUILD_DIR/firmware.bin")

    merged_firmware_dir = join(env.subst("$PROJECT_DIR"), "merged_firmware")
    if not exists(merged_firmware_dir):
        os.makedirs(merged_firmware_dir)

    # Retrieve the firmware version
    firmware_version = None
    for flag in env['BUILD_FLAGS']:
        if flag.startswith("-DFIRMWARE_VERSION="):
            firmware_version = flag.split("=")[1].strip('"').replace('\\', '')

    if not firmware_version:
        print(Fore.RED + "FIRMWARE_VERSION not set in build flags!" + Style.RESET_ALL)
        firmware_version = "UNKNOWN"

    # Determine if USB_IS_DEBUG is set to true
    usb_is_debug = any(flag.startswith("-DUSB_IS_DEBUG=true") for flag in env['BUILD_FLAGS'])

    # Get the board name
    board_name = env.BoardConfig().get("name", "MAKCM").split(";")[0].strip()  # Use 'MAKCM' if not found

    # Determine if the environment is LEFT or RIGHT
    env_name = env.get("PIOENV", "unknown").upper()  # This gives 'LEFT' or 'RIGHT'

    # Filename logic based on USB_IS_DEBUG and environment
    if usb_is_debug:
        # Reverse the filename logic if USB_IS_DEBUG is true
        if env_name == "LEFT":
            file_base_name = f"DEBUG_Right_{board_name}_MCU_{firmware_version}.bin"
        elif env_name == "RIGHT":
            file_base_name = f"DEBUG_Left_{board_name}_MCU_{firmware_version}.bin"
        print(Fore.GREEN + f"USB_IS_DEBUG detected! Creating DEBUG binary with name: {file_base_name}" + Style.RESET_ALL)
    else:
        # Normal filename logic without reversing
        if env_name == "LEFT":
            file_base_name = f"Left_{board_name}_MCU_{firmware_version}.bin"
        elif env_name == "RIGHT":
            file_base_name = f"Right_{board_name}_MCU_{firmware_version}.bin"
        print(Fore.YELLOW + f"USB_IS_DEBUG not detected. Creating normal binary with name: {file_base_name}" + Style.RESET_ALL)

    output_bin = join(merged_firmware_dir, file_base_name)

    # Check if the file already exists
    if exists(output_bin):
        print(Fore.YELLOW + f"File {file_base_name} already exists. Replacing it..." + Style.RESET_ALL)
        os.remove(output_bin)

    # Merge command
    cmd = [
        "--chip", env.get("BOARD_MCU"),
        "merge_bin",
        "-o", output_bin,
        "--flash_mode", env["__get_board_flash_mode"](env),
        "--flash_freq", env["__get_board_f_flash"](env),
        "--flash_size", env.BoardConfig().get("upload.flash_size", "4MB"),
        hex(bootloader_offset), bootloader_bin,
        hex(partitions_offset), partitions_bin,
        hex(app_offset), firmware_bin,
    ]

    print("    Offset | File")
    print(Fore.MAGENTA + f" - {hex(bootloader_offset)} | {bootloader_bin}" + Style.RESET_ALL)
    print(Fore.MAGENTA + f" - {hex(partitions_offset)} | {partitions_bin}" + Style.RESET_ALL)
    print(Fore.MAGENTA + f" - {hex(app_offset)} | {firmware_bin}" + Style.RESET_ALL)

    esptool.main(cmd)

    print(Fore.GREEN + f"Combined binary created: {output_bin}" + Style.RESET_ALL)

# Add post-action to automatically run after creating firmware.bin
env.AddPostAction("$BUILD_DIR/firmware.bin", esp32_create_combined_bin)
