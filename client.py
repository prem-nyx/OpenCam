import socket
import time
import random
import math
import cv2
import sys
import threading

DGRAM = 2**16 - 64

class Client:
    def __init__(self, name):
        self.name = name
        self.streaming = False
        self.socket_tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def connect(self, addr):
        self.addr = addr
        self.socket_tcp.connect(self.addr)

        msg = f"{self.name}"
        self.socket_tcp.send(bytes(msg, 'utf8'))

        while True:
            res = self.socket_tcp.recv(15).decode()
            print(res)
            if res == "streamstart":
                self.streaming = True
            else:
                self.streaming = False

    def udpsend(self, data):
        size = len(data)
        segments = math.ceil(size / DGRAM)

        start = 0
        while segments and self.streaming == True:
            end = min(size, start + DGRAM)
            self.socket_udp.sendto(data[start:end], self.addr)

            start = end
            segments -= 1

            res = self.socket_udp.recvfrom(5)


    def run(self):
        thread1 = threading.Thread(target=self.connect, args=(addr,))
        thread2 = threading.Thread(target=self.start_stream)

        thread1.start()
        thread2.start()

class RandomStreamClient(Client):
    def start_stream(self):
        print("start stream")
        data = bytearray()
        while True:
            if self.streaming == True:
                for i in range(0, 640 * 480 * 3):
                    data.append(random.randint(0, 255))
                
                self.udpsend(data)
            

class OpencvClient(Client):
    def __init__(self, name):
        Client.__init__(self, name)
        self.cap = cv2.VideoCapture(2)

    def start_stream(self):
        while True:
            ret, frame = self.cap.read()
            cv2.imshow("frame", frame)

            if self.streaming == True:
                data = frame.flatten()
                self.udpsend(data)

            if cv2.waitKey(1) == ord('q'):
                break


addr = ("127.0.0.1", 6969)
if sys.argv[1] == 'opencv':
    client = OpencvClient("OpenCVClient")
else:
    client = RandomStreamClient("RandomStreamClient") 

client.run()
