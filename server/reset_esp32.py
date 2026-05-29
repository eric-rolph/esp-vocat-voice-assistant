import serial
import time
s = serial.Serial('COM4', 115200)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setDTR(False)
s.setRTS(False)
s.close()
