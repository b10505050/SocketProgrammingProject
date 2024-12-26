#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <opencv2/opencv.hpp>

#define PORT 8080
#define COMMAND_BUFFER_SIZE 512
#define USERNAME_BUFFER_SIZE 128

void initialize_openssl() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

void cleanup_openssl() {
    EVP_cleanup();
}

SSL_CTX *create_context() {
    const SSL_METHOD *method = TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

// ========== 上傳檔案 ==========
void send_file(SSL *ssl) {
    char filename[USERNAME_BUFFER_SIZE];
    FILE *file;
    int file_size, bytes_read;

    printf("Enter filename to send: ");
    scanf("%s", filename);

    file = fopen(filename, "rb");
    if (!file) {
        perror("[ERROR] Failed to open file");
        return;
    }

    // 發送命令
    char command[COMMAND_BUFFER_SIZE];
    snprintf(command, sizeof(command), "SEND_FILE %s", filename);
    if (SSL_write(ssl, command, strlen(command)) <= 0) {
        perror("[ERROR] Failed to send command to server");
        fclose(file);
        return;
    }
    printf("[DEBUG] Sent command: %s\n", command);

    // 獲取檔案大小
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    printf("[DEBUG] file_size=%d\n", file_size);

    // 發送檔案大小
    int net_file_size = htonl(file_size);
    if (SSL_write(ssl, &net_file_size, sizeof(net_file_size)) <= 0) {
        perror("[ERROR] Failed to send file size");
        fclose(file);
        return;
    }
    printf("[DEBUG] Sent file size: %d bytes\n", file_size);

    // 發送檔案內容
    char file_buffer[1024];
    int total_sent = 0;
    while ((bytes_read = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
        int sent = SSL_write(ssl, file_buffer, bytes_read);
        if (sent <= 0) {
            perror("[ERROR] Failed to send file data");
            break;
        }
        total_sent += sent;
        printf("[DEBUG] Sent %d bytes, total_sent=%d\n", sent, total_sent);
    }
    fclose(file);

    if (total_sent == file_size) {
        printf("File '%s' sent successfully. Total bytes sent: %d\n", filename, total_sent);
    } else {
        printf("[ERROR] File transmission incomplete. Sent %d/%d bytes\n", total_sent, file_size);
    }

    // 接收伺服器回應
    char server_response[COMMAND_BUFFER_SIZE];
    bzero(server_response, sizeof(server_response));
    SSL_read(ssl, server_response, sizeof(server_response));
    printf("From Server: %s\n", server_response);
}

// ========== 下載檔案 ==========
void receive_file(SSL *ssl) {
    char filename[USERNAME_BUFFER_SIZE];
    char new_filename[USERNAME_BUFFER_SIZE];
    FILE *file;
    int file_size, bytes_read;

    printf("Enter filename to receive: ");
    scanf("%s", filename);

    // 發送接收檔案命令
    char command[COMMAND_BUFFER_SIZE];
    snprintf(command, sizeof(command), "RECEIVE_FILE %s", filename);
    SSL_write(ssl, command, strlen(command));

    // 接收檔案大小
    if (SSL_read(ssl, &file_size, sizeof(file_size)) <= 0) {
        perror("[ERROR] Failed to receive file size");
        return;
    }
    file_size = ntohl(file_size);
    printf("[DEBUG] Receiving file: %s, size: %d bytes\n", filename, file_size);

    if (file_size == 0) {
        printf("[ERROR] Received file size=0. Aborting download.\n");
        return;
    }

    // 防止重名
    strcpy(new_filename, filename);
    int counter = 1;
    while (access(new_filename, F_OK) == 0) {
        snprintf(new_filename, sizeof(new_filename), "%s_%d", filename, counter);
        counter++;
    }

    file = fopen(new_filename, "wb");
    if (!file) {
        perror("[ERROR] Failed to open file for writing");
        return;
    }

    // 接收檔案內容
    char file_buffer[1024];
    int total_received = 0;
    while (total_received < file_size) {
        bytes_read = SSL_read(ssl, file_buffer, sizeof(file_buffer));
        if (bytes_read <= 0) break;
        fwrite(file_buffer, 1, bytes_read, file);
        total_received += bytes_read;
        printf("[DEBUG] Received %d bytes, total_received=%d\n", bytes_read, total_received);
    }
    fclose(file);

    if (total_received == file_size) {
        printf("File '%s' received successfully. Saved as '%s'.\n", filename, new_filename);
    } else {
        printf("[ERROR] File reception incomplete. Received %d/%d bytes\n", total_received, file_size);
    }

    // 接收伺服器回應
    char server_response[COMMAND_BUFFER_SIZE];
    bzero(server_response, sizeof(server_response));
    SSL_read(ssl, server_response, sizeof(server_response));
    printf("From Server: %s\n", server_response);
}

// ========== 查詢檔案清單 ==========
void list_files(SSL *ssl) {
    SSL_write(ssl, "LIST_FILES", strlen("LIST_FILES"));

    char file_list[COMMAND_BUFFER_SIZE * 10];
    bzero(file_list, sizeof(file_list));
    int ret = SSL_read(ssl, file_list, sizeof(file_list));

    if (ret <= 0) {
        printf("[ERROR] Failed to retrieve file list\n");
    } else {
        printf("Available files on server:\n%s", file_list);
    }
}

// ========== 串流影片 (Send video) ==========
// 改良版：結束時傳 frame_size=0 通知 Server
void send_video_stream(SSL *ssl) {
    char video_path[USERNAME_BUFFER_SIZE];
    printf("Enter video file path to stream: ");
    scanf("%s", video_path);

    // 發送指令給伺服器，表示要開始串流
    SSL_write(ssl, "STREAM_VIDEO", strlen("STREAM_VIDEO"));

    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) {
        printf("[ERROR] Failed to open video file: %s\n", video_path);
        return;
    }

    while (true) {
        cv::Mat frame;
        cap >> frame; // 讀取下一幀
        if (frame.empty()) {
            // 影片結束, 傳 frame_size=0
            int zero_size = 0;
            int net_zero_size = htonl(zero_size);
            SSL_write(ssl, &net_zero_size, sizeof(net_zero_size));
            printf("[DEBUG] Video ended. Sent frame_size=0 to server.\n");
            break;
        }

        // 壓縮幀為 JPEG
        std::vector<uchar> buffer;
        cv::imencode(".jpg", frame, buffer);

        // 發送幀大小
        int frame_size = buffer.size();
        int net_frame_size = htonl(frame_size);
        if (SSL_write(ssl, &net_frame_size, sizeof(net_frame_size)) <= 0) {
            perror("[ERROR] Failed to send frame size");
            break;
        }

        // 發送幀資料
        int total_sent = 0;
        while (total_sent < frame_size) {
            int sent = SSL_write(ssl, buffer.data() + total_sent, frame_size - total_sent);
            if (sent <= 0) {
                perror("[ERROR] Failed to send frame data");
                break;
            }
            total_sent += sent;
        }
        // 控制幀率 (30ms 約= 33FPS)
        usleep(30000);
    }

    printf("Video stream completed.\n");
}

/**
 * @brief 已登入後的選單
 *  1. View online users
 *  2. Retrieve messages
 *  3. Send message
 *  4. Logout
 *  5. Send file
 *  6. List file
 *  7. Receive file
 *  8. Send video file
 */
void menu(SSL *ssl, const char *username) {
    int choice;
    char buffer[COMMAND_BUFFER_SIZE];
    char message[COMMAND_BUFFER_SIZE];

    while (1) {
        printf("\n====================================\n");
        printf("Logged in as: %s\n", username);
        printf("====================================\n");
        printf("1. View online users\n");
        printf("2. Retrieve messages\n");
        printf("3. Send message\n");
        printf("4. Logout\n");
        printf("5. Send file\n");
        printf("6. List file\n");
        printf("7. Receive file\n");
        printf("8. Send video file\n");
        printf("Enter your choice: ");

        if (scanf("%d", &choice) != 1) {
            fprintf(stderr, "Invalid input. Please enter a number.\n");
            while (getchar() != '\n');
            continue;
        }
        getchar(); // clean leftover '\n'

        switch (choice) {
            case 1:
                SSL_write(ssl, "ONLINE", strlen("ONLINE"));
                bzero(buffer, sizeof(buffer));
                SSL_read(ssl, buffer, sizeof(buffer));
                printf("Online users:\n%s", buffer);
                break;

            case 2:
                SSL_write(ssl, "RETRIEVE", strlen("RETRIEVE"));
                bzero(buffer, sizeof(buffer));
                SSL_read(ssl, buffer, sizeof(buffer));
                if (strstr(buffer, "No new messages") != NULL) {
                    printf("No new messages.\n");
                } else {
                    printf("Messages:\n%s", buffer);
                }
                break;

            case 3: {
                printf("Enter target username: ");
                fgets(buffer, sizeof(buffer), stdin);
                buffer[strcspn(buffer, "\n")] = 0;

                printf("Enter your message: ");
                fgets(message, sizeof(message), stdin);
                message[strcspn(message, "\n")] = 0;

                char send_command[COMMAND_BUFFER_SIZE];
                snprintf(send_command, sizeof(send_command), "SEND %s %s", buffer, message);
                SSL_write(ssl, send_command, strlen(send_command));

                bzero(buffer, sizeof(buffer));
                SSL_read(ssl, buffer, sizeof(buffer));
                printf("From Server: %s", buffer);
                break;
            }

            case 4:
                printf("Logging out...\n");
                SSL_write(ssl, "LOGOUT", strlen("LOGOUT"));
                bzero(buffer, sizeof(buffer));
                SSL_read(ssl, buffer, sizeof(buffer));
                printf("From Server: %s", buffer);
                return;

            case 5:
                send_file(ssl);
                break;

            case 6:
                list_files(ssl);
                break;

            case 7:
                receive_file(ssl);
                break;

            case 8:
                send_video_stream(ssl);
                break;

            default:
                printf("Invalid choice. Try again.\n");
                break;
        }
    }
}

void main_menu(SSL *ssl) {
    while (1) {
        int choice;
        char username[USERNAME_BUFFER_SIZE];
        char password[USERNAME_BUFFER_SIZE];
        char buffer[COMMAND_BUFFER_SIZE];

        printf("\n====================================\n");
        printf("Main Menu:\n");
        printf("1. Register\n");
        printf("2. Login\n");
        printf("3. Exit\n");
        printf("====================================\n");
        printf("Enter your choice: ");

        if (scanf("%d", &choice) != 1) {
            fprintf(stderr, "Invalid input. Please enter a number.\n");
            while (getchar() != '\n');
            continue;
        }
        getchar(); // eat '\n'

        switch (choice) {
            case 1: {
                printf("Enter username for registration: ");
                fgets(username, sizeof(username), stdin);
                username[strcspn(username, "\n")] = 0;

                printf("Enter password for registration: ");
                fgets(password, sizeof(password), stdin);
                password[strcspn(password, "\n")] = 0;

                snprintf(buffer, sizeof(buffer), "REGISTER %s %s", username, password);
                SSL_write(ssl, buffer, strlen(buffer));

                bzero(buffer, sizeof(buffer));
                SSL_read(ssl, buffer, sizeof(buffer));
                printf("From Server: %s", buffer);
                break;
            }

            case 2: {
                printf("Enter username: ");
                fgets(username, sizeof(username), stdin);
                username[strcspn(username, "\n")] = 0;

                printf("Enter password: ");
                fgets(password, sizeof(password), stdin);
                password[strcspn(password, "\n")] = 0;

                snprintf(buffer, sizeof(buffer), "LOGIN %s %s", username, password);
                SSL_write(ssl, buffer, strlen(buffer));

                bzero(buffer, sizeof(buffer));
                SSL_read(ssl, buffer, sizeof(buffer));
                printf("From Server: %s", buffer);

                if (strstr(buffer, "successful")) {
                    menu(ssl, username);
                }
                break;
            }

            case 3:
                printf("Exiting client...\n");
                SSL_write(ssl, "exit", strlen("exit"));
                return;

            default:
                printf("Invalid choice. Try again.\n");
                break;
        }
    }
}

int main() {
    int sockfd;
    struct sockaddr_in servaddr;
    SSL_CTX *ctx;
    SSL *ssl;

    initialize_openssl();
    ctx = create_context();

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
        perror("Connection to the server failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sockfd);

    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        close(sockfd);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        cleanup_openssl();
        exit(EXIT_FAILURE);
    }

    printf("SSL handshake successful\n");
    main_menu(ssl);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(sockfd);
    SSL_CTX_free(ctx);
    cleanup_openssl();

    return 0;
}
