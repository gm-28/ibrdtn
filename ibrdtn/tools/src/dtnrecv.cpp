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
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

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

bool deserializeBundleFromFile(const std::string localFilePath, dtn::data::Bundle& bundle) {
    // Open the input file stream
    std::ifstream inputFile(localFilePath, std::ios::binary);
    if (!inputFile.is_open()) {
        std::cerr << "Error opening input file" << std::endl;
        return false;
    }

    try {
        // Create a DefaultDeserializer object with the input stream
        dtn::data::DefaultDeserializer deserializer(inputFile);

        // Deserialize the bundle from the file
        deserializer >> bundle;

        // Close the input file stream
        inputFile.close();

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error serializing bundle: " << e.what() << std::endl;
        inputFile.close();
        return false;
    }
}

int remove_bundle() {
    int sockfd;
    struct sockaddr_in serverAddr;
    struct hostent* host;

    /* Create a socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* Get the host by name */
    if ((host = gethostbyname("localhost")) == NULL) {
        perror("gethostbyname failed");
        exit(EXIT_FAILURE);
    }

    /* Set up server information */
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(4550);
    memcpy(&serverAddr.sin_addr, host->h_addr, host->h_length);

    /* Connect to the daemon */
    if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("connection failed");
        exit(EXIT_FAILURE);
    }

    /* Read header */
    char header[256];
    if (read(sockfd, header, sizeof(header)) < 0) {
        perror("read header failed");
        exit(EXIT_FAILURE);
    }

    /* Switch to extended protocol mode */
    const char* protocolSwitch = "protocol extended\n";
    if (write(sockfd, protocolSwitch, strlen(protocolSwitch)) < 0) {
        perror("write protocol switch failed");
        exit(EXIT_FAILURE);
    }

    /* Read protocol switch */
    char protocolResponse[256];
    if (read(sockfd, protocolResponse, sizeof(protocolResponse)) < 0) {
        perror("read protocol response failed");
        exit(EXIT_FAILURE);
    }

    /* Add registration */
    const char* addRegistration = "registration add dtn://moreira2-VirtualBox/dtnRecv\n";
    if (write(sockfd, addRegistration, strlen(addRegistration)) < 0) {
        perror("write add registration failed");
        exit(EXIT_FAILURE);
    }

    /* Read add registration response */
    char addRegistrationResponse[256];
    if (read(sockfd, addRegistrationResponse, sizeof(addRegistrationResponse)) < 0) {
        perror("read add registration response failed");
        exit(EXIT_FAILURE);
    }
    if (strstr(addRegistrationResponse, "200") != NULL) {
        std::cout << "Registration added successfully.\n";
    } else {
        std::cout << "Failed to add registration.\n";
    }

    /* Load bundle queue */
    const char* loadQueue = "bundle load queue\n";
    if (write(sockfd, loadQueue, strlen(loadQueue)) < 0) {
        perror("write load queue failed");
        exit(EXIT_FAILURE);
    }

    /* Read load queue response */
    char loadQueueResponse[256];
    if (read(sockfd, loadQueueResponse, sizeof(loadQueueResponse)) < 0) {
        perror("read load queue response failed");
        exit(EXIT_FAILURE);
    }
    if (strstr(loadQueueResponse, "200") != NULL) {
        std::cout << "Bundle queue loaded successfully.\n";
    } else {
        std::cout << "Failed to load bundle queue.\n";
    }

    /* Free bundle */
    const char* freeBundle = "bundle free\n";
    if (write(sockfd, freeBundle, strlen(freeBundle)) < 0) {
        perror("write free bundle failed");
        exit(EXIT_FAILURE);
    }

    /* Read free bundle response */
    char freeBundleResponse[256];
    if (read(sockfd, freeBundleResponse, sizeof(freeBundleResponse)) < 0) {
        perror("read free bundle response failed");
        exit(EXIT_FAILURE);
    }
    if (strstr(freeBundleResponse, "200") != NULL) {
        std::cout << "Bundle freed successfully.\n";
    } else {
        std::cout << "Failed to free bundle.\n";
    }

    /* Delete registration */
    const char* deleteRegistration = "registration del dtn://moreira2-VirtualBox/dtnRecv\n";
    if (write(sockfd, deleteRegistration, strlen(deleteRegistration)) < 0) {
        perror("write delete registration failed");
        exit(EXIT_FAILURE);
    }

    /* Read delete registration response */
    char deleteRegistrationResponse[256];
    if (read(sockfd, deleteRegistrationResponse, sizeof(deleteRegistrationResponse)) < 0) {
        perror("read delete registration response failed");
        exit(EXIT_FAILURE);
    }
    if (strstr(deleteRegistrationResponse, "200") != NULL) {
        std::cout << "Registration deleted successfully.\n";
    } else {
        std::cout << "Failed to delete registration.\n";
    }

    /* Close the socket */
    close(sockfd);

    return 0;
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
	std::string name = "filetransfer";
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

		for(h = 0; h < count; ++h)
		{
			// receive the bundle
			dtn::data::Bundle b = client.getBundle(timeout);

			// get the reference to the blob
			ibrcommon::BLOB::Reference ref = b.find<dtn::data::PayloadBlock>().getBLOB();
			
			dtn::data::Bundle b1;
			deserializeBundleFromFile("ibrdtn/ibrdtn/tools/src/Receiver/bundle.bin",b1);
			dtn::data::BundleID& id = b1;
			printf("Sequence of transfered file %d in %s \n",std::stoi(id.sequencenumber.toString().c_str()));

			// write the data to output
			if (_stdout)
			{
				std::cout << ref.iostream()->rdbuf() << std::flush;
			}
			else
			{
				// write data to temporary file
				try {
					std::cout << "Bundle received (" << (h + 1) << ")." << std::endl;

					file << ref.iostream()->rdbuf();
				} catch (const std::ios_base::failure&) {

				}
			}	
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
	
	remove_bundle();		
	return ret;
}
