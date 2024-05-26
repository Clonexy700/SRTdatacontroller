#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define usleep(x) Sleep(x / 1000)
#else
#include <unistd.h>
#endif

#include "srt/srt.h"

int main(int argc, char** argv)
{
    int ss, st;
    struct sockaddr_in sa;
    int yes = 1;
    const char message [] = "This message should be sent to the other side";

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    printf("srt startup\n");
    srt_startup();

    printf("srt socket\n");
    ss = srt_create_socket();
    if (ss == SRT_ERROR)
    {
        fprintf(stderr, "srt_socket: %s\n", srt_getlasterror_str());
        return 1;
    }

    printf("srt remote address\n");
    sa.sin_family = AF_INET;
    sa.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &sa.sin_addr) != 1)
    {
        return 1;
    }

    printf("srt setsockflag\n");
    if (SRT_ERROR == srt_setsockflag(ss, SRTO_SENDER, &yes, sizeof yes))
    {
        fprintf(stderr, "srt_setsockflag: %s\n", srt_getlasterror_str());
        return 1;
    }

    // Test deprecated
    //srt_setsockflag(ss, SRTO_STRICTENC, &yes, sizeof yes);

    printf("srt connect\n");
    st = srt_connect(ss, (struct sockaddr*)&sa, sizeof sa);
    if (st == SRT_ERROR)
    {
        fprintf(stderr, "srt_connect: %s\n", srt_getlasterror_str());
        return 1;
    }

    int i;
    for (i = 0; i < 100; i++)
    {
        printf("srt sendmsg2 #%d >> %s\n",i,message);
        st = srt_sendmsg2(ss, message, sizeof message, NULL);
        if (st == SRT_ERROR)
        {
            fprintf(stderr, "srt_sendmsg: %s\n", srt_getlasterror_str());
            return 1;
        }

        usleep(1000);   // 1 ms
    }


    sleep(1); // 1 second to give it enough time to receive all messages
    printf("srt close\n");
    st = srt_close(ss);
    if (st == SRT_ERROR)
    {
        fprintf(stderr, "srt_close: %s\n", srt_getlasterror_str());
        return 1;
    }

    printf("srt cleanup\n");
    srt_cleanup();
    return 0;
}