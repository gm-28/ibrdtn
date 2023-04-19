#include "config.h"
#include <ibrdtn/api/Client.h>
#include <ibrcommon/net/socket.h>
#include <ibrcommon/thread/Mutex.h>
#include <ibrcommon/thread/MutexLock.h>
#include <ibrcommon/data/BLOB.h>
#include <ibrcommon/Logger.h>

#include <iostream>
#include <vector>

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


void print_help()
{
	std::cout << "-- dtnsend (IBR-DTN) --" << std::endl
			<< "Syntax: dtnsend [options] <dst> <filename>"  << std::endl
			<< " <dst>            Set the destination eid (e.g. dtn://node/filetransfer)" << std::endl
			<< " <filename>       The file to transfer" << std::endl << std::endl
			<< "* optional parameters *" << std::endl
			<< " -h|--help        Display this text" << std::endl
			<< " --src <name>     Set the source application name (e.g. filetransfer)" << std::endl
			<< " -p <0..2>        Set the bundle priority (0 = low, 1 = normal, 2 = high)" << std::endl
			<< " -g               Receiver is a destination group" << std::endl
			<< " --lifetime <seconds>" << std::endl
			<< "                  Set the lifetime of outgoing bundles; default: 3600" << std::endl
			<< " -U <socket>      Connect to UNIX domain socket API" << std::endl
			<< " -n <copies>      Create <copies> bundle copies" << std::endl
			<< " -b <bytes>       Set the maximum size in <bytes> of each bundle payload" << std::endl
			<< " --encrypt        Request encryption on the bundle layer" << std::endl
			<< " --sign           Request signature on the bundle layer" << std::endl
			<< " --custody        Request custody transfer of the bundle" << std::endl
			<< " --compression    Request compression of the payload" << std::endl;

}

int main(int argc, char *argv[])
{
	bool error = false;
	std::string file_destination = "dtn://local/fileDestination";
	std::string file_source = "dtn://moreira/fileSource";
	unsigned int lifetime = 3600;
	bool use_stdin = false;
	std::string filename;
	ibrcommon::File unixdomain;
	int priority = 1;
	int copies = 1;
	size_t chunk_size = 1024;
	bool bundle_encryption = false;
	bool bundle_signed = false;
	bool bundle_custody = false;
	bool bundle_compression = false;
	bool bundle_group = false;

//	ibrcommon::Logger::setVerbosity(99);
//	ibrcommon::Logger::addStream(std::cout, ibrcommon::Logger::LOGGER_ALL, ibrcommon::Logger::LOG_DATETIME | ibrcommon::Logger::LOG_LEVEL);

	std::list<std::string> arglist;

	for (int i = 0; i < argc; ++i)
	{
		if (argv[i][0] == '-')
		{
			std::string arg = argv[i];

			// print help if requested
			if (arg == "-h" || arg == "--help")
			{
				print_help();
				return 0;
			}
			else if (arg == "--src" && argc > i)
			{
				if (++i > argc)
				{
					std::cout << "argument missing!" << std::endl;
					return -1;
				}

				file_source = argv[i];
			}
			else if (arg == "--lifetime" && argc > i)
			{
				if (++i > argc)
				{
					std::cout << "argument missing!" << std::endl;
					return -1;
				}

				std::stringstream data; data << argv[i];
				data >> lifetime;
			}
			else if (arg == "-p" && argc > i)
			{
				if (++i > argc)
				{
					std::cout << "argument missing!" << std::endl;
					return -1;
				}
				std::stringstream data; data << argv[i];
				data >> priority;
			}
			else if (arg == "-U" && argc > i)
			{
				if (++i > argc)
				{
					std::cout << "argument missing!" << std::endl;
					return -1;
				}

				unixdomain = ibrcommon::File(argv[i]);
			}
			else if (arg == "-n" && argc > i)
			{
				if (++i > argc)
				{
					std::cout << "argument missing!" << std::endl;
					return -1;
				}

				std::stringstream data; data << argv[i];
				data >> copies;

				if( copies < 1 ) {
					std::cout << "invalid number of bundle copies!" << std::endl;
					return -1;
				}
			}
			else if (arg == "-b" && argc > i)
			{
				if (++i > argc)
				{
					std::cout << "argument missing!" << std::endl;
					return -1;
				}

				std::stringstream data_2; data_2 << argv[i];
				data_2 >> chunk_size;

				if( chunk_size < 1 ) {
					std::cout << "invalid chunk size !" << std::endl;
					return -1;
				}
			}
		}
		else
		{
			arglist.push_back(argv[i]);
		}
	}

	if (arglist.size() <= 1)
	{
		print_help();
		return -1;
	} else if (arglist.size() == 2)
	{
		std::list<std::string>::iterator iter = arglist.begin(); ++iter;

		// the first parameter is the destination
		file_destination = (*iter);

		use_stdin = true;
	}
	else if (arglist.size() > 2)
	{
		std::list<std::string>::iterator iter = arglist.begin(); ++iter;

		// the first parameter is the destination
		file_destination = (*iter); ++iter;

		// the second parameter is the filename
		filename = (*iter);
	}

	try {
		// Create a stream to the server (daemon) using TCP.
		ibrcommon::clientsocket *sock = NULL;

		// check if the unixdomain socket exists
		if (unixdomain.exists())
		{
			// connect to the unix domain socket
			sock = new ibrcommon::filesocket(unixdomain);
		}
		else
		{
			ibrcommon::vaddress addr("localhost", 4550);

			// connect to the standard local api port
			sock = new ibrcommon::tcpsocket(addr);
		}

		ibrcommon::socketstream conn(sock);

		try {
			// Initiate a client for synchronous receiving
			dtn::api::Client client(file_source, conn, dtn::api::Client::MODE_BIDIRECTIONAL);

			// Connect to the server. Actually, this function initiate the
			// stream protocol by starting the thread and sending the contact header.
			client.connect();

			// target address
			EID addr = EID(file_destination);

			try {
				std::cout << "Transfer file \"" << filename << "\" to " << addr.getString() << std::endl;
				
				// open file as read-only BLOB
				ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);

				//Split the file BLOB into smaller BLOBs
				auto ref_chunks = splitBlob(ref,chunk_size);

				for(int i = 1; i < ref_chunks.size(); i++)
				{
					for(int u=0; u<copies; ++u)
					{
						// create a bundle from the file
						dtn::data::Bundle b;

						// set the destination
						b.destination = file_destination;

						// add payload block with the reference
						b.push_back(ref);

						// set destination address to non-singleton
						if (bundle_group) b.set(dtn::data::PrimaryBlock::DESTINATION_IS_SINGLETON, false);

						// enable encryption if requested
						if (bundle_encryption) b.set(dtn::data::PrimaryBlock::DTNSEC_REQUEST_ENCRYPT, true);

						// enable signature if requested
						if (bundle_signed) b.set(dtn::data::PrimaryBlock::DTNSEC_REQUEST_SIGN, true);

						// enable custody transfer if requested
						if (bundle_custody) {
							b.set(dtn::data::PrimaryBlock::CUSTODY_REQUESTED, true);
							b.custodian = dtn::data::EID("api:me");
						}

						// enable compression
						if (bundle_compression) b.set(dtn::data::PrimaryBlock::IBRDTN_REQUEST_COMPRESSION, true);

						// set the lifetime
						b.lifetime = lifetime;

						// set the bundles priority
						b.setPriority(dtn::data::PrimaryBlock::PRIORITY(priority));

						// send the bundle
						client << b;

						if (copies > 1)
						{
							std::cout << "sent copy #" << (u+1) << std::endl;
						}
					}
				}

				// flush the buffers
				client.flush();
			} catch (const ibrcommon::IOException &ex) {
				std::cerr << "Error while sending bundle." << std::endl;
				std::cerr << "\t" << ex.what() << std::endl;
				error = true;
			}

			// Shutdown the client connection.
			client.close();

		} catch (const ibrcommon::IOException &ex) {
			std::cout << "Error: " << ex.what() << std::endl;
			error = true;
		} catch (const dtn::api::ConnectionException&) {
			// connection already closed, the daemon was faster
		}

		// close the tcpstream
		conn.close();
	} catch (const std::exception &ex) {
		std::cout << "Error: " << ex.what() << std::endl;
		error = true;
	}

	if (error) return -1;

	return 0;
}
