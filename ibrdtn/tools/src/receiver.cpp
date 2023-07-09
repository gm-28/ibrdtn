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

dtn::data::Bundle processACKBundle( const std::string& localFilePath, EID addr_src , EID addr_dest, int nextExpectedBundle) {
    std::string filename = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/ack.txt";

    dtn::data::Bundle b;

    b.source = addr_src;

    b.destination = addr_dest;

    // set the lifetime
    b.lifetime = 3600;

    b.set(dtn::data::PrimaryBlock::ACK_BUNDLE, true); 
    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);
    // add payload block with the references
    b.push_back(ref);

    dtn::data::BundleID &id = b;
    id.sequencenumber.fromString(std::to_string(nextExpectedBundle).c_str());
    std::cout << "Bundle that was serialized: " << b.sequencenumber.toString().c_str() << std::endl;

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

bool transferFileToRemote(const std::string& localFilePath, const std::string& remoteFilePath, const char* user) {
    std::string scpCommand = "scp " + localFilePath + " root@" + user + ":" + remoteFilePath;
    // Rest of the function implementation remains the same.

    // Execute the scp command
    int status = std::system(scpCommand.c_str());

    // Check the exit status of the scp command
    if (status == 0)
    {
        std::cout << "File copied successfully to remote server." << std::endl;
        return true;
    }
    else
    {
        std::cerr << "Failed to copy file to remote server." << std::endl;
        return false;
    }
}

bool transferFileFromRemote(const std::string& remoteFilePath, const std::string& localFilePath, const char* user)
{
    std::string scpCommand = "scp root@" + std::string(user) + ":" + remoteFilePath + " " + localFilePath;
    // Rest of the function implementation remains the same.

    // Execute the scp command
    int status = std::system(scpCommand.c_str());

    // Check the exit status of the scp command
    if (status == 0)
    {
        std::cout << "Files copied successfully from remote server." << std::endl;
        return true;
    }
    else
    {
        std::cerr << "Failed to copy files from remote server." << std::endl;
        return false;
    }
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
        std::cout << "Chunk file created: " << filename << std::endl;
    } else {
        std::cerr << "Failed to create chunk file: " << filename << std::endl;
    }
}

void ac_downloader(std::string src_ip, std::string src_port, std::string dest_node, std::string timeout, std::string remote_file ,std::string local_file)
{
    std::string filepathack = "/home/ctm/Documents/unet-3.4.0/scripts/bundleac.bin";
    std::string command = "python3 download_file.py " + src_ip + " " + src_port + " " + dest_node + " " + timeout + " " + remote_file + " " + local_file;
    int result = ::system(command.c_str());
    if (result != 0)
    {
        std::cout << "Error sending through ac" << std::endl;
    }
}

void ac_receiver(std::string src_ip, std::string src_port, std::string dest_node, int timeoutSeconds, std::string remote_filepath ,std::string local_filepath){
    while(!terminateFlag){
        std::string command = "python3 download_file.py " + src_ip + " " + src_port + " 10000 " + remote_filepath + " " + local_filepath;
        std::cout << command << std::endl;
        std::ifstream file(local_filepath);
            std::cout << "File is good" << std::endl;
            auto result = removeHeaderFlags(local_filepath);
            int sequenceNumber = std::get<0>(result);
            bool isLastChunk = std::get<1>(result);
	    std::cout << " Ackmap: "<<(ackMap.find(sequenceNumber) == ackMap.end() && ackMap[sequenceNumber] != true) << std::endl;
	    if(ackMap.find(sequenceNumber) == ackMap.end() && ackMap[sequenceNumber] != true){
		    ackMap[sequenceNumber] = true;
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
		            std::lock_guard<std::mutex> lock(bundleMapMutex);
		            lastbundlefound = true;
		            lastsequencenumber = sequenceNumber;
		            // std::cout << "Last sequence number " << lastsequencenumber << std::endl;
		        }

		    }

		    if (sequenceNumber == nextExpectedBundle) {
		        writeToFile(sequenceNumber, bundle,"ac");
		    	bundleMap[sequenceNumber] = bundle;
		    } else {
		        {
		            std::lock_guard<std::mutex> lock(bundleMapMutex);// Store the bundle in the map or update existing entry
		            bundleMap[sequenceNumber] = bundle;
		        } // The lock is automatically released when lock goes out of scope
		    }
		    
		    std::string filenameac = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/ack.txt";
		    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filenameac);

		    std::string filenameack = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleack.bin";
		    createChunkFile(sequenceNumber, ref, filenameack, false ,true); //overhead of 5 bytes

		    std::string command = "python3 upload_file.py " + src_ip+ " " + src_port + " " + std::to_string(timeoutSeconds) + " " + filenameack + " /home/unet/scripts/bundleack.bin";
		    std::cout << command << std::endl;
		    int result1 = ::system(command.c_str());
		    if (result1 != 0){
		        std::cout << "Error uploading" << std::endl;
		    } else {
		        command = "python3 ac_sender.py " + src_ip + " " + src_port + " " + dest_node + " " + std::to_string(timeoutSeconds) + " bundleack.bin";
		        std::cout << command << std::endl;
		        result1 = ::system(command.c_str()); 		
		        if (result1 != 0){
		            std::cout << "Error sending through ac"<< std::endl;
		        }
		}
        }
    }
}

void sendBundle(ssh_session session, const std::string localFilePath, const std::string remoteFilePath, const std::string destination, const char* user,int n) {
    bool transferSuccess = transferFileToRemote(localFilePath, remoteFilePath,user);
    if (!transferSuccess) {
        std::cerr << "Error transferring file: " << localFilePath << std::endl;
        return;
    }

    std::string command = "dtnsend " + destination + " " + remoteFilePath;
    std::cout << user << ": " << command << std::endl;

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
    std::string ack_path = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleAck.bin";
    std::string remote_ack_path = "/root/ibrdtn-repo/ibrdtn/tools/src/Sender/bundleACK.bin";
    
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
            if (!transferFileFromRemote(remoteFilePath, localFilePath,user)) {
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
            std::cout << user << ": Bundle: " << bundle.sequencenumber.toString().c_str() << " FLAG: "<<bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) << std::endl;
            int sequencenumber = std::stoi(id.sequencenumber.toString());
            if(bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) == true){
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    lastbundlefound = true;
                    lastsequencenumber = sequencenumber;
                    std::cout << "Last sequence number " << lastsequencenumber << std::endl;
                }
  
            }
            {
                std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                std::cout << "Map size: " << bundleMap.size() << " Condition: "<< (lastbundlefound && bundleMap.size() == lastsequencenumber || lastsequencenumber == 0) << std::endl;
                if(lastbundlefound && bundleMap.size() == lastsequencenumber || lastsequencenumber == 0){
                    now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    //uint64_t seconds = now / 1000;
                    // std::cout << timeSinceEpochMillisec() << std::endl;

                    std::cout <<user <<": Time: "<< now << std::endl;
                    // std::cout << seconds << std::endl;
                    // std::cout << "Last received bundle: " << bundle.sequencenumber.toString().c_str() << " FLAG: "<<bundle.get(dtn::data::PrimaryBlock::LAST_BUNDLE) << " VM: " << user << std::endl;

                }

            }
            // std::cout << user << ": Received bundle: " << sequencenumber << std::endl;
            if (sequencenumber == nextExpectedBundle) {
                writeToFile(sequencenumber, bundle,user);
            	bundleMap[sequencenumber] = bundle;
            } else {
                // Store the bundle in the map or update existing entry
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    bundleMap[sequencenumber] = bundle;
                } 
            }
            std:: cout << std::atoi(bundle.sequencenumber.toString().c_str()) << std::endl;
            dtn::data::Bundle ack = processACKBundle(ack_path,bundle.destination,bundle.source,sequencenumber);
            // std::__cxx11::string source = bundle.source.getString() + "/ackRecv";
            sendBundle(session, ack_path, remote_ack_path, ackdestination, user ,sequencenumber );
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
    //const char *host1 = "192.168.0.102";
    //const char *user1 = "root";
    //const char *port1 = "22";
    //const char *host2 = "192.168.0.105";
    //const char *user2 = "root";
    //const char *port2 = "22";
    const char* host1 = "192.168.0.103";
    const char* user1 = "root";
    const char* port1 = "22";
    const char* host2 = "192.168.0.106";
    const char* user2 = "root";
    const char* port2 = "22";

    std::string file_destination1 = "dtn://A/ackRecv";
    std::string file_destination2 = "dtn://B/ackRecv";
    // std::string file_destination1 = "dtn://D/ackRecv";
    // std::string file_destination2 = "dtn://C/ackRecv";

    //RF options
    const char* remoteFilePath = "/root/ibrdtn-repo/ibrdtn/tools/src/Receiver/bundle.bin";
    const char* localFilePath1 = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Receiver/bundle1.bin";
    const char* localFilePath2 = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Receiver/bundle2.bin";
    

    // AC options
    std::string src_ip = "192.168.0.73";
    std::string src_port = "1100";
    std::string dest_node = "128";
    int timeout_ac = 1000;
    std::string remote_filepath = "/home/unet/scripts/bundleac.bin";
    std::string local_filepath = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Receiver/bundleac.bin";


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

    rc = ssh_channel_request_exec(channel1, "dtnd -i wlan1");
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

    rc = ssh_channel_request_exec(channel2, "dtnd -i wlan5");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

	file.open(filename.c_str(), std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
	file.exceptions(std::ios::badbit | std::ios::eofbit);

	
    std::cout << "Wait for incoming file..." << std::endl;

    std::thread receiverThread2(receiver,session2 ,host2,localFilePath2,remoteFilePath,file_destination2);
    std::thread receiverThread1(receiver,session1 ,host1,localFilePath1,remoteFilePath,file_destination1);
    //std::thread receiverThread3(ac_receiver,src_ip,src_port, dest_node, timeout_ac, remote_filepath, local_filepath);

    receiverThread2.join();
    receiverThread1.join();
    //receiverThread3.join();

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
