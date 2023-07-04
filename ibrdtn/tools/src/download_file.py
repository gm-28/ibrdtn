import sys
from fjagepy import Gateway, MessageClass

if len(sys.argv) != 6:
    print("Usage: python3 download_file.py <source_ip_address> <port> <timeout(ms)> <remote_file_name> <local_file_name>")
    sys.exit(1)

ip_address = sys.argv[1]
port = int(sys.argv[2])
timeout = int(sys.argv[3])
remote_file_name = sys.argv[4]
local_file_name = sys.argv[5]

gw = Gateway(ip_address, port)
GetFileReq = MessageClass('org.arl.fjage.shell.GetFileReq')
GetFileRsp = MessageClass('org.arl.fjage.shell.GetFileRsp')
shell = gw.agentForService('org.arl.fjage.shell.Services.SHELL')

req = GetFileReq(recipient=shell, filename=remote_file_name)
rsp = gw.request(req, timeout)
first = rsp.contents[0]
list = rsp.contents
gw.close()

unsigned_int_list = [2**8 + element  if element < 0 else element for element in list]   
byte_array = bytes(unsigned_int_list) 

with open(local_file_name, 'wb') as file:
    file.write(byte_array)