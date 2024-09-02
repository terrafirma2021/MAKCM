import os
import sys
import subprocess
import threading
import time
import queue

try:
    import serial
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial"])
    import serial

try:
    import keyboard
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "keyboard"])
    import keyboard

try:
    import mouse
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "mouse"])
    import mouse

should_exit = False
logging_active = False
mouse_hooked = False
log_file_path = os.path.join(os.path.dirname(__file__), "log.txt")
log_queue = queue.Queue()  # Queue to handle logging

def ensure_log_file_exists():
    if not os.path.exists(log_file_path):
        with open(log_file_path, 'a') as log_file:
            log_file.write("Log file created.\n")
    else:
        with open(log_file_path, 'a'):
            pass

def get_com_ports():
    import serial.tools.list_ports
    return list(serial.tools.list_ports.comports())

def log_worker():
    """ Thread worker function to write logs from the queue to the file. """
    with open(log_file_path, "a") as log_file:
        while not should_exit or not log_queue.empty():
            try:
                data = log_queue.get(timeout=1)
                log_file.write(data + "\n")
                log_queue.task_done()
            except queue.Empty:
                continue

def write_to_log(data):
    """ Put data in the log queue. """
    log_queue.put(data)

def read_from_serial(ser):
    global should_exit, logging_active

    while not should_exit:
        if ser.in_waiting > 0:
            try:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    write_to_log(line)
                    print("Received:", line)
                    logging_active = True
                    print("Logging activated. Now logging...")

                    while not should_exit and logging_active:
                        if ser.in_waiting > 0:
                            line = ser.readline().decode('utf-8', errors='ignore').strip()
                            if line:
                                write_to_log(line)
                                print("Logged:", line)
            except Exception as e:
                print(f"Error reading from serial port: {e}")
                break

def check_for_quit(ser):
    global should_exit, logging_active
    while not should_exit:
        if keyboard.is_pressed('q') or keyboard.is_pressed('Q'):
            should_exit = True
            if logging_active:
                print("Sending DEBUG_OFF command...")
                if ser.is_open:
                    ser.write(b"DEBUG_OFF\n")
                print("DEBUG_OFF command sent. Exiting...")
            close_serial_port(ser)
            sys.exit()
        else:
            time.sleep(0.1)

def clear_screen():
    if os.name == 'nt':
        os.system('cls')
    else:
        os.system('clear')

def close_serial_port(ser):
    try:
        if ser and ser.is_open:
            ser.close()
            print("Serial port closed.")
    except Exception as e:
        print(f"Error closing serial port: {e}")

def lock_mouse():
    global mouse_hooked
    mouse.move(0, 0, absolute=True, duration=0)
    mouse.hook(mouse_blocker)
    mouse_hooked = True

def unlock_mouse():
    global mouse_hooked
    if mouse_hooked:
        mouse.unhook(mouse_blocker)
        mouse_hooked = False

def mouse_blocker(event):
    return False

def verify_baud_rate_change(ser):
    try:
        print(f"Current baud rate set in Python: {ser.baudrate}")
        return ser.baudrate == 4000000
    except Exception as e:
        print(f"Error verifying baud rate change: {e}")
        return False

def main():
    global should_exit
    print("Welcome to the USB Debug Tool!")
    print("This tool logs data from the specified COM port.")

    ensure_log_file_exists()

    ports = get_com_ports()
    target_port = None

    while not target_port:
        for port in ports:
            if "CH343" in port.description:
                target_port = port.device
                break

        if target_port is None:
            print("Target COM port not found. Please check the connections and press Enter to retry.")
            input()
            ports = get_com_ports()

    initial_baud_rate = 115200
    new_baud_rate = 4000000

    ser = None
    try:
        ser = serial.Serial(target_port, initial_baud_rate, timeout=0)
        print(f"Connected to {target_port} with baud rate {initial_baud_rate}")

        print("1) Press Enter to start logging.")
        print("2) After a second, the logging will start.")
        print("3) Then disconnect and reconnect mouse.")
        print("4) Move the mouse, click buttons, and scroll the wheel.")
        print("5) Press 'q' to quit.")
        print("6) Look for log.txt file and submit a ticket with the log")
        input("Press Enter to continue...")

        lock_mouse()
        time.sleep(1)
        unlock_mouse()

        print("Sending SERIAL_4000000 command...")
        ser.write(b"SERIAL_4000000\n")
        print("SERIAL_4000000 command sent. Waiting for 3000ms...")

        time.sleep(3)

        ser.baudrate = new_baud_rate
        if verify_baud_rate_change(ser):
            print(f"Baud rate changed to {new_baud_rate} and verified successfully.")
        else:
            print("Failed to verify baud rate change. Exiting...")
            should_exit = True

        if not should_exit:
            print("Sending DEBUG_ON command...")
            ser.write(b"DEBUG_ON\n")
            print("DEBUG_ON command sent. Now logging...\n")

            log_thread = threading.Thread(target=log_worker)
            log_thread.start()

            read_thread = threading.Thread(target=read_from_serial, args=(ser,))
            read_thread.start()

            quit_thread = threading.Thread(target=check_for_quit, args=(ser,))
            quit_thread.start()

            read_thread.join()
            quit_thread.join()
            log_queue.join()  # Ensure all logging is done

    except serial.SerialException as e:
        print(f"Could not open serial port {target_port}: {e}")
        if ser:
            close_serial_port(ser)
        sys.exit()
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        if ser:
            close_serial_port(ser)
        sys.exit()
    finally:
        unlock_mouse()
        should_exit = True
        print("Mouse unlocked. Exiting script...")

if __name__ == "__main__":
    main()
