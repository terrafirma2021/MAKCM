import os
import sys
import subprocess
import serial
import serial.tools.list_ports
import keyboard
import threading
import time

MOVE_COMMAND = "km.move"
MOVETO_COMMAND = "km.moveto"
GETPOS_COMMAND = "km.getpos"
BUTTON1_DOWN_COMMAND = "km.left(1)"
BUTTON1_UP_COMMAND = "km.left(0)"
BUTTON2_DOWN_COMMAND = "km.right(1)"
BUTTON2_UP_COMMAND = "km.right(0)"
BUTTON3_DOWN_COMMAND = "km.middle(1)"
BUTTON3_UP_COMMAND = "km.middle(0)"
BUTTON4_DOWN_COMMAND = "km.side1(1)"
BUTTON4_UP_COMMAND = "km.side1(0)"
BUTTON5_DOWN_COMMAND = "km.side2(1)"
BUTTON5_UP_COMMAND = "km.side2(0)"
WHEEL_COMMAND = "km.wheel"
MIN_SEND_INTERVAL = 1 / 575
last_send_time = 0

def install(package):
    subprocess.check_call([sys.executable, "-m", "pip", "install", package])

def check_and_install_dependencies():
    required_packages = ["pyserial", "keyboard"]
    for package in required_packages:
        try:
            __import__(package)
        except ImportError:
            print(f"{package} not found. Installing...")
            install(package)

def list_com_ports():
    ports = serial.tools.list_ports.comports()
    return [port.device for port in ports]

def select_com_port():
    while True:
        ports = list_com_ports()
        if not ports:
            print("No COM ports found.")
            input("Press Enter to quit.")
            return None
        for i, port in enumerate(ports):
            print(f"{i}: {port}")
        try:
            port_index = int(input("Select COM port by number: "))
            if port_index < 0 or port_index >= len(ports):
                raise ValueError("Invalid selection")
            return ports[port_index]
        except ValueError as e:
            print(f"Error: {e}. Please try again.")

def send_command(ser, command):
    global last_send_time
    current_time = time.time()
    if current_time - last_send_time >= MIN_SEND_INTERVAL:
        command_to_send = f"{command}\r\n"
        ser.write(command_to_send.encode())
        last_send_time = current_time
        print(f"Sent: {command_to_send}", end='\r')

def read_from_port(ser):
    while True:
        try:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8').rstrip()
                print(f"Received: {line}")
        except serial.SerialException as e:
            print(f"Error reading from serial port: {e}")
            break

def main():
    os.system('title MAKCM Test Tool')
    print(r"""
 .----------------.  .----------------.  .----------------.  .----------------.  .----------------. 
| .--------------. || .--------------. || .--------------. || .--------------. || .--------------. |
| | ____    ____ | || |      __      | || |  ___  ____   | || |     ______   | || | ____    ____ | |
| ||_   \  /   _|| || |     /  \     | || | |_  ||_  _|  | || |   .' ___  |  | || ||_   \  /   _|| |
| |  |   \/   |  | || |    / /\ \    | || |   | |_/ /    | || |  / .'   \_|  | || |  |   \/   |  | |
| |  | |\  /| |  | || |   / ____ \   | || |   |  __'.    | || |  | |         | || |  | |\  /| |  | |
| | _| |_\/_| |_ | || | _/ /    \ \_ | || |  _| |  \ \_  | || |  \ `.___.'\  | || | _| |_\/_| |_ | |
| ||_____||_____|| || ||____|  |____|| || | |____||____| | || |   `._____.'  | || ||_____||_____|| |
| |              | || |              | || |              | || |              | || |              | |
| '--------------' || '--------------' || '--------------' || '--------------' || '--------------' |
 '----------------'  '----------------'  '----------------'  '----------------'  '----------------' 
    """)
    print("MAKCM 2024\n")
    print("Listing available COM ports...")
    com_port = select_com_port()
    if com_port is None:
        print("No COM port selected, exiting.")
        return
    print(f"Opening COM port: {com_port} at 115200 baud rate")
    try:
        ser = serial.Serial(com_port, 115200, timeout=1)
    except serial.SerialException as e:
        print(f"Failed to open COM port: {e}")
        input("Press Enter to quit.")
        return
    print(f"COM port {com_port} opened.")
    try:
        print("Starting to read from the serial port in a separate thread.")
        read_thread = threading.Thread(target=read_from_port, args=(ser,))
        read_thread.daemon = True
        read_thread.start()
        print("""
        +---------------------------------------+
        | Use the following keys to control the |
        | mouse:                                |
        |                                       |
        | Movement:                             |
        |   W - Move Up                         |
        |   S - Move Down                       |
        |   A - Move Left                       |
        |   D - Move Right                      |
        |                                       |
        | Buttons:                              |
        |   Z - Left Click                      |
        |   X - Right Click                     |
        |   C - Middle Click                    |
        |   V - Button 4                        |
        |   B - Button 5                        |
        |                                       |
        | Wheel:                                |
        |   N - Wheel Up                        |
        |   M - Wheel Down                      |
        |                                       |
        | Position:                             |
        |   , - Get Position                    |
        +---------------------------------------+
        """)
        def on_event(event):
            if event.event_type == 'down':
                if event.name == 'w':
                    send_command(ser, f"{MOVE_COMMAND}(0,-10)")
                elif event.name == 's':
                    send_command(ser, f"{MOVE_COMMAND}(0,10)")
                elif event.name == 'a':
                    send_command(ser, f"{MOVE_COMMAND}(-10,0)")
                elif event.name == 'd':
                    send_command(ser, f"{MOVE_COMMAND}(10,0)")
                elif event.name == 'z':
                    send_command(ser, BUTTON1_DOWN_COMMAND)
                elif event.name == 'x':
                    send_command(ser, BUTTON2_DOWN_COMMAND)
                elif event.name == 'c':
                    send_command(ser, BUTTON3_DOWN_COMMAND)
                elif event.name == 'v':
                    send_command(ser, BUTTON4_DOWN_COMMAND)
                elif event.name == 'b':
                    send_command(ser, BUTTON5_DOWN_COMMAND)
                elif event.name == 'n':
                    send_command(ser, f"{WHEEL_COMMAND}(1)")
                elif event.name == 'm':
                    send_command(ser, f"{WHEEL_COMMAND}(-1)")
                elif event.name == ',':
                    send_command(ser, GETPOS_COMMAND)
                print(f"Key pressed: {event.name}")
            elif event.event_type == 'up':
                if event.name == 'z':
                    send_command(ser, BUTTON1_UP_COMMAND)
                elif event.name == 'x':
                    send_command(ser, BUTTON2_UP_COMMAND)
                elif event.name == 'c':
                    send_command(ser, BUTTON3_UP_COMMAND)
                elif event.name == 'v':
                    send_command(ser, BUTTON4_UP_COMMAND)
                elif event.name == 'b':
                    send_command(ser, BUTTON5_UP_COMMAND)
                print(f"Key released: {event.name}")
        keyboard.hook(on_event)
        while True:
            time.sleep(0.01)
    except KeyboardInterrupt:
        print("KeyboardInterrupt received, exiting.")
    finally:
        print(f"Closing COM port {com_port}.")
        ser.close()

if __name__ == "__main__":
    install("keyboard")
    main()
