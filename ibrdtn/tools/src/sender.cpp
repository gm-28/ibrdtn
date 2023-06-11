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
#include <ibrcommon/thread/Mutex.h>
#include <ibrcommon/thread/MutexLock.h>
#include <ibrcommon/data/BLOB.h>
#include <ibrcommon/Logger.h>
#include <iostream>
#include <fstream>
#include "ibrdtn/data/Serializer.h"
#include "ibrdtn/data/Bundle.h"
#include <vector>
#include <random>


using namespace std;
std::vector<ibrcommon::BLOB::Reference> blob_vec;

void print_help()
{
	std::cout << "-- Sender --" << std::endl
			<< "Required parameters: " << std::endl 
			<< " --file <path>    The file to transfer" << std::endl << std::endl
			<< "Optional parameters: " << std::endl
			<< " -h|--help        Display this text" << std::endl
            << "                  " << std::endl
            << "SSH Connection 1  " << std::endl
            << " -f1              Set the first EID name" << std::endl
            << " -Host1           Set the first host name" << std::endl
            << " -User1           Set the first user name" << std::endl
			<< " -U1 <socket>     Connect to the first host through this socket " << std::endl
            << "                  " << std::endl
            << "SSH Connection 2  " << std::endl
            << " -f2              Set the second EID name" << std::endl
            << " -Host2           Set the second host name" << std::endl
            << " -User2           Set the second user name" << std::endl
			<< " -U2 <socket>     Connect to the first host through this socket " << std::endl;
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

/**
 * Transfer a file to a remote server using SFTP.
 * 
 * @param session         The SSH session established with the remote server.
 * @param localFilePath   The path to the local file to transfer.
 * @param remoteFilePath  The path to the remote file on the server.
 * @return                True if the file transfer is successful, false otherwise.
 */
bool transferFileToRemote(ssh_session session, const std::string& localFilePath, const std::string& remoteFilePath,const char* user) {
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
    dtn::data::Bundle b1;
    deserializeBundleFromFile(localFilePath,b1);
    dtn::data::BundleID& id = b1;

    return true;
}

void sendBundle(ssh_session session, const std::string localFilePath, const std::string remoteFilePath, const std::string destination,const char* user) {
    bool transferSuccess = transferFileToRemote(session, localFilePath, remoteFilePath,user);
    if (!transferSuccess) {
        std::cerr << "Error transferring file: " << localFilePath << std::endl;
        return;
    }

    std::string command = "dtnsend " + destination + " " + remoteFilePath;
    // std::cout << command.c_str();
    // std::cout << "\n";

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
        // fwrite(buffer2, 1, nbytes, stdout);
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

ibrcommon::BLOB::Reference getBlobChunk(const ibrcommon::BLOB::Reference& blob, size_t chunkSize)
{
    // Create a new BLOB reference for the chunk
    ibrcommon::BLOB::Reference chunkBlob = ibrcommon::BLOB::create();

    // Create a new BLOB reference for this chunk
	ibrcommon::BLOB::Reference blobref = blob;

    // Get an iostream for the BLOB
    ibrcommon::BLOB::iostream blobStream = blobref.iostream();

    // Read the specified chunk size from the original blob
    ibrcommon::BLOB::iostream chunkStream = chunkBlob.iostream();
    ibrcommon::BLOB::copy(*chunkStream, *blobStream, chunkSize);

    return chunkBlob;
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
    int rc;  
    int ret = EXIT_SUCCESS;

    const char* host1 = "localhost";
    const char* user1 = "moreira1";
    const char* port1 = "2222";
    const char* host2 = "localhost";
    const char* user2 = "moreira2";
    const char* port2 = "2223";  

    std::string file_destination1 = "dtn://moreira2-VirtualBox/dtnRecv";
    std::string file_destination2 = "dtn://moreira1-VirtualBox/dtnRecv";
	std::string filename;

// Check if a filename argument is provided
    if (argc >= 2)
    {
        for (int i = 0; i < argc; ++i)
        {
            std::string arg = argv[i];

            // print help if requested
            if (arg == "-h" || arg == "--help")
            {
                print_help();
                return ret;
            }

            if (arg == "--file" && argc > i)
            {
                filename = argv[i + 1];
            }

            if (arg == "-EID1" && argc > i)
            {
                file_destination1 = argv[i + 1];
            }

            if (arg == "-Host1" && argc > i)
            {
                host1 = argv[i + 1];
            }

            if (arg == "-User1" && argc > i)
            {
                user1 = argv[i + 1];
            }

            if (arg == "-U1" && argc > i)
            {
                port1 = argv[i + 1];
            }

            if (arg == "-EID2" && argc > i)
            {
                file_destination2 = argv[i + 1];
            }

            if (arg == "-Host2" && argc > i)
            {
                host2 = argv[i + 1];
            }

            if (arg == "-User2" && argc > i)
            {
                user2 = argv[i + 1];
            }

            if (arg == "-U2" && argc > i)
            {
                port2 = argv[i + 1];
            }
        }
    }
    else
    {
        std::cout << "Provide the filename!" << std::endl;
        return -1;
    }

    std::string file_source = "fileSource";
	unsigned int lifetime = 3600;
	bool use_stdin = false;
	ibrcommon::File unixdomain;
	int priority = 1;
	int copies = 1;
	size_t chunk_size = 1024;
	bool bundle_encryption = false;
	bool bundle_signed = false;
	bool bundle_custody = false;
	bool bundle_compression = false;
	bool bundle_group = false;
    int nextExpectedBundle = 0;

	//Setup the sessions
    ssh_session session1 = start_session(host1, user1, KEY_PATH, port1);
    ssh_session session2 = start_session(host2, user2, KEY_PATH, port2);

    // Start the daemons
    ssh_channel channel1 = ssh_channel_new(session1);
    rc = ssh_channel_open_session(channel1);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel1);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    rc = ssh_channel_request_exec(channel1, "dtnd -i enp0s3");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel1);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    ssh_channel channel2 = ssh_channel_new(session2);
    rc = ssh_channel_open_session(channel2);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel for the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

    rc = ssh_channel_request_exec(channel2, "dtnd -i enp0s3");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command on the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

    sleep(1);

   	std::list<std::string> arglist;

    std::cout << filename <<" is being sent" << std::endl;

    // open file as read-only BLOB
    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);

    //Split the file BLOB into smaller BLOBs
    auto ref_chunks = splitBlob(ref,1000);

    EID addr = EID("dtn://moreira-XPS-15-9570");
    //generate bundle()
    for(int i = 0; i < ref_chunks.size(); i++)
    {
        // create a bundle from the file
        dtn::data::Bundle b;

        b.source = addr;

        b.destination = addr;

        // add payload block with the references
        b.push_back(ref_chunks[i]);

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
        
        if(i == (ref_chunks.size() - 1)){
            b.set(dtn::data::PrimaryBlock::LAST_BUNDLE, true);  
        }

        dtn::data::BundleID& id = b;
        int timestamp = std::stoi(id.timestamp.toString().c_str());

        id.sequencenumber.fromString(std::to_string(nextExpectedBundle).c_str());

        nextExpectedBundle++;
        int sequencenumber = std::stoi(id.sequencenumber.toString().c_str());
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 1);

        int random_number = dist(gen); 

        //send the bundle
        const std::string localFilePath1 = "Sender/bundle1.bin";
        const std::string localFilePath2 = "Sender/bundle2.bin";
        const std::string remoteFilePath = "ibrdtn/ibrdtn/tools/src/Sender/bundle.bin";

        // printf("Timestamp [%d] Sequence Number [%d] fragment offset [%s]\n",timestamp,sequencenumber,id.fragmentoffset.toString().c_str());
        if(random_number == 0){
            // Open the output file stream
            std::ofstream outputFile(localFilePath2, std::ios::binary);
            dtn::data::DefaultSerializer serializer(outputFile);

            // Serialize the bundle and write it to the file
            serializer << b;
            
            // Close the output file stream
            outputFile.close();

            sendBundle(session1,localFilePath2,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira2-VirtualBox/dtnRecv","moreira1");
        }else{
            // Open the output file stream
            std::ofstream outputFile(localFilePath1, std::ios::binary);
            // Create a DefaultSerializer object with the output stream
            dtn::data::DefaultSerializer serializer(outputFile);

            // Serialize the bundle and write it to the file
            serializer << b;
            
            // Close the output file stream
            outputFile.close();
            
            sendBundle(session2,localFilePath1,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira1-VirtualBox/dtnRecv","moreira2");
        }
    }

    // Close the channels and disconnect from the virtual machines
    // disconnect(channel2_cmd, channel2, session2);
    disconnect(channel1);
    disconnect(channel2);
    ssh_disconnect(session1);
    ssh_free(session1);
    ssh_disconnect(session2);
    ssh_free(session2);
    std::cout << filename <<" has been sent" << std::endl;

    return 0;
}

