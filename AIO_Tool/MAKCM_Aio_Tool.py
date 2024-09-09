import customtkinter as ctk
import serial
import serial.tools.list_ports
import threading
import time
import tkinter.filedialog as fd
import random
import os
import subprocess
import webbrowser
import queue
import sys
import shlex

class MAKCM_GUI:
    def __init__(self, root):
        self.root = root
        self.is_connected = False
        self.serial_open = False
        self.serial_connection = None
        self.Normal_boot = True
        self.boot_mode = "Normal"
        self.com_port = ""
        self.print_serial_data = True
        self.IsLogging = False
        self.FlashReady = False
        self.BIN_Path = ""
        self.log_queue = queue.Queue()
        self.monitoring_active = True

        self.available_ports = []
        self.port_mapping = {}

        self.command_history = []
        self.history_position = -1

        threading.Thread(target=self.log_to_file, daemon=True).start()
        self.start_com_port_monitoring()
        self.setup_gui()

        self.set_default_mode()

        

    def setup_gui(self):
        self.root.title("MAKCM v1.1")
        self.root.geometry("800x600")
        self.root.resizable(True, True)
        self.root.protocol("WM_DELETE_WINDOW", self.quit_application)

        self.label_mcu = ctk.CTkLabel(self.root, text="MCU disconnected", text_color="blue")
        self.label_mcu.grid(row=0, column=0, padx=35, pady=5, sticky="w")

        self.create_buttons()
        self.create_com_port_section()
        self.create_text_input()
        self.create_output_box()

        self.root.grid_columnconfigure(0, weight=1)
        self.root.grid_columnconfigure(1, weight=1)
        self.root.grid_columnconfigure(2, weight=0)
        self.root.grid_rowconfigure(7, weight=1)
        self.scan_com_ports()
        if self.available_ports:
            self.com_port_combo.set(self.available_ports[0])
        else:
            self.com_port_combo.set("No COM Ports Found")

    def create_buttons(self):
        button_width = 150

        self.theme_button = ctk.CTkButton(self.root, text="Theme", command=self.change_theme, fg_color="transparent", width=button_width)
        self.theme_button.grid(row=1, column=0, padx=5, pady=5, sticky="w")

        self.mode_button = ctk.CTkButton(self.root, text="Mode: Normal", command=self.toggle_mode, fg_color="transparent", width=button_width)
        self.mode_button.grid(row=2, column=0, padx=5, pady=5, sticky="w")

        self.connect_button = ctk.CTkButton(self.root, text="Connect", command=self.toggle_connection, fg_color="transparent", width=button_width)
        self.connect_button.grid(row=3, column=0, padx=5, pady=5, sticky="w")

        self.quit_button = ctk.CTkButton(self.root, text="Quit", command=self.quit_application, fg_color="transparent", width=button_width)
        self.quit_button.grid(row=4, column=0, padx=5, pady=5, sticky="w")

        self.log_button = ctk.CTkButton(self.root, text="Start Logging", command=self.toggle_logging, fg_color="transparent", width=button_width)
        self.log_button.grid(row=1, column=2, padx=5, pady=5, sticky="e")

        self.control_button = ctk.CTkButton(self.root, text="Test", command=self.test_button_function, fg_color="transparent", width=button_width)
        self.control_button.grid(row=2, column=2, padx=5, pady=5, sticky="e")

        #self.efuse_button = ctk.CTkButton(self.root, text="Efuse", command=self.efuse_burn, fg_color="transparent", width=button_width)
        #self.efuse_button.grid(row=3, column=2, padx=5, pady=5, sticky="e")

        self.open_log_button = ctk.CTkButton(self.root, text="User Logs", command=self.open_log, fg_color="transparent", width=button_width)
        self.open_log_button.grid(row=3, column=2, padx=5, pady=5, sticky="e")

        self.browse_button = ctk.CTkButton(self.root, text="Flash", command=self.browse_file, fg_color="transparent", width=button_width)
        self.browse_button.grid(row=4, column=2, padx=5, pady=5, sticky="e")

        self.send_button = ctk.CTkButton(self.root, text="Send", command=self.send_input, fg_color="transparent", width=button_width, height=35)
        self.send_button.grid(row=6, column=2, padx=5, pady=5, sticky="e")

    def change_theme(self):
        random_fg_color = f"#{random.randint(0, 0xFFFFFF):06x}"
        random_text_color = f"#{random.randint(0, 0xFFFFFF):06x}"

        button_list = [
            self.theme_button,
            self.mode_button,
            self.connect_button,
            self.quit_button,
            self.log_button,
            self.control_button,
        #    self.efuse_button,
            self.open_log_button,
            self.browse_button,
            self.send_button
        ]

        for button in button_list:
            button.configure(fg_color=random_fg_color, text_color=random_text_color)

        self.label_mcu.configure(text_color=random_text_color)


    def create_com_port_section(self):
        self.com_port_combo = ctk.CTkComboBox(self.root, values=["Scanning ports..."], state="readonly", justify="right")
        self.com_port_combo.grid(row=0, column=2, padx=5, pady=5, sticky="e")
        self.com_port_combo.bind("<Enter>", self.on_combo_hover)
        self.com_port_combo.bind("<Leave>", self.on_combo_leave)
        self.scan_com_ports()

    def on_combo_hover(self, event=None):
        current_selection = self.com_port_combo.get()
        self.scan_com_ports()

        if current_selection in self.available_ports:
            self.com_port_combo.set(current_selection)
        else:
            if self.available_ports:
                self.com_port_combo.set(self.available_ports[0])

        self.com_port_combo.event_generate("<Down>")

    def on_combo_leave(self, event=None):
        self.com_port_combo.event_generate("<Escape>")

    def start_com_port_monitoring(self):
        """Starts a background thread to continuously scan COM ports."""
        def monitor_ports():
            previous_ports = set(self.available_ports)
            while True:
                current_ports = set(f"{port.description}" for port in serial.tools.list_ports.comports())

                if current_ports != previous_ports:
                    self.available_ports = list(current_ports)
                    self.port_mapping = {port.description: port.device for port in serial.tools.list_ports.comports()}


                    self.toggle_serial_printing(False)


                    self.root.after(0, self.update_com_port_combo)


                    if not self.is_connected:
                        if len(self.available_ports) == 1:
                            first_available_port = self.available_ports[0]
                            self.terminal_print(f"Only one port detected: {first_available_port}. Please press 'Connect' to proceed.")
                            self.com_port_combo.set(first_available_port)
                        else:
                            new_ports = current_ports - previous_ports
                            if new_ports:
                                new_port_description = new_ports.pop()
                                self.terminal_print(f"New port detected: {new_port_description}. Please select and press 'Connect'.")
                                self.com_port_combo.set(new_port_description)


                    self.toggle_serial_printing(True)

                    previous_ports = current_ports

                time.sleep(1)

        monitoring_thread = threading.Thread(target=monitor_ports, daemon=True)
        monitoring_thread.start()



    def update_com_port_combo(self):
        """Updates the COM port combo box with the latest available ports."""
        if self.available_ports:
            self.com_port_combo.configure(values=self.available_ports)

            if self.com_port_combo.get() not in self.available_ports:
                self.com_port_combo.set(self.available_ports[0])
                self.terminal_print(f"Automatically selected {self.available_ports[0]}.")
        else:
            self.com_port_combo.configure(values=["No COM Ports Found"])
            self.com_port_combo.set("No COM Ports Found")
            self.terminal_print("No COM Ports Found. Please connect a device.")


    def scan_com_ports(self):
        """Scan available COM ports immediately and update the list."""
        self.available_ports = [f"{port.description}" for port in serial.tools.list_ports.comports()]
        self.port_mapping = {port.description: port.device for port in serial.tools.list_ports.comports()}


        self.com_port_combo.update_idletasks()


    def create_text_input(self):
        self.text_input = ctk.CTkEntry(self.root, height=35)
        self.text_input.grid(row=6, column=0, columnspan=2, padx=5, pady=5, sticky="ew")
        self.text_input.bind("<Return>", self.send_input)
        self.text_input.bind("<Up>", self.handle_history)
        self.text_input.bind("<Down>", self.handle_history)

    def create_output_box(self):
        self.output_text = ctk.CTkTextbox(self.root, height=150, state="disabled")
        self.output_text.grid(row=7, column=0, columnspan=3, padx=5, pady=5, sticky="nsew")

    def append_to_terminal(self, message):
        self.output_text.configure(state="normal")
        self.output_text.insert(ctk.END, f"{message}\n")
        self.output_text.configure(state="disabled")
        self.output_text.see(ctk.END)

    def toggle_serial_printing(self, enable: bool):

        self.print_serial_data = enable

        status = "enabled" if enable else "disabled"
    #    self.terminal_print(f"Serial data printing {status}.")


    def terminal_print(self, *args, sep=" ", end="\n"):
        message = sep.join(map(str, args)) + end

        if self.print_serial_data:
            self.append_to_terminal(message)

        self.log_queue.put(message.strip())


    def send_input(self, event=None):
        command = self.text_input.get().strip()
        if command:
        #    self.terminal_print(f"GUI Input: {command}")
            if not self.is_connected or not self.serial_open:
                self.terminal_print("Connect to Device first")
            else:
                self.text_input.delete(0, ctk.END)
                if not command.endswith("\n"):
                    command += "\n"
                self.serial_connection.write(command.encode())
            #    self.terminal_print(f"Sent command: {command.strip()}")  # Debugging feedback



    def set_default_mode(self):
          """Set default mode to Normal and configure buttons accordingly."""
          self.Normal_boot = True  # Set mode to Normal
          self.mode_button.configure(text="Mode: Normal")  # Ensure the mode button shows 'Normal'
          
          # Disable Efuse and Flash buttons, enable Logging and Test buttons
#          self.efuse_button.configure(state="disabled")
          self.browse_button.configure(state="disabled")
          self.log_button.configure(state="normal")
          self.control_button.configure(state="normal")
          
          self.update_mcu_status()  # Ensure MCU status reflects the current mode

    def toggle_mode(self):
        if self.is_connected:
            self.terminal_print("Cannot switch mode while connected. Disconnect first.")
            return  

        # Toggle between Normal and Boot mode
        self.Normal_boot = not self.Normal_boot
        mode_text = "Normal" if self.Normal_boot else "Boot"
        self.mode_button.configure(text=f"Mode: {mode_text}")

        # Print the mode change to the terminal
        #self.terminal_print(f"Switched to {mode_text} mode.")

        # Update button states based on the selected mode
        if self.Normal_boot:
            # In Normal mode:
            # Disable Efuse and Flash buttons
        #    self.efuse_button.configure(state="disabled")
            self.browse_button.configure(state="disabled")

            # Enable Start Logging and Test buttons
            self.log_button.configure(state="normal")
            self.control_button.configure(state="normal")
        else:
            # In Boot mode:
            # Disable Start Logging and Test buttons
            self.log_button.configure(state="disabled")
            self.control_button.configure(state="disabled")

            # Enable Efuse and Flash buttons
    #        self.efuse_button.configure(state="normal")
            self.browse_button.configure(state="normal")

        self.update_mcu_status()





    def toggle_connection(self, baudrate=115200):
        if self.is_connected:
            try:
                if self.serial_connection and self.serial_connection.is_open:
                    self.serial_connection.write("DEBUG_OFF\n".encode())
                    time.sleep(0.5)
                self.serial_connection.close()
                self.is_connected = False
                self.serial_open = False
                self.IsLogging = False  # Reset logging state when disconnecting
                self.log_button.configure(text="Start Logging")  # Update log button state
                self.connect_button.configure(text="Connect")
                self.com_port_combo.configure(state="normal")
                # self.terminal_print("Serial connection closed.")
                self.update_mcu_status()
            except Exception as e:
                self.terminal_print(f"Failed to close serial connection: {e}")
        else:
            selected_port = self.com_port_combo.get()

            if selected_port != "No COM Ports Found" and selected_port in self.available_ports:
                self.com_port = self.port_mapping[selected_port]
                try:
                    self.serial_connection = serial.Serial(
                        port=self.com_port,
                        baudrate=baudrate,
                        timeout=0.5,
                        parity=serial.PARITY_NONE,
                        stopbits=serial.STOPBITS_ONE,
                        bytesize=serial.EIGHTBITS
                    )
                    self.serial_connection.flushInput()
                    self.serial_connection.flushOutput()
                    time.sleep(0.5)

                    self.is_connected = True
                    self.serial_open = True
                    self.com_speed = baudrate
                    self.connect_button.configure(text="Disconnect")
                    self.com_port_combo.configure(state="disabled")
                #    self.terminal_print(f"Successfully connected to {self.com_port} at {baudrate} baud.")
                    threading.Thread(target=self.serial_communication_thread, daemon=True).start()
                    threading.Thread(target=self.monitor_com_port, daemon=True).start()
                    self.update_mcu_status()
                except Exception as e:
                    self.terminal_print(f"Failed to connect: {e}")
            else:
                self.terminal_print("Please select a new COM port.")


    def open_log(self):
        log_file_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'log.txt')
        self.open_file_explorer(log_file_path)

    def log_to_file(self):
        log_file_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'log.txt')

        if not os.path.exists(log_file_path):
            with open(log_file_path, "w") as f:
                f.write("Log File Created\n")

        while True:
            if not self.log_queue.empty():
                log_message = self.log_queue.get()
                with open(log_file_path, "a") as f:
                    f.write(f"{log_message}\n")
            time.sleep(0.1)

    def get_esptool_path(self):
        if getattr(sys, 'frozen', False):
            # If the script is compiled with PyInstaller
            base_path = sys._MEIPASS  # This is where PyInstaller unpacks files
        else:
            # If running from source
            base_path = os.path.dirname(os.path.abspath(__file__))

        esptool_path = os.path.join(base_path, 'esptool.exe')
        return esptool_path

    def open_file_explorer(self, file_path):
        if os.path.exists(file_path):
            subprocess.Popen(f'explorer /select,"{file_path}"')
        else:
            self.terminal_print("File does not exist.")

    def browse_file(self):
        # Check if the device is connected
        if not self.is_connected:
            self.terminal_print("Serial connection is not established. Please connect first.")
            return

        # Check if the device is in Boot mode
        if self.Normal_boot:
            self.terminal_print("Firmware update can only be done in Boot mode. Switch to Boot mode first.")
            return

        # Allow file selection if both conditions are met
        file_path = fd.askopenfilename(filetypes=[("BIN files", "*.bin")])
        if file_path:
            self.BIN_Path = file_path
            self.FlashReady = True
            self.terminal_print(f"Loaded file: {self.BIN_Path}")
            self.browse_button.configure(text="Ready", command=self.flash_firmware)

    def flash_firmware(self):
        """Start flashing firmware in a separate thread to keep the GUI responsive."""
        if not self.FlashReady:
            self.terminal_print("No firmware file loaded or flash not ready.")
            return

        flash_thread = threading.Thread(target=self.flash_firmware_thread)
        flash_thread.start()

    def flash_firmware_thread(self):
        if not self.FlashReady:
            self.terminal_print("FlashReady flag is not set.")
            return

        if not self.BIN_Path:
            self.terminal_print("No BIN file selected.")
            return

        if not self.com_port:
            self.terminal_print("COM port is not set.")
            return

        self.terminal_print("Starting to flash now. Please wait for 10 seconds.")

        if self.is_connected:
            self.toggle_connection()  # Disconnect first

        time.sleep(0.5)

        try:
            # Hide all output until we detect "Writing at"
            self.toggle_serial_printing(False)

            esptool_path = self.get_esptool_path()

            # Normalize the BIN file path to ensure correct format (backslashes on Windows)
            bin_path = os.path.normpath(self.BIN_Path)

            # Create esptool arguments
            esptool_args = [
                esptool_path,
                '--port', self.com_port,
                '--baud', str(self.com_speed),
                'write_flash', '0x0', bin_path  # Use the normalized path
            ]

            process = subprocess.Popen(
                esptool_args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                shell=True
            )

            found_writing = False
            process_failed = False

            while True:
                output = process.stdout.readline()
                if output == '' and process.poll() is not None:
                    break
                if output:
                    # Check for the "Writing at" line to turn on serial printing
                    if "Writing at" in output and not found_writing:
                        self.toggle_serial_printing(True)  # Show progress starting with "Writing at"
                        found_writing = True
                        self.terminal_print(output.strip())
                    elif found_writing:
                        if "Leaving..." in output or "Hash of data verified" in output:
                            self.toggle_serial_printing(False)  # Hide output after "Writing at"
                        else:
                            self.terminal_print(output.strip())
                    else:
                        # Do nothing and hide all other output until writing starts
                        pass

            stderr_output = process.stderr.read()

            # Handle specific error codes, treat error code 1 as non-critical
            if process.returncode == 1:
                self.toggle_serial_printing(True)  # Unhide to show "Finished"
                self.terminal_print("Finished.!! Another .ihack beast is unlocked!!!")  # Print finished even for return code 1
            elif process.returncode != 0:
                process_failed = True
                self.toggle_serial_printing(True)  # Unhide on critical failure
                self.terminal_print(f"Flashing failed with error code {process.returncode}.")
                self.terminal_print("You need to use the outer USB port to flash firmware.")
                if stderr_output:
                    self.terminal_print(f"Error output: {stderr_output.strip()}")

        except Exception as e:
            self.terminal_print(f"Flashing error: {e}")

        finally:
            if not process_failed:
                self.toggle_serial_printing(True)
            # Reconnect after flashing
            self.toggle_connection()
            self.browse_button.configure(text="Flash", command=self.browse_file)
            self.FlashReady = False




    def quit_application(self):
        if self.serial_open and self.serial_connection.is_open:
            try:
                self.serial_connection.close()
                self.terminal_print("Serial connection closed before quitting.")
            except Exception as e:
                self.terminal_print(f"Error while closing serial connection: {e}")
        self.root.quit()
        self.root.destroy()

    def test_button_function(self):
        if self.is_connected:
            if self.Normal_boot:
                if self.serial_connection and self.serial_connection.is_open:
                    try:
                        serial_command = "km.move(50,50)\n"
                        self.serial_connection.write(serial_command.encode())
                        self.terminal_print("Mouse move command sent, did mouse move?")
                    except Exception as e:
                        self.terminal_print(f"Error sending command: {e}")
                else:
                    self.terminal_print("Serial connection is not open.")
            else:
                self.terminal_print(f"MCU is connected in {self.boot_mode} mode. This function won't work.")
        else:
            self.terminal_print("Serial connection is not established. Please connect first.")

    def toggle_logging(self):
        if self.is_connected and self.Normal_boot:
            if self.serial_connection and self.serial_connection.is_open:
                try:
                    if not self.IsLogging:
                        # Start logging process
                    #    self.terminal_print("Starting logging by sending SERIAL_4000000...")

                        # Send SERIAL_4000000 to switch baud rate
                        self.serial_connection.write("SERIAL_4000000\n".encode())
                    #    self.terminal_print("Sent SERIAL_4000000 command.")

                        time.sleep(0.5)  # Delay to let the command process

                        # Disconnect and reconnect at 4Mbps
                    #    self.terminal_print("Reconnecting at 4Mbps...")
                        self.toggle_connection()  # Disconnect
                        time.sleep(0.5)  # Delay for stability
                        self.toggle_connection(baudrate=4000000)  # Reconnect at 4Mbps

                        # Send DEBUG_ON to start logging
                        self.serial_connection.write("DEBUG_ON\n".encode())
                    #    self.terminal_print("Sent DEBUG_ON command to enable logging.")

                        self.IsLogging = True
                        self.log_button.configure(text="Stop Logging")
                    #    self.terminal_print("Logging started successfully.")

                    else:
                        # Stop logging process
                    #    self.terminal_print("Stopping logging by sending DEBUG_OFF...")

                        # Send DEBUG_OFF to stop logging
                        self.serial_connection.write("DEBUG_OFF\n".encode())
                    #    self.terminal_print("Sent DEBUG_OFF command to disable logging.")

                        # Disconnect and reconnect at 115200 baud
                    #    self.terminal_print("Reconnecting at 115200...")
                        self.toggle_connection()  # Disconnect
                        time.sleep(1)  # Delay for stability
                        self.toggle_connection(baudrate=115200)  # Reconnect at default baud

                        # Reset logging state
                        self.IsLogging = False
                        self.log_button.configure(text="Start Logging")
                    #    self.terminal_print("Logging stopped successfully.")

                    self.update_mcu_status()
                #    self.terminal_print("MCU status updated.")
                except Exception as e:
                    self.terminal_print(f"Error during logging toggle: {e}")
            else:
                self.terminal_print("Serial connection is not open.")
        else:
            self.terminal_print("MCU is not connected in Normal mode.")


    def efuse_burn(self):
#       """This function starts the efuse burn in a new thread to keep the GUI responsive."""
#       if not self.is_connected:
#           self.terminal_print("Serial connection is not established. Please connect first.")
#       elif self.Normal_boot:  # If the mode is not Boot
#           self.terminal_print("Efuse burn can only be done in Boot mode. Connect in Boot mode first.")
#       else:
#           # Start the burn_efuse_thread in a new thread
#           efuse_thread = threading.Thread(target=self.burn_efuse_thread)
#           efuse_thread.start()
        self.terminal_print("Moved to fw.")

    def burn_efuse_thread(self):
#    """This function runs the efuse burn subprocess in a separate thread."""
#    if self.com_port and self.com_speed:
#        stored_com_port = self.com_port 
#
#        try:
#            if self.is_connected:
#                # Disconnect the serial connection
#                self.terminal_print("Flashing, please Wait for 5 Seconds!!!")
#                self.toggle_connection()
#                time.sleep(1)  # Allow the port to fully release    
#
#                self.monitoring_active = False  
#
#            # Explicitly ensure the serial port is fully closed
#            if self.serial_connection and self.serial_connection.is_open:
#                self.terminal_print("Serial port still open, trying to close it...")
#                self.serial_connection.close()
#                time.sleep(1)  # Give some time for the port to close   
#
#            # Double-check that the serial port is closed
#            self.serial_connection = None   
#
#            # Disable terminal printing but keep logging
#            self.toggle_serial_printing(False)  
#
#            espefuse_cmd = [
#                self.get_espefuse_path(),
#                '--port', stored_com_port,
#                '--baud', str(self.com_speed),
#                'burn_efuse',
#                'USB_PHY_SEL'
#            ]   
#
#            # Log the command being run
#            self.terminal_print(f"Running espefuse command: {' '.join(espefuse_cmd)}")  
#
#            startupinfo = None
#            if os.name == 'nt':
#                # Hide subprocess window on Windows
#                startupinfo = subprocess.STARTUPINFO()
#                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW  
#
#            process = subprocess.Popen(
#                espefuse_cmd,
#                stdin=subprocess.PIPE,
#                stdout=subprocess.PIPE,
#                stderr=subprocess.PIPE,
#                text=True,
#                universal_newlines=True,
#                startupinfo=startupinfo
#            )   
#
#            burn_needed = False
#            burn_prompt_found = False
#            burn_error_found = False    
#
#            # Continuously read stdout and stderr from the subprocess
#            while True:
#                line = process.stdout.readline()
#                if not line:
#                    break   
#
#                # Log each line from the process
#                self.terminal_print(f"espefuse stdout: {line.strip()}") 
#
#                if "Type 'BURN' (all capitals) to continue." in line:
#                    # Log that we're sending the 'BURN' command
#                    self.terminal_print("Sending 'BURN' command to proceed with efuse burning.")
#                    process.stdin.write("BURN\n")
#                    process.stdin.flush()  # Ensure the input is flushed
#                    burn_needed = True
#                    burn_prompt_found = True
#                    break   
#
#                if "The same value for USB_PHY_SEL is already burned" in line:
#                    self.terminal_print("Efuse already burned.")
#                    break   
#
#                # Handle subprocess-level errors reported by espefuse
#                if "Error" in line or "failed" in line:
#                    burn_error_found = True
#                    self.terminal_print(f"espefuse error: {line.strip()}")
#                    break   
#
#            # Ensure stderr is also read and logged
#            stderr_output = process.stderr.read().strip()
#            if stderr_output:
#                self.terminal_print(f"espefuse stderr: {stderr_output}")    
#
#            # Wait for the process to complete and get the return code
#            process.wait()  
#
#            # Enable terminal printing again for the final user output
#            self.toggle_serial_printing(True)   
#
#            # Log results based on subprocess output
#            if burn_error_found:
#                self.terminal_print("Efuse burning failed: Error detected during burning.")
#            elif burn_prompt_found or burn_needed:
#                self.terminal_print("Efuse burning complete!")
#            else:
#                self.terminal_print("Efuse already burned.")    
#
#        except Exception as e:
#            # Handle actual exceptions raised during the process
#            self.toggle_serial_printing(True)  # Ensure exceptions are shown to the user
#            self.terminal_print(f"Efuse burning error: {e}")
#        finally:
#            # Ensure the subprocess is fully done before attempting to reconnect
#            time.sleep(1)  # Short delay to ensure the port is released
#            self.toggle_connection()  # Reconnect the serial connection after the subprocess is done    
#
#            # Restart the COM port monitoring thread
#            self.monitoring_active = True
#            threading.Thread(target=self.monitor_com_port, daemon=True).start()
#    else:
#        self.terminal_print("COM port or Baud rate not set.") 
         self.terminal_print("Efuse burn functionality moved to fw .")




    def serial_communication_thread(self):
        while self.is_connected and self.serial_open:
            try:
                if self.serial_connection and self.serial_connection.is_open:
                    bytes_to_read = self.serial_connection.in_waiting
                    if bytes_to_read > 0:
                        data = self.serial_connection.read(bytes_to_read).decode("utf-8", errors="ignore")

                        self.log_queue.put(data.strip())

                        if self.print_serial_data:
                            self.terminal_print(f"Serial Data Received: {data.strip()}")

                    time.sleep(0.1)
            except Exception as e:
        #        self.terminal_print(f"Error reading from serial: {e}")
                break


    def monitor_com_port(self):
        while self.serial_open:
            if not self.monitoring_active:
                return  # Exit the thread if monitoring is disabled

            try:
                available_ports = [port.device for port in serial.tools.list_ports.comports()]
                if self.com_port not in available_ports:
                    self.is_connected = False
                    self.serial_open = False
                    self.root.after(0, self.handle_disconnect)
                    break
                time.sleep(0.1)
            except Exception as e:
                self.terminal_print(f"Error in COM port monitoring: {e}")
                break




    def handle_disconnect(self):
        self.terminal_print("Device disconnected. Please check the USB connection or select a new COM port.")

        # The combo box will automatically update via monitoring, so just unlock it
        self.com_port_combo.configure(state="normal")

        # Reset the connect button text to 'Connect'
        self.connect_button.configure(text="Connect")

        # Update MCU status
        self.update_mcu_status()




    def update_mcu_status(self):
        if self.is_connected:
            status_color = "#0acc1e" if self.Normal_boot else "#bf0a37"
            mode_text = "Normal" if self.Normal_boot else "Boot"
            self.mcu_status = f"MCU connected in {mode_text} mode on {self.com_port} at {self.com_speed} baud"
        else:
            self.mcu_status = "MCU disconnected"
            status_color = "#1860db"
        self.label_mcu.configure(text=self.mcu_status, text_color=status_color)

#    def get_espefuse_path(self):
#        base_path = os.path.dirname(os.path.abspath(__file__))
#        return os.path.join(base_path, 'espefuse.exe')

    def handle_history(self, event):
        if event.keysym == "Up" and self.history_position < len(self.command_history) - 1:
            self.history_position += 1
            self.text_input.delete(0, ctk.END)
            self.text_input.insert(0, self.command_history[-self.history_position - 1])
        elif event.keysym == "Down" and self.history_position > 0:
            self.history_position -= 1
            self.text_input.delete(0, ctk.END)
            self.text_input.insert(0, self.command_history[-self.history_position - 1])

    def open_support(self):
        webbrowser.open("https://github.com/terrafirma2021/MAKCM")


if __name__ == "__main__":
    root = ctk.CTk()
    app = MAKCM_GUI(root)
    root.mainloop()
