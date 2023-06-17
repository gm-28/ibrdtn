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

bool ackBundle = false;
bool ackBundle1 = false;
bool ackBundle2 = false;
bool lastbundle = false;

// size_t offset = 0;
std::mutex bundleMapMutex;
std::mutex stopMutex;
std::map<int, int> offsetMap;
std::map<int, int> bundleSize;
std::map<int, bool> ackMap;
int datasent = 0;

uint64_t timeSinceEpochMillisec() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct WirelessInfo {
    std::string signalLevel;
    std::string noiseLevel;
    std::string bitRate;
    double snr;
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

std::string getNoiseLevel(int frequency) {
    std::ostringstream command;
    command << "iw dev wlp59s0 survey dump";

    std::string result;
    char buffer[128];
    FILE* stream = popen(command.str().c_str(), "r");
    if (stream) {
        while (fgets(buffer, sizeof(buffer), stream) != nullptr) {
            result += buffer;
        }
        pclose(stream);
    } else {
        std::cerr << "Error executing command." << std::endl;
        return "";
    }

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

WirelessInfo getWirelessInfo(const std::string& interface, int channel) {
    std::ostringstream command;
    command << "iwconfig " << interface;
    
    std::string result;
    char buffer[128];
    FILE* stream = popen(command.str().c_str(), "r");
    if (stream) {
        while (fgets(buffer, sizeof(buffer), stream) != nullptr) {
            result += buffer;
        }
        pclose(stream);
    } else {
        std::cerr << "Error executing command." << std::endl;
        return {};
    }
    
    if (result.empty()) {
        std::cerr << "No wireless information found for interface " << interface << "." << std::endl;
        return {};
    }
    
    WirelessInfo wirelessInfo;
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
        size_t posSignal = line.find("Signal level=");
        if (posSignal != std::string::npos) {
            wirelessInfo.signalLevel = line.substr(posSignal + 13); // 13 is the length of "Signal level="
        }
        
        size_t posBitRate = line.find("Bit Rate=");
        if (posBitRate != std::string::npos) {
            std::string bitRateInfo = line.substr(posBitRate + 9); // 9 is the length of "Bit Rate="
            size_t posTxPower = bitRateInfo.find(" Tx-Power=");
            if (posTxPower != std::string::npos) {
                wirelessInfo.bitRate = bitRateInfo.substr(0, posTxPower);
            } else {
                wirelessInfo.bitRate = bitRateInfo;
            }
        }
    }

    wirelessInfo.noiseLevel = getNoiseLevel(5180);

    wirelessInfo.snr = std::stod(wirelessInfo.signalLevel) - std::stod(wirelessInfo.noiseLevel);

    return wirelessInfo;
}

int convertBitrateToBytes(const std::string& bitrateString)
{
    std::istringstream iss(bitrateString);
    int bitrateValue = 0;
    std::string unit;

    if (iss >> bitrateValue >> unit)
    {
        if (unit == "Mb/s")
        {
            bitrateValue *= 1000000 / 8;  // Convert from Megabits to Bytes
        }
        else if (unit == "kb/s")
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
        std::cout << user << ": " << command << std::endl;
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
                std::cout << user << ": Timeout reached" << std::endl;
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
                    std::cout <<user <<": Ack of " << id.sequencenumber.toString().c_str() <<" received." << std::endl;

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

    receiver(session,user,ackdest,"ibrdtn/ibrdtn/tools/src/Receiver/bundleACK.bin",2,nextExpectedBundle);

    return;
}

ibrcommon::BLOB::Reference getBlobChunk(const ibrcommon::BLOB::Reference& blob, size_t chunkSize,int sequencenumber)
{
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
    std::cout << "Remaining size: " << remainingSize << std::endl;

    size_t chunkLength = std::min(chunkSize, remainingSize);

    if((remainingSize - chunkLength)  <= 0){
        lastbundle = true;
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

    if(lastbundle){
        // std::cout << "LAST BUNDLE "<< nextExpectedBundle << std::endl;
        b.set(dtn::data::PrimaryBlock::LAST_BUNDLE, true);
        std::cout << "Last bundle = " << nextExpectedBundle << std::endl;
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

int findnextsequencenumber(int start_counter){
    int counter = start_counter;
    if(!ackMap.empty()){
        bool keyFound =  (ackMap.find(counter) != ackMap.end());
        while(keyFound){
            std::cout << "Counter " << counter << std::endl;
            counter++;
            keyFound = (ackMap.find(counter) != ackMap.end());
        }
    }
    return counter;
}

int main(int argc, char *argv[])
{
    int rc;  
    int ret = EXIT_SUCCESS;
    WirelessInfo wirelessInfo1;
    WirelessInfo wirelessInfo2;

    int rf_low_th_24ghz;
    int rf_high_th_24ghz;
    int rf_low_th_5ghz;
    int rf_high_th_5ghz; 
   
    const char* host1 = "localhost";
    const char* user1 = "moreira1";
    const char* port1 = "2222";
    const char* host2 = "localhost";
    const char* user2 = "moreira2";
    const char* port2 = "2223";  

    std::string file_destination1 = "dtn://moreira2-VirtualBox/dtnRecv";
    std::string file_destination2 = "dtn://moreira1-VirtualBox/dtnRecv";
	std::string filename;
    
    const std::string localFilePath1 = "Sender/bundle1.bin";
    const std::string localFilePath2 = "Sender/bundle2.bin";
    const std::string remoteFilePath = "ibrdtn/ibrdtn/tools/src/Sender/bundle.bin";

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
    // remainingSize = ref.size();
    std::vector<ibrcommon::BLOB::Reference> blob_vec;

    wirelessInfo1 = getWirelessInfo("wlp59s0",5180);
    std::cout <<  std::endl;
    std::cout << "---WIRELESS INFO---" << std::endl;
    std::cout << "Signal Level: " << wirelessInfo1.signalLevel << std::endl;
    std::cout << "Noise Level: " << wirelessInfo1.noiseLevel << std::endl;
    std::cout << "Bit Rate: " << wirelessInfo1.bitRate << std::endl;
    std::cout << "SNR: " << wirelessInfo1.snr << std::endl;
    std::cout << "Bit rate: " << convertBitrateToBytes(wirelessInfo1.bitRate) << " Bps"<< std::endl;
    std::cout << "---WIRELESS INFO---" << std::endl << std::endl;


    EID addr_source = EID("dtn://moreira-XPS-15-9570");
    EID addr_dest = EID("dtn://moreira-XPS-15-9570");

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 2);
    uint64_t now;
    dtn::data::Bundle b1;
    dtn::data::Bundle b2;
    int it =0; 
    while(datasent < ref.size()){
        std::cout << "Data sent =  " << datasent << std::endl;
        std::cout << "Iteration: " << it << std::endl;
        it++;
        nextExpectedBundle = findnextsequencenumber(0);
        if(nextExpectedBundle == 0){
            offsetMap[nextExpectedBundle] = 0;
        }
        std::cout << "Sequence number = " << nextExpectedBundle << std::endl;
        wirelessInfo1 = getWirelessInfo("wlp59s0",2417);
        wirelessInfo2 = getWirelessInfo("wlp59s0",2417);

        //EXECUTE THE MANAGER ALGORITHIM

        int condition = 4;
        if (condition == 1){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,40000,nextExpectedBundle);

            b1 = processBundle(ref1,localFilePath2,addr_source,addr_dest,nextExpectedBundle);
            if(std::atoi(b1.sequencenumber.toString().c_str())==0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            // bundleList.push_back(b);

            sendBundle(session1,localFilePath2,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira2-VirtualBox/dtnRecv","moreira1","Receiver/bundleack1.bin",nextExpectedBundle);
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

            char command1[256];
            snprintf(command1, sizeof(command1), "rm -f %s", localFilePath2.c_str());
            system(command1);
        }else if (condition == 2){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,40000,nextExpectedBundle);

            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            if(std::atoi(b1.sequencenumber.toString().c_str())==0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            // bundleList.push_back(b);

            sendBundle(session2,localFilePath1,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira1-VirtualBox/dtnRecv","moreira2","Receiver/bundleack2.bin",nextExpectedBundle);
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

            char command1[256];
            snprintf(command1, sizeof(command1), "rm -f %s", localFilePath1.c_str());
            system(command1);
        }else if(condition == 3){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,40000,nextExpectedBundle);

            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            b2 = processBundle(ref1,localFilePath2,addr_source,addr_dest,nextExpectedBundle);

            //sender threads
            std::thread senderThread2(sendBundle,session2,localFilePath1,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira1-VirtualBox/dtnRecv","moreira2","Receiver/bundleack1.bin",nextExpectedBundle);
            std::thread senderThread1(sendBundle,session1,localFilePath2,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira2-VirtualBox/dtnRecv","moreira1","Receiver/bundleack1.bin",nextExpectedBundle);

            senderThread2.join();
            senderThread1.join();

            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    // remainingSize -= static_cast<int>(b1.getPayloadLength());
                }
            }

            char command3[256];
            snprintf(command3, sizeof(command3), "rm -f %s", localFilePath1.c_str());
            system(command3);

        }else if(condition == 4){
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,40000,nextExpectedBundle);
            int temp = nextExpectedBundle;
            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            
            nextExpectedBundle = findnextsequencenumber(nextExpectedBundle+1);
            std::cout << "Sequence of 2nd number = " << nextExpectedBundle << std::endl;

            //mudar o tamanho na condição
            if(ref1.size() >= 40000 ){
                ibrcommon::BLOB::Reference ref2 = getBlobChunk(ref,40000,nextExpectedBundle);
                b2 = processBundle(ref2,localFilePath2,addr_source,addr_dest,nextExpectedBundle);
            }
            
            now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            // uint64_t seconds = now / 1000;
            
            //sender threads
            if(ref1.size() >= 40000 ){
                std::thread senderThread1(sendBundle,session1,localFilePath2,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira2-VirtualBox/dtnRecv","moreira1","Receiver/bundleack1.bin",nextExpectedBundle);
                std::thread senderThread2(sendBundle,session2,localFilePath1,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira1-VirtualBox/dtnRecv","moreira2","Receiver/bundleack2.bin",temp);
                senderThread1.join();
                senderThread2.join();

            }else{
                std::thread senderThread2(sendBundle,session2,localFilePath1,"ibrdtn/ibrdtn/tools/src/Sender/bundle.bin", "dtn://moreira1-VirtualBox/dtnRecv","moreira2","Receiver/bundleack1.bin",nextExpectedBundle);
                senderThread2.join();
            }
            char command[256];
            snprintf(command, sizeof(command), "rm -f %s", localFilePath1.c_str());
            system(command);
            char command2[256];
            snprintf(command2, sizeof(command2), "rm -f %s", localFilePath2.c_str());
            system(command2);
            
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b1.getPayloadLength());
                    std::cout << "ACK " << std::atoi(b1.sequencenumber.toString().c_str()) << std::endl;
                }
            }
            if(!ackMap.empty()){
                if((ackMap.find(std::atoi(b2.sequencenumber.toString().c_str())) != ackMap.end())){
                    datasent+=static_cast<int>(b2.getPayloadLength());
                    std::cout << "ACK " << std::atoi(b2.sequencenumber.toString().c_str()) << std::endl;
                }
            }
        }else if(condition == 5){
            std::cout << "Condition AC" << std::endl;
            int ac_bitrate;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref,ac_bitrate,nextExpectedBundle);

            b1 = processBundle(ref1,localFilePath1,addr_source,addr_dest,nextExpectedBundle);
            
            //sendbundle with ac!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
            if(!ackMap.empty() && (ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())){
                datasent+=static_cast<int>(b1.getPayloadLength());
                // remainingSize -= static_cast<int>(b1.getPayloadLength());
            }

            char command2[256];
            snprintf(command2, sizeof(command2), "rm -f %s", localFilePath1.c_str());
            system(command2);
        }else if(condition == 6){
                std::cout << "Condition AC + RF1 SAME BUNDLES" << std::endl;
        }else if(condition == 7){
                std::cout << "Condition AC + RF2 SAME BUNDLES" << std::endl;
        }else if(condition == 8){
                std::cout << "Condition AC + RF1 DIFFERENT BUNDLES" << std::endl;
        }else if(condition == 9){
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
    std::cout << "Transmition time(miliseconds) of the first bundle: " << now << std::endl;
    std::cout << filename <<" has been sent" << std::endl;

    return 0;
}