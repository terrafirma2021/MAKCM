import customtkinter as ctk
import esptool
import serial
import serial.tools.list_ports
import threading
import queue
import time
import tkinter.filedialog as fd
import random
import os
import subprocess
import webbrowser
import os
import sys

# Global variables
mcu_status = "disconnected"
com_speed = "115200"
is_connected = False
serial_open = False
serial_connection = None
boot_mode = ""
com_port = ""
BIN_Path = ""
FlashReady = False
serial_queue = queue.Queue()
command_queue = queue.Queue()
log_queue = queue.Queue()
command_history = []
history_position = -1
history_listbox = None
IsLogging = False


def log_to_file():
    log_file = "log.txt"

    if not os.path.exists(log_file):
        with open(log_file, "w") as f:
            f.write("Log File Created\n")

    while True:
        if not log_queue.empty():
            log_message = log_queue.get()
            with open(log_file, "a") as f:
                f.write(f"{log_message}\n")
        time.sleep(0.1)


def open_log():
    log_file = "log.txt"
    if os.path.exists(log_file):
        subprocess.Popen(["notepad.exe", log_file])
    else:
        print("Log file does not exist yet.")


def change_theme():

    random_bg_color = f"#{random.randint(0, 0xFFFFFF):06x}"
    random_button_color = f"#{random.randint(0, 0xFFFFFF):06x}"
    random_text_color = f"#{random.randint(0, 0xFFFFFF):06x}"

    root.configure(bg=random_bg_color)

    label_mcu.configure(text_color=random_text_color)

    connect_button.configure(fg_color=random_button_color)
    quit_button.configure(fg_color=random_button_color)
    send_button.configure(fg_color=random_button_color)
    browse_button.configure(fg_color=random_button_color)
    theme_button.configure(fg_color=random_button_color)
    open_log_button.configure(fg_color=random_button_color)
    control_button.configure(fg_color=random_button_color)
    efuse_button.configure(fg_color=random_button_color)
    support_button.configure(fg_color=random_button_color)
    log_button.configure(fg_color=random_button_color)

    if not is_connected:
        label_mcu.configure(text_color=random_text_color)


def update_output_box():
    try:
        while not serial_queue.empty():
            data = serial_queue.get()

            terminal_print(data)

            log_queue.put(data)
    except queue.Empty:
        pass
    root.after(100, update_output_box)


def append_to_terminal(message):
    output_text.configure(state="normal")
    output_text.insert(ctk.END, f"{message}\n")
    output_text.configure(state="disabled")
    output_text.see(ctk.END)


def terminal_print(*args, sep=" ", end="\n"):

    message = sep.join(map(str, args)) + end

    append_to_terminal(message)

    log_queue.put(message)


def update_connect_button_label():
    """Update the button label based on the connection status."""
    if is_connected:
        connect_button.configure(text="Disconnect")
    else:
        connect_button.configure(text="Connect")


def toggle_connection():
    global is_connected
    if is_connected:
        disconnect_serial()
        connect_button.configure(text="Connect")
    else:
        port, device_type = find_target_com_port()
        if port and connect_to_serial(port, device_type):
            connect_button.configure(text="Disconnect")
        else:
            print("No suitable COM port found.")


threading.Thread(target=log_to_file, daemon=True).start()

def serial_communication_thread():
    global serial_connection, is_connected, serial_open
    while True:
        if not is_connected or not serial_open:
            time.sleep(1)
            continue
        try:
            if serial_connection and serial_connection.in_waiting > 0:
                data = serial_connection.read(serial_connection.in_waiting).decode(
                    "utf-8", errors="ignore"
                )
                terminal_print(data)
            time.sleep(0.01)
        except Exception as e:
            terminal_print(f"Error reading from serial: {e}")
            serial_open = False
            is_connected = False
            try:
                if serial_connection and serial_connection.is_open:
                    serial_connection.close()
                terminal_print("Serial connection closed due to error.")
            except Exception as e:
                terminal_print(f"Error closing serial connection: {e}")
            update_mcu_status()


def list_com_ports():
    return serial.tools.list_ports.comports()


def find_target_com_port():
    ports = list_com_ports()
    for port in ports:
        print(f"Port: {port.device}, Description: {port.description}")
        if "CH343" in port.description:
            return port.device, "CH343"
        elif "USB Serial Device" in port.description or "USB JTAG/serial debug unit" in port.description:
            return port.device, "USB"
    return None, None


def connect_to_serial(port, device_type):
    global serial_connection, serial_open, is_connected, boot_mode, com_port, com_speed

    try:
        com_speed = 115200

        boot_mode = "Normal" if device_type == "CH343" else "Boot"
        com_port = port

        serial_connection = serial.Serial(
            port=port,
            baudrate=com_speed,
            timeout=0.5,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            bytesize=serial.EIGHTBITS,
        )
        serial_connection.flushInput()
        serial_connection.flushOutput()
        time.sleep(1)

        is_connected = True
        serial_open = True
        terminal_print(f"Connected to {device_type} on port {port}")
        update_mcu_status()

        return True
    except Exception as e:
        terminal_print(f"Failed to connect: {e}")
        return False


def disconnect_serial():
    global serial_connection, serial_open, is_connected, boot_mode, com_port

    try:
        if serial_open and serial_connection.is_open:
            serial_connection.write("DEBUG_OFF\n".encode())
            time.sleep(1)

            serial_connection.close()
            serial_connection = None
            serial_open = False
            is_connected = False

            boot_mode = ""
            com_port = ""
            toggle_logging(force_disable=True)
            update_mcu_status()

            return True
        else:
            terminal_print("Serial connection is already closed or invalid.")
            return False
    except Exception as e:
        terminal_print(f"Error during disconnect: {e}")
        return False


def update_mcu_status():
    global mcu_status, boot_mode, com_port, com_speed
    if is_connected:
        if boot_mode == "Normal":
            color = "#0acc1e"
        else:
            color = "#bf0a37"

        mcu_status = (
            f"MCU connected in {boot_mode} mode {com_port} with speed {com_speed}"
        )
        label_mcu.configure(text=mcu_status, text_color=color)

        connect_button.configure(text="Disconnect")
    else:
        mcu_status = "MCU disconnected"
        label_mcu.configure(text=mcu_status, text_color="#1860db")

        connect_button.configure(text="Connect")


def update_output_box():
    try:
        while not serial_queue.empty():
            data = serial_queue.get()

            output_text.configure(state="normal")
            output_text.insert(ctk.END, f"{data}\n")
            output_text.configure(state="disabled")
            output_text.see(ctk.END)

            log_queue.put(data)
    except queue.Empty:
        pass
    root.after(100, update_output_box)


def send_input(event=None):
    global history_position
    command = text_input.get().strip()
    if not command:
        return

    if command and is_connected and serial_open:
        command = command + "\n"
        try:
            serial_connection.write(command.encode())
            terminal_print(f"Sent: {command.strip()}")
        except Exception as e:
            terminal_print(f"Error sending command: {e}")
    else:
        terminal_print("No serial connection. Please connect first.")

    command_history.append(command.strip())
    history_position = -1

    text_input.delete(0, ctk.END)


def handle_history(event):
    global history_position
    if event.keysym == "Up":
        if command_history and history_position < len(command_history) - 1:
            history_position += 1
            text_input.delete(0, ctk.END)
            text_input.insert(0, command_history[-history_position - 1])
    elif event.keysym == "Down":
        if history_position > 0:
            history_position -= 1
            text_input.delete(0, ctk.END)
            text_input.insert(0, command_history[-history_position - 1])
        elif history_position == 0:
            history_position = -1
            text_input.delete(0, ctk.END)


def browse_file():
    global BIN_Path, FlashReady, boot_mode

    # Check if the device is connected
    if not is_connected:
        terminal_print("Serial connection is not established. Please connect first.")
        return

    # Check if in "Boot" mode before allowing file selection
    if boot_mode != "Boot":
        terminal_print(f"Firmware update can only be done in Boot mode. Current mode: {boot_mode}")
        return

    # Proceed with file browsing if checks pass
    file_path = fd.askopenfilename(filetypes=[("BIN files", "*.bin")])
    if file_path:
        BIN_Path = file_path
        FlashReady = True
        terminal_print(f"Loaded file: {BIN_Path}")
        browse_button.configure(text="Ready", command=flash_firmware)



def restart_gui():
    """Restart the entire GUI application."""
    terminal_print("Restarting the GUI to reset the environment...")
    python = sys.executable
    os.execl(python, python, *sys.argv)

def flash_firmware_thread():
    """This function will run in a separate thread to avoid GUI lock."""
    global FlashReady, BIN_Path, com_port, com_speed, serial_connection, serial_open, is_connected

    if FlashReady and BIN_Path and com_port:
        terminal_print(f"Flashing firmware on port {com_port} at {com_speed} baud.")
        terminal_print("Flashing started. ETA: 10 seconds.")

        stored_com_port = com_port

        if is_connected:
            toggle_connection()

        try:
            if not stored_com_port:
                terminal_print("COM port not set!")
                return

            esptool_args = [
                '--port', stored_com_port,    
                '--baud', str(com_speed),     
                'write_flash', '0x0', BIN_Path
            ]

            esptool.main(esptool_args)

            for countdown in range(14, 0, -1):
                terminal_print(f"Flashing... {countdown} seconds remaining", end="\r")
                time.sleep(1)

            terminal_print("Flash completed. Please remove the cable.")

        except Exception as e:
            terminal_print(f"Flashing error: {e}")

        finally:
            terminal_print("Reconnecting serial port after flashing.")
            toggle_connection()

            browse_button.configure(text="Flash", command=browse_file)
            FlashReady = False

            restart_gui()

    else:
        terminal_print("No BIN file selected or COM port unavailable.")

def flash_firmware():
    """This function starts the flashing in a new thread to keep the GUI responsive."""
    flash_thread = threading.Thread(target=flash_firmware_thread)
    flash_thread.start()


def serial_communication_thread():
    global serial_connection, is_connected, serial_open
    while True:
        if not is_connected or not serial_open:
            time.sleep(1)
            continue
        try:
            if serial_connection and serial_connection.in_waiting > 0:
                data = serial_connection.read(serial_connection.in_waiting).decode(
                    "utf-8", errors="ignore"
                )
                print(f"Received from serial: {data}")
                serial_queue.put(data)
            time.sleep(0.01)
        except Exception as e:
            print(f"Error reading from serial: {e}")
            serial_open = False
            is_connected = False
            try:
                if serial_connection and serial_connection.is_open:
                    serial_connection.close()
                print("Serial connection closed due to error.")
            except Exception as e:
                print(f"Error closing serial connection: {e}")
            update_mcu_status()


def command_input_thread():
    while True:
        if not is_connected or not serial_open:
            time.sleep(1)
            continue
        try:
            command = command_queue.get(block=True)
            if command:
                serial_connection.write(command.encode())
                print(f"Sent command: {command}")
                serial_queue.put(f"Sent: {command}")
        except Exception as e:
            print(f"Error sending command: {e}")


def quit_application():
    global serial_connection, is_connected, serial_open
    is_connected = False
    serial_open = False

    if serial_connection is not None and serial_connection.is_open:
        try:
            serial_connection.close()
            print("Serial connection closed.")
        except Exception as e:
            print(f"Error closing serial connection: {e}")
        finally:
            serial_connection = None

    root.quit()
    root.destroy()


def test_button_function():
    global is_connected, boot_mode, serial_connection

    if is_connected:
        if boot_mode == "Normal":
            if serial_connection and serial_connection.is_open:
                try:
                    serial_command = "km.move(50,50)\n"
                    serial_connection.write(serial_command.encode())
                except Exception as e:
                    terminal_print(f"Error sending command: {e}")
            else:
                terminal_print("Serial connection is not open.")
        else:
            terminal_print(
                f"MCU is connected in {boot_mode} mode. Function will not work!"
            )
    else:
        terminal_print("Serial connection is not established. Please connect first.")


def Log_function():
    global is_connected, boot_mode, serial_connection, IsLogging, com_speed

    if is_connected:
        if boot_mode == "Normal":
            if serial_connection and serial_connection.is_open:
                try:

                    serial_connection.write("SERIAL_4000000\n".encode())
                    terminal_print("Sent command to change baud rate to 4000000")

                    time.sleep(1)

                    com_speed = 4000000
                    serial_connection.baudrate = com_speed
                    terminal_print(f"Baud rate set to: {com_speed} locally")

                    update_mcu_status()

                    time.sleep(1)

                    serial_connection.write("DEBUG_ON\n".encode())

                    time.sleep(0.5)

                    IsLogging = True

                    terminal_print("Logging enabled. Please disconnect the mouse.")
                    return True
                except Exception as e:
                    terminal_print(f"Error executing command: {e}")
            else:
                terminal_print("Serial connection is not open.")
        else:
            terminal_print(
                f"MCU is connected in {boot_mode} mode. Logging will not work."
            )
    else:
        terminal_print("Serial connection is not established. Please connect first.")

    return False


def End_Log_function():
    global serial_connection, IsLogging, com_speed

    if IsLogging:
        try:

            serial_connection.write("DEBUG_OFF\n".encode())

            time.sleep(0.5)

            com_speed = 115200
            serial_connection.baudrate = com_speed
            terminal_print(f"Baud rate set to: {com_speed}")

            IsLogging = False

            update_mcu_status()

            terminal_print(
                "Logging has been disabled. Please open log and send in a ticket."
            )

        except Exception as e:
            terminal_print(f"Error ending logging: {e}")


def toggle_logging(force_disable=False):
    global IsLogging

    if force_disable:

        print("Force stopping logging...")
        End_Log_function()

        IsLogging = False
        log_button.configure(text="Log")

    elif IsLogging:

        End_Log_function()
        IsLogging = False
        log_button.configure(text="Log")

    else:

        if Log_function():
            IsLogging = True
            log_button.configure(text="Stop")


def burn_efuse_thread():
    """This function will run in a separate thread to burn the efuse and avoid GUI lock."""
    global com_port, com_speed, is_connected, boot_mode

    if com_port and com_speed:
        terminal_print(f"Burning efuse on port {com_port} at {com_speed} baud.")
        terminal_print("Efuse burning started.")

        stored_com_port = com_port

        # Disconnect if the device is already connected
        if is_connected:
            toggle_connection()

        try:
            if not stored_com_port:
                terminal_print("COM port not set!")
                return

            espefuse_args = [
                'python', '-m', 'espefuse',
                '--port', stored_com_port,
                '--baud', str(com_speed),
                'burn_efuse', 'USB_PHY_SEL'
            ]

            process = subprocess.Popen(
                espefuse_args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            burn_needed = False
            enter_sent = False

            for line in process.stdout:
                line = line.strip()

                if "Type 'BURN' (all capitals) to continue." in line:
                    process.stdin.write("BURN\n")
                    process.stdin.flush()
                    burn_needed = True

                if "Press Enter to continue..." in line:
                    process.stdin.write("\n")
                    process.stdin.flush()
                    enter_sent = True

                if "The same value for USB_PHY_SEL is already burned" in line:
                    break

            process.stdout.close()
            process.wait()

            if enter_sent:
                terminal_print("Efuse burning complete!")
            else:
                terminal_print("Efuse already burned!")

        except Exception as e:
            terminal_print(f"Efuse burning error: {e}")

        finally:
            terminal_print("Reconnecting serial port after Efuse burn.")
            toggle_connection()
            terminal_print("Efuse process finished.")

    else:
        terminal_print("COM port or Baud rate not set.")


def efuse_burn():
    """This function starts the efuse burn in a new thread to keep the GUI responsive."""
    if not is_connected:
        terminal_print("Serial connection is not established. Please connect first.")
    elif boot_mode != "Boot":
        terminal_print(f"Efuse burn can only be done in Boot mode. Current mode: {boot_mode}")
    else:
        efuse_thread = threading.Thread(target=burn_efuse_thread)
        efuse_thread.start()



def test_button_function():
    """A test button function that only works in Normal mode and requires an active serial connection."""
    global is_connected, boot_mode, serial_connection

    if is_connected:
        if boot_mode == "Normal":
            if serial_connection and serial_connection.is_open:
                try:
                    serial_command = "km.move(50,50)\n"
                    serial_connection.write(serial_command.encode())
                    terminal_print("Command sent successfully.")
                except Exception as e:
                    terminal_print(f"Error sending command: {e}")
            else:
                terminal_print("Serial connection is not open.")
        else:
            terminal_print(f"MCU is connected in {boot_mode} mode. Function will not work!")
    else:
        terminal_print("Serial connection is not established. Please connect first.")

    
def open_support():
    url = "https://github.com/terrafirma2021/MAKCM"
    if os.name == "nt":
        os.startfile(url)
    else:
        webbrowser.open(url)


root = ctk.CTk()
root.title("MAKCM")
root.geometry("800x600")
root.resizable(True, True)

root.lift()
root.focus_force()

root.protocol("WM_DELETE_WINDOW", quit_application)

label_mcu = ctk.CTkLabel(root, text=f"MCU disconnected", text_color="blue")
label_mcu.grid(row=0, column=0, columnspan=3, padx=5, pady=5, sticky="n")


button_width = 150


theme_button = ctk.CTkButton(
    root, text="Theme", command=change_theme, fg_color="transparent", width=button_width
)
theme_button.grid(row=1, column=0, padx=0, pady=5, sticky="w")


connect_button = ctk.CTkButton(
    root,
    text="Connect",
    command=toggle_connection,
    fg_color="transparent",
    width=button_width,
)
connect_button.grid(row=2, column=0, padx=0, pady=5, sticky="w")


quit_button = ctk.CTkButton(
    root,
    text="Quit",
    command=quit_application,
    fg_color="transparent",
    width=button_width,
)
quit_button.grid(row=3, column=0, padx=0, pady=5, sticky="w")


support_button = ctk.CTkButton(
    root,
    text="Support",
    command=open_support,
    fg_color="transparent",
    width=button_width,
)
support_button.grid(row=4, column=0, padx=0, pady=5, sticky="w")

# Right side buttons:

log_button = ctk.CTkButton(
    root, text="Log", command=toggle_logging, fg_color="transparent", width=150
)
log_button.grid(row=1, column=2, padx=5, pady=5, sticky="e")


control_button = ctk.CTkButton(
    root,
    text="Test",
    command=test_button_function,
    fg_color="transparent",
    width=button_width,
)
control_button.grid(row=2, column=2, padx=5, pady=5, sticky="e")


efuse_button = ctk.CTkButton(
    root, text="Efuse", command=efuse_burn, fg_color="transparent", width=button_width
)
efuse_button.grid(row=3, column=2, padx=5, pady=5, sticky="e")


open_log_button = ctk.CTkButton(
    root, text="Open Log", command=open_log, fg_color="transparent", width=button_width
)
open_log_button.grid(row=4, column=2, padx=5, pady=5, sticky="e")

browse_button = ctk.CTkButton(
    root, text="Flash", command=browse_file, fg_color="transparent", width=button_width
)
browse_button.grid(row=5, column=2, padx=5, pady=5, sticky="e")

send_button = ctk.CTkButton(
    root,
    text="Send",
    command=send_input,
    fg_color="transparent",
    width=button_width,
    height=35,
)
send_button.grid(row=6, column=2, padx=5, pady=5, sticky="e")


text_input = ctk.CTkEntry(root, height=35)
text_input.grid(row=6, column=0, columnspan=2, padx=5, pady=5, sticky="ew")


text_input.bind("<Return>", send_input)


text_input.bind("<Up>", handle_history)
text_input.bind("<Down>", handle_history)


output_text = ctk.CTkTextbox(root, height=150, state="disabled")
output_text.grid(row=7, column=0, columnspan=3, padx=5, pady=5, sticky="nsew")


root.grid_columnconfigure(0, weight=1)
root.grid_columnconfigure(1, weight=1)
root.grid_columnconfigure(2, weight=0)
root.grid_rowconfigure(5, weight=0)
root.grid_rowconfigure(6, weight=0)
root.grid_rowconfigure(7, weight=1)


threading.Thread(target=serial_communication_thread, daemon=True).start()
threading.Thread(target=command_input_thread, daemon=True).start()

update_output_box()

root.mainloop()
