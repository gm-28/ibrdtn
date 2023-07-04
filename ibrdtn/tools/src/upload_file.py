import sys
import base64
from fjagepy import Gateway, MessageClass

if len(sys.argv) != 6:
    print("Usage: python3 upload_file.py <source_ip_address> <port> <timeout(ms)> <file_to_upload> <save_file_as>")
    sys.exit(1)

ip_address = sys.argv[1]
port = int(sys.argv[2])
timeout = int(sys.argv[3])
file_name = sys.argv[4]
remote_file_name = sys.argv[5]

gw = Gateway(ip_address, port)
PutFileReq = MessageClass('org.arl.fjage.shell.PutFileReq')
shell = gw.agentForService('org.arl.fjage.shell.Services.SHELL')

with open(file_name, 'rb') as file:
    contents = file.read()
    encoded_contents = base64.b64encode(contents).decode('utf-8')

req = PutFileReq(recipient=shell, filename=remote_file_name, contents=encoded_contents)
rsp = gw.request(req, timeout)

gw.close()
