#include "aida64.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

#include "ui.h"
#include "lv_port.h"
#include "display_config.h"

static const char *TAG = "AIDA64";

extern void ui_set_connected(bool connected);

static TaskHandle_t aida64_task_handle = NULL;
static bool aida64_running = false;
static int aida64_sock = -1;
static bool is_connected = false;
static uint16_t s_port = 7789;
static char s_ip[64] = {0};

/* SIV 值缓存：SIV0-SIV31，每个最多 31 字符 */
#define MAX_SIV 32
#define SIV_VAL_LEN 32
static char s_siv[MAX_SIV][SIV_VAL_LEN];
static bool s_siv_valid[MAX_SIV];

/* 解析 SSE 数据中的所有 SIV 字段 */
static void parse_siv_data(const char *data, int len)
{
 const char *end = data + len;
 const char *p = data;

 while (p < end) {
 /* 找 "SIV" */
 p = strstr(p, "SIV");
 if (!p || p >= end) break;

 p += 3;
 /* 读 SIV 编号 */
 int num = 0;
 bool got_num = false;
 while (p < end && *p >= '0' && *p <= '9') {
 num = num * 10 + (*p - '0');
 p++;
 got_num = true;
 }
 if (!got_num || num < 0 || num >= MAX_SIV) continue;

 /* 跳到 '|' 后面 */
 while (p < end && *p != '|') p++;
 if (p >= end || *p != '|') continue;
 p++; /* 跳过 '|' */

 /* 读值直到 '{' 或 '\r' 或 '\n' 或 '|' */
 int vi = 0;
 while (p < end && *p != '{' && *p != '\r' && *p != '\n' && *p != '|' && vi < SIV_VAL_LEN - 1) {
 s_siv[num][vi++] = *p++;
 }
 s_siv[num][vi] = '\0';
 s_siv_valid[num] = true;

 ESP_LOGD(TAG, "SIV%d = %s", num, s_siv[num]);
 }
}

/* 按 display_config 映射推送到 UI */
static const char *unit_for_label(const char *label)
{
 if (strstr(label, "\xe4\xbd\xbf\xe7\x94\xa8\xe7\x8e\x87")) return "%";
 if (strstr(label, "\xe6\xb8\xa9\xe5\xba\xa6")) return "\xe2\x84\x83";
 if (strstr(label, "\xe5\x8a\x9f\xe7\x8e\x87")) return "W";
 if (strstr(label, "\xe9\xa2\x91\xe7\x8e\x87")) return "M";
 return "";
}

static void push_to_ui(void)
{
 display_config_t *cfg = ui_get_config();
 for (int row = 0; row < cfg->row_count && row < MAX_ROWS; row++) {
 const char *unit = unit_for_label(cfg->rows[row].label);
 for (int col = 0; col < 2; col++) {
 int siv = cfg->rows[row].siv[col];
 if (siv >= 0 && siv < MAX_SIV && s_siv_valid[siv]) {
 char vbuf[48];
 const char *raw = s_siv[siv];
 /* 若为纯数值（可带小数/负号）则取整；含冒号的时间等保留原样 */
 int is_num = (raw[0] != '\0');
 for (const char *pp = raw; *pp; pp++) {
 if (!((*pp >= '0' && *pp <= '9') || *pp == '.' || *pp == '-' || *pp == '+')) { is_num = 0; break; }
 }
 if (is_num) {
 double dv = atof(raw);
 long iv = (long)(dv < 0 ? dv - 0.5 : dv + 0.5);
 snprintf(vbuf, sizeof(vbuf), "%ld%s", iv, unit);
 } else {
 snprintf(vbuf, sizeof(vbuf), "%s%s", raw, unit);
 }
 ui_update_value(row, col, vbuf);
 }
 }
 }

 /* 内存：使用配置的 SIV 索引 */
 int mem_pct_siv = cfg->mem_siv_pct;
 int mem_used_siv = cfg->mem_siv_used;
 if (mem_pct_siv >= 0 && mem_pct_siv < MAX_SIV && mem_used_siv >= 0 && mem_used_siv < MAX_SIV
 && s_siv_valid[mem_pct_siv] && s_siv_valid[mem_used_siv]) {
 int pct = (int)(atof(s_siv[mem_pct_siv]) + 0.5);
 double used = atof(s_siv[mem_used_siv]);
 double total = 0;
 char memtxt[24];
 if (pct > 0) total = used * 100.0 / pct;
 if (pct < 0) pct = 0;
 if (pct > 100) pct = 100;
 if (total > 0.01)
 snprintf(memtxt, sizeof(memtxt), "%.1f/%.0fG", used, total);
 else
 snprintf(memtxt, sizeof(memtxt), "%.1fG", used);
 ui_update_mem(memtxt, pct);
 } else if (s_siv_valid[11]) {
 int pct = (int)(atof(s_siv[11]) + 0.5);
 if (pct < 0) pct = 0;
 if (pct > 100) pct = 100;
 ui_update_mem("--", pct);
 }
}

/* SSE 行缓冲 */
#define SSE_BUF_LEN 1024
static char sse_buf[SSE_BUF_LEN];
static size_t sse_len = 0;

static void sse_reset(void) { sse_len = 0; }

static void sse_feed(const char *chunk, int len)
{
 for (int i = 0; i < len && sse_len < SSE_BUF_LEN - 1; i++) {
 sse_buf[sse_len++] = chunk[i];
 }
 sse_buf[sse_len] = '\0';

 /* SSE 事件边界：空行 */
 if (sse_len >= 2 && (strstr(sse_buf, "\n\n") || strstr(sse_buf, "\r\n\r\n"))) {
 /* 跳过 HTTP 响应头（找到 data: 行） */
 char *data_line = strstr(sse_buf, "data:");
 if (data_line) {
 data_line += 5;
 while (*data_line == ' ') data_line++;
 parse_siv_data(data_line, sse_len - (data_line - sse_buf));
 push_to_ui();
 }
 sse_reset();
 } else if (sse_len >= SSE_BUF_LEN - 2) {
 /* 缓冲区溢出：尝试解析后重置 */
 char *data_line = strstr(sse_buf, "data:");
 if (data_line) {
 data_line += 5;
 while (*data_line == ' ') data_line++;
 parse_siv_data(data_line, sse_len - (data_line - sse_buf));
 push_to_ui();
 }
 sse_reset();
 }
}

static int aida64_socket_connect(const char *ip)
{
 struct sockaddr_in dest;
 memset(&dest, 0, sizeof(dest));
 dest.sin_len = sizeof(dest);
 dest.sin_family = AF_INET;
 dest.sin_port = htons(s_port);
 if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
 ESP_LOGE(TAG, "invalid IP: %s", ip);
 return -1;
 }

 int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock < 0) {
 ESP_LOGE(TAG, "socket() failed: %d", errno);
 return -1;
 }

 struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
 setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 int keepalive = 1;
 setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

 ESP_LOGI(TAG, "connecting %s:%u ...", ip, s_port);
 if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
 ESP_LOGE(TAG, "connect() failed: %d (%s)", errno, strerror(errno));
 close(sock);
 return -1;
 }
 ESP_LOGI(TAG, "connected to AIDA64");
 return sock;
}

static bool aida64_socket_send_get(int sock, const char *ip)
{
 char req[256];
 int len = snprintf(req, sizeof(req),
 "GET /sse HTTP/1.1\r\n"
 "Host: %s:%u\r\n"
 "User-Agent: ESP32S3-AIDA64\r\n"
 "Accept: text/event-stream\r\n"
 "Cache-Control: no-cache\r\n"
 "\r\n",
 ip, s_port);

 int sent = 0;
 while (sent < len) {
 int n = write(sock, req + sent, len - sent);
 if (n < 0) {
 ESP_LOGE(TAG, "write() failed: %d", errno);
 return false;
 }
 sent += n;
 }
 return true;
}

static void aida64_monitor_task(void *param)
{
 const char *ip = (const char *)param;
 uint8_t rxbuf[512];

 /* 初始化 SIV 缓存 */
 memset(s_siv, 0, sizeof(s_siv));
 memset(s_siv_valid, 0, sizeof(s_siv_valid));

 while (aida64_running) {
 int sock = aida64_socket_connect(ip);
 if (sock < 0) {
 vTaskDelay(pdMS_TO_TICKS(10000));
 continue;
 }
 aida64_sock = sock;

 if (!aida64_socket_send_get(sock, ip)) {
 close(sock);
 aida64_sock = -1;
 vTaskDelay(pdMS_TO_TICKS(10000));
 continue;
 }

 is_connected = true;
 ui_set_connected(true);
 sse_reset();

 bool alive = true;
 while (aida64_running && alive) {
 int n = read(sock, rxbuf, sizeof(rxbuf));
 if (n > 0) {
 ESP_LOGD(TAG, "recv (%dB)", n);
 sse_feed((const char *)rxbuf, n);
 } else if (n == 0) {
 ESP_LOGW(TAG, "server closed, reconnecting...");
 alive = false;
 } else {
 if (errno == EINTR || errno == EAGAIN) {
 continue;
 }
 if (errno == ETIMEDOUT) {
 /* SSE 流可能暂时安静 - 继续等待即可 */
 continue;
 }
 ESP_LOGE(TAG, "read() error: %d, reconnecting...", errno);
 alive = false;
 }
 }

 if (aida64_sock == sock) aida64_sock = -1;
 close(sock);
 is_connected = false;
 ui_set_connected(false);

 if (aida64_running) {
 vTaskDelay(pdMS_TO_TICKS(10000));
 }
 }

 aida64_task_handle = NULL;
 vTaskDelete(NULL);
}

void aida64_monitor_start(const char *ip, uint16_t port)
{
 if (aida64_running) {
 ESP_LOGI(TAG, "already running, skipping");
 return;
 }
 aida64_running = true;
 s_port = port;
 strncpy(s_ip, ip, sizeof(s_ip) - 1);
 s_ip[sizeof(s_ip) - 1] = '\0';
 ESP_LOGI(TAG, "starting monitor: %s:%u", s_ip, (unsigned)s_port);
 xTaskCreatePinnedToCore(aida64_monitor_task, "AIDA64", 8192,
 (void *)s_ip, 3, &aida64_task_handle, 1);
}

void aida64_monitor_stop(void)
{
 aida64_running = false;
 int sock = aida64_sock;
 if (sock >= 0) {
 /* shutdown + close 确保 read() 立即返回错误，任务自己退出 */
 shutdown(sock, SHUT_RDWR);
 close(sock);
 aida64_sock = -1;
 }
 /* 等待任务自己退出（最多等500ms） */
 if (aida64_task_handle) {
 TaskHandle_t h = aida64_task_handle;
 for (int i = 0; i < 50; i++) {
 if (eTaskGetState(h) == eDeleted) break;
 vTaskDelay(pdMS_TO_TICKS(10));
 }
 aida64_task_handle = NULL;
 }
 is_connected = false;
 ui_set_connected(false);
}

int aida64_monitor_isconnect(void)
{
 return is_connected;
}