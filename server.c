#include <stdio.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>       // for directory operations (file list)
#include <sys/stat.h>     // for mkdir (ensure_store_directory)
#include <sys/types.h>    // for mkdir (ensure_store_directory)
#include <opencv2/opencv.hpp>

#define PORT 8080
#define MAX_CLIENTS 10
#define MAX_MESSAGES 100
#define USERNAME_BUFFER_SIZE 128
#define COMMAND_BUFFER_SIZE 512

// ========== 確保有 store/ 資料夾可存放檔案 ==========
void ensure_store_directory() {
    struct stat st = {0};
    if (stat("./store", &st) == -1) {
        mkdir("./store", 0700); // 創建 store 資料夾，設置讀寫權限
        printf("[INFO] 'store' directory created for file uploads.\n");
    }
}

// ========== 資料結構定義 ==========
typedef struct {
    char username[USERNAME_BUFFER_SIZE];
    SSL *ssl;
} Client;

typedef struct {
    char sender[USERNAME_BUFFER_SIZE];
    char receiver[USERNAME_BUFFER_SIZE];
    char message[COMMAND_BUFFER_SIZE];
} Message;

// 全域變數
Client clients[MAX_CLIENTS];
Message messages[MAX_MESSAGES];
int message_count = 0;

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t messages_mutex = PTHREAD_MUTEX_INITIALIZER;

// ========== 處理影片串流 ==========
// 修正重點：若收到 frame_size=0，就代表串流結束
void handle_video_stream(SSL *ssl) {
    cv::namedWindow("Video Stream", cv::WINDOW_AUTOSIZE);

    while (1) {
        int net_frame_size;
        // 先讀4 bytes 影格大小
        int ret = SSL_read(ssl, &net_frame_size, sizeof(net_frame_size));
        if (ret <= 0) {
            perror("[ERROR] Failed to receive frame size");
            break; 
        }

        int frame_size = ntohl(net_frame_size);
        if (frame_size == 0) {
            // Client 傳 0 代表串流結束
            printf("[DEBUG] Received frame_size=0, stopping stream.\n");
            break;
        }

        printf("[DEBUG] Receiving frame of size: %d bytes\n", frame_size);
        std::vector<uchar> frame_buffer(frame_size);
        int total_received = 0;
        while (total_received < frame_size) {
            int bytes = SSL_read(ssl, frame_buffer.data() + total_received, frame_size - total_received);
            if (bytes <= 0) {
                perror("[ERROR] Failed to receive frame data");
                break;
            }
            total_received += bytes;
        }

        if (total_received < frame_size) {
            printf("[ERROR] Received incomplete frame data. total_received=%d\n", total_received);
            break;
        }

        // 解碼並顯示
        cv::Mat frame = cv::imdecode(frame_buffer, cv::IMREAD_COLOR);
        if (frame.empty()) {
            printf("[ERROR] Failed to decode frame.\n");
            continue;
        }

        cv::imshow("Video Stream", frame);
        if (cv::waitKey(30) == 27) { // ESC
            printf("[DEBUG] ESC pressed. Stopping stream.\n");
            break;
        }
    }

    cv::destroyWindow("Video Stream");
    printf("Video stream ended.\n");
}

// ========== 上傳 / 下載 檔案操作 ==========

void handle_list_files(SSL *ssl) {
    struct dirent *entry;
    DIR *dir = opendir("./store");
    char file_list[COMMAND_BUFFER_SIZE * 10] = ""; // 預估能放多個檔案名

    if (!dir) {
        perror("[ERROR] Failed to open 'store' directory");
        SSL_write(ssl, "Failed to retrieve file list\n", strlen("Failed to retrieve file list\n"));
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // 排除 . 和 .. 目錄
        if (entry->d_name[0] == '.') continue;

        // 添加文件名到列表
        strcat(file_list, entry->d_name);
        strcat(file_list, "\n");
    }
    closedir(dir);

    if (strlen(file_list) == 0) {
        strcpy(file_list, "No files available for download\n");
    }

    SSL_write(ssl, file_list, strlen(file_list));
}

void handle_send_file(SSL *ssl, char *buffer) {
    char filename[USERNAME_BUFFER_SIZE];
    char filepath[USERNAME_BUFFER_SIZE + 10];
    int file_size, bytes_read;
    FILE *file;

    sscanf(buffer + 10, "%s", filename); // "SEND_FILE filename"
    snprintf(filepath, sizeof(filepath), "./store/%s", filename);

    file = fopen(filepath, "wb");
    if (!file) {
        perror("[ERROR] Failed to open file for writing");
        SSL_write(ssl, "File upload failed\n", strlen("File upload failed\n"));
        return;
    }

    // 接收檔案大小
    if (SSL_read(ssl, &file_size, sizeof(file_size)) <= 0) {
        perror("[ERROR] Failed to receive file size");
        fclose(file);
        return;
    }
    file_size = ntohl(file_size);
    printf("[DEBUG] Receiving file: %s, size: %d bytes\n", filename, file_size);

    if (file_size == 0) {
        printf("[ERROR] Received file size=0. Aborting upload.\n");
        fclose(file);
        SSL_write(ssl, "File size is 0. Upload aborted\n", strlen("File size is 0. Upload aborted\n"));
        return;
    }

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
        printf("[UPLOAD] File '%s' uploaded successfully. Size=%d\n", filename, total_received);
        SSL_write(ssl, "File uploaded successfully\n", strlen("File uploaded successfully\n"));
    } else {
        printf("[UPLOAD] File '%s' upload incomplete. Received %d/%d bytes\n", filename, total_received, file_size);
        SSL_write(ssl, "File upload incomplete\n", strlen("File upload incomplete\n"));
    }
}

void handle_receive_file(SSL *ssl, char *buffer) {
    char filename[USERNAME_BUFFER_SIZE];
    char filepath[USERNAME_BUFFER_SIZE + 10];
    int file_size, bytes_read;
    FILE *file;

    sscanf(buffer + 13, "%s", filename); // "RECEIVE_FILE filename"
    snprintf(filepath, sizeof(filepath), "./store/%s", filename);

    file = fopen(filepath, "rb");
    if (!file) {
        printf("[ERROR] File '%s' not found in 'store' directory.\n", filename);
        SSL_write(ssl, "File not found\n", strlen("File not found\n"));
        return;
    }

    // 取得檔案大小
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    int net_file_size = htonl(file_size);
    SSL_write(ssl, &net_file_size, sizeof(net_file_size));
    printf("[DEBUG] Sending file: %s, size: %d bytes\n", filename, file_size);

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
        printf("[DOWNLOAD] File '%s' downloaded successfully. Size=%d\n", filename, total_sent);
        SSL_write(ssl, "File download complete\n", strlen("File download complete\n"));
    } else {
        printf("[DOWNLOAD] File '%s' download incomplete. Sent=%d/%d\n", filename, total_sent, file_size);
        SSL_write(ssl, "File download incomplete\n", strlen("File download incomplete\n"));
    }
}

// ========== SSL 初始化及配置 ==========

SSL_CTX *create_context() {
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

void configure_context(SSL_CTX *ctx) {
    if (SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

// ========== 輔助函式 ==========

int is_user_online(const char *username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].ssl != NULL && strcmp(clients[i].username, username) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return 0;
}

int username_exists_in_db(const char *username) {
    FILE *file = fopen("user_db", "r");
    if (!file) {
        perror("Failed to open user_db");
        return 0;
    }

    char stored_username[USERNAME_BUFFER_SIZE];
    char stored_password[USERNAME_BUFFER_SIZE];
    while (fscanf(file, "%s %s", stored_username, stored_password) != EOF) {
        if (strcmp(stored_username, username) == 0) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

void log_user_login(const char *username) {
    time_t now = time(NULL);
    char *timestamp = ctime(&now);
    timestamp[strcspn(timestamp, "\n")] = 0;
    printf("[LOGIN] User '%s' logged in at %s\n", username, timestamp);
}

// ========== 客戶端管理 ==========

void add_client(const char *username, SSL *ssl) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].ssl == NULL) {
            strncpy(clients[i].username, username, USERNAME_BUFFER_SIZE);
            clients[i].ssl = ssl;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(SSL *ssl) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].ssl == ssl) {
            clients[i].ssl = NULL;
            bzero(clients[i].username, USERNAME_BUFFER_SIZE);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

SSL *find_client(const char *username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].ssl != NULL && strcmp(clients[i].username, username) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return clients[i].ssl;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

// ========== 訊息管理 ==========

void store_message(const char *sender, const char *receiver, const char *message) {
    pthread_mutex_lock(&messages_mutex);
    if (message_count < MAX_MESSAGES) {
        strncpy(messages[message_count].sender, sender, USERNAME_BUFFER_SIZE);
        strncpy(messages[message_count].receiver, receiver, USERNAME_BUFFER_SIZE);
        strncpy(messages[message_count].message, message, COMMAND_BUFFER_SIZE);
        message_count++;
    }
    pthread_mutex_unlock(&messages_mutex);
}

void get_messages(const char *username, char *output) {
    pthread_mutex_lock(&messages_mutex);
    output[0] = '\0';
    for (int i = 0; i < message_count; i++) {
        if (strcmp(messages[i].receiver, username) == 0) {
            char msg_buffer[COMMAND_BUFFER_SIZE + 50];
            snprintf(msg_buffer, sizeof(msg_buffer), "From %s: %s\n",
                     messages[i].sender, messages[i].message);
            strcat(output, msg_buffer);

            // 將已讀的消息往前覆蓋
            for (int j = i; j < message_count - 1; j++) {
                messages[j] = messages[j + 1];
            }
            message_count--;
            i--;
        }
    }
    pthread_mutex_unlock(&messages_mutex);
}

// ========== 客戶端連線執行緒 ==========

void *client_handler(void *arg) {
    SSL *ssl = (SSL *)arg;
    char buffer[COMMAND_BUFFER_SIZE];
    char command[COMMAND_BUFFER_SIZE];
    char username[USERNAME_BUFFER_SIZE];
    int logged_in = 0;

    bzero(username, sizeof(username));

    while (1) {
        bzero(buffer, sizeof(buffer));
        int bytes_received = SSL_read(ssl, buffer, sizeof(buffer));
        if (bytes_received <= 0) {
            if (logged_in) {
                printf("Client %s disconnected.\n", username);
                remove_client(ssl);
            } else {
                printf("Anonymous client disconnected.\n");
            }
            break;
        }

        sscanf(buffer, "%s", command);
        printf("[DEBUG] Received command: %s\n", command);

        if (strcmp(command, "REGISTER") == 0) {
            char reg_username[USERNAME_BUFFER_SIZE];
            char reg_password[USERNAME_BUFFER_SIZE];
            sscanf(buffer, "REGISTER %s %s", reg_username, reg_password);

            if (username_exists_in_db(reg_username)) {
                SSL_write(ssl, "Username already exists\n", strlen("Username already exists\n"));
            } else {
                FILE *file = fopen("user_db", "a");
                if (!file) {
                    perror("Failed to open user_db");
                    SSL_write(ssl, "Registration failed\n", strlen("Registration failed\n"));
                } else {
                    fprintf(file, "%s %s\n", reg_username, reg_password);
                    fclose(file);
                    printf("[REGISTER] New user: %s\n", reg_username);
                    SSL_write(ssl, "Registration successful\n", strlen("Registration successful\n"));
                }
            }

        } else if (strcmp(command, "LOGIN") == 0) {
            char tmp_user[USERNAME_BUFFER_SIZE];
            char tmp_pass[USERNAME_BUFFER_SIZE];
            int ret = sscanf(buffer, "LOGIN %s %s", tmp_user, tmp_pass);
            if (ret < 2) {
                SSL_write(ssl, "Login command parse error\n", strlen("Login command parse error\n"));
                continue;
            }
            if (is_user_online(tmp_user)) {
                SSL_write(ssl, "User already logged in\n", strlen("User already logged in\n"));
            } else {
                add_client(tmp_user, ssl);
                log_user_login(tmp_user);
                SSL_write(ssl, "Login successful\n", strlen("Login successful\n"));
                logged_in = 1;
                strncpy(username, tmp_user, USERNAME_BUFFER_SIZE);
            }

        } else if (strcmp(command, "RETRIEVE") == 0) {
            char output[COMMAND_BUFFER_SIZE * 10];
            get_messages(username, output);
            if (strlen(output) == 0) {
                strcpy(output, "No new messages\n");
            }
            SSL_write(ssl, output, strlen(output));

        } else if (strcmp(command, "ONLINE") == 0) {
            char online_users[COMMAND_BUFFER_SIZE];
            online_users[0] = '\0';
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].ssl != NULL && strcmp(clients[i].username, username) != 0) {
                    strcat(online_users, clients[i].username);
                    strcat(online_users, "\n");
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (strlen(online_users) == 0) {
                strcpy(online_users, "No other users online.\n");
            }
            SSL_write(ssl, online_users, strlen(online_users));

        } else if (strcmp(command, "LOGOUT") == 0) {
            printf("Client %s logged out.\n", username);
            remove_client(ssl);
            bzero(username, sizeof(username));
            logged_in = 0;
            SSL_write(ssl, "Logged out successfully\n", strlen("Logged out successfully\n"));

        } else if (strcmp(command, "exit") == 0) {
            printf("Client %s disconnected.\n", username);
            remove_client(ssl);
            break;

        // ========== 關鍵：先檢查 SEND_FILE，再檢查 SEND ==========
        } else if (strncmp(command, "SEND_FILE", 9) == 0) {
            handle_send_file(ssl, buffer);

        } else if (strncmp(command, "SEND", 4) == 0) {
            // SEND <target_username> <message...>
            char target_username[USERNAME_BUFFER_SIZE];
            char msg_content[COMMAND_BUFFER_SIZE];
            sscanf(buffer, "SEND %s %[^\n]", target_username, msg_content);

            SSL *target_ssl = find_client(target_username);
            if (target_ssl != NULL) {
                store_message(username, target_username, msg_content);
                SSL_write(ssl, "Message sent\n", strlen("Message sent\n"));
            } else {
                SSL_write(ssl, "Target user not found\n", strlen("Target user not found\n"));
            }

        } else if (strcmp(command, "LIST_FILES") == 0) {
            handle_list_files(ssl);

        } else if (strncmp(command, "RECEIVE_FILE", 12) == 0) {
            handle_receive_file(ssl, buffer);

        } else if (strncmp(command, "STREAM_VIDEO", 12) == 0) {
            // 此處處理影片串流
            handle_video_stream(ssl);

        } else {
            SSL_write(ssl, "Unknown command\n", strlen("Unknown command\n"));
        }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    pthread_exit(NULL);
}

// ========== 主程式入口 ==========

int main() {
    int sockfd, connfd;
    struct sockaddr_in servaddr, cli;
    SSL_CTX *ctx;

    ensure_store_directory(); // 確保有 store/ 目錄
    ctx = create_context();
    configure_context(ctx);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&servaddr, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(PORT);

    bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(sockfd, 5);

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        socklen_t len = sizeof(cli);
        connfd = accept(sockfd, (struct sockaddr *)&cli, &len);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, connfd);

        if (SSL_accept(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            close(connfd);
            SSL_free(ssl);
            continue;
        }

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, client_handler, ssl);
        pthread_detach(thread_id);
    }

    close(sockfd);
    SSL_CTX_free(ctx);
    return 0;
}
