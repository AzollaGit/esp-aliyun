/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2015 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP8266 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include "iot_export_ota.h"
#include "esp_common.h"
#include "lwip/mem.h"
#include "aliyun_config.h"
#include "ota.h"
#include "aliyun_port.h"

uint32 download_length = 0;
os_timer_t upgrade_timer;

bool http_200_check = false;
bool resp_body_start = false;
int content_len = 0;

extern int got_ip_flag;
struct upgrade_param *upgrade;

struct upgrade_param {
    uint32 fw_bin_addr;
    uint16 fw_bin_sec;
    uint16 fw_bin_sec_num;
    uint16 fw_bin_sec_earse;
    uint8 extra;
    uint8 save[4];
    uint8 *buffer;
};

LOCAL bool OUT_OF_RANGE(uint16 erase_sec)
{
    uint8 spi_size_map = system_get_flash_size_map();
    uint16 sec_num = 0;
    uint16 start_sec = 0;

    if (spi_size_map == FLASH_SIZE_8M_MAP_512_512 ||
            spi_size_map == FLASH_SIZE_16M_MAP_512_512 ||
            spi_size_map == FLASH_SIZE_32M_MAP_512_512) {
        start_sec = (system_upgrade_userbin_check() == USER_BIN2) ? 1 : 129;
        sec_num = 123;
    } else if (spi_size_map == FLASH_SIZE_16M_MAP_1024_1024 ||
               spi_size_map == FLASH_SIZE_32M_MAP_1024_1024) {
        start_sec = (system_upgrade_userbin_check() == USER_BIN2) ? 1 : 257;
        sec_num = 251;
    } else {
        start_sec = (system_upgrade_userbin_check() == USER_BIN2) ? 1 : 65;
        sec_num = 59;
    }

    if ((erase_sec >= start_sec) && (erase_sec <= (start_sec + sec_num))) {
        return false;
    } else {
        return true;
    }
}


/******************************************************************************
 * FunctionName : user_upgrade_internal
 * Description  : a
 * Parameters   :
 * Returns      :
*******************************************************************************/
LOCAL bool system_upgrade_internal(struct upgrade_param *upgrade, uint8 *data, u32 len)
{
    bool ret = false;
    uint16 secnm = 0;

    if (data == NULL || len == 0) {
        return true;
    }

    /*got the sumlngth,erase all upgrade sector*/
    if (len > SPI_FLASH_SEC_SIZE) {
        upgrade->fw_bin_sec_earse = upgrade->fw_bin_sec;

        secnm = ((upgrade->fw_bin_addr + len) >> 12) + (len & 0xfff ? 1 : 0);

        while (upgrade->fw_bin_sec_earse != secnm) {
            taskENTER_CRITICAL();

            if (OUT_OF_RANGE(upgrade->fw_bin_sec_earse)) {
                printf("fw_bin_sec_earse:%d, Out of range\n", upgrade->fw_bin_sec_earse);
                break;

            } else {
                spi_flash_erase_sector(upgrade->fw_bin_sec_earse);
                upgrade->fw_bin_sec_earse++;
            }

            taskEXIT_CRITICAL();
            vTaskDelay(10 / portTICK_RATE_MS);
        }

        printf("flash erase over\n");
        return true;
    }

    upgrade->buffer = (uint8 *)zalloc(len + upgrade->extra);

    memcpy(upgrade->buffer, upgrade->save, upgrade->extra);
    memcpy(upgrade->buffer + upgrade->extra, data, len);

    len += upgrade->extra;
    upgrade->extra = len & 0x03;
    len -= upgrade->extra;

    if (upgrade->extra <= 4) {
        memcpy(upgrade->save, upgrade->buffer + len, upgrade->extra);
    } else {
        printf("ERR3:arr_overflow,%u,%d\n", __LINE__, upgrade->extra);
    }

    do {
        if (upgrade->fw_bin_addr + len >= (upgrade->fw_bin_sec + upgrade->fw_bin_sec_num) * SPI_FLASH_SEC_SIZE) {
            printf("spi_flash_write exceed\n");
            break;
        }

        if (spi_flash_write(upgrade->fw_bin_addr, (uint32 *)upgrade->buffer, len) != SPI_FLASH_RESULT_OK) {
            printf("spi_flash_write failed\n");
            break;
        }

        ret = true;
        upgrade->fw_bin_addr += len;
    } while (0);

    free(upgrade->buffer);
    upgrade->buffer = NULL;
    return ret;
}

/******************************************************************************
 * FunctionName : system_get_fw_start_sec
 * Description  : a
 * Parameters   :
 * Returns      :
*******************************************************************************/
uint16 system_get_fw_start_sec()
{
    if (upgrade != NULL) {
        return upgrade->fw_bin_sec;
    } else {
        return 0;
    }
}

/******************************************************************************
 * FunctionName : user_upgrade
 * Description  : a
 * Parameters   :
 * Returns      :
*******************************************************************************/
bool system_upgrade(uint8 *data, uint32 len)
{
    bool ret;
    ret = system_upgrade_internal(upgrade, data, len);
    return ret;
}

void upgrade_recycle(void)
{
    printf("upgrade recycle\n");
    download_length = 0;
    http_200_check = false;
    resp_body_start = false;
    content_len = 0;

    system_upgrade_deinit();

    if (system_upgrade_flag_check() == UPGRADE_FLAG_FINISH) {
        system_upgrade_reboot(); // if need
    }

    return;
}

/******************************************************************************
 * FunctionName : system_upgrade_init
 * Description  : a
 * Parameters   :
 * Returns      :
*******************************************************************************/
void
system_upgrade_init(void)
{
    uint32 user_bin2_start, user_bin1_start;
    uint8 spi_size_map = system_get_flash_size_map();

    if (upgrade == NULL) {
        upgrade = (struct upgrade_param *)zalloc(sizeof(struct upgrade_param));
    }

    user_bin1_start = 1;

    if (spi_size_map == FLASH_SIZE_8M_MAP_512_512 ||
            spi_size_map == FLASH_SIZE_16M_MAP_512_512 ||
            spi_size_map == FLASH_SIZE_32M_MAP_512_512) {
        user_bin2_start = 129;
        upgrade->fw_bin_sec_num = 123;
    } else if (spi_size_map == FLASH_SIZE_16M_MAP_1024_1024 ||
               spi_size_map == FLASH_SIZE_32M_MAP_1024_1024) {
        user_bin2_start = 257;
        upgrade->fw_bin_sec_num = 251;
    } else {
        user_bin2_start = 65;
        upgrade->fw_bin_sec_num = 59;
    }

    upgrade->fw_bin_sec = (system_upgrade_userbin_check() == USER_BIN1) ? user_bin2_start : user_bin1_start;

    upgrade->fw_bin_addr = upgrade->fw_bin_sec * SPI_FLASH_SEC_SIZE;

    upgrade->fw_bin_sec_earse = upgrade->fw_bin_sec;
}

/******************************************************************************
 * FunctionName : system_upgrade_deinit
 * Description  : a
 * Parameters   :
 * Returns      :
*******************************************************************************/
void
system_upgrade_deinit(void)
{
    if (upgrade != NULL) {
        free(upgrade);
        upgrade = NULL;
    }

    printf("ota end, free heap size:%d\n", system_get_free_heap_size());
    return;
}

/*read buffer by byte still delim ,return read bytes counts*/
static int read_until(char *buffer, char delim, int len)
{
//  /*TODO: delim check,buffer check,further: do an buffer length limited*/
    int i = 0;

    while (buffer[i] != delim && i < len) {
        ++i;
    }

    return i + 1;
}

bool read_past_http_header(char text[], int total_len)
{
    /* i means current position */
    int i = 0, i_read_len = 0;
    char *ptr = NULL, *ptr2 = NULL;
    char length_str[32] = {0};

    while (text[i] != 0 && i < total_len) {
        if (content_len == 0 && (ptr = (char *)strstr(text, "Content-Length")) != NULL) {
            ptr += 16;
            ptr2 = (char *)strstr(ptr, "\r\n");
            memset(length_str, 0, sizeof(length_str));
            memcpy(length_str, ptr, ptr2 - ptr);
            content_len = atoi(length_str);
            printf("parse Content-Length:%d\n", content_len);
        }

        i_read_len = read_until(&text[i], '\n', total_len);

        // if resolve \r\n line, http header is finished
        if (i_read_len == 2) {
            if (content_len == 0) {
                print_error("did not parse Content-Length item");
            }

            // erase flash when first flash, for save new bin
            if (false == system_upgrade(text, content_len)) {
                system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
                upgrade_recycle();
            }

            // the valid left http body length
            int i_write_len = total_len - (i + 2);

            if (false == system_upgrade(&(text[i + 2]), i_write_len)) {
                system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
                upgrade_recycle();
                print_error("system upgrade error");
            }

            download_length += i_write_len;
            printf("first download len:%d\n", download_length);
            return true;
        }

        i += i_read_len;
    }

    return false;
}

/******************************************************************************
 * FunctionName : upgrade_download
 * Description  : parse http response ,and download remote data and write in flash
 * Parameters   : int sta_socket : ota client socket fd
 *                char *pusrdata : remote data
 *                length         : data length
 * Returns      : none
*******************************************************************************/
void upgrade_download(int sta_socket, char *pusrdata, unsigned short length)
{
    char *ptr = NULL;
    char *ptmp2 = NULL;
    char lengthbuffer[32];

    // first response should include state code:200
    if (!http_200_check && strstr(pusrdata, "200") == NULL) {
        print_error("ota url is invalid or bin is not exist");
    }

    http_200_check = true;

    if (!resp_body_start) {
        // deal with http header
        resp_body_start = read_past_http_header(pusrdata, length);
        return;
    }

    // deal with http body
    // http transmit body more than content-length occasionally
    // default bin size = content-length, throw up the other http body
    if (download_length + length > content_len) {
        length = content_len - download_length;
        download_length = content_len;
    } else {
        download_length += length;
    }

    printf("downloaded len:%d\n", download_length);

    // save http body(bin) to flash
    if (false == system_upgrade(pusrdata, length)) {
        system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
        upgrade_recycle();
    }

    return;
}

/******************************************************************************
 * FunctionName : local_ota_begin
 * Description  : ota_task function
 * Parameters   : task param
 * Returns      : none
*******************************************************************************/
void local_ota_begin()
{
    int read_bytes;
    int sin_size;
    int sta_socket;
    char recv_buf[1460];
    uint8 user_bin[21] = {0};
    struct sockaddr_in remote_ip;
    printf("Hello, welcome to local ota!\r\n");
    printf("ota server addr %s port %d\n", LOCAL_OTA_SERVER_IP, LOCAL_OTA_SERVER_PORT);

    while (1) {
        sta_socket = socket(PF_INET, SOCK_STREAM, 0);

        if (-1 == sta_socket) {
            close(sta_socket);
            printf("socket fail !\r\n");
            continue;
        }

        printf("socket ok!\r\n");
        bzero(&remote_ip, sizeof(struct sockaddr_in));
        remote_ip.sin_family = AF_INET;

        remote_ip.sin_addr.s_addr = inet_addr(LOCAL_OTA_SERVER_IP);
        remote_ip.sin_port = htons(LOCAL_OTA_SERVER_PORT);

        if (0 != connect(sta_socket, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))) {
            close(sta_socket);
            printf("connect fail!\r\n");
            system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
            upgrade_recycle();
            continue;
        }

        printf("connect ok!\r\n");

        if (system_upgrade_userbin_check() == UPGRADE_FW_BIN1) {
            memcpy(user_bin, "user2.2048.new.5.bin", 21);

        } else if (system_upgrade_userbin_check() == UPGRADE_FW_BIN2) {
            memcpy(user_bin, "user1.2048.new.5.bin", 21);
        }

        /*send GET request to http server*/
        const char *GET_FORMAT =
            "GET %s HTTP/1.0\r\n"
            "Host: %s:%d\r\n"
            "User-Agent: esp8266-rtos-sdk/1.0 esp8266\r\n\r\n";

        char *http_request = NULL;
        int get_len = asprintf(&http_request, GET_FORMAT, user_bin, LOCAL_OTA_SERVER_IP, LOCAL_OTA_SERVER_PORT);

        if (get_len < 0) {
            printf("malloc memory failed.\n");
            system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
            upgrade_recycle();
            free(http_request);
            close(sta_socket);
            continue;
        }

        print_debug(http_request, strlen(http_request), "send http request");

        if (write(sta_socket, http_request, strlen(http_request) + 1) < 0) {
            close(sta_socket);
            printf("send fail\n");
            free(http_request);
            upgrade_recycle();
            continue;
        }

        printf("send success\n");
        free(http_request);

        while ((read_bytes = read(sta_socket , recv_buf, 1460)) >= 0) {
            if (read_bytes > 0) {
                upgrade_download(sta_socket, recv_buf, read_bytes);
            } else {
                printf("peer close socket\n");
                break;
            }

            // default bin size equal to content-length
            if (download_length == content_len && download_length != 0) {
                printf("upgrade file download finished, bin size:%d\n", download_length);

                if (upgrade_crc_check(system_get_fw_start_sec(), download_length) != true) {
                    printf("upgrade crc check failed !\n");
                    system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
                } else {
                    printf("bin check crc ok\n");
                    system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
                }

                upgrade_recycle();
            }
        }

        printf("read data fail! ret:%d\r\n", read_bytes);
        close(sta_socket);
        system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
        upgrade_recycle();
    }
}

// local_ota_task
void local_ota_task(void *pvParameter)
{
    printf("\nlocal OTA task started...\n");

    while (!got_ip_flag) {
        vTaskDelay(TASK_CYCLE / portTICK_RATE_MS);
        printf("wait for fetching IP...\n");
    }

    printf("ota begin, free heap size:%d\n", system_get_free_heap_size());
    system_upgrade_flag_set(UPGRADE_FLAG_START);
    system_upgrade_init();

    local_ota_begin();

    // OTA timeout
    os_timer_disarm(&upgrade_timer);
    os_timer_setfn(&upgrade_timer, (os_timer_func_t *)upgrade_recycle, NULL);
    os_timer_arm(&upgrade_timer, OTA_TIMEOUT, 0);
}
