# -*- coding: utf-8 -*-
import tkinter as tk
import customtkinter as ctk
import serial
import serial.tools.list_ports
import threading
import time
import tkinter.filedialog as fd
import subprocess
import webbrowser
import queue
import sys
import os
from PIL import Image, ImageTk

class MAKCM_GUI:
    # Constants for text configuration
    TEXT_SIZE = 12
    TEXT_FONT = ("Helvetica", TEXT_SIZE)
    
    def __init__(self, root):
        self.root = root
        self.is_connected = False
        self.serial_open = False
        self.serial_connection = None
        self.Normal_boot = True
        self.boot_mode = "Normal"
        self.com_port = ""
        self.print_serial_data = True
        self.FlashReady = False
        self.BIN_Path = ""
        self.log_queue = queue.Queue()
        self.monitoring_active = True
        self.theme_is_dark = True

        self.tooltip_label = None
        self.available_ports = []
        self.port_mapping = {}

        self.command_history = [] 
        self.history_position = -1

        self.discord_icon_path = self.get_icon_path("Discord.png")
        self.github_icon_path = self.get_icon_path("GitHub.png")

        self.define_theme_colors()

        threading.Thread(target=self.log_to_file, daemon=True).start()
        self.start_com_port_monitoring()
        self.setup_gui()

        self.set_default_mode()

    def define_theme_colors(self):
        """Defines colors based on the current theme."""
        if self.theme_is_dark:
            self.dropdown_bg = "#2b2b2b"       # Dark background for drop-down
            self.dropdown_fg = "white"         # White text
            self.dropdown_selected_bg = "#555555"  # Selected item background
        else:
            self.dropdown_bg = "lightgrey"     # Light background for drop-down
            self.dropdown_fg = "black"         # Black text
            self.dropdown_selected_bg = "#d3d3d3"  # Selected item background

    def create_tooltip(self, widget, text):
        """Create a tooltip that appears at the top center when hovering over a widget."""
        def show_tooltip(_):
            if self.tooltip_label:
                self.tooltip_label.destroy()

            self.tooltip_label = ctk.CTkLabel(self.root, text=text, bg_color="transparent", font=self.TEXT_FONT)
            window_width = self.root.winfo_width()
            self.tooltip_label.place(x=window_width // 2, y=5, anchor="n")

        def hide_tooltip(_):
            if self.tooltip_label:
                self.tooltip_label.destroy()
                self.tooltip_label = None

        widget.bind("<Enter>", show_tooltip)
        widget.bind("<Leave>", hide_tooltip)

    def get_icon_path(self, filename):
        """Handle PyInstaller packed and script execution paths."""
        if getattr(sys, 'frozen', False):
            base_path = sys._MEIPASS
        else:
            base_path = os.path.dirname(os.path.abspath(__file__))

        return os.path.join(base_path, filename)

    def setup_gui(self):
        self.root.title("MAKCM v1.2")
        self.root.geometry("800x600")
        self.root.resizable(True, True)
        self.root.protocol("WM_DELETE_WINDOW", self.quit_application)

        self.create_icons()

        self.status_frame = ctk.CTkFrame(self.root, fg_color="transparent")
        self.status_frame.grid(row=0, column=0, columnspan=3, padx=10, pady=5, sticky="w")

        self.label_mcu = ctk.CTkLabel(self.status_frame, text="MCU disconnected", text_color="blue", width=300, anchor="w", font=self.TEXT_FONT)
        self.label_mcu.pack(side="left")

        self.create_buttons()
        self.create_com_port_section()
        self.create_text_input()
        self.create_output_box()


        self.root.grid_columnconfigure(0, weight=0)
        self.root.grid_columnconfigure(1, weight=1)
        self.root.grid_columnconfigure(2, weight=0)
        self.root.grid_rowconfigure(7, weight=1)
        self.scan_com_ports()
        if self.available_ports:
            self.com_port_combo.set(self.available_ports[0])
        else:
            self.com_port_combo.set("No COM Ports Found")

    def create_icons(self, parent_frame=None):
        """Load and display Discord and GitHub icons below the buttons, with hover rotation and click events."""

        icon_size = (20, 20)

        self.discord_image = Image.open(self.discord_icon_path).resize(icon_size)
        self.github_image = Image.open(self.github_icon_path).resize(icon_size)

        self.discord_icon = ImageTk.PhotoImage(self.discord_image)
        self.github_icon = ImageTk.PhotoImage(self.github_image)

        if parent_frame is None:
            parent_frame = self.root

        self.github_icon_label = ctk.CTkLabel(parent_frame, image=self.github_icon, text="")
        self.github_icon_label.grid(row=5, column=0, padx=(70, 0), pady=10, sticky="w")
        self.github_icon_label.bind("<Enter>", lambda event: self.animate_icon(self.github_icon_label, self.github_image, 360))
        self.github_icon_label.bind("<Leave>", lambda event: self.animate_icon(self.github_icon_label, self.github_image, -360))
        self.github_icon_label.bind("<Button-1>", lambda event: webbrowser.open("https://github.com/terrafirma2021/MAKCM"))

        self.discord_icon_label = ctk.CTkLabel(parent_frame, image=self.discord_icon, text="")
        self.discord_icon_label.grid(row=5, column=2, padx=(0, 70), pady=10, sticky="e")
        self.discord_icon_label.bind("<Enter>", lambda event: self.animate_icon(self.discord_icon_label, self.discord_image, 360))
        self.discord_icon_label.bind("<Leave>", lambda event: self.animate_icon(self.discord_icon_label, self.discord_image, -360))
        self.discord_icon_label.bind("<Button-1>", lambda event: webbrowser.open("https://discord.gg/6TJBVtdZbq"))

        self.discord_icon_label.image = self.discord_icon
        self.github_icon_label.image = self.github_icon

    def animate_icon(self, label, pil_image, angle, duration=1000):
        """Animate the icon by rotating it over time."""
        steps = 20  # Number of steps for the rotation
        interval = duration // steps
        angle_step = angle / steps 

        def rotate_step(current_angle=0, step=0):
            if step <= steps:
                rotated_image = pil_image.rotate(current_angle, expand=True)
                rotated_image_ctk = ImageTk.PhotoImage(rotated_image)

                label.configure(image=rotated_image_ctk)
                label.image = rotated_image_ctk

                self.root.after(interval, rotate_step, current_angle + angle_step, step + 1)

        rotate_step()

    def create_buttons(self):
        button_width = 150
        button_height = 35

        self.theme_button = ctk.CTkButton(self.root, text="Dark Mode", command=self.change_theme, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.theme_button.grid(row=1, column=0, padx=5, pady=5, sticky="w")
        self.create_tooltip(self.theme_button, "Sets colour change.")

        self.mode_button = ctk.CTkButton(self.root, text="Mode: Comm", command=self.toggle_mode, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.mode_button.grid(row=2, column=0, padx=5, pady=5, sticky="w")
        self.create_tooltip(self.mode_button, "To flash firmware, you need Flash. Other functions need Comm mode.")

        self.connect_button = ctk.CTkButton(self.root, text="Connect", command=self.toggle_connection, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.connect_button.grid(row=3, column=0, padx=5, pady=5, sticky="w")
        self.create_tooltip(self.connect_button, "Will sync the tool to the MCU.")

        self.quit_button = ctk.CTkButton(self.root, text="Quit", command=self.quit_application, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.quit_button.grid(row=4, column=0, padx=5, pady=5, sticky="w")
        self.create_tooltip(self.quit_button, "Safe shut down to restart the MCUs.")

        self.control_button = ctk.CTkButton(self.root, text="Test", command=self.test_button_function, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.control_button.grid(row=2, column=2, padx=5, pady=5, sticky="e")
        self.create_tooltip(self.control_button, "Test: Simple mouse nudge test.")

        self.open_log_button = ctk.CTkButton(self.root, text="User Logs", command=self.open_log, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.open_log_button.grid(row=3, column=2, padx=5, pady=5, sticky="e")
        self.create_tooltip(self.open_log_button, "Submit the file if requested for support.")

        self.browse_button = ctk.CTkButton(self.root, text="Flash", command=self.browse_file, fg_color="transparent", width=button_width, height=button_height, font=self.TEXT_FONT)
        self.browse_button.grid(row=4, column=2, padx=5, pady=5, sticky="e")
        self.create_tooltip(self.browse_button, "Hold the nearest button and insert USB, don't let go of the button until you connect!")

        self.clear_button = ctk.CTkButton(self.root, text="Clear Terminal", command=self.clear_terminal, fg_color="transparent", width=150, height=button_height, font=self.TEXT_FONT)
        self.clear_button.grid(row=6, column=0, padx=5, pady=5, sticky="w")
        self.create_tooltip(self.clear_button, "Clears the terminal output.")

        self.send_button = ctk.CTkButton(
            self.root, 
            text="Send", 
            command=self.send_input, 
            fg_color="transparent", 
            width=150,
            height=button_height,
            font=self.TEXT_FONT
        )
        self.send_button.grid(row=6, column=2, padx=5, pady=5, sticky="e")
        self.create_tooltip(self.send_button, "For quick commands like echo testing.")

    def change_theme(self):
        if self.theme_is_dark:
            ctk.set_appearance_mode("light")
            self.root.configure(bg="white")

            button_list = [
                self.theme_button,
                self.mode_button,
                self.connect_button,
                self.quit_button,
                self.control_button,
                self.open_log_button,
                self.browse_button,
                self.send_button,
                self.clear_button
            ]

            for button in button_list:
                button.configure(fg_color="black", text_color="white")

            self.label_mcu.configure(text_color="black")
            self.theme_button.configure(text="Light Mode")
        else:
            ctk.set_appearance_mode("dark")
            self.root.configure(bg="#2b2b2b")

            button_list = [
                self.theme_button,
                self.mode_button,
                self.connect_button,
                self.quit_button,
                self.control_button,
                self.open_log_button,
                self.browse_button,
                self.send_button,
                self.clear_button
            ]

            for button in button_list:
                button.configure(fg_color="gray", text_color="white")

            self.label_mcu.configure(text_color="blue")
            self.theme_button.configure(text="Dark Mode")

        self.theme_is_dark = not self.theme_is_dark
        self.define_theme_colors()

        if self.history_dropdown and tk.Toplevel.winfo_exists(self.history_dropdown):
            frame = self.history_dropdown.winfo_children()[0] 
            frame.configure(bg=self.dropdown_bg)
            self.history_listbox.configure(bg=self.dropdown_bg, fg=self.dropdown_fg, selectbackground=self.dropdown_selected_bg, font=self.TEXT_FONT)

    def create_com_port_section(self):
        self.com_port_combo = ctk.CTkComboBox(self.root, values=["Scanning ports..."], state="readonly", justify="right", font=self.TEXT_FONT)
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
        """Creates the text input field along with a history button."""

        input_frame = ctk.CTkFrame(self.root, fg_color="transparent")
        input_frame.grid(row=6, column=1, padx=5, pady=5, sticky="ew")
        input_frame.grid_columnconfigure(0, weight=1)
        input_frame.grid_columnconfigure(1, weight=0)

        self.text_input = ctk.CTkEntry(input_frame, height=35, font=self.TEXT_FONT)
        self.text_input.grid(row=0, column=0, padx=(0, 5), pady=0, sticky="ew")
        self.text_input.bind("<Return>", self.send_input)
        self.text_input.bind("<Up>", self.handle_history)
        self.text_input.bind("<Down>", self.handle_history)

        self.text_input.bind("<Configure>", lambda event: self.update_history_dropdown_position())


        self.history_button = ctk.CTkButton(
            input_frame,
            text="▼", 
            width=30, 
            height=35,
            command=self.show_history_menu,
            fg_color="transparent",
            hover_color="lightgray",
            border_width=0,
            font=self.TEXT_FONT
        )
        self.history_button.grid(row=0, column=1, padx=(0, 0), pady=0, sticky="e")
        self.create_tooltip(self.history_button, "Show command history")


        self.history_dropdown = None
        self.max_history_display = 20 

    def create_output_box(self):
        self.output_text = ctk.CTkTextbox(self.root, height=150, state="disabled", font=self.TEXT_FONT)
        self.output_text.grid(row=7, column=0, columnspan=3, padx=5, pady=5, sticky="nsew")

    def append_to_terminal(self, message):
        self.output_text.configure(state="normal")
        self.output_text.insert(ctk.END, f"{message}\n")
        self.output_text.configure(state="disabled")
        self.output_text.see(ctk.END)

    def toggle_serial_printing(self, enable: bool):
        self.print_serial_data = enable

    def terminal_print(self, *args, sep=" ", end="\n"):
        message = sep.join(map(str, args)) + end

        if self.print_serial_data:
            self.append_to_terminal(message)

        self.log_queue.put(message.strip())

    def send_input(self, event=None):
        command = self.text_input.get().strip()
        if command:
            if not self.is_connected or not self.serial_open:
                self.terminal_print("Connect to Device first")
            else:
                self.text_input.delete(0, ctk.END)
                if not command.endswith("\n"):
                    command += "\n"
                try:
                    self.serial_connection.write(command.encode())
                    self.terminal_print(f"Sent command: {command.strip()}")

                    if len(self.command_history) >= self.max_history_display:
                        self.command_history.pop(0) 
                    self.command_history.append(command.strip())
                    self.update_history_dropdown()
                except Exception as e:
                    self.terminal_print(f"Failed to send command: {e}")

    def toggle_mode(self):
        if self.is_connected:
            self.terminal_print("Cannot switch mode while connected. Disconnect first.")
            return  

        self.Normal_boot = not self.Normal_boot
        mode_text = "Comm" if self.Normal_boot else "Flash"
        self.mode_button.configure(text=f"Mode: {mode_text}")

        if self.Normal_boot:
            self.browse_button.configure(state="disabled")
            self.control_button.configure(state="normal")
        else:
            self.control_button.configure(state="disabled")
            self.browse_button.configure(state="normal")

        self.update_mcu_status()

    def set_default_mode(self):
        """Set default mode to Comm and configure buttons accordingly."""
        self.Normal_boot = True
        self.mode_button.configure(text="Mode: Comm")
        self.browse_button.configure(state="disabled")
        self.control_button.configure(state="normal")
        self.update_mcu_status()

    def toggle_connection(self, baudrate=4000000):
        if self.is_connected:
            try:
                if self.serial_connection and self.serial_connection.is_open:
                    self.serial_connection.write("DEBUG_OFF\n".encode())
                    time.sleep(0.5)
                self.serial_connection.close()
                self.is_connected = False
                self.serial_open = False
                self.connect_button.configure(text="Connect")
                self.com_port_combo.configure(state="normal")
                self.update_mcu_status()
                self.history_button.configure(state="disabled")
                self.hide_history_dropdown() 
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
                    self.serial_connection.write("DEBUG_ON\n".encode())

                    threading.Thread(target=self.serial_communication_thread, daemon=True).start()
                    threading.Thread(target=self.monitor_com_port, daemon=True).start()
                    self.update_mcu_status()
                    self.history_button.configure(state="normal") 
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
            base_path = sys._MEIPASS
        else:
            base_path = os.path.dirname(os.path.abspath(__file__))

        esptool_path = os.path.join(base_path, 'esptool.exe')
        return esptool_path

    def open_file_explorer(self, file_path):
        if os.path.exists(file_path):
            subprocess.Popen(f'explorer /select,"{file_path}"')
        else:
            self.terminal_print("File does not exist.")

    def browse_file(self):
        if not self.is_connected:
            self.terminal_print("Serial connection is not established. Please connect first.")
            return

        if self.Normal_boot:
            self.terminal_print("Firmware update can only be done in Boot mode. Switch to Boot mode first.")
            return

        file_path = fd.askopenfilename(filetypes=[("BIN files", "*.bin")])
        if file_path:
            self.BIN_Path = file_path
            self.FlashReady = True
            self.terminal_print(f"Loaded file: {self.BIN_Path}")
            self.browse_button.configure(text="Flash", command=self.flash_firmware, state="normal")

    def flash_firmware(self):
        """Start flashing firmware in a separate thread to keep the GUI responsive."""
        if not self.FlashReady:
            self.terminal_print("No firmware file loaded or flash not ready.")
            return

        self.browse_button.configure(text="Started", state="disabled")
        self.terminal_print("Flashing started...")

        flash_thread = threading.Thread(target=self.flash_firmware_thread, daemon=True)
        flash_thread.start()

    def flash_firmware_thread(self):
        if not self.FlashReady:
            self.terminal_print("FlashReady flag is not set.")
            self.reset_flash_button()
            return

        if not self.BIN_Path:
            self.terminal_print("No BIN file selected.")
            self.reset_flash_button()
            return

        if not self.com_port:
            self.terminal_print("COM port is not set.")
            self.reset_flash_button()
            return

        self.terminal_print("Starting to flash now. Please wait for 10 seconds.")

        if self.is_connected:
            self.toggle_connection()

        time.sleep(0.5)

        process_failed = False
        process = None

        try:
            self.toggle_serial_printing(False)

            esptool_path = self.get_esptool_path()

            bin_path = os.path.normpath(self.BIN_Path)

            esptool_args = [
                esptool_path,
                '--port', self.com_port,
                '--baud', str(self.com_speed),
                'write_flash', '0x0', bin_path
            ]

            process = subprocess.Popen(
                esptool_args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                shell=True
            )

            found_writing = False

            while True:
                output = process.stdout.readline()
                if output == '' and process.poll() is not None:
                    break
                if output:
                    if "Writing at" in output and not found_writing:
                        self.toggle_serial_printing(True)
                        found_writing = True
                        self.terminal_print(output.strip())
                    elif found_writing:
                        if "Leaving..." in output or "Hash of data verified" in output:
                            self.toggle_serial_printing(False)
                        else:
                            self.terminal_print(output.strip())

            stderr_output = process.stderr.read()

            if process.returncode == 1:
                self.toggle_serial_printing(True)
                self.terminal_print("Another .ihack beast is unlocked!!!")
            elif process.returncode != 0:
                process_failed = True
                self.toggle_serial_printing(True)
                self.terminal_print(f"Flashing failed with error code {process.returncode}.")
                self.terminal_print("You need to use the outer USB port to flash firmware.")
                if stderr_output:
                    self.terminal_print(f"Error output: {stderr_output.strip()}")

        except Exception as e:
            self.terminal_print(f"Flashing error: {e}")
            process_failed = True

        finally:
            if not process_failed:
                self.terminal_print("Finished!")
            self.toggle_connection()
            self.browse_button.configure(text="Flash", command=self.browse_file, state="normal")
            self.FlashReady = False

    def reset_flash_button(self):
        """Resets the Flash button to its default state."""
        self.root.after(0, lambda: self.browse_button.configure(text="Flash", command=self.browse_file, state="normal"))

    def quit_application(self):
        if self.serial_open and self.serial_connection.is_open:
            try:
                self.serial_connection.write("DEBUG_OFF\n".encode())
                self.serial_connection.close()
                self.terminal_print("Serial connection closed before quitting.")
            except Exception as e:
                self.terminal_print(f"Error while closing serial connection: {e}")
        self.hide_history_dropdown() 
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

    def clear_terminal(self):
        """Clears the terminal output."""
        self.output_text.configure(state="normal")
        self.output_text.delete('1.0', tk.END)  
        self.output_text.configure(state="disabled")

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
                self.terminal_print(f"Serial communication error: {e}")
                break

    def monitor_com_port(self):
        while self.serial_open:
            if not self.monitoring_active:
                return

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

        self.com_port_combo.configure(state="normal")
        self.connect_button.configure(text="Connect")
        self.history_button.configure(state="disabled") 
        self.update_mcu_status()
        self.hide_history_dropdown() 

    def update_mcu_status(self):
        if self.is_connected:
            status_color = "#0acc1e" if self.Normal_boot else "#bf0a37"
            mode_text = "Normal" if self.Normal_boot else "Boot"
            self.mcu_status = f"MCU connected in {mode_text} mode on {self.com_port} at {self.com_speed} baud"
        else:
            self.mcu_status = "MCU disconnected"
            status_color = "#1860db"
        self.label_mcu.configure(text=self.mcu_status, text_color=status_color)

    def handle_history(self, event):
        if not self.command_history:
            return

        if event.keysym == "Up":
            if self.history_position < len(self.command_history) - 1:
                self.history_position += 1
                self.text_input.delete(0, ctk.END)
                self.text_input.insert(0, self.command_history[-self.history_position - 1])
        elif event.keysym == "Down":
            if self.history_position > 0:
                self.history_position -= 1
                self.text_input.delete(0, ctk.END)
                self.text_input.insert(0, self.command_history[-self.history_position - 1])
            elif self.history_position == 0:
                self.history_position = -1
                self.text_input.delete(0, ctk.END)

    def update_history_dropdown(self):
        """Updates the history dropdown with the latest command history."""
        if self.history_dropdown and tk.Toplevel.winfo_exists(self.history_dropdown):
            self.history_listbox.delete(0, tk.END)
            for cmd in reversed(self.command_history[-self.max_history_display:]): 
                self.history_listbox.insert(tk.END, cmd)

    def show_history_menu(self):
        """Displays the history dropdown below the text input field."""
        if not self.command_history:
            return

        if self.history_dropdown and tk.Toplevel.winfo_exists(self.history_dropdown):
            return

        self.history_dropdown = tk.Toplevel(self.root)
        self.history_dropdown.wm_overrideredirect(True) 
        self.history_dropdown.configure(bg=self.dropdown_bg)

        self.root.update_idletasks() 
        input_x = self.text_input.winfo_rootx()
        input_y = self.text_input.winfo_rooty() + self.text_input.winfo_height()

        input_width = self.text_input.winfo_width()

        self.history_dropdown.wm_geometry(f"{input_width}x200+{input_x}+{input_y}")

        frame = tk.Frame(self.history_dropdown, bg=self.dropdown_bg, bd=1, relief="solid")
        frame.pack(fill="both", expand=True)

        self.history_listbox = tk.Listbox(
            frame,
            selectmode=tk.SINGLE,
            height=10,
            bg=self.dropdown_bg,
            fg=self.dropdown_fg,
            selectbackground=self.dropdown_selected_bg,
            font=self.TEXT_FONT
        )
        for cmd in reversed(self.command_history[-self.max_history_display:]):
            self.history_listbox.insert(tk.END, cmd)
        self.history_listbox.pack(side="left", fill="both", expand=True)

        self.history_listbox.bind("<<ListboxSelect>>", self.on_history_select)

        self.history_listbox.bind("<MouseWheel>", lambda event: self.history_listbox.yview_scroll(int(-1*(event.delta/120)), "units"))
        self.history_listbox.bind("<Button-4>", lambda event: self.history_listbox.yview_scroll(-1, "units"))
        self.history_listbox.bind("<Button-5>", lambda event: self.history_listbox.yview_scroll(1, "units"))  

        self.root.bind("<Button-1>", self.on_click_outside)

        self.history_listbox.focus_set()

    def update_history_dropdown_position(self):
        """Updates the position and width of the history dropdown to align with the text input."""
        if self.history_dropdown and tk.Toplevel.winfo_exists(self.history_dropdown):
            input_x = self.text_input.winfo_rootx()
            input_y = self.text_input.winfo_rooty() + self.text_input.winfo_height()
            input_width = self.text_input.winfo_width()
            self.history_dropdown.wm_geometry(f"{input_width}x200+{input_x}+{input_y}")

    def on_history_select(self, event):
        selected_indices = self.history_listbox.curselection()
        if selected_indices:
            selected_command = self.history_listbox.get(selected_indices[0])
            self.select_history_command(selected_command)
            self.hide_history_dropdown()

    def on_click_outside(self, event):
        """Hide the dropdown if clicking outside of it."""
        if self.history_dropdown:
            x1 = self.history_dropdown.winfo_rootx()
            y1 = self.history_dropdown.winfo_rooty()
            x2 = x1 + self.history_dropdown.winfo_width()
            y2 = y1 + self.history_dropdown.winfo_height()

            if not (x1 <= event.x_root <= x2 and y1 <= event.y_root <= y2):
                self.hide_history_dropdown()

    def hide_history_dropdown(self):
        """Destroy the history dropdown if it exists."""
        if self.history_dropdown and tk.Toplevel.winfo_exists(self.history_dropdown):
            self.history_dropdown.destroy()
            self.history_dropdown = None
            self.root.unbind("<Button-1>") 

    def select_history_command(self, command):
        """Sets the selected history command into the text input."""
        self.text_input.delete(0, ctk.END)
        self.text_input.insert(0, command)

    def open_support(self):
        webbrowser.open("https://github.com/terrafirma2021/MAKCM")

if __name__ == "__main__":
    ctk.set_appearance_mode("dark")  
    ctk.set_default_color_theme("blue") 
    root = ctk.CTk()
    app = MAKCM_GUI(root)
    root.mainloop()
