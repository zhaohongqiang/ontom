#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "config.h"
#include "log.h"
#include "error.h"
#include "blueprint.h"

static int uart4_bp_evt_handle(struct bp_uart *self, BP_UART_EVENT evt,
                     struct bp_evt_param *param);

struct bp_uart uarts[] = {
    {"/dev/ttyO4", -1, BP_FRAME_UNSTABLE, 0, uart4_bp_evt_handle},
    {"/dev/ttyO5", -1, BP_FRAME_UNSTABLE, 0, NULL}
};

#define GPIO_TO_PIN(bank, gpio)	(32 * (bank) + (gpio))
#define	SERIAL4_CTRL_PIN	GPIO_TO_PIN(0, 19)
#define	SERIAL5_CTRL_PIN	GPIO_TO_PIN(0, 20)

#define	RX_LOW_LEVEL			0
#define	TX_HIGH_LEVEL			1

static unsigned long arg_send_pin = 0, arg_rec_pin = 0;
static int fd_rec = -1, fd_send = -1;
FILE *fp = NULL;

int configure_uart(int fd, int speed, int databits, int stopbits, int parity)
{
    int   status;
    struct termios options;

    tcgetattr(fd, &options);
    tcflush(fd, TCIOFLUSH);
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);

    options.c_cflag &= ~CSIZE;

    switch (databits) {
        case 7:
            options.c_cflag |= CS7;
            break;
        case 8:
            options.c_cflag |= CS8;
            break;
        default:
            options.c_cflag |= CS8;
            break;
    }

    switch (parity) {
        case 'n':
        case 'N':
            options.c_cflag &= ~PARENB;   /* Clear parity enable */
            options.c_iflag &= ~INPCK;     /* Enable parity checking */
            break;
        case 'o':
        case 'O':
        case 1:
            options.c_cflag |= (PARODD | PARENB);
            options.c_iflag |= INPCK;             /* Disnable parity checking */
            break;
        case 'e':
        case 'E':
        case 2:
            options.c_cflag |= PARENB;     /* Enable parity */
            options.c_cflag &= ~PARODD;
            options.c_iflag |= INPCK;      /* Disnable parity checking */
            break;
        case 'S':
        case 's':  /*as no parity*/
        case 0:
            options.c_cflag &= ~PARENB;
            options.c_cflag &= ~CSTOPB;
            break;
        default:
            options.c_cflag &= ~PARENB;   /* Clear parity enable */
            options.c_iflag &= ~INPCK;     /* Enable parity checking */
            break;

    }

    switch (stopbits) {
        case 1:
            options.c_cflag |= CSTOPB;
            break;
        case 2:
            options.c_cflag &= ~CSTOPB;
            break;
        default:
            options.c_cflag &= ~CSTOPB;
            break;
    }

    /* Set input parity option */
    if (parity != 'n')
        options.c_iflag |= INPCK;

    //options.c_cflag   |= CRTSCTS;
    options.c_iflag &=~(IXON | IXOFF | IXANY);
    options.c_iflag &=~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    //options.c_lflag |= ISIG;

    tcflush(fd,TCIFLUSH);
    options.c_oflag = 0;
    //options.c_lflag = 0;
    options.c_cc[VTIME] = 15; 						// delay 15 seconds
    options.c_cc[VMIN] = 0; 						// Update the options and do it NOW

    status = tcsetattr(fd, TCSANOW, &options);
    if  (status != 0) {
        log_printf(ERR, "tcsetattr fd");
        return ERR_UART_CONFIG_FAILE;
    }

    tcflush(fd, TCIOFLUSH);
    return ERR_OK;
}

/********************************************************
Name:				gpio_export
Description:		导出所需要控制的GPIO的引脚编号
Input:
Return:
*********************************************************/
void gpio_export(int pin)
{
    // 导出所需要设置的GPIO口
    fp = fopen("/sys/class/gpio/export","w");
    rewind(fp);
    fprintf(fp, "%d\n", pin);
    fclose(fp);

    return;
}

/********************************************************
Name:				gpio_unexport
Description:		释放所需要控制的GPIO的引脚编号
Input:
Return:
*********************************************************/
void gpio_unexport(int pin)
{
    // 释放导出的GPIO口
    fp = fopen("/sys/class/gpio/unexport","w");
    rewind(fp);
    fprintf(fp, "%d\n", pin);
    fclose(fp);

    return;
}

/***********************************************************
Name:				set_gpio_output
Description:		设置gpio引脚为输出，并设定输出值
Input:
    1、pin:			gpio引脚号
    2、value:		0:低电平 1:高电平
Return:				0
************************************************************/
int set_gpio_output(int pin, int value)
{
    char file[40], direction[5];

    sprintf(file, "/sys/class/gpio/gpio%d/direction", pin);
    fp = fopen(file, "w");

    sprintf(direction,"out");

    rewind(fp);
    if (fprintf(fp, "%s",direction) < 0)
        return -2;

    fclose(fp);

    sprintf(file, "/sys/class/gpio/gpio%d/value", pin);
    fp = fopen(file, "w");
    rewind(fp);
    fprintf(fp, "%d\n", value);
    fclose(fp);

    return 0;
}

/*
 * 串口事件响应函数
 */
static int uart4_bp_evt_handle(struct bp_uart *self, BP_UART_EVENT evt,
                     struct bp_evt_param *param)
{
    int ret;
    switch ( evt ) {
    // 串口数据结构初始化
    case BP_EVT_INIT:
        break;
    // 串口配置
    case BP_EVT_CONFIGURE:
        self->dev_handle = open(self->dev_name, O_RDWR);
        if ( self->dev_handle == -1 ) {
            return ERR_UART_OPEN_FAILE;
        }
        ret = configure_uart(self->dev_handle, B9600, 8, 1, 0);
        if ( ret == ERR_UART_CONFIG_FAILE ) {
            log_printf(ERR, "configure uart faile.");
            return ERR_UART_CONFIG_FAILE;
        }
        gpio_export(SERIAL4_CTRL_PIN);
        self->status = BP_UART_STAT_RD;
        break;
    // 关闭串口
    case BP_EVT_KILLED:
        close(self->dev_handle);
        gpio_unexport(SERIAL4_CTRL_PIN);
        break;
    // 串口数据帧校验
    case BP_EVT_FRAME_CHECK:
        break;
    // 切换到发送模式
    case BP_EVT_SWITCH_2_TX:
        set_gpio_output(SERIAL4_CTRL_PIN, TX_HIGH_LEVEL);
        self->status = BP_UART_STAT_WR;
        break;
    // 切换到接收模式
    case BP_EVT_SWITVH_2_RX:
        set_gpio_output(SERIAL4_CTRL_PIN, RX_LOW_LEVEL);
        self->status = BP_UART_STAT_RD;
        break;

    // 串口接收到新数据
    case BP_EVT_RX_DATA:
        break;
    // 串口收到完整的数据帧
    case BP_EVT_RX_FRAME:
        break;

    // 串口发送数据请求
    case BP_EVT_TX_FRAME_REQUEST:
        break;
    // 串口发送确认
    case BP_EVT_TX_FRAME_CONFIRM:
        break;
    // 串口数据发送完成事件
    case BP_EVT_TX_FRAME_DONE:
        break;

    // 串口接收单个字节超时，出现在接收帧的第一个字节
    case BP_EVT_RX_BYTE_TIMEOUT:
        break;
    // 串口接收帧超时, 接受的数据不完整
    case BP_EVT_RX_FRAME_TIMEOUT:
        break;

    // 串口IO错误
    case BP_EVT_IO_ERROR:
        break;
    // 帧校验失败
    case BP_EVT_FRAME_CHECK_ERROR:
        break;
    }
    return ERR_OK;
}

void *thread_uart_service(void *arg) ___THREAD_ENTRY___
{
    int *done = (int *)arg;
    int mydone = 0, ret;
    struct bp_uart *thiz = &uarts[0];
    fd_set rd_set, wr_set;
    struct timeval tv;
    int retval, max_handle = 0;

    FD_ZERO(&rd_set);
    FD_ZERO(&wr_set);
    if ( done == NULL ) done = &mydone;

    if ( thiz ) {
        // 出错误后尝试的次数
        thiz->init_magic = 5;
    }

    while ( ! *done ) {
        usleep(5000);
        if ( thiz == NULL ) continue;
        if ( thiz->bp_evt_handle == NULL ) continue;
        if ( thiz->status == BP_UART_STAT_ALIENT ) continue;

        if ( thiz->status == BP_UART_STAT_INVALID ) {
            if ( thiz->init_magic <= 0 ) {
                thiz->status = BP_UART_STAT_ALIENT;
                log_printf(ERR, "open UART faile, thread panic.....");
                continue;
            }

            // 初始化数据结构, 设定串口的初始状态
            // 串口的初始状态决定了串口的工作模式
            thiz->bp_evt_handle(thiz, BP_EVT_INIT, NULL);

            // 打开并配置串口
            // 如果初始化失败，则会不断的尝试
            ret = thiz->bp_evt_handle(thiz, BP_EVT_CONFIGURE, NULL);
            if ( ret == ERR_UART_OPEN_FAILE ) {
                thiz->status = BP_UART_STAT_INVALID;
                log_printf(ERR, "try open %s faile.", thiz->dev_name);
                thiz->init_magic --;
                continue;
            }
            if ( ret == ERR_UART_CONFIG_FAILE ) {
                thiz->status = BP_UART_STAT_INVALID;
                // 首先关闭串口，然后才能进行下一次尝试
                thiz->bp_evt_handle(thiz, BP_EVT_KILLED, NULL);
                log_printf(ERR, "configure %s faile.", thiz->dev_name);
                thiz->init_magic --;
                continue;
            }

            if ( thiz->status != BP_UART_STAT_RD &&
                 thiz->status != BP_UART_STAT_WR ) {
                // 默认采用被动方式
                thiz->status = BP_UART_STAT_RD;
            }

            ret = ERR_ERR;
            if ( thiz->status == BP_UART_STAT_RD ) {
                int trynr = 0;
                do {
                    usleep(50000);
                    ret = thiz->bp_evt_handle(thiz, BP_EVT_SWITVH_2_RX, NULL);
                } while ( ret != ERR_OK && trynr ++ < 100 );
            }
            if ( thiz->status == BP_UART_STAT_WR ) {
                int trynr = 0;
                do {
                    usleep(50000);
                    ret = thiz->bp_evt_handle(thiz, BP_EVT_SWITCH_2_TX, NULL);
                } while ( ret != ERR_OK && trynr ++ < 100  );
            }
            if ( ret != ERR_OK ) {
                thiz->bp_evt_handle(thiz, BP_EVT_KILLED, NULL);
                thiz->status = BP_UART_STAT_ALIENT;
                log_printf(ERR, "switch to defaute mode faile, thread panic..");
                continue;
            }

            if ( thiz->dev_handle > max_handle ) {
                max_handle = thiz->dev_handle;
            }

            FD_SET(thiz->dev_handle, &rd_set);
            FD_SET(thiz->dev_handle, &wr_set);

            log_printf(INF, "open UART %s correct.", thiz->dev_name);
            continue;
        }

        tv.tv_sec  = 1;
        tv.tv_usec = 100 * 1000; // 100 ms.
        retval =
            select(max_handle, &rd_set, NULL, NULL, &tv);
        if ( retval == -1 ) {
            log_printf(DBG_LV0, "toto..");
            continue;
        } else {
            static int i = 0;
            if ( i ++ % 100 == 0 )
                log_printf(DBG_LV1, "select fetched <%d>", retval);
        }

        if ( thiz->status == BP_UART_STAT_RD ) {
            retval = FD_ISSET(thiz->dev_handle, &rd_set);
            if ( retval ) {
                char buff[32] = {0};
                if ( read(thiz->dev_handle, buff, 32) > 0 ) {
                    log_printf(DBG_LV1, "<%s>", buff);
                }
            } else {
                static int i = 0;
                if ( i ++ % 100 == 0 )
                    log_printf(DBG_LV1, "not fetch rd_set <%d>", retval);
            }
            continue;
        }

        if ( thiz->status == BP_UART_STAT_WR ) {
            retval = FD_ISSET(thiz->dev_handle, &wr_set);
            if ( retval ) {
                write(thiz->dev_handle, "0123456789", 11);
                thiz->bp_evt_handle(thiz, BP_EVT_SWITVH_2_RX, NULL);
                thiz->status = BP_UART_STAT_WR;
            } else {
                static int i = 0;
                if ( i ++ % 100 == 0 )
                    log_printf(DBG_LV1, "not fetch wr_set <%d>", retval);
            }
            continue;
        }
    }
}
