#!/usr/bin/env python2
import socket
import sys


arguments = sys.argv
script_name = arguments[0]
timestamp = arguments[1]
seq_nr = arguments[2]
source_eid = arguments[3]

HOST = 'localhost'
PORT = 4550

# Create a socket
try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
except socket.error as msg:
    sys.stderr.write("[ERROR] %s\n" % msg)
    sys.exit(1)

# Connect to the daemon
try:
    sock.connect((HOST, PORT))
    fsock = sock.makefile()
except socket.error as msg:
    sys.stderr.write("[ERROR] %s\n" % msg)
    sys.exit(1)

# Read the header
fsock.readline()

# Switch into extended management protocol mode
sock.send("protocol management\n")

# Read the protocol switch
fsock.readline()

# Load bundle based on timestamp, sequence number, and source EID
sock.send("bundle clear \n")

# Close the socket
sock.close()

# Finally exit the script
sys.exit(0)

