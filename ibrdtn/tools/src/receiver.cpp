//receiver.cpp 

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
#include <ibrcommon/data/BLOB.h>
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
#include <mutex>
#include <fstream>
#include <chrono>
#include <ctime>
#include <tuple>

int nextExpectedBundle = 0;
int exitStatus = 0;
int lastsequencenumber = -1;
bool lastbundlefound = false;

std::fstream file;
std::map<int,bool > ackMap;
std::map<int, dtn::data::Bundle> bundleMap;
std::atomic<bool> terminateFlag(false);
std::mutex bundleMapMutex;
std::mutex stopMutex;
std::time_t currentTime;
uint64_t now;

dtn::data::Bundle processACKBundle( const std::string& localFilePath, EID addr_src , EID addr_dest, int nextExpectedBundle) {
    // create a bundle fro the file
    dtn::data::Bundle b;

    b.source = addr_src;

    b.destination = addr_dest;

    // set the lifetime
    b.lifetime = 3600;

    b.set(dtn::data::PrimaryBlock::ACK_BUNDLE, true); 
	std::string filename = "Sender/ack.txt";
    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);
    // add payload block with the references
    b.push_back(ref);

    dtn::data::BundleID& id = b;
    id.sequencenumber.fromString(std::to_string(nextExpectedBundle).c_str());

    // Open the output file stream
    std::ofstream outputFile(localFilePath, std::ios::binary);
    // Create a DefaultSerializer object with the output stream
    dtn::data::DefaultSerializer serializer(outputFile);

    // Serialize the bundle and write it to the file
    serializer << b;
    
    // Close the output file stream
    outputFile.close();

    return b;
}

void print_help()
{
	std::cout << "-- Receiver --" << std::endl
			<< "Required parameters: " << std::endl 
			<< " --file <path>    Write the incoming data to this file" << std::endl << std::endl
			<< "Optional parameters: " << std::endl
			<< " -h|--help        Display this text" << std::endl
            << "                  " << std::endl
            << "SSH Connection 1  " << std::endl
            << " -Host1           Set the first host name" << std::endl
            << " -User1           Set the first user name" << std::endl
			<< " -U1 <socket>     Connect to the first host through this socket " << std::endl
            << "                  " << std::endl
            << "SSH Connection 2  " << std::endl			
            << " -Host2           Set the second host name" << std::endl
            << " -User2           Set the second user name" << std::endl
			<< " -U2 <socket>     Connect to the first host through this socket " << std::endl;
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
 * Transfer a file from a remote server to the local machine using SFTP.
 *
 * @param session         The SSH session established with the remote server.
 * @param remoteFilePath  The path to the remote file on the server.
 * @param localFilePath   The path to the local file to save the transferred file.
 * @return                True if the file transfer is successful, false otherwise.
 */
bool transferFileFromRemote(ssh_session session, const std::string& remoteFilePath, const std::string& localFilePath,const char* user) {
    // Create an SFTP session
    sftp_session sftp = sftp_new(session);
    if (sftp == nullptr) {
        std::cerr << user << "Error creating SFTP session: " << ssh_get_error(session) << std::endl;
        return false;
    }

    // Initialize the SFTP session
    int rc = sftp_init(sftp);
    if (rc != SSH_OK) {
        std::cerr << user << ": Error opening remote file: " <<  remoteFilePath.c_str() << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        return false;
    }

    // Open the remote file for reading
    sftp_file remoteFile = sftp_open(sftp, remoteFilePath.c_str(), O_RDONLY, 0);
    if (remoteFile == nullptr) {
        std::cerr << user << ": Error opening remote file: " <<  remoteFilePath.c_str() << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        return false;
    }

    // Create a local file for writing
    std::ofstream localFile(localFilePath, std::ios::binary);
    if (!localFile.is_open()) {
        std::cerr << user << "Error creating local file: "<< localFilePath.c_str() << std::endl;
        sftp_close(remoteFile);
        sftp_free(sftp);
        return false;
    }

    sftp_attributes attrs = sftp_fstat(remoteFile);
    if (!attrs) {
        std::cerr << "Error getting file attributes: " << ssh_get_error(session) << std::endl;
        localFile.close();
        sftp_close(remoteFile);
        sftp_free(sftp);
        return false;
    }

    // Read the remote file data and write it to the local file
    const int bufferSize = attrs->size;
    // const int bufferSize = 1024;
    // std::cout << bufferSize << std::endl;
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


    return true;
}

dtn::data::Bundle processBundle(const std::string& localFilePath, EID addr_src , EID addr_dest, int nextExpectedBundle) {
    // create a bundle fro the file
    dtn::data::Bundle b;

    b.source = addr_src;

    b.destination = addr_dest;

    // set the lifetime
    b.lifetime = 3600;
    
    b.set(dtn::data::PrimaryBlock::ACK_BUNDLE, true);

    dtn::data::BundleID& id = b;
    id.sequencenumber.fromString(std::to_string(nextExpectedBundle).c_str());

    // Open the output file stream
    std::ofstream outputFile(localFilePath, std::ios::binary);
    // Create a DefaultSerializer object with the output stream
    dtn::data::DefaultSerializer serializer(outputFile);

    // Serialize the bundle and write it to the file
    serializer << b;
    
    // Close the output file stream
    outputFile.close();

    return b;
}

void writeToFile(int bundleNumber, dtn::data::Bundle& bundle,const char* user) {
    // get the reference to the blob
    ibrcommon::BLOB::Reference ref = bundle.find<dtn::data::PayloadBlock>().getBLOB();
    
    // std::cout << "writing bundle " << bundle.sequencenumber.toString().c_str() << std::endl;

    // Write the bundle content to the file
    {
        std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
        file << ref.iostream()->rdbuf();
        ++nextExpectedBundle;
    } // The lock is automatically released when lock goes out of scope


    if(bundle.sequencenumber == lastsequencenumber){
        // Set the terminateFlag to stop the worker thread
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            terminateFlag = true;
        }
        return;
    }

    // Check if there are subsequent bundles in the map and write them to the file
    while (true) {
        std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
        auto it = bundleMap.find(nextExpectedBundle);
        if (it != bundleMap.end()) {
            dtn::data::Bundle& nextBundle = it->second;

            // get the reference to the blob
            ibrcommon::BLOB::Reference ref2 = nextBundle.find<dtn::data::PayloadBlock>().getBLOB();
            // std::cout << "Subsequent writing bundle " << nextBundle.sequencenumber.toString().c_str()  << std::endl;

            // Write the bundle content to the file
            file << ref2.iostream()->rdbuf();
            bundleMap.erase(it);
            ++nextExpectedBundle;

            if(nextBundle.sequencenumber == lastsequencenumber){
                // Set the terminateFlag to stop the worker thread
                {
                    std::lock_guard<std::mutex> lock(stopMutex);
                    terminateFlag = true;
                }
            }
        } else {
            break;
        }
    }
}

std::tuple<int, bool, bool> removeHeaderFlags(const std::string& filename)
{
    // Open the original file
    std::ifstream originalFile(filename, std::ios::binary);
    if (!originalFile.is_open()) {
        // File opening failed
        std::cerr << "Failed to open file: " << filename << std::endl;
        return std::make_tuple(-1, false, false);
    }

    // Get the size of the file
    originalFile.seekg(0, std::ios::end);
    std::streampos fileSize = originalFile.tellg();
    originalFile.seekg(0, std::ios::beg);

    // Read the header flags
    char flags;
    int sequenceNumber;
    bool lastChunkFlag;
    bool ackFlag;

    originalFile.read(&flags, sizeof(flags));
    originalFile.read(reinterpret_cast<char*>(&sequenceNumber), sizeof(sequenceNumber));

    lastChunkFlag = (flags & 0x01) != 0;  // Extract last chunk flag from flags
    ackFlag = (flags & 0x02) != 0;        // Extract ACK flag from flags

    // Calculate the size of the remaining data
    std::streampos remainingSize = fileSize - sizeof(flags) - sizeof(sequenceNumber);

    // Open a new file for writing the data without the header flags
    std::ofstream outputFile("Receiver/newbundleac.bin", std::ios::binary);
    if (!outputFile.is_open()) {
        // File creation failed
        std::cerr << "Failed to create file: newbundleac.bin" << filename << std::endl;
        originalFile.close();
        return std::make_tuple(sequenceNumber, lastChunkFlag, ackFlag);
    }

    // Copy the data without the header flags
    char buffer[1024];
    std::streampos bytesRead = 0;
    while (bytesRead < remainingSize) {
        std::streamsize chunkSize = std::min(static_cast<std::streamsize>(sizeof(buffer)), static_cast<std::streamsize>(remainingSize - bytesRead));
        originalFile.read(buffer, chunkSize);
        outputFile.write(buffer, chunkSize);
        bytesRead += chunkSize;
    }

    // Close the files
    originalFile.close();
    outputFile.close();

    return std::make_tuple(sequenceNumber, lastChunkFlag, ackFlag);
}

void createChunkFile(int sequenceNumber, const ibrcommon::BLOB::Reference& chunkBlob, std::string filename, bool isLastChunk, bool ackFlag)
{
    std::ofstream file(filename, std::ios::binary);
    ibrcommon::BLOB::Reference blobref = chunkBlob;
    ibrcommon::BLOB::iostream blobStream = blobref.iostream();

    if (file.is_open()) {
        ibrcommon::BLOB::iostream chunkStream = blobStream;

        // Write the flags
        char flags = 0;
        if (isLastChunk) flags |= 0x01;   // Set the last chunk flag
        if (ackFlag) flags |= 0x02;       // Set the ACK flag
        file.write(&flags, sizeof(flags));

        // Write the sequence number as a header to the file
        file.write(reinterpret_cast<const char*>(&sequenceNumber), sizeof(sequenceNumber));

        // Write the chunk data to the file
        file << chunkStream->rdbuf();

        file.close();
        // std::cout << "Chunk file created: " << filename << std::endl;
    } else {
        std::cerr << "Failed to create chunk file: " << filename << std::endl;
    }
}

void ac_receiver(const std::string& filename){
    std::string filepath = "/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin";
    while(!terminateFlag){
        std::ifstream file(filepath);
        if(file.good()){
            auto result = removeHeaderFlags("/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin");
            int sequenceNumber = std::get<0>(result);
            bool isLastChunk = std::get<1>(result);

            std::__cxx11::string filename2 = "Receiver/newbundleac.bin";
            ibrcommon::BLOB::Reference chunkBlob = ibrcommon::BLOB::open(filename2);
            
            std::cout << "Received bundle: " << sequenceNumber << " on AC size: "<< chunkBlob.size() << std::endl;
            
            EID addr_src = EID("dtn://moreira-XPS-15-9570");
            EID addr_dest = EID("dtn://moreira-XPS-15-9570");
            dtn::data::Bundle bundle;
            bundle.source = addr_src;
            bundle.destination = addr_dest;
            bundle.push_back(chunkBlob);
            bundle.set(dtn::data::PrimaryBlock::LAST_BUNDLE, isLastChunk);

            dtn::data::BundleID& id = bundle;

            // std::cout << "Bundle: " << bundle.sequencenumber.toString().c_str() << " FLAG: "<<bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) << " VM: " << user << std::endl;
            id.sequencenumber.fromString(std::to_string(sequenceNumber).c_str());
            if(bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) == true){
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    lastbundlefound = true;
                    lastsequencenumber = sequenceNumber;
                    // std::cout << "Last sequence number " << lastsequencenumber << std::endl;
                }

            }

            if (sequenceNumber == nextExpectedBundle) {
                writeToFile(sequenceNumber, bundle,"ac");
            } else {
                // Store the bundle in the map or update existing entry
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    bundleMap[sequenceNumber] = bundle;
                } // The lock is automatically released when lock goes out of scope
            }
            
            std::string filenameac = "Sender/ack.txt";
            ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filenameac);

            createChunkFile(sequenceNumber, ref, "/home/moreira/Documents/unet-3.4.0/scripts/bundleack.bin", false ,true); //overhead of 4 bytes because of int

            std::string src_ip = "192.168.1.70";
            std::string src_port = "1102";
            std::string dest_node = "232";
            std::string timeout = "10000";
            std::string filenameack = "bundleack.bin";
        
            std::string command = "python3 ac_sender.py " + src_ip + " " + src_port + " " + dest_node + " " + timeout + " " + filenameack;
            int result1 = ::system(command.c_str()); 		
            if (result1 != 0){
                std::cout << "Error sending through ac"<< std::endl;
            }

            sleep(1);
            std::string command2 = "rm /home/moreira/Documents/unet-3.4.0/scripts/bundleack.bin";
            result1 = std::system(command2.c_str());
            if (result1 == 0) {
                std::cout << "File removed successfully." << std::endl;
            } else {
                std::cout << "Failed to remove file." << std::endl;
            }
        }
    }
}

void sendBundle(ssh_session session, const std::string localFilePath, const std::string remoteFilePath, const std::string destination, const char* user,int n) {
    bool transferSuccess = transferFileToRemote(session, localFilePath, remoteFilePath,user);
    if (!transferSuccess) {
        std::cerr << "Error transferring file: " << localFilePath << std::endl;
        return;
    }

    std::string command = "dtnsend " + destination + " " + remoteFilePath;
    // std::cout << user << ": " << command << std::endl;

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

    ssh_channel channel2 = ssh_channel_new(session);
    rc = ssh_channel_open_session(channel2);
    if (rc != SSH_OK) {
        fprintf(stderr, "Failed to open SSH channel 2: %s\n", ssh_get_error(session));
        ssh_channel_free(channel2);
        return;
    }

    // Execute the rm command to delete the file
    char command2[256];
    snprintf(command2, sizeof(command2), "rm -f %s", remoteFilePath.c_str());

    rc = ssh_channel_request_exec(channel2, command2);
    if (rc != SSH_OK) {
        fprintf(stderr, "Failed to delete file: %s\n", ssh_get_error(session));
        ssh_channel_free(channel2);
        return;
    }

    // Get the exit status of the command
    int exitStatus2 = ssh_channel_get_exit_status(channel2);
    if (exitStatus2 == 0) {
        // printf("File deleted successfully on the remote host\n");
    } else {
        fprintf(stderr, "%s\n", ssh_get_error(session));
    }

    // Wait for the command to complete
    ssh_channel_send_eof(channel2);
    ssh_channel_is_eof(channel2); // Wait for end of file indication

    // Close the SSH channel
    ssh_channel_free(channel2);

    // std::cout << user << ": Sent ACK " << n << std::endl;
    return;
}

void receiver(ssh_session session, const char* user, const std::string& localFilePath, const std::string& remoteFilePath, const std::string ackdestination) {    
    int rc;

    // Execute the receiver command in a loop
    while (!terminateFlag) {

        // Create a channel for executing the receiver command
        ssh_channel channel = ssh_channel_new(session);
        rc = ssh_channel_open_session(channel);
        if (rc != SSH_OK) {
            fprintf(stderr, "Failed to open SSH channel 1: %s\n", ssh_get_error(session));
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
        // std::cout << user << ": "<< command.c_str() <<std::endl;

        ssh_channel_set_blocking(channel,0);
        int exitStatus = ssh_channel_get_exit_status(channel);
        while (exitStatus == -1){
            exitStatus = ssh_channel_get_exit_status(channel);
            // std::cout << terminateFlag << std::endl;
            // std::cout << "exit != 0"<< std::endl;

            if(terminateFlag) break;
        }
        ssh_channel_set_blocking(channel,1);

        if (exitStatus == 0) {
            // When the bundle is received, transfer it to the destination host
            if (!transferFileFromRemote(session, remoteFilePath, localFilePath,user)) {
                printf( "Error transferring bundle.bin from remote\n");
                disconnect(channel);
                return;
            }

            ssh_channel channel2 = ssh_channel_new(session);
            rc = ssh_channel_open_session(channel2);
            if (rc != SSH_OK) {
                fprintf(stderr, "Failed to open SSH channel 2: %s\n", ssh_get_error(session));
                ssh_channel_free(channel2);
                return;
            }

            // Execute the rm command to delete the file
            char command2[256];
            snprintf(command2, sizeof(command2), "rm -f %s", remoteFilePath.c_str());

            rc = ssh_channel_request_exec(channel2, command2);
            if (rc != SSH_OK) {
                fprintf(stderr, "Failed to delete file: %s\n", ssh_get_error(session));
                ssh_channel_free(channel2);
                return;
            }

            // Get the exit status of the command
            int exitStatus2 = ssh_channel_get_exit_status(channel2);
            if (exitStatus2 == 0) {
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
            deserializeBundleFromFile(localFilePath, bundle);

            dtn::data::BundleID& id = bundle;
            // std::cout << "Bundle: " << bundle.sequencenumber.toString().c_str() << " FLAG: "<<bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) << " VM: " << user << std::endl;
            int sequencenumber = std::stoi(id.sequencenumber.toString());
            if(bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) == true){
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    lastbundlefound = true;
                    lastsequencenumber = sequencenumber;
                    // std::cout << "Last sequence number " << lastsequencenumber << std::endl;
                }
  
            }
            {
                std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                if(lastbundlefound && bundleMap.size() == lastsequencenumber-1 || lastsequencenumber == 0){
                    now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    //uint64_t seconds = now / 1000;
                    // std::cout << timeSinceEpochMillisec() << std::endl;

                    // std::cout << now << std::endl;
                    // std::cout << seconds << std::endl;
                    // std::cout << "Last received bundle: " << bundle.sequencenumber.toString().c_str() << " FLAG: "<<bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) << " VM: " << user << std::endl;

                }

            }
            // std::cout << user << ": Received bundle: " << sequencenumber << std::endl;
            if (sequencenumber == nextExpectedBundle) {
                writeToFile(sequencenumber, bundle,user);
            } else {
                // Store the bundle in the map or update existing entry
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    bundleMap[sequencenumber] = bundle;
                } 
            }

            dtn::data::Bundle ack = processACKBundle("Sender/bundleAck.bin",bundle.destination,bundle.source,std::atoi(bundle.sequencenumber.toString().c_str()));
            // std::__cxx11::string source = bundle.source.getString() + "/ackRecv";
            sendBundle(session,"Sender/bundleAck.bin","/root/ibrdtn-repo/ibrdtn/tools/src/Sender/bundleACK.bin",ackdestination,user, std::atoi(bundle.sequencenumber.toString().c_str()) );
        }

        disconnect(channel);
    }

    return;
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
    ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, "rsa-sha2-256,rsa-sha2-512,ecdh-sha2-nistp256,ssh-rsa");

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
	int ret = EXIT_SUCCESS;
    int rc;

	std::string filename;
	dtn::data::EID group;
    const char* host1 = "192.168.0.102";
    const char* user1 = "root";
    const char* port1 = "22";
    const char* host2 = "192.168.0.105";
    const char* user2 = "root";
    const char* port2 = "22";

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

	//Setup the sessions
    ssh_session session1 = start_session(host1, user1, KEY_PATH, port1);
    ssh_session session2 = start_session(host2, user2, KEY_PATH, port2);

    ssh_channel channel1 = ssh_channel_new(session1);
    rc = ssh_channel_open_session(channel1);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel for the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel1);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    rc = ssh_channel_request_exec(channel1, "dtnd -i enp0s3");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session2));
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
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

	std::cout << "Wait for incoming file..." << std::endl;
	file.open(filename.c_str(), std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
	file.exceptions(std::ios::badbit | std::ios::eofbit);

    const char* remoteFilePath = "/root/ibrdtn-repo/ibrdtn/tools/src/Receiver/bundle.bin";
    const char* localFilePath1 = "Receiver/bundle1.bin";
    const char* localFilePath2 = "Receiver/bundle2.bin";

    std::thread receiverThread2(receiver,session2 ,"192.168.0.105",localFilePath2,remoteFilePath,"dtn://A/ackRecv");
    std::thread receiverThread1(receiver,session1 ,"192.168.0.102",localFilePath1,remoteFilePath,"dtn://B/ackRecv");
    // std::thread receiverThread3(ac_receiver,"Sender/bundleac.bin");

    receiverThread2.join();
    receiverThread1.join();
    // receiverThread3.join();

    file.close();
	disconnect(channel2);
    ssh_disconnect(session2);
    ssh_free(session2);
    disconnect(channel1);
    ssh_disconnect(session1);
    ssh_free(session1);

    std::cout << filename <<" has been received!" << std::endl;
    std::cout << "Reception time(miliseconds) of the last bundle: " << now << std::endl;
    return ret;
}