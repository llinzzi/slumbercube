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
in_dump = False
while time.time() - start < 30:
    line = ser.readline()
    if line:
        try:
            s = line.decode('utf-8', errors='replace').rstrip()
            if 'Guru Meditation' in s:
                in_dump = True
                print(s)
            elif in_dump:
                print(s)
                if 'Rebooting' in s or 'rst:' in s or 'ESP-ROM' in s or 'ets' in s:
                    in_dump = False
                    print('--- end of dump ---')
            elif any(k in s for k in ['WEATHER_SVC', 'WIFI', 'MAIN', 'Parsed', 'Decompressed']):
                print(s)
        except:
            pass
ser.close()
