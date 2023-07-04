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

try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
except socket.error as msg:
    sys.stderr.write("[ERROR] %s\n" % msg)
    sys.exit(1)

try:
    sock.connect((HOST, PORT))
    fsock = sock.makefile()
except socket.error as msg:
    sys.stderr.write("[ERROR] %s\n" % msg)
    sys.exit(1)

fsock.readline()

sock.send("protocol extended\n")

fsock.readline()

load_command = "bundle load {} {} {}\n".format(timestamp, seq_nr, source_eid)
sock.send(load_command)

sock.send("bundle free\n")

sock.close()

sys.exit(0)

