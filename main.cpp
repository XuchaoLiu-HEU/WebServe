#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));  // memset函数将sa结构体中的所有成员都初始化为0
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);  // 表示在执行处理函数期间屏蔽所有信号。
    assert(sigaction(sig, &sa, NULL) != -1);
}

int main(int argc, char* argv[]) {

    if (argc <= 1) {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);
    // 将 SIGPIPE 信号的处理方式设置为忽略，
    // 即当进程向一个已经关闭写端的 socket 发送数据时，不会触发 SIGPIPE 信号，从而避免程序异常终止。
    addsig(SIGPIPE, SIG_IGN);

    // 主线程创建一个线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    }
    catch (...) {
        return 1;
    }

    // 创建一个储存http连接对象的数组
    http_conn* users = new http_conn[MAX_FD];

    // 创建socket、端口复用、绑定、开始监听
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));    // 端口复用
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    ret = listen(listenfd, 5);

    // 创建事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    // 创建epoll对象
    int epollfd = epoll_create(5);

    // 添加socket监听文件描述符到epoll对象中，epoll监听 EPOLLIN | EPOLLRDHUP事件
    addfd(epollfd, listenfd, false);
    // 每一个http连接都被注册到同一个epoll内核事件中
    http_conn::m_epollfd = epollfd;

    while (true) {
        // 循环等待事件发生
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);

        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            // 发生改变的文件描述符
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd) {
                // epoll监听到socket文件描述符来了新连接事件
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                // 通信的文件描述符
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }
                // 初始化一个 http 连接
                users[connfd].init(connfd, client_address);
            }

            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 对方异常断开, 关闭连接
                users[sockfd].close_conn();
            }

            else if (events[i].events & EPOLLIN) {
                // 读事件发生
                if (users[sockfd].read()) {
                    // 一次性把所有数据读完
                    pool->append(users + sockfd);
                }
                else {
                    // 读失败
                    users[sockfd].close_conn();
                }

            }
            else if (events[i].events & EPOLLOUT) {

                if (!users[sockfd].write()) {  // 一次性写完所有数据
                    // 写失败
                    users[sockfd].close_conn();
                }

            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}