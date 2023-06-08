#!/usr/bin/python3
import socket
import sys

HOST = 'localhost'
PORT = 4550

''' Create a socket '''
try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
except socket.error as msg:
    sys.stderr.write("[ERROR] %s\n" % msg)
    sys.exit(1)

''' Connect to the daemon '''
try:
    sock.connect((HOST, PORT))
    fsock = sock.makefile()
except socket.error as msg:
    sys.stderr.write("[ERROR] %s\n" % msg)
    sys.exit(1)

''' Read the header '''
fsock.readline()

''' Switch into extended management protocol mode '''
sock.send(b"protocol extended\n")

''' Read the protocol switch '''
fsock.readline()

''' Add registration '''
sock.send(b"registration add dtn://moreira1-VirtualBox/dtnRecv\n")

data = fsock.read(3)
sys.stdout.write(data)


''' Load bundle queue '''
sock.send(b"bundle load queue\n")

data = fsock.readline()
sys.stdout.write(data)
data = fsock.readline()

''' Free bundle from queue '''
sock.send(b"bundle free\n")

data = fsock.readline()
sys.stdout.write(data)
data = fsock.readline()
sys.stdout.write(data)

''' Close the socket '''
sock.close()

''' Finally exit the script '''
sys.exit(0)

