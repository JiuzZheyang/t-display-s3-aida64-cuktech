#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_ROWS    4
#define MAX_LABEL   16
#define MAX_TITLE   16

typedef struct {
    char titles[2][MAX_TITLE + 1];          /* 列标题, 如 "CPU" "GPU" */
    int  row_count;                           /* 行数 1-4 */
    struct {
        char label[MAX_LABEL + 1];           /* 行标签, 如 "占用率" */
        int  siv[2];                          /* SIV 索引, 如 4, 5 */
    } rows[MAX_ROWS];
    /* 内存 SIV 配置 */
    int mem_siv_pct;                          /* 内存使用率 SIV（默认 11） */
    int mem_siv_used;                         /* 内存已用 SIV（默认 12） */
} display_config_t;

/* 加载显示配置（NVS 失败返回默认值） */
display_config_t display_config_load(void);

/* 保存显示配置到 NVS */
bool display_config_save(const display_config_t *cfg);

/* 主题索引持久化（单独存 key，不影响 layout blob） */
int  display_config_load_theme(void);
bool display_config_save_theme(int idx);

/* 获取主题背景色（4 套主题） */
uint32_t theme_get_bg(int idx);

/* 时钟配置 */
uint8_t clock_config_load_fmt(void);   /* 0=24h, 1=12h */
bool    clock_config_save_fmt(uint8_t fmt);
uint8_t clock_config_load_sec(void);   /* 0=hide seconds, 1=show */
bool    clock_config_save_sec(uint8_t sec);
uint8_t clock_config_load_style(void); /* 0=flip, 1=minimal, 2=nixie */
bool    clock_config_save_style(uint8_t style);

#endif /* DISPLAY_CONFIG_H */
