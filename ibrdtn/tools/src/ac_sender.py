import sys
from fjagepy import Gateway, MessageClass

# Check if the correct number of command-line arguments is provided
if len(sys.argv) != 6:
    print("Usage: python3 ac_sender.py <source_ip_address> <port> <destination_address> <timeout(ms)><filename>")
    sys.exit(1)

ip_address = sys.argv[1]
port = int(sys.argv[2])
node = int(sys.argv[3])
timeout = int(sys.argv[4])
file_name = sys.argv[5]

# Create the Gateway object
gw = Gateway(ip_address, port)

# Define the message class
ShellExecReq = MessageClass('org.arl.fjage.shell.ShellExecReq')

# Get the agent for the shell service
shell = gw.agentForService('org.arl.fjage.shell.Services.SHELL')

# Create the ShellExecReq message with the desired command
cmd = 'fput {}, {}'.format(node, file_name)
req = ShellExecReq(recipient=shell, cmd=cmd)

rsp = gw.request(req, timeout)
print(rsp)

# Close the gateway connection
gw.close()
