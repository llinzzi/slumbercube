import sys
import time
sys.path.insert(0, '/Users/zulin/.espressif/python_env/idf5.5_py3.14_env/lib/python3.14/site-packages')
import serial

ser = serial.Serial('/dev/cu.usbmodem1301', 115200, timeout=1)
time.sleep(0.5)
ser.reset_input_buffer()

start = time.time()
while time.time() - start < 25:
    line = ser.readline()
    if line:
        try:
            print(line.decode('utf-8', errors='replace').rstrip())
        except:
            pass
ser.close()
