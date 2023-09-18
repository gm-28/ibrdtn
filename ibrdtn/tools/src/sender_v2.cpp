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

double rf_low_th_24ghz = 4;
double rf_high_th_24ghz = 15;
double rf_low_th_5ghz = 4;
double rf_high_th_5ghz = 25;

int channelsize1 = 0;
int channelsize2 = 0;
int channelsizeAC = 0;
int contador1 = 0;
int contador2 = 0;
int average_rate1 = 0;
int average_rate2 = 0;

bool no_rf1 = false;
bool no_rf2 = false;
// size_t offset = 0;
std::mutex bundleMapMutex;
std::mutex stopMutex;
std::map<int, int> offsetMap;
std::map<int, int> bundleSize;
std::map<int, bool> ackMap;
std::map<int, bool> acMap;
bool retransmision = false;
int ac_time = 1;
int temp_number = -1;
bool tempackvalue = false;

struct WirelessInfo
{
    std::string signalLevel;
    std::string noiseLevel;
    std::string bitRate;
    double snr;
    double timeout;
    int state;
};

void print_help()
{
    std::cout << "-- Sender --" << std::endl
              << "Required parameters: " << std::endl
              << " --file <path>    The file to transfer" << std::endl
              << std::endl
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

uint64_t timeSinceEpochMillisec(){
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string &str){
    std::string trimmed = str;
    
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(),
                                                [](int ch)
                                                { return !std::isspace(ch); }));

    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(),
                               [](int ch)
                               { return !std::isspace(ch); })
                      .base(),
                  trimmed.end());
    return trimmed;
}

std::string executeRemoteCommand(ssh_session session, const std::string &command)
{
    std::string result;

    ssh_channel channel = ssh_channel_new(session);
    if (channel == nullptr)
    {
        std::cerr << "Failed to create SSH channel." << std::endl;
        return result;
    }

    int rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK)
    {
        std::cerr << "Failed to open SSH channel session: " << ssh_get_error(session) << std::endl;
        ssh_channel_free(channel);
        return result;
    }

    rc = ssh_channel_request_exec(channel, command.c_str());
    if (rc != SSH_OK)
    {
        std::cerr << "Failed to execute remote command: " << ssh_get_error(session) << std::endl;
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return result;
    }

    char buffer[128];
    int nbytes;
    while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0)
    {
        result.append(buffer, nbytes);
    }

    if (nbytes < 0)
    {
        std::cerr << "Error reading remote command output: " << ssh_get_error(session) << std::endl;
    }

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    return result;
}

std::string getNoiseLevel(ssh_session session, const std::string &interface, int frequency)
{
    std::ostringstream command;
    command << "iw dev " << interface << " survey dump";

    std::string result = executeRemoteCommand(session, command.str());

    std::istringstream iss(result);
    std::string line;
    std::string noiseLevel;
    bool foundFrequency = false;
    while (std::getline(iss, line))
    {
        if (line.find("frequency:\t\t") != std::string::npos)
        {
            std::string freq = line.substr(line.find(":") + 1);
            if (std::stoi(freq) == frequency)
            {
                foundFrequency = true;
            }
        }
        if (foundFrequency && line.find("noise:\t\t\t") != std::string::npos)
        {
            noiseLevel = line.substr(line.find(":") + 1);
            noiseLevel = trim(noiseLevel);
            break;
        }
    }

    if (noiseLevel.empty())
    {
        std::cerr << "No noise level data found for frequency " << frequency << " MHz." << std::endl;
        return " 1000 dBm";
    }

    return noiseLevel;
}

WirelessInfo getWirelessInfo(ssh_session session, const std::string &interface, int channel){
    WirelessInfo wirelessInfo;
    
    std::ostringstream command;
    command << "iw dev " << interface << " station dump";

    std::string result = executeRemoteCommand(session, command.str());
    //std::cout << result << "empty: "<< result.empty() <<std::endl;
    if (result.empty()){
        //std::cout << "No wireless information found for interface " << interface << " "<< channel <<"." << std::endl;
        wirelessInfo.snr = 0;
        if(channel == 2442){
            no_rf1 = true;
        }
        if(channel == 5180){
            no_rf2 = true; 
        }
        return wirelessInfo;
    }


    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)){
        size_t posSignal = line.find("signal: ");
        if (posSignal != std::string::npos){
            wirelessInfo.signalLevel = line.substr(posSignal + 8); // 8 is the length of "signal: "
        }

        size_t posBitRate = line.find("tx bitrate:");
        if (posBitRate != std::string::npos){
            wirelessInfo.bitRate = line.substr(posBitRate + 12); // 12 is the length of "tx bitrate:"
        }
    }

    wirelessInfo.noiseLevel = getNoiseLevel(session, interface, channel);
    wirelessInfo.snr = std::stod(wirelessInfo.signalLevel) - std::stod(wirelessInfo.noiseLevel);

    if(channel == 2442){
        no_rf1 = false;
    }
    if(channel == 5180){
        no_rf2 = false;
    }
    return wirelessInfo;
}

int convertBitrateToBytes(const std::string &bitrateString){
    std::istringstream iss(bitrateString);
    double bitrateValue = 0.0;
    std::string unit;

    if (iss >> bitrateValue >> unit)
    {
        if (unit == "Mb/s" || unit == "MBit/s")
        {
            bitrateValue *= 1000000 / 8; // Convert from Megabits to Bytes
        }
        else if (unit == "kb/s" || unit == "kBit/s" || unit == "KBit/s")
        {
            bitrateValue *= 1000 / 8; // Convert from Kilobits to Bytes
        }

        int tmp = bitrateValue;
        return tmp;
    }

    throw std::invalid_argument("Invalid bitrate string: " + bitrateString);
}

void print_rf_info(WirelessInfo wirelessInfo1){
    std::cout << std::endl;
    std::cout << "---WIRELESS INFO---" << std::endl;
    std::cout << "Signal Level: " << wirelessInfo1.signalLevel << std::endl;
    std::cout << "Noise Level: " << wirelessInfo1.noiseLevel << std::endl;
    std::cout << "Bit Rate: " << wirelessInfo1.bitRate << std::endl;
    std::cout << "Bit rate: " << convertBitrateToBytes(wirelessInfo1.bitRate) << " Bps" << std::endl;
    std::cout << "SNR: " << wirelessInfo1.snr << std::endl;
    std::cout << "---WIRELESS INFO---" << std::endl
              << std::endl;
}

bool serializeBundleToFile(const std::string filename, const dtn::data::Bundle bundle){
    // Open the output file stream
    std::ofstream outputFile(filename, std::ios::binary);
    if (!outputFile.is_open())
    {
        std::cerr << "Error opening output file" << std::endl;
        return false;
    }

    try
    {
        // Create a DefaultSerializer object with the output stream
        dtn::data::DefaultSerializer serializer(outputFile);

        // Serialize the bundle and write it to the file
        serializer << bundle;

        // Close the output file stream
        outputFile.close();

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error serializing bundle: " << e.what() << std::endl;
        outputFile.close();
        return false;
    }
}

void disconnect(ssh_channel channel){
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
}

bool deserializeBundleFromFile(const std::string localFilePath, dtn::data::Bundle &bundle){
    // Open the input file stream
    std::ifstream inputFile(localFilePath, std::ios::binary);
    if (!inputFile.is_open())
    {
        std::cerr << "Error opening input file" << std::endl;
        return false;
    }

    try{
        // Create a DefaultDeserializer object with the input stream
        dtn::data::DefaultDeserializer deserializer(inputFile);

        // Deserialize the bundle from the file
        deserializer >> bundle;

        // Close the input file stream
        inputFile.close();

        return true;
    }catch(const std::exception &e){
        std::cerr << "Error serializing bundle: " << e.what() << std::endl;
        inputFile.close();
        return false;
    }
}

bool transferFileToRemote(const std::string& localFilePath, const std::string& remoteFilePath, const char* user) {
    std::string scpCommand = "scp " + localFilePath + " root@" + user + ":" + remoteFilePath;

    // Execute the scp command
    int status = std::system(scpCommand.c_str());

    // Check the exit status of the scp command
    if (status == 0){
        std::cout << "File copied successfully to remote server." << std::endl;
        return true;
    }else{
        std::cerr << "Failed to copy file to remote server." << std::endl;
        return false;
    }
}

bool transferFileFromRemote(const std::string& remoteFilePath, const std::string& localFilePath, const char* user){
    std::string scpCommand = "scp root@" + std::string(user) + ":" + remoteFilePath + " " + localFilePath;

    // Execute the scp command
    int status = std::system(scpCommand.c_str());

    // Check the exit status of the scp command
    if (status == 0){
        std::cout << "Files copied successfully from remote server." << std::endl;
        return true;
    }else{
        std::cerr << "Failed to copy files from remote server." << std::endl;
        return false;
    }
}

void receiver(ssh_session session, const char *user, const std::string &localFilePath, const std::string &remoteFilePath, int timeoutSeconds, int nextExpectedBundle){
    int rc;
    bool terminateFlag = false;
    auto startTime = std::chrono::steady_clock::now();

    while (!terminateFlag){
        // Execute the receiver command in a loop
        ssh_channel channel = ssh_channel_new(session);
        rc = ssh_channel_open_session(channel);
        if (rc != SSH_OK){
            fprintf(stderr, "Failed to open SSH channel 1: %s\n", ssh_get_error(session));
            std::cout << user << ": Error " << std::endl;
            disconnect(channel);
            return;
        }

        std::string command = "dtnrecv --file " + remoteFilePath + " --name ackRecv";
        std::cout << user << ": " << command << std::endl;
        rc = ssh_channel_request_exec(channel, command.c_str());
        if (rc != SSH_OK){
            fprintf(stderr, "Failed to execute SSH command: %s\n", ssh_get_error(session));
            disconnect(channel);
            return;
        }

        ssh_channel_set_blocking(channel, 0);
        int exitStatus = ssh_channel_get_exit_status(channel);
        while (exitStatus == -1){
            if (ackMap[nextExpectedBundle] == true){
                ssh_channel_set_blocking(channel, 1);
                disconnect(channel);
                return;
            }

            exitStatus = ssh_channel_get_exit_status(channel);

            // Check if the timeout has been reached
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);
            if (elapsedTime >= std::chrono::seconds(timeoutSeconds)){
                // std::cout << user << ": Timeout reached" << std::endl;
                ssh_channel_set_blocking(channel, 1);
                disconnect(channel);
                return;
            }
        }
        ssh_channel_set_blocking(channel, 1);
        
        if (exitStatus == 0){
            // When the bundle is received, transfer it to the remote host
            if (!transferFileFromRemote(remoteFilePath, localFilePath, user)){
                printf("Error transferring bundle.bin from remote\n");
                disconnect(channel);
                return;
            }

            ssh_channel channel2 = ssh_channel_new(session);
            rc = ssh_channel_open_session(channel2);
            if (rc != SSH_OK){
                fprintf(stderr, "Failed to open SSH channel 2: %s\n", ssh_get_error(session));
                ssh_channel_free(channel2);
                return;
            }

            // Execute the rm command to delete the file
            char command2[256];
            snprintf(command2, sizeof(command2), "rm -f %s", remoteFilePath.c_str());

            rc = ssh_channel_request_exec(channel2, command2);
            if (rc != SSH_OK){
                fprintf(stderr, "Failed to delete file: %s\n", ssh_get_error(session));
                ssh_channel_free(channel2);
                return;
            }

            // Get the exit status of the command
            int exitStatus2 = ssh_channel_get_exit_status(channel2);
            if (exitStatus2 == 0){
                // std::cout << "File deleted successfully on the remote host" << std::endl;
            }else{
                fprintf(stderr, "%s\n", ssh_get_error(session));
            }

            ssh_channel_send_eof(channel2);
            ssh_channel_is_eof(channel2); 
            ssh_channel_free(channel2);

            //The received file is the binary file of the bundle, as such we need to desirialize it
            dtn::data::Bundle bundle;
            deserializeBundleFromFile(localFilePath, bundle);

            dtn::data::BundleID &id = bundle;
            int sequencenumber = std::stoi(id.sequencenumber.toString());

            // std::cout << user << ": Bundle: " << bundle.sequencenumber.toString().c_str() << " FLAG: "<<bundle.get(dtn::data::PrimaryBlock::ACK_BUNDLE) << " Sequence number: "<< sequencenumber << " Expected sequence number: " << nextExpectedBundle << std::endl;
            std::cout << std::stoi(id.sequencenumber.toString()) << std::endl;
            
            //if ((bundle.get(dtn::data::PrimaryBlock::ACK_BUNDLE) == 1) && (sequencenumber == nextExpectedBundle) && ackMap[sequencenumber] != true )
            if((bundle.get(dtn::data::PrimaryBlock::ACK_BUNDLE) == true) && (sequencenumber == nextExpectedBundle)){
                {
                    std::lock_guard<std::mutex> lock(bundleMapMutex);
                    ackMap[sequencenumber] = true;
                    terminateFlag = true;
                    std::cout << user << ": Ack of " << id.sequencenumber.toString().c_str() << " received." << std::endl;
                }
            }
        }
        disconnect(channel);

    }
    return;
}

void sendBundle(ssh_session session, const std::string localFilePath, const std::string remoteFilePath, const std::string destination, const char *user, const std::string ackdest, int nextExpectedBundle, int timeout_rf){
    bool transferSuccess = transferFileToRemote( localFilePath, remoteFilePath, user);
    if (!transferSuccess){
        std::cerr << "Error transferring file: " << localFilePath << std::endl;
        return;
    }

    std::string command = "dtnsend " + destination + " " + remoteFilePath;
    std::cout << user << ": " << command.c_str() << std::endl;

    ssh_channel channel_cmd = ssh_channel_new(session);
    int rc = ssh_channel_open_session(channel_cmd);
    if (rc != SSH_OK){
        fprintf(stderr, "Error opening channel: %s\n", ssh_get_error(session));
        ssh_channel_free(channel_cmd);
        return;
    }

    rc = ssh_channel_request_exec(channel_cmd, command.c_str());
    if (rc != SSH_OK){
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session));
        ssh_channel_free(channel_cmd);
        return;
    }

    int c = 0;
    char buffer2[1024];
    int nbytes;
    while ((nbytes = ssh_channel_read(channel_cmd, buffer2, sizeof(buffer2), 0)) > 0){
        // fwrite(buffer2, 1, nbytes, stdout);
        c++;
        if (c == 5)
            break;
    }

    disconnect(channel_cmd);

    ssh_channel channel2 = ssh_channel_new(session);
    rc = ssh_channel_open_session(channel2);
    if (rc != SSH_OK){
        fprintf(stderr, "Failed to open SSH channel 2: %s\n", ssh_get_error(session));
        ssh_channel_free(channel2);
        return;
    }

    // Execute the rm command to delete the file
    char command2[256];
    snprintf(command2, sizeof(command2), "rm -f %s", remoteFilePath.c_str());

    rc = ssh_channel_request_exec(channel2, command2);
    if (rc != SSH_OK){
        fprintf(stderr, "Failed to delete file: %s\n", ssh_get_error(session));
        ssh_channel_free(channel2);
        return;
    }

    // Get the exit status of the command
    int exitStatus2 = ssh_channel_get_exit_status(channel2);
    if (exitStatus2 == 0){
        // printf("File deleted successfully on the remote host\n");
    }else{
        fprintf(stderr, "%s\n", ssh_get_error(session));
    }

    ssh_channel_send_eof(channel2);
    ssh_channel_is_eof(channel2);
    ssh_channel_free(channel2);

    receiver(session, user, ackdest, "/root/ibrdtn-repo/ibrdtn/tools/src/Receiver/bundleACK.bin", timeout_rf, nextExpectedBundle);

    return;
}

ibrcommon::BLOB::Reference getBlobChunk(const ibrcommon::BLOB::Reference &blob, size_t chunkSize, int sequencenumber,bool condition_4,bool second)
{
    bool found = false;
    if (!bundleSize.empty()){
        if ((bundleSize.find(sequencenumber) == bundleSize.end())){
            bundleSize[sequencenumber] = chunkSize;
            found = true;
        }else{
            chunkSize = bundleSize[sequencenumber];
        }
    }

    if (sequencenumber == 0){
        offsetMap[sequencenumber] = 0;
    }

    size_t remainingSize = 0;
    int current_offset = offsetMap[sequencenumber];


    // Create a new BLOB reference for this chunk
    // Get an iostream for the BLOB
    ibrcommon::BLOB::Reference blobref = blob;
    ibrcommon::BLOB::iostream blobStream = blobref.iostream();
    // std::cout <<" Stream size: " << blobStream.size() << std::endl;
    // std::cout <<" Current offset: " << offsetMap[sequencenumber] << std::endl;

    remainingSize = blobStream.size() - current_offset;
    // std::cout << "Remaining size: " << remainingSize << std::endl;
    // std::cout << "Chunk size: " << chunkSize << std::endl;

    size_t chunkLength = std::min(chunkSize, remainingSize);
    std::cout << "Size of chunk being sent: " << chunkLength << std::endl;
    if (remainingSize <= chunkLength ){
        lastbundle_sequence = sequencenumber;
        // if(condition_4){
        //     std::cout << " Condition 4"<< std::endl;
        //     chunkLength=chunkLength/2;
        //     if(!second){
        //         lastbundle_sequence = lastbundle_sequence + 1;
        //         channelsize2 = static_cast<int>(chunkLength);
        //         std::cout << " 5180, channelsize =" << channelsize1 <<" last bundle sequence =" << lastbundle_sequence << std::endl;
        //     }
        //     if(second){
        //         channelsize1 = chunkLength;
        //         std::cout << " 5180, channelsize =" << channelsize1 <<" last bundle sequence =" << lastbundle_sequence << std::endl;
        //     }
        // }
    }

    blobStream->seekg(current_offset);

    ibrcommon::BLOB::Reference chunkBlob = ibrcommon::BLOB::create();
    ibrcommon::BLOB::iostream chunkStream = chunkBlob.iostream();
    ibrcommon::BLOB::copy(*chunkStream, *blobStream, chunkLength);

    // Update the offset of the next bundle with the chunk size
    sequencenumber += 1;
    offsetMap[sequencenumber] = current_offset + static_cast<int>(chunkLength);
    //std::cout <<" Next offset: " << offsetMap[sequencenumber] << std::endl;
    return chunkBlob;
}

void createChunkFile(int sequenceNumber, const ibrcommon::BLOB::Reference &chunkBlob, std::string filename, bool isLastChunk, bool ackFlag, bool retransmissionFlag) {
    if(acMap.find(sequenceNumber) != acMap.end()){
        retransmision = true;
    }else{
        retransmision = false;
    }

    
    if (sequenceNumber == temp_number) {
        tempackvalue = !tempackvalue;
        ackFlag = tempackvalue;   // Retransmission
        //std::cout << "Retransmission: " << ackFlag << std::endl;
    }
    temp_number = sequenceNumber;

    std::ofstream file(filename, std::ios::binary);
    ibrcommon::BLOB::Reference blobref = chunkBlob;
    ibrcommon::BLOB::iostream blobStream = blobref.iostream();

    if (file.is_open()) {
        ibrcommon::BLOB::iostream chunkStream = blobStream;

        // Write the flags
        char flags = 0;
        if (isLastChunk)
            flags |= 0x01; // Set the last chunk flag
        if (ackFlag)
            flags |= 0x02; // Set the ACK flag
        if (retransmissionFlag)
            flags |= 0x04; // Set the retransmission flag
        file.write(&flags, sizeof(flags));

        // Write the sequence number as a header to the file
        file.write(reinterpret_cast<const char *>(&sequenceNumber), sizeof(sequenceNumber));

        // Write the chunk data to the file
        file << chunkStream->rdbuf();

        file.close();
        // std::cout << "Chunk file created: " << filename << std::endl;
    } else {
        std::cerr << "Failed to create chunk file: " << filename << std::endl;
    }
}

static int auth_keyfile(ssh_session session, const char *keyfile){
    ssh_key key = NULL;
    char pubkey[256] = {0}; // Public key file path
    int rc;

    ::snprintf(pubkey, sizeof(pubkey), "%s.pub", keyfile);

    rc = ssh_pki_import_pubkey_file(pubkey, &key);
    if (rc != SSH_OK){
        fprintf(stderr, "Error importing public key file: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    rc = ssh_userauth_try_publickey(session, NULL, key);
    ssh_key_free(key);
    if (rc != SSH_AUTH_SUCCESS){
        fprintf(stderr, "Public key authentication failed: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    rc = ssh_pki_import_privkey_file(keyfile, NULL, NULL, NULL, &key);
    if (rc != SSH_OK){
        fprintf(stderr, "Error importing private key file: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    rc = ssh_userauth_publickey(session, NULL, key);
    ssh_key_free(key);
    if (rc != SSH_AUTH_SUCCESS){
        fprintf(stderr, "Public key authentication failed: %s\n", ssh_get_error(session));
        return SSH_AUTH_DENIED;
    }

    return rc;
}

static ssh_session start_session(const char *host, const char *user, const char *keyfile, const char *port){
    ssh_session session = ssh_new();
    if (session == NULL){
        fprintf(stderr, "Error creating SSH session\n");
        exit(EXIT_FAILURE);
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host);
    ssh_options_set(session, SSH_OPTIONS_USER, user);
    ssh_options_set(session, SSH_OPTIONS_PORT_STR, port);
    ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, "rsa-sha2-256,rsa-sha2-512,ecdh-sha2-nistp256,ssh-rsa");
    
    int rc = ssh_connect(session);
    if (rc != SSH_OK){
        fprintf(stderr, "Error connecting to virtual machine: %s %s\n", host, ssh_get_error(session));
        ssh_free(session);
        exit(EXIT_FAILURE);
    }

    rc = auth_keyfile(session, keyfile);
    if (rc != SSH_AUTH_SUCCESS){
        fprintf(stderr, "Error authenticating with virtual machine: %s\n", host);
        ssh_disconnect(session);
        ssh_free(session);
        exit(EXIT_FAILURE);
    }

    return session;
}

dtn::data::Bundle processBundle(ibrcommon::BLOB::Reference ref, const std::string &localFilePath, EID addr_src, EID addr_dest, int nextExpectedBundle){
    // create a bundle from the file
    dtn::data::Bundle b;

    b.source = addr_src;
    b.destination = addr_dest;
    b.lifetime = 36000;
    b.push_back(ref);

    bundleSize[nextExpectedBundle] = static_cast<int>(ref.size());

    if (nextExpectedBundle == lastbundle_sequence){
        b.set(dtn::data::PrimaryBlock::LAST_BUNDLE, true);
        std::cout << "Last bundle = " << nextExpectedBundle << std::endl;
    }

    dtn::data::BundleID &id = b;
    id.sequencenumber.fromString(std::to_string(nextExpectedBundle).c_str());

    std::ofstream outputFile(localFilePath, std::ios::binary);
    dtn::data::DefaultSerializer serializer(outputFile);

    serializer << b;

    outputFile.close();

    acMap[nextExpectedBundle] = true;
    return b;
}

std::tuple<int, bool, bool, bool> removeHeaderFlags(const std::string &filename) {
    std::ifstream originalFile(filename, std::ios::binary);
    if (!originalFile.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return std::make_tuple(-1, false, false, false);
    }

    originalFile.seekg(0, std::ios::end);
    std::streampos fileSize = originalFile.tellg();
    originalFile.seekg(0, std::ios::beg);

    char flags;
    int sequenceNumber;
    bool lastChunkFlag;
    bool ackFlag;
    bool retransmissionFlag;  // New flag for retransmission

    originalFile.read(&flags, sizeof(flags));
    originalFile.read(reinterpret_cast<char *>(&sequenceNumber), sizeof(sequenceNumber));

    lastChunkFlag = (flags & 0x01) != 0;
    ackFlag = (flags & 0x02) != 0;
    retransmissionFlag = (flags & 0x04) != 0;  // Extract retransmission flag from flags

    std::streampos remainingSize = fileSize - sizeof(flags) - sizeof(sequenceNumber);

    std::ofstream outputFile("newbundleac.bin", std::ios::binary);
    if (!outputFile.is_open()) {
        std::cerr << "Failed to create file: newbundleac.bin" << filename << std::endl;
        originalFile.close();
        return std::make_tuple(sequenceNumber, lastChunkFlag, ackFlag, retransmissionFlag);
    }

    char buffer[1024];
    std::streampos bytesRead = 0;
    while (bytesRead < remainingSize) {
        std::streamsize chunkSize = std::min(static_cast<std::streamsize>(sizeof(buffer)), static_cast<std::streamsize>(remainingSize - bytesRead));
        originalFile.read(buffer, chunkSize);
        outputFile.write(buffer, chunkSize);
        bytesRead += chunkSize;
    }

    originalFile.close();
    outputFile.close();

    return std::make_tuple(sequenceNumber, lastChunkFlag, ackFlag, retransmissionFlag);
}

int findnextsequencenumber(int start_counter){
    int counter = start_counter;

    if (!ackMap.empty()){
        bool keyFound = (ackMap.find(counter) != ackMap.end() && ackMap[counter] != false);
        while (keyFound){
            //std::cout<< " Ack map[" << counter << "]: " << ackMap[counter] << std::endl;    
            counter++;
            keyFound = (ackMap.find(counter) != ackMap.end() && ackMap[counter] != false);
        }
        //std::cout << "Next sequence number: "  << counter << " ACK Received? "<< ackMap[counter] << std::endl;
    }
    return counter;
}

int state_update(double snr, double low_th, double high_th){
    if (snr < low_th)
    {
        return OFF;
    }
    else if (low_th <= snr && snr < high_th)
    {
        return HANDOVER;
    }
    else if (high_th <= snr)
    {
        return ACTIVE;
    }
    return -1;
}

int running_average(int previous_average, int new_value, int counter){
    double weight1 = 0.75;
    double weight2 = 0.35;
    return (((previous_average * counter * weight2) + (new_value * weight1)) / (counter));
}

int decision_unit(WirelessInfo wirelessInfo1, WirelessInfo wirelessInfo2, int datasent, std::streamsize fullsize, bool ac_state, int ac_size){
    int ret = -1;
    int remaining = fullsize - datasent;
    
    wirelessInfo1.state = state_update(wirelessInfo1.snr, rf_low_th_24ghz, rf_high_th_24ghz);
    wirelessInfo2.state = state_update(wirelessInfo2.snr, rf_low_th_5ghz, rf_high_th_5ghz);

    if(wirelessInfo1.state == OFF){
        std::cout << " 2.4 Off " << no_rf1 << std::endl;
        contador1 = 0;
        average_rate1 = 0;
    }
    if(wirelessInfo2.state == OFF){
        std::cout << " 5.0 Off " << no_rf2 << std::endl;
        contador1 = 0;
        average_rate2 = 0;
    }
    if(ac_state){
        std::cout << " AC on " << ac_state << std::endl;
    }

    contador1++; //needed to calculate the running average
    contador2++;

    if (!ac_state){
        if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == OFF) // rf1
        {
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            average_rate1 = running_average(average_rate1,channelsize1,contador1);
            ret = 1;           
        }
        else if (wirelessInfo1.state == OFF && wirelessInfo2.state == ACTIVE) // rf2;
        {
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            average_rate2 = running_average(average_rate2,channelsize2,contador2);
            ret = 2; 
        }
        else if (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == HANDOVER)
        {
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            ret = 3;
        }
        else if ((wirelessInfo1.state == ACTIVE && wirelessInfo2.state == HANDOVER) || (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == ACTIVE)) // rf1 + rf2 same bundles;
        {   
            if (wirelessInfo1.state == ACTIVE)
            {
                // atualizar tamanho
                channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
                average_rate1 = running_average(average_rate1,channelsize1,contador1);
            }
            else if (wirelessInfo2.state == ACTIVE)
            {
                // atualizar tamanho
                channelsize1 = convertBitrateToBytes(wirelessInfo2.bitRate);
                average_rate1 = running_average(average_rate1,channelsize1,contador1);
            } 
            ret = 3;
        }
        else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == ACTIVE) // ac + rf1 + rf2 DIFF bundles;
        {
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);

            average_rate1 = running_average(average_rate1,channelsize1,contador1);
            average_rate2 = running_average(average_rate2,channelsize2,contador2);

            ret = 4;
        }
    }else{
        if (wirelessInfo1.state == OFF && wirelessInfo2.state == OFF)
        {
            channelsizeAC = ac_size;
            ret = 0;
        }
        else if (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == OFF) // ac + rf1 same bundles;
        {
            ret = 5; 
        }
        else if (wirelessInfo1.state == OFF && wirelessInfo2.state == HANDOVER) // ac + rf2 same bundles;
        {
            ret = 6; 
        }
        else if (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == HANDOVER) // ac + rf1 + rf2 same bundles;
        {
            ret = 7;
        }
        else if ((wirelessInfo1.state == ACTIVE && wirelessInfo2.state == HANDOVER) || (wirelessInfo1.state == HANDOVER && wirelessInfo2.state == ACTIVE)) // rf1 + rf2 same bundles;
        {   
            if (wirelessInfo1.state == ACTIVE)
            {
                // atualizar tamanho
                channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            }
            else if (wirelessInfo2.state == ACTIVE)
            {
                // atualizar tamanho
                channelsize1 = convertBitrateToBytes(wirelessInfo2.bitRate);
            }
            ret = 3;
        }
        else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == ACTIVE) // ac + rf1 + rf2 DIFF bundles;
        {
            channelsize1 = convertBitrateToBytes(wirelessInfo2.bitRate); // carefully choose this, it depends on the rf card and to each remote machine it is connected
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            ret = 4;
        } else if (wirelessInfo1.state == ACTIVE && wirelessInfo2.state == OFF) // rf1
        {
            channelsize1 = convertBitrateToBytes(wirelessInfo1.bitRate);
            average_rate1 = running_average(average_rate1,channelsize1,contador1);
            ret = 1;           
        }
        else if (wirelessInfo1.state == OFF && wirelessInfo2.state == ACTIVE) // rf2;
        {
            channelsize2 = convertBitrateToBytes(wirelessInfo2.bitRate);
            average_rate2 = running_average(average_rate2,channelsize2,contador2);
            ret = 2; 
        }
    }

    std::cout << "Condition: " << ret << std::endl;
    return ret;
}

void ac_sender(std::string src_ip, std::string src_port, std::string dest_node, int timeoutreceiver, int timeoutRequestMiliSeconds, std::string filenameac, int expectedsequencenumber){
    std::string filepathac = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleac.bin";
    
    std::string command = "python3 upload_file.py " + src_ip + " " + src_port + " " + std::to_string(timeoutRequestMiliSeconds) + " " + filepathac + " /home/unet/scripts/bundleac.bin";
    std::cout << command << std::endl;
    int result = ::system(command.c_str());
    if (result != 0)
    {
        std::cout << "Error uploading" << std::endl;
    }
    
    command = "python3 ac_sender.py " + src_ip + " " + src_port + " " + dest_node + " " + std::to_string(timeoutRequestMiliSeconds) + " bundleac.bin";
    std::cout << command << std::endl;
    result = ::system(command.c_str());
    if (result != 0){
        std::cout << "Error sending through ac" << std::endl;
    }

    //acMap[expectedsequencenumber] = true;

    bool ack_received = false;
    std::string filepathack = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Receiver/bundleack.bin";

    auto startTime = std::chrono::steady_clock::now();
    while (!ack_received)
    {
        if (ackMap[expectedsequencenumber] == true){
            return;
        }

        command = "python3 download_file.py " + src_ip + " " + src_port + " " + std::to_string(timeoutRequestMiliSeconds) + " /home/unet/scripts/bundleack.bin " + filepathack;
        //std::cout << command << std::endl;
        int result2 = ::system(command.c_str());
        //std::cout << result << std::endl;

        std::ifstream file(filepathack);
        if (result2 == 0){
            auto result = removeHeaderFlags(filepathack);
            
            int sequenceNumber1 = std::get<0>(result);
            bool isLastChunk = std::get<1>(result);
            ack_received = std::get<2>(result) && sequenceNumber1 == expectedsequencenumber;
            
            if(ack_received){
                std::cout << "Ack of bundle: " << sequenceNumber1 << " received. Expected sequence number: " << expectedsequencenumber << std::endl;
            }
            ackMap[sequenceNumber1] = true;
            
        }

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);
        if (elapsedTime >= std::chrono::seconds(timeoutreceiver)){
            std::cout << " Time out AC! "<<std::endl;
            return;
        }
    }

    std::string command2 = "rm " + filepathack;
    result = std::system(command2.c_str());
    if (result == 0){
        //std::cout << "File removed successfully." << std::endl;
    }else{
        std::cout << "Failed to remove file." << std::endl;
    }
}

int main(int argc, char *argv[])
{
    int rc;
    int ret = EXIT_SUCCESS;
    WirelessInfo wirelessInfo1;
    WirelessInfo wirelessInfo2;

    const char *host1 = "192.168.0.102";
    const char *user1 = "root";
    const char *port1 = "22";
    const char *host2 = "192.168.0.105";
    const char *user2 = "root";
    const char *port2 = "22";

    // const char *host1 = "192.168.0.103";
    // const char *user1 = "root";
    // const char *port1 = "22";
    // const char *host2 = "192.168.0.106";
    // const char *user2 = "root";
    // const char *port2 = "22";

    int timeout_rf = 20;

    std::string file_destination1 = "dtn://C/dtnRecv";
    std::string file_destination2 = "dtn://D/dtnRecv";
    std::string filename;

    const std::string localFilePath1 = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundle1.bin";
    const std::string localFilePath2 = "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundle2.bin";
    const std::string remoteFilePath = "/root/ibrdtn-repo/ibrdtn/tools/src/Sender/bundle.bin";
    EID addr_source = EID("dtn://moreira-XPS-15-9570");
    EID addr_dest = EID("dtn://moreira-XPS-15-9570");

    // AC options
    std::string src_ip = "192.168.0.218";                                                           // 192.168.0.218
    std::string src_port = "1100";                                                                  // 1100
    std::string dest_node = "183";                                                                  // 182
    std::string filenameac = "/home/unet/scripts/bundleac.bin";                                      // "home/unet/scripts/bundleac.bin"
    std::string filepathack = "/home/unet/scripts/bundleack.bin";
    std::string remotePath = "bundle.bin";
    int timeoutreceiver =30;             // this is in seconds
    int timeoutRequestMiliSeconds = 10000; // this is in miliseconds
    int ac_mtu = 990;                      // 128
    ac_mtu = ac_mtu - 6;
    bool ac_state = true;

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

    int nextExpectedBundle = 0;

    // Setup the sessions
    ssh_session session1 = start_session(host1, user1, KEY_PATH, port1);
    ssh_session session2 = start_session(host2, user2, KEY_PATH, port2);

    // Start the daemons
    ssh_channel channel1 = ssh_channel_new(session1);
    rc = ssh_channel_open_session(channel1);
    if (rc != SSH_OK)
    {
        fprintf(stderr, "Error opening channel: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel1);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    rc = ssh_channel_request_exec(channel1, "dtnd -i wlan0");
    if (rc != SSH_OK)
    {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel1);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    ssh_channel channel2 = ssh_channel_new(session2);
    rc = ssh_channel_open_session(channel2);
    if (rc != SSH_OK)
    {
        fprintf(stderr, "Error opening channel for the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

    rc = ssh_channel_request_exec(channel2, "dtnd -i wlan0");
    if (rc != SSH_OK)
    {
        fprintf(stderr, "Error executing command on the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

    sleep(1);

    std::cout << filename << " is being sent" << std::endl;
    std::cout << "......." << std::endl;

    uint64_t now;
    dtn::data::Bundle b1;
    dtn::data::Bundle b2;
    ibrcommon::BLOB::Reference ref = ibrcommon::BLOB::open(filename);
    int it =0;
    while (datasent < ref.size()){
        nextExpectedBundle = findnextsequencenumber(0);
        if (nextExpectedBundle == 0)
        {
            offsetMap[nextExpectedBundle] = 0;
        }
        std::cout << "Sequence number = " << nextExpectedBundle << std::endl;
        wirelessInfo1 = getWirelessInfo(session1, "wlan0", 2442);
        //std::cout << "No rf1: " << no_rf1 << " No rf2: " << no_rf2<<std::endl;

        wirelessInfo2 = getWirelessInfo(session2, "wlan0", 5180);
        //std::cout << "No rf1: " << no_rf1 << " No rf2: " << no_rf2<<std::endl;

        int condition = decision_unit(wirelessInfo1, wirelessInfo2, datasent, ref.size(), ac_state, ac_mtu);
        // if(it < 3){
        //     condition =0;
        // }else if (it>=3 && it < 6){
        //     condition = 5;
        // }else if (it>=6 && it < 9){
        //     condition = 6;
        // }else{
        //     condition = 0;
        // }
        // // else if (it>=15 && it <= 18){
        // //     condition = 7;
        // // }else{
        // //     condition = 4;
        // // }
        // it ++;
        if(!no_rf1){
            print_rf_info(wirelessInfo1);
        }
        if(!no_rf2){
            print_rf_info(wirelessInfo2);
        }
        
        if (condition == 0){
            std::cout<< "Acoustical "<<std::endl;
            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, ac_mtu, nextExpectedBundle,false,false);
            if (lastbundle_sequence == nextExpectedBundle){
                std::cout << " Last " << lastbundle_sequence << std::endl;
                last = true;
            }

            createChunkFile(nextExpectedBundle, ref1, "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleac.bin", last, false,retransmision); // overhead of 5 bytes

            if (nextExpectedBundle == 0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            }
            ac_sender(src_ip, src_port, dest_node, timeoutreceiver, timeoutRequestMiliSeconds, filenameac, nextExpectedBundle);

            if (!ackMap.empty()){
                if ((ackMap.find(nextExpectedBundle) != ackMap.end())  && ackMap[nextExpectedBundle] == true)
                {
                    datasent += static_cast<int>(ref1.size());
                    std::cout << "Datasent: "<< datasent << std::endl;
                }
            }
        }
        else if (condition == 1)
        {
            std::cout<< "RF 2.5 GHz"<<std::endl;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, channelsize1, nextExpectedBundle,false,false);

            b1 = processBundle(ref1, localFilePath2, addr_source, addr_dest, nextExpectedBundle);
            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            sendBundle(session1, localFilePath2, remoteFilePath, file_destination1, host1, "Receiver/bundleack1.bin", nextExpectedBundle, timeout_rf);
            if (!ackMap.empty())
            {
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true)
                {
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }

            char command1[256];
            ::snprintf(command1, sizeof(command1), "rm -f %s", localFilePath2.c_str());
            ::system(command1);
        }
        else if (condition == 2)
        {
            std::cout<< "RF 5 GHz"<<std::endl;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, channelsize2, nextExpectedBundle,false,false);

            b1 = processBundle(ref1, localFilePath1, addr_source, addr_dest, nextExpectedBundle);
            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0)
            {
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            sendBundle(session2, localFilePath1, remoteFilePath, file_destination2, host2, "Receiver/bundleack2.bin", nextExpectedBundle, timeout_rf);
            if (!ackMap.empty())
            {
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true)
                {
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }

            char command1[256];
            ::snprintf(command1, sizeof(command1), "rm -f %s", localFilePath1.c_str());
            ::system(command1);
        }
        else if (condition == 3){
            std::cout<< "RF 2.5 GHz + 5.0 GHz Same bundles (Reliable mode)" << std::endl;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, channelsize1, nextExpectedBundle,false,false);

            b1 = processBundle(ref1, localFilePath1, addr_source, addr_dest, nextExpectedBundle);
            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0)
            {
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            b1 = processBundle(ref1, localFilePath1, addr_source, addr_dest, nextExpectedBundle);
            b2 = processBundle(ref1, localFilePath2, addr_source, addr_dest, nextExpectedBundle);

            // sender threads
            std::thread senderThread2(sendBundle, session2, localFilePath1, remoteFilePath, file_destination2, host2, "Receiver/bundleack2.bin", nextExpectedBundle, timeout_rf);
            std::thread senderThread1(sendBundle, session1, localFilePath2, remoteFilePath, file_destination1, host1, "Receiver/bundleack1.bin", nextExpectedBundle, timeout_rf);

            senderThread2.join();
            senderThread1.join();

            if (!ackMap.empty())
            {
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true)
                {
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }

            char command3[256];
            ::snprintf(command3, sizeof(command3), "rm -f %s", localFilePath1.c_str());
            ::system(command3);
        }
        else if (condition == 4){
            std::cout<< "RF 2.5 GHz + 5.0 GHz diff bundles" << std::endl;

            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, channelsize2, nextExpectedBundle,true,false);
            int temp = nextExpectedBundle;
            b1 = processBundle(ref1, localFilePath1, addr_source, addr_dest, nextExpectedBundle);

            nextExpectedBundle = findnextsequencenumber(nextExpectedBundle + 1);
            std::cout << "Sequence number = " << nextExpectedBundle << std::endl;

            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                std::cout<< now <<endl;
            }
            // mudar o tamanho na condição
            if (ref1.size() >= channelsize2){
                ibrcommon::BLOB::Reference ref2 = getBlobChunk(ref, channelsize1, nextExpectedBundle,true,true);
                b2 = processBundle(ref2, localFilePath2, addr_source, addr_dest, nextExpectedBundle);
            }

            // uint64_t seconds = now / 1000;

            // sender threads
            if (ref1.size() >= channelsize2){
                std::thread senderThread2(sendBundle, session2, localFilePath1, remoteFilePath, file_destination2, host2, "Receiver/bundleack2.bin", temp, timeout_rf);
                std::thread senderThread1(sendBundle, session1, localFilePath2, remoteFilePath, file_destination1, host1, "Receiver/bundleack1.bin", nextExpectedBundle, timeout_rf);

                senderThread1.join();
                senderThread2.join();
            }else{
                std::thread senderThread2(sendBundle, session2, localFilePath1, remoteFilePath, file_destination2, host2, "Receiver/bundleack2.bin", temp, timeout_rf);
                senderThread2.join();
            }

            char command[256];
            ::snprintf(command, sizeof(command), "rm -f %s", localFilePath1.c_str());
            ::system(command);
            char command2[256];
            ::snprintf(command2, sizeof(command2), "rm -f %s", localFilePath2.c_str());
            ::system(command2);

            if (!ackMap.empty()){
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true ){
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }
            if (!ackMap.empty()){
                if ((ackMap.find(std::atoi(b2.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b2.sequencenumber.toString().c_str())] == true){
                    datasent += static_cast<int>(b2.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }
        }
        else if (condition == 5)
        { // ac + rf1 same bundles
            std::cout<< "Acoustical + RF 2.5 GHz (Reliable mode)" << std::endl;

            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, ac_mtu, nextExpectedBundle,false,false);
            if (lastbundle_sequence == nextExpectedBundle)
            {
                // std::cout << " Last " << lastbundle_sequence << std::endl;
                last = true;
            }
            createChunkFile(nextExpectedBundle, ref1, "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleac.bin", last, false,retransmision); // overhead of 5 bytes

            b1 = processBundle(ref1, localFilePath2, addr_source, addr_dest, nextExpectedBundle);
            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0)
            {
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            std::thread senderThread2(ac_sender, src_ip, src_port, dest_node, timeoutreceiver, timeoutRequestMiliSeconds, filenameac, nextExpectedBundle);
            std::thread senderThread1(sendBundle, session1, localFilePath2, remoteFilePath, file_destination1, host1, "Receiver/bundleack1.bin", nextExpectedBundle, timeout_rf);

            senderThread2.join();
            senderThread1.join();

            if (!ackMap.empty())
            {
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end())&& ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true)
                {
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }
        }
        else if (condition == 6)
        { // ac + rf2 same bundles
            std::cout<< "Acoustical + RF 5 GHz (Reliable mode)" << std::endl;

            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, ac_mtu, nextExpectedBundle,false,false);
            if (lastbundle_sequence == nextExpectedBundle)
            {
                // std::cout << " Last " << lastbundle_sequence << std::endl;
                last = true;
            }
            createChunkFile(nextExpectedBundle, ref1, "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleac.bin", last, false,retransmision); // overhead of 5 bytes

            b1 = processBundle(ref1, localFilePath1, addr_source, addr_dest, nextExpectedBundle);
            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0)
            {
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            std::thread senderThread1(sendBundle, session2, localFilePath1, remoteFilePath, file_destination2, host2, "Receiver/bundleack2.bin", nextExpectedBundle, timeout_rf);
            std::thread senderThread2(ac_sender, src_ip, src_port, dest_node, timeoutreceiver, timeoutRequestMiliSeconds, filenameac, nextExpectedBundle);

            senderThread1.join();
            senderThread2.join();

            if (!ackMap.empty())
            {
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true )
                {
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }
        }
        else if (condition == 7)
        { // Condition AC + RF1 + RF2 SAME BUNDLES"
            std::cout<< "Acoustical + RF 2.5GHz + RF 5 GHz (Reliable mode)" << std::endl;

            bool last = false;
            ibrcommon::BLOB::Reference ref1 = getBlobChunk(ref, ac_mtu, nextExpectedBundle,false,false);
            if (lastbundle_sequence == nextExpectedBundle){
                // std::cout << " Last " << lastbundle_sequence << std::endl;
                last = true;
            }
            createChunkFile(nextExpectedBundle, ref1, "/home/ctm/Documents/ibrdtn/ibrdtn/tools/src/Sender/bundleac.bin", last, false ,retransmision); // overhead of 5 bytes

            b1 = processBundle(ref1, localFilePath2, addr_source, addr_dest, nextExpectedBundle);
            if (std::atoi(b1.sequencenumber.toString().c_str()) == 0){
                now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                // uint64_t seconds = now / 1000;
            }

            std::thread senderThread3(ac_sender, src_ip, src_port, dest_node, timeoutreceiver, timeoutRequestMiliSeconds, filenameac, nextExpectedBundle);
            std::thread senderThread1(sendBundle, session2, localFilePath2, remoteFilePath, file_destination2, host2, "Receiver/bundleack2.bin", nextExpectedBundle, timeout_rf);
            std::thread senderThread2(sendBundle, session1, localFilePath2, remoteFilePath, file_destination1, host1, "Receiver/bundleack1.bin", nextExpectedBundle, timeout_rf);

            senderThread1.join();
            senderThread2.join();
            senderThread3.join();

            if (!ackMap.empty()){
                if ((ackMap.find(std::atoi(b1.sequencenumber.toString().c_str())) != ackMap.end()) && ackMap[std::atoi(b1.sequencenumber.toString().c_str())] == true ){
                    datasent += static_cast<int>(b1.getPayloadLength());
                    std::cout<< "Data sent: " << datasent << std::endl;
                }
            }
        }else{
            sleep(1);
            std::cout << " No connection " << std::endl;
        }
    }

    // Close the channels and disconnect from the virtual machines
    disconnect(channel1);
    disconnect(channel2);
    ssh_disconnect(session1);
    ssh_free(session1);
    ssh_disconnect(session2);
    ssh_free(session2);
    std::cout << filename << " has been sent" << std::endl;
    std::cout << "Transmition time(miliseconds) of the first bundle: " << now << std::endl;

    return 0;
}
