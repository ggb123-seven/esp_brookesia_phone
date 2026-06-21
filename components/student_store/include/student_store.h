/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STUDENT_STORE_DEFAULT_PATH             "/spiffs/students.csv"
#define STUDENT_STORE_SPIFFS_IMPORT_PATH       "/spiffs/students_import.csv"
#define STUDENT_STORE_SDCARD_IMPORT_PATH       "/sdcard/students.csv"
#define STUDENT_STORE_NO_PAGE_ID               UINT16_MAX
#define STUDENT_STORE_PAGE_ID_MAX              299
#define STUDENT_STORE_MAX_RECORDS              128
#define STUDENT_STORE_STUDENT_ID_LEN           24
#define STUDENT_STORE_NAME_LEN                 32
#define STUDENT_STORE_CLASS_NAME_LEN           32

typedef enum {
    STUDENT_STORE_STATUS_OK = 0,
    STUDENT_STORE_STATUS_STUDENT_NOT_FOUND,
    STUDENT_STORE_STATUS_PAGE_ID_UNBOUND,
    STUDENT_STORE_STATUS_DUPLICATE_STUDENT_ID,
    STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED,
    STUDENT_STORE_STATUS_STORE_FULL,
    STUDENT_STORE_STATUS_PARSE_ERROR,
    STUDENT_STORE_STATUS_STORAGE_ERROR,
} student_store_status_t;

typedef struct {
    char student_id[STUDENT_STORE_STUDENT_ID_LEN];
    char name[STUDENT_STORE_NAME_LEN];
    char class_name[STUDENT_STORE_CLASS_NAME_LEN];
    uint16_t fingerprint_page_id;
    bool enabled;
} student_store_record_t;

esp_err_t student_store_init(const char *path);
esp_err_t student_store_deinit(void);
esp_err_t student_store_reload(student_store_status_t *status);
esp_err_t student_store_save(student_store_status_t *status);
esp_err_t student_store_import_csv(const char *path, student_store_status_t *status);
esp_err_t student_store_import_csv_merge(const char *path, student_store_status_t *status);
esp_err_t student_store_add(const student_store_record_t *record, student_store_status_t *status);
esp_err_t student_store_get_by_id(const char *student_id, student_store_record_t *record,
                                  student_store_status_t *status);
esp_err_t student_store_get_by_page_id(uint16_t page_id, student_store_record_t *record,
                                       student_store_status_t *status);
esp_err_t student_store_list(student_store_record_t *records, size_t capacity, size_t *count,
                             student_store_status_t *status);
esp_err_t student_store_bind_page_id(const char *student_id, uint16_t page_id,
                                     student_store_status_t *status);
esp_err_t student_store_unbind_student(const char *student_id, uint16_t *old_page_id,
                                       student_store_status_t *status);
esp_err_t student_store_unbind_page_id(uint16_t page_id, student_store_status_t *status);
esp_err_t student_store_delete_student(const char *student_id, uint16_t *old_page_id,
                                       student_store_status_t *status);
const char *student_store_status_to_text(student_store_status_t status);

#ifdef __cplusplus
}
#endif
