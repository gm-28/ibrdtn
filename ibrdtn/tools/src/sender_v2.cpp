//sender.cpp 

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
#include <regex>
#include <thread>
#include <mutex>
#include <future>

#include <chrono>
#include <ctime>

using namespace std;

#define OFF 0
#define HANDOVER 1
#define ACTIVE 2

bool ackBundle = false;
bool ackBundle1 = false;
bool ackBundle2 = false;
bool lastbundle = false;
int lastbundle_sequence = -1;
int datasent = 0;

double rf_low_th_24ghz = 35;
double rf_high_th_24ghz = 40;
double rf_low_th_5ghz = 40;
double rf_high_th_5ghz = 43; 

int channelsize1 = 0;
int channelsize2 = 0;
int channelsizeAC = 0;

// size_t offset = 0;
std::mutex bundleMapMutex;
std::mutex stopMutex;
std::map<int, int> offsetMap;
std::map<int, int> bundleSize;
std::map<int, bool> ackMap;
int ac_time = 1;

uint64_t timeSinceEpochMillisec() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct WirelessInfo{
    std::string signalLevel;
    std::string noiseLevel;
    std::string bitRate;
    double snr;
    double timeout;
    int state;
};

std::string trim(const std::string& str) {
    std::string trimmed = str;
    // Remove leading whitespace
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(),
        [](int ch) { return !std::isspace(ch); }));
    // Remove trailing whitespace
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(),
        [](int ch) { return !std::isspace(ch); }).base(), trimmed.end());
    return trimmed;
}

#include <libssh/libssh.h>
#include <iostream>
#include <sstream>
#include <string>


std::string executeRemoteCommand(ssh_session session, const std::string& command) {
    std::string result;

    ssh_channel channel = ssh_channel_new(session);
    if (channel == nullptr) {
        std::cerr << "Failed to create SSH channel." << std::endl;
        return result;
    }

    int rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK) {
        std::cerr << "Failed to open SSH channel session: " << ssh_get_error(session) << std::endl;
        ssh_channel_free(channel);
        return result;
    }

    rc = ssh_channel_request_exec(channel, command.c_str());
    if (rc != SSH_OK) {
        std::cerr << "Failed to execute remote command: " << ssh_get_error(session) << std::endl;
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return result;
    }

    char buffer[128];
    int nbytes;
    while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
        result.append(buffer, nbytes);
    }

    if (nbytes < 0) {
        std::cerr << "Error reading remote command output: " << ssh_get_error(session) << std::endl;
    }

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    return result;
}

std::string getNoiseLevel(ssh_session session, const std::string& interface, int frequency) {
    std::ostringstream command;
    command << "iw dev " << interface << " survey dump";

    std::string result = executeRemoteCommand(session, command.str());

    std::istringstream iss(result);
    std::string line;
    std::string noiseLevel;
    bool foundFrequency = false;
    while (std::getline(iss, line)) {
        if (line.find("frequency:\t\t") != std::string::npos) {
            std::string freq = line.substr(line.find(":") + 1);
            if (std::stoi(freq) == frequency) {
                foundFrequency = true;
            }
        }
        if (foundFrequency && line.find("noise:\t\t\t") != std::string::npos) {
            noiseLevel = line.substr(line.find(":") + 1);
            noiseLevel = trim(noiseLevel);
            break;
        }
    }

    if (noiseLevel.empty()) {
        std::cerr << "No noise level data found for frequency " << frequency << " MHz." << std::endl;
        return "";
    }

    return noiseLevel;
}

WirelessInfo getWirelessInfo(ssh_session session, const std::string& interface, int channel) {
    std::ostringstream command;
    command << "iw dev " << interface << " station dump";  // Updated command

    std::string result = executeRemoteCommand(session, command.str());

    if (result.empty()) {
        std::cerr << "No wireless information found for interface " << interface << "." << std::endl;
        return {};
    }

    WirelessInfo wirelessInfo;
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
        // Process the output of the "iw dev wlan0 station dump" command here
        // Extract the desired information from each line and update the wirelessInfo object accordingly
        // Example code to extract signal level and bit rate:

        size_t posSignal = line.find("signal: ");
        if (posSignal != std::string::npos) {
            wirelessInfo.signalLevel = line.substr(posSignal + 8); // 8 is the length of "signal: "
        }

        size_t posBitRate = line.find("tx bitrate:");
        if (posBitRate != std::string::npos) {
            wirelessInfo.bitRate = line.substr(posBitRate + 12); // 12 is the length of "tx bitrate:"
        }
    }

    wirelessInfo.noiseLevel = getNoiseLevel(session, interface, channel);

    wirelessInfo.snr = std::stod(wirelessInfo.signalLevel) - std::stod(wirelessInfo.noiseLevel);

    return wirelessInfo;
}


int convertBitrateToBytes(const std::string& bitrateString)
{
    std::istringstream iss(bitrateString);
    double bitrateValue = 0.0;
    std::string unit;

    if (iss >> bitrateValue >> unit)
    {
        if (unit == "Mb/s" || unit == "MBit/s")
        {
            bitrateValue *= 1000000 / 8;  // Convert from Megabits to Bytes
        }
        else if (unit == "kb/s" || unit == "kBit/s" || unit == "KBit/s")
        {
            bitrateValue *= 1000 / 8;  // Convert from Kilobits to Bytes
        }
        // Add more conversions for other units if needed

        return bitrateValue;
    }

    // Handle invalid bitrate string
    throw std::invalid_argument("Invalid bitrate string: " + bitrateString);
}

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
        std::cerr << "Error opening local file: " << localFilePath.c_str() << std::endl;
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
        std::cerr << user << "Error creating SFTP session: " << ssh_get_error(session) << std::endl;
        delete[] buffer;
        return false;
    }

    // Initialize the SFTP session
    int rc = sftp_init(sftp);
    if (rc != SSH_OK) {
        std::cerr << user << ": Error opening remote file: " <<  remoteFilePath.c_str() << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        delete[] buffer;
        return false;
    }

    // Open a file on the remote server for writing
    sftp_file remoteFile = sftp_open(sftp, remoteFilePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (remoteFile == nullptr) {
        std::cerr << user << ": Error opening remote file: " <<  remoteFilePath.c_str() << ssh_get_error(session) << std::endl;
        sftp_free(sftp);
        delete[] buffer;
        return false;
    }

    // Write the file data to the remote file
    // std::cout << "Size of the file/bundle being sent: " << fileSize << std::endl;
    rc = sftp_write(remoteFile, buffer, fileSize);
    if (rc < 0) {
        std::cerr << user << ": Error writing to remote file: " <<  remoteFilePath.c_str() << ssh_get_error(session) << std::endl;
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

bool transferFileFromRemote(ssh_session session, const std::string& remoteFilePath, const std::string& localFilePath,const char* user) {
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
        std::cerr << "Error opening remote file: " << ssh_get_error(session) <<" " << remoteFilePath << " on "<< user << std::endl;
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

void receiver(ssh_session session, const char* user, const std::string& localFilePath, const std::string& remoteFilePath,int timeoutSeconds, int nextExpectedBundle) {    
    int rc;
    bool terminateFlag = false;

    while(!terminateFlag){
        // Execute the receiver command in a loop
        // Create a channel for executing the receiver command
        ssh_channel channel = ssh_channel_new(session);
        rc = ssh_channel_open_session(channel);
        if (rc != SSH_OK) {
            fprintf(stderr, "Failed to open SSH channel 1: %s\n", ssh_get_error(session));
            disconnect(channel);
            return;
        }

        std::string command = "dtnrecv --file " + remoteFilePath + " --name ackRecv";
        // std::cout << user << ": " << command << std::endl;
        rc = ssh_channel_request_exec(channel, command.c_str());
        if (rc != SSH_OK) {
            fprintf(stderr, "Failed to execute SSH command: %s\n", ssh_get_error(session));
            disconnect(channel);
            return;
        }

        ssh_channel_set_blocking(channel, 0);
        int exitStatus = ssh_channel_get_exit_status(channel);
        
        auto startTime = std::chrono::steady_clock::now();
        while (exitStatus == -1) {
            exitStatus = ssh_channel_get_exit_status(channel);
            // std::cout << "exit != 0"<< std::endl;
            // Check if the timeout has been reached
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);
            if (elapsedTime >= std::chrono::seconds(timeoutSeconds)) {
                // std::cout << user << ": Timeout reached" << std::endl;
                ssh_channel_set_blocking(channel, 1);
                return;
            }
        }

        ssh_channel_set_blocking(channel, 1);
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
            if((bundle.get(dtn::data::PrimaryBlock::ACK_BUNDLE) == true) && (sequencenumber == nextExpectedBundle)){
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex); // Acquire the lock
                    ackMap[sequencenumber] = true;
                    terminateFlag = true;
                    // std::cout <<user <<": Ack of " << id.sequencenumber.toString().c_str() <<" received." << std::endl;

                }
            }            

            disconnect(channel);
        }
    }
    return;
}

void sendBundle(ssh_session session, const std::string localFilePath, const std::string remoteFilePath, const std::string destination,const char* user,const char* ackdest, int nextExpectedBundle) {
    bool transferSuccess = transferFileToRemote(session, localFilePath, remoteFilePath,user);
    if (!transferSuccess) {
        std::cerr << "Error transferring file: " << localFilePath << std::endl;
        return;
    }

    std::string command =  "dtnsend " + destination + " " + remoteFilePath;
    // std::cout << user << ": "<< command.c_str() <<std::endl;

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

    receiver(session,user,ackdest,"/root/ibrdtn-repo/ibrdtn/tools/src/Receiver/bundleACK.bin",2,nextExpectedBundle);

    return;
}

ibrcommon::BLOB::Reference getBlobChunk(const ibrcommon::BLOB::Reference& blob, size_t chunkSize,int sequencenumber)
{
    bool found =false;
    if(!bundleSize.empty()){
        if((bundleSize.find(sequencenumber) != bundleSize.end())){
            chunkSize = bundleSize[sequencenumber];
            found = true;
        }
    }

    if(sequencenumber == 0){
        offsetMap[sequencenumber] = 0;
    }

    size_t remainingSize = 0 ;
    int current_offset = offsetMap[sequencenumber];

    // Get an iostream for the BLOB
    // Create a new BLOB reference for this chunk
	ibrcommon::BLOB::Reference blobref = blob;
    ibrcommon::BLOB::iostream blobStream = blobref.iostream();

    remainingSize = blobStream.size() - current_offset;
    // std::cout << "Remaining size: " << remainingSize << std::endl;

    size_t chunkLength = std::min(chunkSize, remainingSize);
    // std::cout << "Size of chunk: " << chunkLength << std::endl;
    if((remainingSize - chunkLength)  <= 0){
        lastbundle_sequence = sequencenumber;
    }

    blobStream->seekg(current_offset);

    ibrcommon::BLOB::Reference chunkBlob = ibrcommon::BLOB::create();
    ibrcommon::BLOB::iostream chunkStream = chunkBlob.iostream();   
    ibrcommon::BLOB::copy(*chunkStream, *blobStream, chunkLength);

    // Update the offset of the next bundle with the chunk size
    sequencenumber +=1;
    offsetMap[sequencenumber] = current_offset + static_cast<int>(chunkLength);

    return chunkBlob;
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

static int auth_keyfile(ssh_session session, const char* keyfile) {
    ssh_key key = NULL;
    char pubkey[256] = {0}; // Public key file path
    int rc;

    ::snprintf(pubkey, sizeof(pubkey), "%s.pub", keyfile);

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
        fprintf(stderr, "Error connecting to virtual machine: %s %s\n", host ,ssh_get_error(session));
        ssh_free(session);
        exit(EXIT_FAILURE);
    }

    rc = auth_keyfile(session, keyfile);
    if (rc != SSH_AUTH_SUCCESS) {
        fprintf(stderr, "Error authenticating with virtual machine: %s\n",host);
        ssh_disconnect(session);
        ssh_free(session);
        exit(EXIT_FAILURE);
    }

    return session;
}

dtn::data::Bundle processBundle(ibrcommon::BLOB::Reference ref, const std::string& localFilePath, EID addr_src , EID addr_dest, int nextExpectedBundle) {
    // create a bundle fro the file
    dtn::data::Bundle b;

    b.source = addr_src;

    b.destination = addr_dest;

    // set the lifetime
    b.lifetime = 3600;
    
    // add payload block with the references
    b.push_back(ref);
    
    bundleSize[nextExpectedBundle] = static_cast<int>(ref.size());

    if(nextExpectedBundle == lastbundle_sequence){
        // std::cout << "LAST BUNDLE "<< nextExpectedBundle << std::endl;
        b.set(dtn::data::PrimaryBlock::LAST_BUNDLE, true);
        // std::cout << "Last bundle = " << nextExpectedBundle << std::endl;
    }

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
    std::ofstream outputFile("newbundleac.bin", std::ios::binary);
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

int findnextsequencenumber(int start_counter){
    int counter = start_counter;
    if(!ackMap.empty()){
        bool keyFound =  (ackMap.find(counter) != ackMap.end());
        while(keyFound){
            // std::cout << "Counter " << counter << std::endl;
            counter++;
            keyFound = (ackMap.find(counter) != ackMap.end());
        }
    }
    return counter;
}

int state_update(double snr,double low_th, double high_th ){
    if(snr < low_th){
        return OFF;
    }else if (low_th <= snr && snr < high_th ){
        return HANDOVER;
    }else if (high_th <= snr){
        return ACTIVE;
    }
    return -1;
}

int decision_unit(WirelessInfo wirelessInfo1, WirelessInfo wirelessInfo2,int datasent, std::streamsize fullsize,bool ac_state,int ac_size){
    int ret;
    // std::cout << "SNR: " << wirelessInfo1.snr << std::endl;
    // std::cout << "SNR: " << wirelessInfo2.snr << std::endl;
    wirelessInfo1.state = state_update(wirelessInfo1.snr,rf_low_th_24ghz,rf_high_th_24ghz);
    wirelessInfo2.state = state_update(wirelessInfo2.snr,rf_low_th_5ghz,rf_high_th_5ghz);

    if(ac_state){
        if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == OFF){
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            channelsize1 = 40000;
            ret = 1;
            //return ac + rf1 diff bundles;
        }else if (wirelessInfo1.state == OFF && wirelessInfo2.state == ACTIVE){
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            ret = 2;
            //return ac + rf1 + rf2 same bundles;
        }else if (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == HANDOVER){
            //return ac + rf1 + rf2 same bundles;
        }else if ((wirelessInfo1.state == ACTIVE && wirelessInfo2.state == HANDOVER) || (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == ACTIVE)){
            if(wirelessInfo1.state == ACTIVE){
                //atualizar tamanho
                channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
                channelsize1 = 40000;
            }else if(wirelessInfo2.state == ACTIVE){
                //atualizar tamanho
                channelsize1 = convertBitrateToBytes(wirelessInfo2.bitRate);
                channelsize1 = 40000;
            }

            ret = 3;
            //return ac + rf1 + rf2 same bundles;
        }else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == ACTIVE){
            //atualizar tamanho
            channelsize1 = convertBitrateToBytes(wirelessInfo2.bitRate); //carefully choose this, it depends on the rf card and to each remote machine it is connected
            channelsize2 = convertBitrateToBytes(wirelessInfo1.bitRate);

            channelsize1 = 40000;
            channelsize2 = 20000;
            ret = 4;
            //return ac + rf1 + rf2 DIFF bundles;
        }
    }else {
        if (wirelessInfo1.state == OFF && wirelessInfo2.state == OFF)
        {
            channelsizeAC = ac_size;
            ret = 0;
        }
        else if (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == OFF)
        {
            channelsize1 = ac_size;
            ret = 5; //return ac + rf1 same bundles;
        }
        else if (wirelessInfo1.state == OFF && wirelessInfo2.state == HANDOVER)
        {
            channelsize2 = ac_size;
            ret = 6; //return ac + rf1 same bundles;
        }
        else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == OFF)
        {
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            if((datasent + channelsize2) < fullsize){
                ret = 7; //return ac + rf1 diff bundles;
            }else{
                ret = 1; //return rf1;
            }
        }
        else if (wirelessInfo1.state == OFF && wirelessInfo2.state == ACTIVE)
        {
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            if((datasent + channelsize2) < fullsize){
                ret = 8; //return ac + rf2 diff bundles;
            }else{
                ret = 2; //return rf2;
            }
        }
        else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == HANDOVER)
        {   
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            channelsize2 = channelsize1;
            if((datasent + channelsize1 + channelsize2) < fullsize){
                ret = 9; //return ac + rf1 diff bundles + rf1 + rf2 same bundles;
            }else{
                ret = 3; //return rf1 + rf2 same bundles;
            }
        }
        else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == HANDOVER)
        {   
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            channelsize1 = channelsize2;
            if((datasent + channelsize1 + channelsize2) < fullsize){
                ret = 9; //return ac + rf2 diff bundles + rf1 + rf2 same bundles;
            }else{
                ret = 3; //return rf1 + rf2 same bundles;
            }
        }
        else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == ACTIVE)
        {   
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            if((datasent + channelsize1 + channelsize2) < fullsize){
                ret = 10; //return ac + rf1 + rf2 diff bundles;
            }else{
                ret = 4; //return rf1 + rf2 diff bundles;
            }
        }
    }

    // std::cout << "Condition " << ret << std::endl;
    return ret;
}

void ac_sender(std::string src_ip , std::string src_port , std::string dest_node ,std::string timeout , std::string filenameac ){
    uint64_t end;
    uint64_t start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::string filepathack = "/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin";
    std::string command = "python3 ac_sender.py " + src_ip + " " + src_port + " " + dest_node + " " + timeout + " " + filenameac;
    int result = ::system(command.c_str()); 		
    if (result != 0){
        std::cout << "Error sending through ac"<< std::endl;
    }

    bool ack_received = false;
    while(!ack_received){
        std::ifstream file(filepathack);
        if(file.good()){
            auto result = removeHeaderFlags("/home/moreira/Documents/unet-3.4.0/scripts/bundleack.bin");
            int sequenceNumber1 = std::get<0>(result);
            bool isLastChunk = std::get<1>(result);
            ack_received = std::get<2>(result);
            if(ack_received){
                ackMap[sequenceNumber1] = true;
                end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                ac_time = end - start;
            }
        }
    }
    sleep(1);

    std::string command2 = "rm /home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin";
    result = std::system(command2.c_str());
    if (result == 0) {
        std::cout << "File removed successfully." << std::endl;
    } else {
        std::cout << "Failed to remove file." << std::endl;
    }

}

int main(int argc, char *argv[])
{
    int rc;  
    int ret = EXIT_SUCCESS;
    WirelessInfo wirelessInfo1;
    WirelessInfo wirelessInfo2;
   
    const char* host1 = "192.168.0.102";
    const char* user1 = "root";
    const char* port1 = "22";
    const char* host2 = "192.168.0.105";
    const char* user2 = "root";
    const char* port2 = "22";  

    std::string file_destination1 = "dtn://B/dtnRecv";
    std::string file_destination2 = "dtn://A/dtnRecv";
	std::string filename;
    
    const std::string localFilePath1 = "Sender/bundle1.bin";
    const std::string localFilePath2 = "Sender/bundle2.bin";
    const std::string remoteFilePath = "/root/ibrdtn-repo/ibrdtn/tools/src/Sender/bundle.bin";

    EID addr_source = EID("dtn://moreira-XPS-15-9570");
    EID addr_dest = EID("dtn://moreira-XPS-15-9570");

    //AC options
    std::string src_ip = "192.168.111.101";
    std::string src_port = "1100";
    std::string dest_node = "31";
    std::string timeout = "10000";
    std::string filenameac = "bundleac.bin";
    std::string filepathack = "/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin";

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

    rc = ssh_channel_request_exec(channel1, "dtnd -i wlan0");
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

    rc = ssh_channel_request_exec(channel2, "dtnd -i wlan0");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command on the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }
    bool ac_state = false;
    if(ac_state){
        
    }

    sleep(1);
    
    // wirelessInfo1 = getWirelessInfo(session1,"wlan0",2442);
    // std::cout <<  std::endl;
    // std::cout << "---WIRELESS INFO---" << std::endl;
    // std::cout << "Signal Level: " << wirelessInfo1.signalLevel << std::endl;
    // std::cout << "Noise Level: " << wirelessInfo1.noiseLevel << std::endl;
    // std::cout << "Bit Rate: " << wirelessInfo1.bitRate << std::endl;
    // std::cout << "SNR: " << wirelessInfo1.snr << std::endl;
    // std::cout << "Bit rate: " << convertBitrateToBytes(wirelessInfo1.bitRate) << " Bps"<< std::endl;
    // std::cout << "---WIRELESS INFO---" << std::endl << std::endl;

    uint64_t now;
    dtn::data::Bundle b1;
    dtn::data::Bundle b2;
    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);
    std::cout << filename <<" is being sent" << std::endl;
    std::cout << "......." << std::endl;

    int it =0;
    while(datasent < ref.size()){
        nextExpectedBundle = findnextsequencenumber(0);
        if(nextExpectedBundle == 0){
            offsetMap[nextExpectedBundle] = 0;
        }
        it++;
        // std::cout << "Sequence number = " << nextExpectedBundle << std::endl;
        wirelessInfo1 = getWirelessInfo(session1,"wlan0",2442);
        wirelessInfo2 = getWirelessInfo(session2,"wlan0",2442);

        int condition = decision_unit(wirelessInfo1,wirelessInfo2,datasent,ref.size(),false,50);
        condition = 1;

        if (condition == 0){
            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,50,nextExpectedBundle);
            if(lastbundle_sequence == nextExpectedBundle){
                // std::cout << " Last " << lastbundle_sequence << std::endl;
                last = true;
            }
            createChunkFile(nextExpectedBundle, ref1, "/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin", last ,false); //overhead of 4 bytes because of int
            
            ac_sender(src_ip,src_port,dest_node,timeout,filenameac);

            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }
        }else if (condition == 1){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,channelsize1,nextExpectedBundle);
            // std::cout <<host1 << ": Size: " << channelsize1 << std::endl;
            b1 = processBundle(ref1,localFilePath2,addr_source,addr_dest,nextExpectedBundle);
            if(std::atoi(b1.sequencenumber.toString().c_str())==0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            sendBundle(session1,localFilePath2,remoteFilePath, "dtn://B/dtnRecv","192.168.0.102","Receiver/bundleack1.bin",nextExpectedBundle);
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

            char command1[256];
            ::snprintf(command1, sizeof(command1), "rm -f %s", localFilePath2.c_str());
            ::system(command1);
        }else if (condition == 2){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,channelsize2,nextExpectedBundle);

            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            if(std::atoi(b1.sequencenumber.toString().c_str())==0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            sendBundle(session2,localFilePath1,remoteFilePath,"dtn://A/dtnRecv","192.168.0.105","Receiver/bundleack2.bin",nextExpectedBundle);
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

            char command1[256];
            ::snprintf(command1, sizeof(command1), "rm -f %s", localFilePath1.c_str());
            ::system(command1);
        }else if(condition == 3){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,channelsize1,nextExpectedBundle);

            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            b2 = processBundle(ref1,localFilePath2,addr_source,addr_dest,nextExpectedBundle);

            //sender threads
            std::thread senderThread2(sendBundle,session2,localFilePath1,remoteFilePath, "dtn://A/dtnRecv","192.168.0.105","Receiver/bundleack2.bin",nextExpectedBundle);
            std::thread senderThread1(sendBundle,session1,localFilePath2,remoteFilePath, "dtn://B/dtnRecv","192.168.0.102","Receiver/bundleack1.bin",nextExpectedBundle);

            senderThread2.join();
            senderThread1.join();

            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

            char command3[256];
            ::snprintf(command3, sizeof(command3), "rm -f %s", localFilePath1.c_str());
            ::system(command3);

        }else if(condition == 4){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,70268,nextExpectedBundle);
            int temp = nextExpectedBundle;
            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            
            nextExpectedBundle = findnextsequencenumber(nextExpectedBundle+1);
            // std::cout << "Sequence of 2nd number = " << nextExpectedBundle << std::endl;

            //mudar o tamanho na condição
            if(ref1.size() >= 70268 ){
                ibrcommon::BLOB::Reference ref2 = getBlobChunk(ref,70269,nextExpectedBundle);
                b2 = processBundle(ref2,localFilePath2,addr_source,addr_dest,nextExpectedBundle);
            }
            
            now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            // uint64_t seconds = now / 1000;
            
            //sender threads
            if(ref1.size() >= 70268 ){
                std::thread senderThread2(sendBundle,session2,localFilePath1,remoteFilePath, "dtn://A/dtnRecv","192.168.0.105","Receiver/bundleack2.bin",temp);
                std::thread senderThread1(sendBundle,session1,localFilePath2,remoteFilePath, "dtn://B/dtnRecv","192.168.0.102","Receiver/bundleack1.bin",nextExpectedBundle);

                senderThread1.join();
                senderThread2.join();

            }else{
                std::thread senderThread2(sendBundle,session2,localFilePath1,remoteFilePath, "dtn://A/dtnRecv","192.168.0.105","Receiver/bundleack2.bin",temp);
                senderThread2.join();
            }

            char command[256];
            ::snprintf(command, sizeof(command), "rm -f %s", localFilePath1.c_str());
            ::system(command);
            char command2[256];
            ::snprintf(command2, sizeof(command2), "rm -f %s", localFilePath2.c_str());
            ::system(command2);
            
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // std::cout << "ACK " << std::atoi(b1.sequencenumber.toString().c_str()) << std::endl;
                }
            }
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b2.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b2.getPayloadLength());
                    // std::cout << "ACK " << std::atoi(b2.sequencenumber.toString().c_str()) << std::endl;
                }
            }
        }else if(condition == 5){
            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,50,nextExpectedBundle);
            if(lastbundle_sequence == nextExpectedBundle){
                // std::cout << " Last " << lastbundle_sequence << std::endl;
                last = true;
            }
            createChunkFile(nextExpectedBundle, ref1, "/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin", last ,false); //overhead of 4 bytes because of int
            
            ac_sender(src_ip,src_port,dest_node,timeout,filenameac);

            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }
        }else if(condition == 6){
            std::cout << "Condition AC + RF1 SAME BUNDLES" << std::endl;
            
            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,50,nextExpectedBundle);
            if(lastbundle_sequence == nextExpectedBundle){
                last = true;
            }
            createChunkFile(nextExpectedBundle, ref1, "/home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin", last ,false); //overhead of 4 bytes because of int
            
            std::string command = "python3 ac_sender.py " + src_ip + " " + src_port + " " + dest_node + " " + timeout + " " + filenameac;
            int result = ::system(command.c_str()); 		
            if (result != 0){
                std::cout << "Error sending through ac"<< std::endl;
            }
            // std::string command2 = "rm /home/moreira/Documents/unet-3.4.0/scripts/bundleac.bin";
            // result = std::system(command2.c_str());
            // if (result == 0) {
            //     std::cout << "File removed successfully." << std::endl;
            // } else {
            //     std::cout << "Failed to remove file." << std::endl;
            // }
            bool ack_received = false;
            while(!ack_received){
                std::ifstream file(filepathack);
                if(file.good()){
                    auto result = removeHeaderFlags("/home/moreira/Documents/unet-3.4.0/scripts/bundleack.bin");
                    int sequenceNumber1 = std::get<0>(result);
                    bool isLastChunk = std::get<1>(result);
                    ack_received = std::get<2>(result);
                    if(ack_received){
                        ackMap[sequenceNumber1] = true;
                    }
                }
            }
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

        }else if(condition == 7){
                std::cout << "Condition AC + RF2 SAME BUNDLES" << std::endl;
        }else if(condition == 8){
                std::cout << "Condition AC + RF1 DIFFERENT BUNDLES" << std::endl;
        }else if(condition == 9){
                std::cout << "Condition AC + RF2 DIFFERENT BUNDLES" << std::endl;
        }else if(condition == 10){
                std::cout << "Condition AC + RF2 DIFFERENT BUNDLES" << std::endl;
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
    std::cout << "Transmition time(miliseconds) of the first bundle: " << now << std::endl;

    return 0;
}