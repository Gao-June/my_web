#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include "epoll_server.h"

#define MAXSIZE 2000

// 头文件 文件声明
int  init_listen_fd(int port, int epfd);		// 初始化用于监听的套接字
void epoll_run(int port);						// 启动epoll模型 ，启动后面一系列操作
void do_accept(int lfd, int epfd);				// 接受新连接处理
void do_read(int cfd, int epfd);	// 读数据
int  get_line(int sock, char *buf, int size);	// 解析http请求消息的每一行内容
void disconnect(int cfd, int epfd);	// 断开连接的函数
void http_request(const char* request, int cfd);// http请求处理
void send_respond_head(int cfd, int no, const char* desp, const char* type, long len);	// 发送响应头
void send_file(int cfd, const char* filename);	// 发送文件
void send_dir(int cfd, const char* dirname);	// 发送目录内容
void encode_str(char* to, int tosize, const char* from);
void decode_str(char *to, char *from);
const char *get_file_type(const char *name);	// 通过文件名获取文件的类型

int main(int argc, const char* argv[])
{
    if(argc < 3)
    {
        printf("eg: ./my_web.out  port  path\n");		// 端口、资源目录
        exit(1);
    }

    // 得到端口
    int port = atoi(argv[1]);
	
    // 修改进程的工作目录, 方便后续资源访问操作
    int ret = chdir(argv[2]);	
    if(ret == -1)
    {
        perror("chdir error");
        exit(1);
    }
    
    // 启动epoll模型 
    epoll_run(port);

    return 0;
}




void epoll_run(int port)
{
    // 创建一个epoll树的根节点 epfd
    int epfd = epoll_create( MAXSIZE );
    if(epfd == -1)
    {
        perror("epoll_create error\n\n");
        exit(1);
    }

    // 添加要监听的节点
    // 先添加监听lfd
    int lfd = init_listen_fd(port, epfd);

    // 委托内核检测添加到树上的节点。
    struct epoll_event all[MAXSIZE];
    while(1)
    {
        int ret = epoll_wait(epfd, all, MAXSIZE, -1);
        if(ret == -1)
        {
            perror("epoll_wait error\n\n");
            exit(1);
        }

        for(int i=0; i<ret; ++i)
        {
            struct epoll_event *pev = &all[i];
			
			
            if( !(pev->events & EPOLLIN) )
            {
				// 不是读事件
                continue;
            }
            if(pev->data.fd == lfd)
            {
                // 接受连接请求
                do_accept(lfd, epfd);
            }
            else
            {
                // 读数据	进行通信
                do_read(pev->data.fd, epfd);
            }
        }
    }
}

// 读数据，读客户端发来的数据。
void do_read(int cfd, int epfd)
{
    // 将浏览器发过来的数据, 读到buf中 
    char line[1024] = {0};
	
    // 读请求行
    int len = get_line(cfd, line, sizeof(line));
    
	if(len == 0)
    {
        printf("客户端断开了连接...\n\n");
		
        // 关闭套接字, cfd 从 epoll 上删除
        disconnect(cfd, epfd);         
    }
    else
    {
        printf("请求行数据: %s", line);
        printf("============= 请求头 ============\n");
		
        // 还有数据没读完
        // 继续读
        while(len)
        {
            char buf[1024] = {0};
            len = get_line(cfd, buf, sizeof(buf));
            printf("-----: %s", buf);
        }
        printf("============= The End ============\n");
    }

    // 请求行: get /xxx  http/1.1
    // 判断是不是get请求，	前三个字符是不是 get
    if(strncasecmp("get", line, 3) == 0)
    {
        // 处理http请求
        http_request(line, cfd);
		
        // 关闭套接字, cfd从epoll上del
        disconnect(cfd, epfd);         
    }
}


// 断开连接的函数
void disconnect(int cfd, int epfd)	// 关闭套接字, cfd 从 epoll 上 del
{
    int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
    if(ret == -1)
    {
        perror("epoll_ctl del cfd error\n");
        exit(1);
    }
    close(cfd);
}


// http请求处理
void http_request(const char* request, int cfd)
{
    // 函数主要做的事：拆分http请求行
    // get /xxx http/1.1	中间有空格，所以用正则表达式拆分
    char method[12], path[1024], protocol[12];		// 三个装拆分的数组
    sscanf(request, "%[^ ] %[^ ] %[^ ]", method, path, protocol);	// sscanf() 正则表达式

	// 检查是否正确
    printf("method = %s, path = %s, protocol = %s\n", method, path, protocol);

    // 转码 将不能识别的中文乱码 - > 中文
    // 解码 %23 %34 %5f 这种变成  中文汉字
    decode_str(path, path);			// 解码函数
	
    // 处理path  /xxx
    
    char* file = path+1;
	
		
    // 如果没有指定访问的资源, 默认显示当前资源目录中的内容
    if(strcmp(path, "/") == 0)
    {
        // file的值：资源目录的当前位置
        file = "./";
    }

    // 获取文件属性
    struct stat st;
    int ret = stat(file, &st);
    if(ret == -1)
    {
        // show 404 页面， 说明资源不存在
        send_respond_head(cfd, 404, "File Not Found", ".html", -1);
        send_file(cfd, "404.html");
    }

    // 判断是目录还是文件 
    if(S_ISDIR(st.st_mode))			// 是目录
    {
        // 发送头信息
        send_respond_head(cfd, 200, "OK", get_file_type(".html"), -1);	
		
        // 发送目录信息
        send_dir(cfd, file);
    }
    else if(S_ISREG(st.st_mode))	// 文件
    {
        
        // 发送消息报头
        send_respond_head(cfd, 200, "OK", get_file_type(file), st.st_size);
		
        // 发送文件内容
        send_file(cfd, file);
    }
}


// 发送目录内容
void send_dir(int cfd, const char* dirname)		// dirname 目录名字
{
    // 拼出一个html页面<table></table>	// table 多行多列
	
    char buf[4094] = {0};	// 4K
	
    sprintf(buf, "<html><head><title>Gaojun的Web服务器: %s</title></head>", dirname);
	
    sprintf(buf+strlen(buf), "<body><h1>当前目录: %s</h1><table>", dirname);	// h1 是标题

    char enstr[1024] = {0};
    char path[1024] = {0};
	
    // 目录项二级指针
    struct dirent** ptr;
		// num：复制到 ptr 数组中的目录的数据结构数目
    int num = scandir(dirname, &ptr, NULL, alphasort);	
	
    // 遍历， 在 ptr 中取目录数据就行
    for(int i=0; i<num; ++i)
    {
        char* name = ptr[i]->d_name;	

        // 拼接文件的完整路径
        sprintf(path, "%s/%s", dirname, name);
        printf("path = %s ===================\n", path);
        struct stat st;
        stat(path, &st);
		
		
        encode_str(enstr, sizeof(enstr), name);		// 进行编码的函数  
		
		// 开始判断 是 文件 ？ 目录 ？
        // 文件
        if(S_ISREG(st.st_mode))
        {
			// 接着拼接 <a > 超链接
            sprintf(buf+strlen(buf),  
                    "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                    enstr, name, (long)st.st_size);
        }
        // 目录
        else if(S_ISDIR(st.st_mode))
        {
            sprintf(buf+strlen(buf), 
                    "<tr><td><a href=\"%s/\">%s/</a></td><td>%ld</td></tr>",
                    enstr, name, (long)st.st_size);
        }
		
        send(cfd, buf, strlen(buf), 0);		// 把数据发出去
        memset(buf, 0, sizeof(buf));		
        
    }

    sprintf(buf+strlen(buf), "</table></body></html>");		
    send(cfd, buf, strlen(buf), 0);		//	信息全部发出去了

    printf("dir message send OK!!!!\n");
	   

}


// 发送响应头
// 对 浏览器 发任何数据之前都得先发送头，所以直接封装成函数
void send_respond_head(int cfd, int no, const char* desp, const char* type, long len)
{
    char buf[1024] = {0};	
	
    // 状态行
	// no : 状态码  desp ：对状态码的描述	
	// type ：Content-Type 告诉浏览器发送的数据是什么类型  len：发送数据的长度 (
    sprintf(buf, "http/1.1 %d %s\r\n", no, desp);	
    send(cfd, buf, strlen(buf), 0);
	
    // 消息报头
    sprintf(buf, "Content-Type:%s\r\n", type);
    sprintf(buf+strlen(buf), "Content-Length:%ld\r\n", len);	// buf+strlen(buf) 接到后面了
    send(cfd, buf, strlen(buf), 0);
	
    // 空行
    send(cfd, "\r\n", 2, 0);
}


// 发送文件
void send_file(int cfd, const char* filename)
{
    // 打开文件
    int fd = open(filename, O_RDONLY);
    if(fd == -1)
    {

        return;
    }

    // 循环读文件
    char buf[4096] = {0};	// 4k
    int len = 0;
    while( (len = read(fd, buf, sizeof(buf))) > 0 )
    {
        // 发送读出的数据
        send(cfd, buf, len, 0);
    }
	
	// 读完了
    if(len == -1)
    {
        perror("read file error\n\n");
        exit(1);
    }

    close(fd);
}


// 解析http请求消息的每一行内容
int get_line(int sock, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;
    while ((i < size - 1) && (c != '\n'))
    {
        n = recv(sock, &c, 1, 0);
        if (n > 0)
        {
            if (c == '\r')
            {
                n = recv(sock, &c, 1, MSG_PEEK);
                if ((n > 0) && (c == '\n'))
                {
                    recv(sock, &c, 1, 0);
                }
                else
                {
                    c = '\n';
                }
            }
            buf[i] = c;
            i++;
        }
        else
        {
            c = '\n';
        }
    }
    buf[i] = '\0';

    return i;	// 字符串长度
}


// 接受新连接处理	
void do_accept(int lfd, int epfd)		
{
    struct sockaddr_in client;
    socklen_t len = sizeof(client);
    int cfd = accept(lfd, (struct sockaddr*)&client, &len);
    if(cfd == -1)
    {
        perror("accept error");
        exit(1);
    }

    // 打印客户端信息
    char ip[64] = {0};
    printf("New Client IP: %s, Port: %d, cfd = %d\n",
           inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)),		
           ntohs(client.sin_port), cfd);									

    // 设置cfd为非阻塞
    int flag = fcntl(cfd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);

    // 得到的新节点挂到epoll树上
    struct epoll_event ev;
    ev.data.fd = cfd;
	
    // 边沿非阻塞模式 epoll ET
    ev.events = EPOLLIN | EPOLLET;
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
    if(ret == -1)
    {
        perror("epoll_ctl add cfd error");
        exit(1);
    }
}


// 初始化用于监听的套接字
int init_listen_fd(int port, int epfd)	
{
    //　创建监听的套接字
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if(lfd == -1)
    {
        perror("socket error");
        exit(1);
    }

    // lfd绑定本地IP和port
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    serv.sin_addr.s_addr = htonl(INADDR_ANY);

    // 端口复用
    int flag = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	
	// 绑定	（在端口复用后）
    int ret = bind(lfd, (struct sockaddr*)&serv, sizeof(serv));
    if(ret == -1)
    {
        perror("bind error");
        exit(1);
    }

    // 设置监听
    ret = listen(lfd, 64);
    if(ret == -1)
    {
        perror("listen error");
        exit(1);
    }

    // lfd 添加到 epoll 树上
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if(ret == -1)
    {
        perror("epoll_ctl add lfd error\n\n");
        exit(1);
    }

    return lfd;
}

// 16进制数转化为10进制
int hexit(char c)		
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

/*
 *  这里的内容是处理%20之类的东西！是"解码"过程。
 *  %20 URL编码中的‘ ’(space)
 *  %21 '!' %22 '"' %23 '#' %24 '$'
 *  %25 '%' %26 '&' %27 ''' %28 '('......
 *  相关知识html中的‘ ’(space)是&nbsp
 */
 // 编码函数
 // 保证 from 的原始性，将转换后的字符串保存到 to 中
void encode_str(char* to, int tosize, const char* from)		
{
    int tolen;

    for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) 
    {
		 	// isalnum() 判断是不是数字，这些数字不需要编码转换
			// /_.-~     这些特殊字符也不需要转换
        if (isalnum(*from) || strchr("/_.-~", *from) != (char*)0)	// 不需要转
        {
            *to = *from;
            ++to;
            ++tolen;
        } 
        else 	// 开始转码
        {
			// 得到 eg. %23  %04
            sprintf(to, "%%%02x", (int) *from & 0xff);	// 两个 %, 表示转义出%字符，就是一个 %了
            to += 3;
            tolen += 3;
        }

    }
    *to = '\0';
}

// 解码函数
// 解码 %23 %34 %5f 这种变成  中文汉字
void decode_str(char *to, char *from)		
{
    for ( ; *from != '\0'; ++to, ++from  ) 
    {
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) 
        { 

            *to = hexit(from[1])*16 + hexit(from[2]);

            from += 2;                      
        } 
        else
        {
            *to = *from;

        }

    }
    *to = '\0';

}


// 通过文件名获取文件的类型
const char *get_file_type(const char *name)
{
    char* dot;

    // 自右向左查找‘.’字符, 如不存在返回NULL
    dot = strrchr(name, '.');   
    if (dot == NULL)
        return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp( dot, ".wav" ) == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}


