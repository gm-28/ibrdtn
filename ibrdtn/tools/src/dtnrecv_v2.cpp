/*
 * dtnrecv.cpp
 *
 * Copyright (C) 2011 IBR, TU Braunschweig
 *
 * Written-by: Johannes Morgenroth <morgenroth@ibr.cs.tu-bs.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <iostream>
#include "keyconfig.h" 
#include <fcntl.h>

#include "config.h"
#include <ibrdtn/api/Client.h>
#include <ibrcommon/net/socket.h>
#include <ibrcommon/net/socketstream.h>
#include <ibrcommon/thread/Mutex.h>
#include <ibrcommon/thread/MutexLock.h>
#include <ibrcommon/thread/SignalHandler.h>
#include <ibrcommon/Logger.h>

#include <sys/types.h>
#include <iostream>

#include <stdio.h>
#include <stdlib.h>
#include <libssh/libssh.h>

void print_help()
{
	std::cout << "-- dtnrecv (IBR-DTN) --" << std::endl
			<< "Syntax: dtnrecv [options]"  << std::endl << std::endl
			<< "* optional parameters *" << std::endl
			<< " -h|--help        Display this text" << std::endl
			<< " --file <path>    Write the incoming data to the a file instead of the" << std::endl
			<< "                  standard output" << std::endl
			<< " --name <name>    Set the application name (e.g. filetransfer)" << std::endl
			<< " --timeout <seconds>" << std::endl
			<< "                  Receive timeout in seconds" << std::endl
			<< " --count <n>      Receive that many bundles" << std::endl
			<< " --group <group>  Join a group" << std::endl
			<< " -U <socket>      Connect to UNIX domain socket API" << std::endl;
}

dtn::api::Client *_client = NULL;
ibrcommon::socketstream *_conn = NULL;

int h = 0;
bool _stdout = true;

void term(int signal)
{
	if (!_stdout)
	{
		std::cout << h << " bundles received." << std::endl;
	}

	if (signal >= 1)
	{
		if (_client != NULL)
		{
			_client->close();
			_conn->close();
		}
	}
}

bool serializeBundleToFile(const std::string filename, const dtn::data::Bundle bundle) {
    // Open the output file stream
    std::ofstream outputFile(filename, std::ios::binary);
    if (!outputFile.is_open()) {
        std::cerr << "Error opening output file" << std::endl;
        return false;
    }

    try {
        // Create a DefaultSerializer object with the output stream
        dtn::data::DefaultSerializer serializer(outputFile);

        // Serialize the bundle and write it to the file
        serializer << bundle;

        // Close the output file stream
        outputFile.close();

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error serializing bundle: " << e.what() << std::endl;
        outputFile.close();
        return false;
    }
}
void disconnect(ssh_channel channel) {
    // Close the channel
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
}

/**
 * Transfer a file to a remote server using SFTP.
 * 
 * @param session         The SSH session established with the remote server.
 * @param localFilePath   The path to the local file to transfer.
 * @param remoteFilePath  The path to the remote file on the server.
 * @return                True if the file transfer is successful, false otherwise.
 */
bool transferFileToRemote(ssh_session session, const std::string& localFilePath, const std::string& remoteFilePath) {
    // Open the file for reading
    std::ifstream file(localFilePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error opening local file" << std::endl;
        return false;
    }

    // Get the size of the file
    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Allocate a buffer to store the file data
    char* buffer = new char[fileSize];

    // Read the file data into the buffer
    file.read(buffer, fileSize);
    file.close();

    // Create an SFTP session
    sftp_session sftp = sftp_new(session);
    if (sftp == nullptr) {
        std::cerr << "Error creating SFTP session: " << ssh_get_error(session) << std::endl;
        delete[] buffer;
        return false;
    }

    // Initialize the SFTP session
    int rc = sftp_init(sftp);
    if (rc != SSH_OK) {
        std::cerr << "Error initializing SFTP session: " << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        delete[] buffer;
        return false;
    }

    // Open a file on the remote server for writing
    sftp_file remoteFile = sftp_open(sftp, remoteFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (remoteFile == nullptr) {
        std::cerr << "Error opening remote file: " << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        delete[] buffer;
        return false;
    }

    // Write the file data to the remote file
    rc = sftp_write(remoteFile, buffer, fileSize);
    if (rc < 0) {
        std::cerr << "Error writing to remote file: " << ssh_get_error(session) << std::endl;
        sftp_close(remoteFile);
        sftp_free(sftp);
        delete[] buffer;
        return false;
    }

    // Close the remote file and free the SFTP session
    sftp_close(remoteFile);
    sftp_free(sftp);
    delete[] buffer;

    return true;
}

void sendBundle(ssh_session session, const std::string localFilePath, const std::string remoteFilePath, const std::string destination) {
    bool transferSuccess = transferFileToRemote(session, localFilePath, remoteFilePath);
    if (!transferSuccess) {
        std::cerr << "Error transferring file: " << localFilePath << std::endl;
        return;
    }

    std::string command = "dtnsend_v2 " + destination + " " + remoteFilePath;
    std::cout << command.c_str();
    std::cout << "\n";

    ssh_channel channel_cmd = ssh_channel_new(session);
    int rc = ssh_channel_open_session(channel_cmd);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel: %s\n", ssh_get_error(session));
        ssh_channel_free(channel_cmd);
        return;
    }

    rc = ssh_channel_request_exec(channel_cmd, command.c_str());
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session));
        ssh_channel_free(channel_cmd);
        return;
    }

    int c = 0;
    char buffer2[1024];
    int nbytes;
    while ((nbytes = ssh_channel_read(channel_cmd, buffer2, sizeof(buffer2), 0)) > 0) {
        fwrite(buffer2, 1, nbytes, stdout);
        c++;
        if (c == 5) break;
    }

    disconnect(channel_cmd);
}
/**
 * Splits aa BLOB into smaller chunks of a given size and returns a vector of BLOB references.
 * 
 * @param blob The BLOB to be split.
 * @param chunkSize The size of each chunk in bytes.
 * 
 * @return A vector of BLOB references, each representing a chunk of the original BLOB.
 */
std::vector<ibrcommon::BLOB::Reference> splitBlob(const ibrcommon::BLOB::Reference &blob, const size_t chunkSize)
{
    std::vector<ibrcommon::BLOB::Reference> chunks;

	// Create a new BLOB reference for this chunk
	ibrcommon::BLOB::Reference blobref = blob;

    // Get an iostream for the BLOB
	ibrcommon::BLOB::iostream io = blobref.iostream();

    size_t remainingSize = blob.size();
    size_t offset = 0;

    while (remainingSize > 0) {
        size_t chunkLength = std::min(chunkSize, remainingSize);

        ibrcommon::BLOB::Reference chunkBlob = ibrcommon::BLOB::create();
        ibrcommon::BLOB::iostream chunkStream = chunkBlob.iostream();

        (*io).seekg(offset, std::ios::beg);
        ibrcommon::BLOB::copy(*chunkStream, *io, chunkLength);

        chunks.push_back(chunkBlob);

        remainingSize -= chunkLength;
        offset += chunkLength;
    }

    return chunks;
}


static int auth_keyfile(ssh_session session, const char* keyfile) {
    ssh_key key = NULL;
    char pubkey[256] = {0}; // Public key file path
    int rc;

    snprintf(pubkey, sizeof(pubkey), "%s.pub", keyfile);

    // Import public key file
    rc = ssh_pki_import_pubkey_file(pubkey, &key);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error importing public key file: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    // Try public key authentication
    rc = ssh_userauth_try_publickey(session, NULL, key);
    ssh_key_free(key);
    if (rc != SSH_AUTH_SUCCESS) {
        fprintf(stderr, "Public key authentication failed: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    // Import private key file
    rc = ssh_pki_import_privkey_file(keyfile, NULL, NULL, NULL, &key);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error importing private key file: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    // Authenticate with public key
    rc = ssh_userauth_publickey(session, NULL, key);
    ssh_key_free(key);
    if (rc != SSH_AUTH_SUCCESS) {
        fprintf(stderr, "Public key authentication failed: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    return rc;
}

static ssh_session start_session(const char* host, const char* user, const char* keyfile, const char* port) {
    ssh_session session = ssh_new();
    if (session == NULL) {
        fprintf(stderr, "Error creating SSH session\n");
        exit(EXIT_FAILURE);
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host);
    ssh_options_set(session, SSH_OPTIONS_USER, user);
    ssh_options_set(session, SSH_OPTIONS_PORT_STR, port);

    int rc = ssh_connect(session);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error connecting to virtual machine: %s\n", ssh_get_error(session));
        ssh_free(session);
        exit(EXIT_FAILURE);
    }

    rc = auth_keyfile(session, keyfile);
    if (rc != SSH_AUTH_SUCCESS) {
        fprintf(stderr, "Error authenticating with virtual machine\n");
        ssh_disconnect(session);
        ssh_free(session);
        exit(EXIT_FAILURE);
    }

    return session;
}

int main(int argc, char *argv[])
{
	// logging options
	const unsigned char logopts = ibrcommon::Logger::LOG_DATETIME | ibrcommon::Logger::LOG_LEVEL;

	// error filter
	unsigned char loglevel = 0;

	// create signal handler
	ibrcommon::SignalHandler sighandler(term);
	sighandler.handle(SIGINT);
	sighandler.handle(SIGTERM);
	sighandler.initialize();

	int ret = EXIT_SUCCESS;
	std::string filename = "";
	std::string name = "dtnRecv";
	dtn::data::EID group;
	int timeout = 0;
	int count   = 1;
	ibrcommon::File unixdomain;

	for (int i = 0; i < argc; ++i)
	{
		std::string arg = argv[i];

		// print help if requested
		if (arg == "-h" || arg == "--help")
		{
			print_help();
			return ret;
		}

		if (arg == "--logging")
		{
			loglevel |= ibrcommon::Logger::LOGGER_ALL ^ ibrcommon::Logger::LOGGER_DEBUG;
		}

		if (arg == "--debug")
		{
			loglevel |= ibrcommon::Logger::LOGGER_DEBUG;
		}

		if (arg == "--name" && argc > i)
		{
			name = argv[i + 1];
		}

		if (arg == "--file" && argc > i)
		{
			filename = argv[i + 1];
			_stdout = false;
		}

		if (arg == "--timeout" && argc > i)
		{
			timeout = atoi(argv[i + 1]);
		}

		if (arg == "--group" && argc > i)
		{
			group = std::string(argv[i + 1]);
		}

		if (arg == "--count" && argc > i) 
		{
			count = atoi(argv[i + 1]);
		}

		if (arg == "-U" && argc > i)
		{
			if (++i > argc)
			{
				std::cout << "argument missing!" << std::endl;
				return -1;
			}

			unixdomain = ibrcommon::File(argv[i]);
		}
	}

	if (loglevel > 0)
	{
		// add logging to the cerr
		ibrcommon::Logger::addStream(std::cerr, loglevel, logopts);
	}

	try {
		// Create a stream to the server using TCP.
		ibrcommon::clientsocket *sock = NULL;

		// check if the unixdomain socket exists
		if (unixdomain.exists())
		{
			// connect to the unix domain socket
			sock = new ibrcommon::filesocket(unixdomain);
		}
		else
		{
			// connect to the standard local api port
			ibrcommon::vaddress addr("localhost", 4550);
			sock = new ibrcommon::tcpsocket(addr);
		}

    	ibrcommon::socketstream conn(sock);

		// Initiate a client for synchronous receiving
		dtn::api::Client client(name, group, conn);

		// export objects for the signal handler
		_conn = &conn;
		_client = &client;

		// Connect to the server. Actually, this function initiate the
		// stream protocol by starting the thread and sending the contact header.
		client.connect();

		std::fstream file;

		if (!_stdout)
		{
			std::cout << "Wait for incoming bundle... " << std::endl;
			file.open(filename.c_str(), std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
			file.exceptions(std::ios::badbit | std::ios::eofbit);
		}

        EID addr = EID("dtn://moreira-XPS-15-9570");

		for(h = 0; h < count; ++h)
		{
			// receive the bundle
			dtn::data::Bundle b = client.getBundle(timeout);
            //b.source = addr;

			// get the reference to the blob
			ibrcommon::BLOB::Reference ref = b.find<dtn::data::PayloadBlock>().getBLOB();

            // Open the output file stream
            std::ofstream outputFile("Receiver/bundle.bin", std::ios::binary);

            // Create a DefaultSerializer object with the output stream
            dtn::data::DefaultSerializer serializer(outputFile);

            // Serialize the bundle and write it to the file
            serializer << b;
            
            // Close the output file stream
            outputFile.close();
		}

		if (!_stdout)
		{
			file.close();
			std::cout << "done." << std::endl;
		}

		// Shutdown the client connection.
		client.close();

		// close the tcp connection
		conn.close();
	} catch (const dtn::api::ConnectionTimeoutException&) {
		std::cerr << "Timeout." << std::endl;
		ret = EXIT_FAILURE;
	} catch (const dtn::api::ConnectionAbortedException&) {
		std::cerr << "Aborted." << std::endl;
		ret = EXIT_FAILURE;
	} catch (const dtn::api::ConnectionException&) {
	} catch (const std::exception &ex) {
		std::cerr << "Error: " << ex.what() << std::endl;
		ret = EXIT_FAILURE;
	}

	return ret;
}
