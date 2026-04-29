import sys
import time
sys.path.insert(0, '/Users/zulin/.espressif/python_env/idf5.5_py3.14_env/lib/python3.14/site-packages')
import serial

ser = serial.Serial('/dev/cu.usbmodem1301', 115200, timeout=1)

# Reset
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setDTR(False)
ser.setRTS(False)
time.sleep(0.1)
ser.setDTR(True)
time.sleep(0.1)
ser.setDTR(False)
ser.reset_input_buffer()

start = time.time()
capture = False
while time.time() - start < 30:
    line = ser.readline()
    if line:
        try:
            s = line.decode('utf-8', errors='replace').rstrip()
            # Start capture at Parsed log, continue through crash dump until reboot
            if 'Parsed' in s or 'Guru' in s or 'Backtrace' in s or 'MCAUSE' in s or 'MTVAL' in s or 'MEPC' in s:
                print(s)
            if 'WEATHER_SVC: HTTP' in s:
                print(s)
            if 'WEATHER_SVC: Decompressed' in s:
                print(s)
            if 'WEATHER_SVC: Parsed' in s:
                print(s)
            # Print register dump lines (contain ':0x')
            if ':0x' in s and ('M' in s or 'S' in s or 'A' in s or 'T' in s or 'GP' in s):
                print(s)
        except:
            pass
ser.close()
