#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libssh/libssh.h>
#include <iostream>
#include "keyconfig.h" 

using namespace std;

void disconnect(ssh_channel channel_cmd, ssh_channel channel, ssh_session session) {
    // Close the data channel
    ssh_channel_send_eof(channel_cmd);
    ssh_channel_close(channel_cmd);
    ssh_channel_free(channel_cmd);

    // Close the daemon process channel
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    // Disconnect from the virtual machine
    ssh_disconnect(session);
    ssh_free(session);
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

int main() {
    int rc;

    ssh_session session1 = start_session("localhost", "moreira1", KEY_PATH, "2222");
    ssh_session session2 = start_session("localhost", "moreira2", KEY_PATH, "2223");

    // Execute a command on the virtual machine
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

    // Execute a command on the second virtual machine
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

    ssh_channel channel_cmd = ssh_channel_new(session1);
    rc = ssh_channel_open_session(channel_cmd);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel_cmd);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }


    rc = ssh_channel_request_exec(channel_cmd, "dtnping dtn://moreira2-VirtualBox/echo");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session1));
        ssh_channel_free(channel_cmd);
        ssh_disconnect(session1);
        ssh_free(session1);
        exit(EXIT_FAILURE);
    }

    int c = 0;
    // Read and print the command output
    char buffer[1024];
    int nbytes;
    // Read and print the command output
    while ((nbytes = ssh_channel_read(channel_cmd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, nbytes, stdout);
        c++;
        if(c == 5) break;
    }

    ssh_channel channel2_cmd = ssh_channel_new(session2);
    rc = ssh_channel_open_session(channel2_cmd);
    if (rc != SSH_OK) {
        fprintf(stderr, "Error opening channel for the second virtual machine: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2_cmd);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }


    rc = ssh_channel_request_exec(channel2_cmd, "dtnping dtn://moreira1-VirtualBox/echo");
    if (rc != SSH_OK) {
        fprintf(stderr, "Error executing command: %s\n", ssh_get_error(session2));
        ssh_channel_free(channel2_cmd);
        ssh_disconnect(session2);
        ssh_free(session2);
        exit(EXIT_FAILURE);
    }

    c =0;
    // Read and print the command output
    while ((nbytes = ssh_channel_read(channel2_cmd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, nbytes, stdout);
        c++;
        if(c == 5) break;
    }
    
    // Close the channels and disconnect from the virtual machines
    disconnect(channel_cmd, channel, session1);
    disconnect(channel2_cmd, channel2, session2);

    return 0;
}
