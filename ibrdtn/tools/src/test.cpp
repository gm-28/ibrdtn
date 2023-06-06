#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <iostream>
#include "keyconfig.h" 
#include <fcntl.h>

#include "config.h"
// #include "manager.h"
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

using namespace std;

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

    std::string command = "dtnsend " + destination + " " + remoteFilePath;
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

int main()
{
    int rc;
    //Manager manager;
    
    //Setup the sessions
    ssh_session session1 = start_session("localhost", "moreira1", KEY_PATH, "2222");
    ssh_session session2 = start_session("localhost", "moreira2", KEY_PATH, "2223");

    // Start the daemons
    ssh_channel channel = ssh_channel_new(session1);
    rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    rc = ssh_channel_request_exec(channel, "dtnd -i enp0s3");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    sleep(1);
    // ssh_channel channel2 = ssh_channel_new(session2);
    // rc = ssh_channel_open_session(channel2);
    // if (rc != SSH_OK) {
    //     fprintf(stderr, "Error opening channel for the second virtual machine: %s\n", ssh_get_error(session2));
    //     ssh_channel_free(channel2);
    //     ssh_disconnect(session2);
    //     ssh_free(session2);
    //     exit(EXIT_FAILURE);
    // }

    // rc = ssh_channel_request_exec(channel2, "dtnd -i enp0s3");
    // if (rc != SSH_OK) {
    //     fprintf(stderr, "Error executing command on the second virtual machine: %s\n", ssh_get_error(session2));
    //     ssh_channel_free(channel2);
    //     ssh_disconnect(session2);
    //     ssh_free(session2);
    //     exit(EXIT_FAILURE);
    // }

    std::string file_destination1 = "dtn://moreira2-VirtualBox/dtnRecv";
    std::string file_destination2 = "dtn://moreira1-VirtualBox/dtnRecv";

    std::string file_source = "fileSource";
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

   	std::list<std::string> arglist;

    // if (arglist.size() <= 1)
	// {
	// 	return -1;
	// } else if (arglist.size() == 2)
	// {
	// 	std::list<std::string>::iterator iter = arglist.begin(); ++iter;

	// 	// the first parameter is the destination
	// 	file_destination = (*iter);

	// 	use_stdin = true;
	// }
	// else if (arglist.size() > 2)
	// {
	// 	std::list<std::string>::iterator iter = arglist.begin(); ++iter;

	// 	// the first parameter is the destination
	// 	file_destination = (*iter); ++iter;

	// 	// the second parameter is the filename
	// 	filename = (*iter);
	// }

    filename = "sendFile";

    // open file as read-only BLOB
    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);

    //Split the file BLOB into smaller BLOBs
    auto ref_chunks = splitBlob(ref,5);

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

        // Open the output file stream
        std::ofstream outputFile("Sender/bundle.bin", std::ios::binary);

        // Create a DefaultSerializer object with the output stream
        dtn::data::DefaultSerializer serializer(outputFile);

        // Serialize the bundle and write it to the file
        serializer << b;
        
        // Close the output file stream
        outputFile.close();
        
        //send the bundle

        const std::string localFilePath = "Sender/bundle.bin";
        const std::string remoteFilePath = "ibrdtn/ibrdtn/tools/src/Sender/bundle.bin";
        
        sendBundle(session1,localFilePath,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira2-VirtualBox/dtnRecv");

        // if(i < 2){
        //     sendBundle(session1,localFilePath,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira2-VirtualBox/dtnRecv");
        // }else{
        //     sendBundle(session2,localFilePath,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira1-VirtualBox/dtnRecv");
        // }
    }
    // ssh_channel channel2_cmd = ssh_channel_new(session2);
    // rc = ssh_channel_open_session(channel2_cmd);
    // if (rc != SSH_OK) {
    //     fprintf(stderr, "Error opening channel for the second virtual machine: %s\n", ssh_get_error(session2));
    //     ssh_channel_free(channel2_cmd);
    //     ssh_disconnect(session2);
    //     ssh_free(session2);
    //     exit(EXIT_FAILURE);
    // }


    // rc = ssh_channel_request_exec(channel2_cmd, "dtnping dtn://moreira1-VirtualBox/echo");
    // if (rc != SSH_OK) {
    //     fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session2));
    //     ssh_channel_free(channel2_cmd);
    //     ssh_disconnect(session2);
    //     ssh_free(session2);
    //     exit(EXIT_FAILURE);
    // }

    // c =0;
    // // Read and print the command output
    // while ((nbytes = ssh_channel_read(channel2_cmd, buffer2, sizeof(buffer2), 0)) > 0) {
    //     fwrite(buffer2, 1, nbytes, stdout);
    //     c++;
    //     if(c == 5) break;
    // }

    // // Open the input file stream
    // std::ifstream inputFile("bundle.bin", std::ios::binary);

    // // Create a DefaultDeserializer object with the input stream
    // dtn::data::DefaultDeserializer deserializer(inputFile);

    // // Create a bundle object
    // dtn::data::Bundle bundle;

    // // Deserialize the bundle from the file
    // deserializer >> bundle;

    // // Close the input file stream
    // inputFile.close();

    // // get the reference to the blob
    // ibrcommon::BLOB::Reference ref2 = bundle.find<dtn::data::PayloadBlock>().getBLOB();
    // std::fstream file;
    // // write the data to output
    // bool _stdout = true;

    // if (_stdout)
    // {
    //     std::cout << ref.iostream()->rdbuf() << std::flush;
    // }
    // else
    // {
    //     // write data to temporary file
    //     try {
    //         std::cout << "Bundle received." << std::endl;

    //         file << ref.iostream()->rdbuf();
    //     } catch (const std::ios_base::failure&) {

    //     }
    // }

    // Close the channels and disconnect from the virtual machines
    // disconnect(channel2_cmd, channel2, session2);
    // disconnect(channel);
    // disconnect(channel2);
    ssh_disconnect(session1);
    ssh_free(session1);
    // ssh_disconnect(session2);
    // ssh_free(session2);
    return 0;
}

    /* To Do:
    Create a bundle [Done]
    Generate the bundle file [Done]
    Send the bundle file to virtual machine 1 via ssh [Done]
        Send the bundle with dtnsend to the neighbor to do this request the exectuion of command dtnsend
        Receive the bundle on virtual machine 2
    Generate the bundle from the file [Done]
        Send the bundle file to host via ssh
    Recreate the bundle [Done]
    */
