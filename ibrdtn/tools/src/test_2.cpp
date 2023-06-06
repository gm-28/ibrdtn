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

#include <thread>
#include <chrono>
#include <atomic>

#include <map>

std::fstream file;
std::map<int, dtn::data::Bundle> bundleMap;
int nextExpectedBundle = 0;

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
int exitStatus = 0;

std::atomic<bool> terminateFlag(false);

int h = 0;
bool _stdout = true;
bool receiver_finished = false;
int counter = 0;

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

void disconnect(ssh_channel channel) {
    // Close the channel
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
}

/**
 * Transfer a file from a remote server to the local machine using SFTP.
 *
 * @param session         The SSH session established with the remote server.
 * @param remoteFilePath  The path to the remote file on the server.
 * @param localFilePath   The path to the local file to save the transferred file.
 * @return                True if the file transfer is successful, false otherwise.
 */
bool transferFileFromRemote(ssh_session session, const std::string& remoteFilePath, const std::string& localFilePath) {
    // Create an SFTP session
    sftp_session sftp = sftp_new(session);
    if (sftp == nullptr) {
        std::cerr << "Error creating SFTP session: " << ssh_get_error(session) << std::endl;
        return false;
    }

    // Initialize the SFTP session
    int rc = sftp_init(sftp);
    if (rc != SSH_OK) {
        std::cerr << "Error initializing SFTP session: " << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        return false;
    }

    // Open the remote file for reading
    sftp_file remoteFile = sftp_open(sftp, remoteFilePath.c_str(), O_RDONLY, 0);
    if (remoteFile == nullptr) {
        std::cerr << "Error opening remote file: " << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        return false;
    }

    // Create a local file for writing
    std::ofstream localFile(localFilePath, std::ios::binary);
    if (!localFile.is_open()) {
        std::cerr << "Error creating local file" << std::endl;
        sftp_close(remoteFile);
        sftp_free(sftp);
        return false;
    }

    // Read the remote file data and write it to the local file
    const int bufferSize = 1024;
    char buffer[bufferSize];
    ssize_t bytesRead;
    do {
        bytesRead = sftp_read(remoteFile, buffer, bufferSize);
        if (bytesRead > 0) {
            localFile.write(buffer, bytesRead);
        } else if (bytesRead < 0) {
            std::cerr << "Error reading remote file: " << ssh_get_error(session) << std::endl;
            localFile.close();
            sftp_close(remoteFile);
            sftp_free(sftp);
            return false;
        }
    } while (bytesRead > 0);

    // Close the local file, remote file, and free the SFTP session
    localFile.close();
    sftp_close(remoteFile);
    sftp_free(sftp);

    return true;
}

bool checkRemoteFileExists(ssh_session session, const char* remoteFilePath) {
    // Create an SSH channel
    ssh_channel channel = ssh_channel_new(session);
    if (channel == nullptr) {
        std::cerr << "Error creating SSH channel: " << ssh_get_error(session) << std::endl;
        return false;
    }

    // Open the channel
    int rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK) {
        std::cerr << "Error opening channel: " << ssh_get_error(session) << std::endl;
        ssh_channel_free(channel);
        return false;
    }

    // Execute the command to check file existence and size
    std::string command = "du -b " + std::string(remoteFilePath) + " | cut -f1";
    rc = ssh_channel_request_exec(channel, command.c_str());
    if (rc != SSH_OK) {
        std::cerr << "Error executing command: " << ssh_get_error(session) << std::endl;
        ssh_channel_free(channel);
        return false;
    }

    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    rc = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
    if (rc < 0) {
        std::cerr << "Error reading channel: " << ssh_get_error(session) << std::endl;
        ssh_channel_free(channel);
        return false;
    }

    long fileSize = std::atol(buffer);
    disconnect(channel);

    return fileSize > 0;
}

bool compareFileContents(ssh_session session, const char* remoteFilePath, const char* tempFilePath) {
    // Create an SFTP session
    sftp_session sftp = sftp_new(session);
    if (sftp == nullptr) {
        std::cerr << "Error creating SFTP session: " << ssh_get_error(session) << std::endl;
        return false;
    }

    // Initialize the SFTP session
    int rc = sftp_init(sftp);
    if (rc != SSH_OK) {
        std::cerr << "Error initializing SFTP session: " << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        return false;
    }
	
	sftp_file remoteFile = sftp_open(sftp, remoteFilePath, O_RDONLY, 0);
    
	if (!remoteFile)
        return false;

    FILE* localFile = fopen(tempFilePath, "r");
    if (!localFile) {
        sftp_close(remoteFile);
        return false;
    }

    const int bufferSize = 1024;
    char remoteBuffer[bufferSize];
    char localBuffer[bufferSize];
    size_t remoteBytesRead, localBytesRead;

    do {
        remoteBytesRead = sftp_read(remoteFile, remoteBuffer, bufferSize);
        localBytesRead = fread(localBuffer, 1, bufferSize, localFile);
        if (remoteBytesRead != localBytesRead || memcmp(remoteBuffer, localBuffer, remoteBytesRead) != 0) {
            fclose(localFile);
            sftp_close(remoteFile);
            return false;
        }
    } while (remoteBytesRead > 0);

    fclose(localFile);
    sftp_close(remoteFile);
    return true;
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
        std::cerr << "Error deserializing bundle: " << e.what() << std::endl;
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

void writeToFile(int bundleNumber, dtn::data::Bundle& bundle) {
    // get the reference to the blob
    ibrcommon::BLOB::Reference ref = bundle.find<dtn::data::PayloadBlock>().getBLOB();

    // Write the bundle content to the file
    file << ref.iostream()->rdbuf();
       
    // Update the next expected bundle number
    ++nextExpectedBundle;
    
    // Check if there are subsequent bundles in the map and write them to the file
    while (bundleMap.find(nextExpectedBundle) != bundleMap.end()) {
        dtn::data::Bundle& nextBundle = bundleMap[nextExpectedBundle];

        // get the reference to the blob
        ibrcommon::BLOB::Reference ref2 = bundle.find<dtn::data::PayloadBlock>().getBLOB();

        // Write the bundle content to the file
        file << ref2.iostream()->rdbuf();
        bundleMap.erase(nextExpectedBundle);
        ++nextExpectedBundle;
    }
}

// Function to execute the receiver command via SSH and continuously receive bundles
void receiver(ssh_session session, const char* user, const std::string& localFilePath, const std::string& remoteFilePath) {    
    int rc;

    // Execute the receiver command in a loop
    while (true) {
        // Create a channel for executing the receiver command
        ssh_channel channel = ssh_channel_new(session);
        rc = ssh_channel_open_session(channel);
        if (rc != SSH_OK) {
            fprintf(stderr, "Failed to open SSH channel: %s\n", ssh_get_error(session));
                    disconnect(channel);
            return;
        }

        std::string command = "dtnrecv --file " + remoteFilePath + " --name dtnRecv";
        rc = ssh_channel_request_exec(channel, command.c_str());
        if (rc != SSH_OK) {
            fprintf(stderr, "Failed to execute SSH command: %s\n", ssh_get_error(session));
            disconnect(channel);
            return;
        }

        int exitStatus = ssh_channel_get_exit_status(channel);
        printf("%s\n", command.c_str());

        // Contact the main program based on the command's return value
        if (exitStatus == 0) {
            printf("Received bundle in VM: %s\n", user);
        
            // When the bundle is received transfer it to the distination host
            if (!transferFileFromRemote(session,remoteFilePath,localFilePath)){
                fprintf(stderr, "Error transfering bundle.bin from remote\n");
                disconnect(channel);
                return;
            }

            ssh_channel channel2 = ssh_channel_new(session);
            rc = ssh_channel_open_session(channel2);
            if (rc != SSH_OK) {
                fprintf(stderr, "Failed to open SSH channel: %s\n", ssh_get_error(session));
                ssh_channel_free(channel2);
                return;
            }

            // Execute the rm command to delete the file
            char command[256];
            snprintf(command, sizeof(command), "rm -f %s", remoteFilePath.c_str());

            rc = ssh_channel_request_exec(channel2, command);
            if (rc != SSH_OK) {
                fprintf(stderr, "Failed to delete file: %s\n", ssh_get_error(session));
                ssh_channel_free(channel2);
                return;
            }

            // Get the exit status of the command
            int exitStatus = ssh_channel_get_exit_status(channel2);
            if (exitStatus == 0) {
                // printf("File deleted successfully on the remote host\n");
            } else {
                fprintf(stderr, "%s\n", ssh_get_error(session));
            }

            // Wait for the command to complete
            ssh_channel_send_eof(channel2);
            ssh_channel_is_eof(channel2); // Wait for end of file indication

            // Close the SSH channel
            ssh_channel_free(channel2);


            dtn::data::Bundle bundle;
            deserializeBundleFromFile(localFilePath,bundle);

            ibrcommon::BLOB::Reference ref = bundle.find<dtn::data::PayloadBlock>().getBLOB();

            // Write the bundle content to the file
            file << ref.iostream()->rdbuf();
            dtn::data::BundleID &id = bundle;

            int sequencenumber = std::stoi(id.sequencenumber.toString());
            printf("%d \n",sequencenumber);
            
            if (!sequencenumber < nextExpectedBundle){
                if (sequencenumber == nextExpectedBundle) {
                    writeToFile(sequencenumber, bundle);
                } else {
                    // Store the bundle in the map or update existing entry
                    bundleMap[sequencenumber] = bundle;
                }
            }
        }

        disconnect(channel);
    }
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

	int ret = EXIT_SUCCESS;
	std::string filename = "receivedFile.txt";
	std::string name = "filetransfer";
	dtn::data::EID group;
	int timeout = 0;
	int count   = 1;
	ibrcommon::File unixdomain;

    int rc;
 
	//Setup the sessions
    ssh_session session2 = start_session("localhost", "moreira2", KEY_PATH, "2223");

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
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

	std::cout << "Wait for incoming bundle from vm... " << std::endl;
	file.open(filename.c_str(), std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
	file.exceptions(std::ios::badbit | std::ios::eofbit);

    const char* remoteFilePath = "ibrdtn/ibrdtn/tools/src/Receiver/bundle.bin";
    const char* localFilePath = "Receiver/bundle.bin";

    int numBundles = 1;
    
    std::thread receiverThread1(receiver,session2 ,"moreira2",localFilePath,remoteFilePath);

    receiverThread1.join();
    
    file.close();
	disconnect(channel2);
    ssh_disconnect(session2);
    ssh_free(session2);
    return 0;
}
// std::string localFilePath = "bundle" + std::to_string(i) + ".bin";