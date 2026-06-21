/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "student_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define STUDENT_STORE_LINE_LEN      192
#define STUDENT_STORE_PATH_LEN      96
#define STUDENT_STORE_TMP_SUFFIX    ".tmp"
#define STUDENT_STORE_CSV_HEADER    "student_id,name,class_name,fingerprint_page_id,enabled\n"

typedef struct {
    student_store_record_t record;
    bool page_id_provided;
} parsed_record_t;

static const char *TAG = "StudentStore";

static student_store_record_t s_records[STUDENT_STORE_MAX_RECORDS];
static size_t s_record_count;
static bool s_initialized;
static char s_path[STUDENT_STORE_PATH_LEN] = STUDENT_STORE_DEFAULT_PATH;

static void set_status(student_store_status_t *status, student_store_status_t value)
{
    if (status != NULL) {
        *status = value;
    }
}

static bool file_exists(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

static char *trim(char *text)
{
    if (text == NULL) {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    *end = '\0';
    return text;
}

static int split_csv_line(char *line, char *fields[], int max_fields)
{
    int count = 0;
    char *cursor = line;

    while (count < max_fields) {
        fields[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }

    return count;
}

static bool copy_field(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0 || src == NULL) {
        return false;
    }
    const size_t len = strlen(src);
    if (len == 0 || len >= dst_size) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static bool parse_bool_field(const char *text, bool *value)
{
    if (value == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0') {
        *value = true;
        return true;
    }

    char normalized[8] = {0};
    size_t len = strlen(text);
    if (len >= sizeof(normalized)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        normalized[i] = (char)tolower((unsigned char)text[i]);
    }

    if (strcmp(normalized, "1") == 0 || strcmp(normalized, "true") == 0 ||
        strcmp(normalized, "yes") == 0 || strcmp(normalized, "on") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(normalized, "0") == 0 || strcmp(normalized, "false") == 0 ||
        strcmp(normalized, "no") == 0 || strcmp(normalized, "off") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_page_id_field(const char *text, uint16_t *page_id, bool *provided)
{
    if (page_id == NULL || provided == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0') {
        *page_id = STUDENT_STORE_NO_PAGE_ID;
        *provided = false;
        return true;
    }

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > STUDENT_STORE_PAGE_ID_MAX) {
        return false;
    }

    *page_id = (uint16_t)value;
    *provided = true;
    return true;
}

static bool parse_record_line(char *line, parsed_record_t *parsed)
{
    if (line == NULL || parsed == NULL) {
        return false;
    }

    char *fields[5] = {0};
    int field_count = split_csv_line(line, fields, 5);
    if (field_count < 3) {
        return false;
    }

    for (int i = 0; i < field_count; ++i) {
        fields[i] = trim(fields[i]);
    }

    memset(parsed, 0, sizeof(*parsed));
    parsed->record.fingerprint_page_id = STUDENT_STORE_NO_PAGE_ID;
    parsed->record.enabled = true;

    if (!copy_field(parsed->record.student_id, sizeof(parsed->record.student_id), fields[0]) ||
        !copy_field(parsed->record.name, sizeof(parsed->record.name), fields[1]) ||
        !copy_field(parsed->record.class_name, sizeof(parsed->record.class_name), fields[2])) {
        return false;
    }

    if (field_count >= 4 &&
        !parse_page_id_field(fields[3], &parsed->record.fingerprint_page_id, &parsed->page_id_provided)) {
        return false;
    }

    if (field_count >= 5 && !parse_bool_field(fields[4], &parsed->record.enabled)) {
        return false;
    }

    return true;
}

static bool is_header_line(const char *line)
{
    return line != NULL && strncmp(line, "student_id,", strlen("student_id,")) == 0;
}

static int find_by_id_in(const student_store_record_t *records, size_t count, const char *student_id)
{
    if (records == NULL || student_id == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(records[i].student_id, student_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_by_page_id_in(const student_store_record_t *records, size_t count, uint16_t page_id)
{
    if (records == NULL || page_id == STUDENT_STORE_NO_PAGE_ID) {
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (records[i].fingerprint_page_id == page_id) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t validate_records(const student_store_record_t *records, size_t count,
                                  student_store_status_t *status)
{
    if (records == NULL && count > 0) {
        set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < count; ++i) {
        const student_store_record_t *record = &records[i];
        if (record->student_id[0] == '\0' || record->name[0] == '\0' || record->class_name[0] == '\0') {
            set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
            return ESP_ERR_INVALID_ARG;
        }
        if (record->fingerprint_page_id != STUDENT_STORE_NO_PAGE_ID &&
            record->fingerprint_page_id > STUDENT_STORE_PAGE_ID_MAX) {
            set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = i + 1; j < count; ++j) {
            if (strcmp(record->student_id, records[j].student_id) == 0) {
                set_status(status, STUDENT_STORE_STATUS_DUPLICATE_STUDENT_ID);
                return ESP_ERR_INVALID_STATE;
            }
            if (record->fingerprint_page_id != STUDENT_STORE_NO_PAGE_ID &&
                record->fingerprint_page_id == records[j].fingerprint_page_id) {
                set_status(status, STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    set_status(status, STUDENT_STORE_STATUS_OK);
    return ESP_OK;
}

static esp_err_t load_csv_file(const char *path, student_store_record_t *records, size_t capacity,
                               size_t *count, student_store_status_t *status)
{
    if (path == NULL || records == NULL || count == NULL) {
        set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_FAIL;
    }

    char line[STUDENT_STORE_LINE_LEN];
    size_t loaded = 0;
    bool first_line = true;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = trim(line);
        if (text[0] == '\0') {
            continue;
        }
        if (first_line && is_header_line(text)) {
            first_line = false;
            continue;
        }
        first_line = false;

        if (loaded >= capacity) {
            fclose(file);
            set_status(status, STUDENT_STORE_STATUS_STORE_FULL);
            return ESP_ERR_INVALID_STATE;
        }

        parsed_record_t parsed;
        if (!parse_record_line(text, &parsed)) {
            fclose(file);
            set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
            return ESP_ERR_INVALID_ARG;
        }
        records[loaded++] = parsed.record;
    }

    fclose(file);

    esp_err_t err = validate_records(records, loaded, status);
    if (err != ESP_OK) {
        return err;
    }

    *count = loaded;
    return ESP_OK;
}

static esp_err_t write_records_to_path(const char *path, const student_store_record_t *records, size_t count,
                                       student_store_status_t *status)
{
    if (path == NULL || records == NULL) {
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    char tmp_path[STUDENT_STORE_PATH_LEN + sizeof(STUDENT_STORE_TMP_SUFFIX)];
    int ret = snprintf(tmp_path, sizeof(tmp_path), "%s%s", path, STUDENT_STORE_TMP_SUFFIX);
    if (ret < 0 || ret >= (int)sizeof(tmp_path)) {
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(tmp_path, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for write", tmp_path);
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_FAIL;
    }

    if (fputs(STUDENT_STORE_CSV_HEADER, file) < 0) {
        fclose(file);
        remove(tmp_path);
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < count; ++i) {
        const student_store_record_t *record = &records[i];
        int written = 0;
        if (record->fingerprint_page_id == STUDENT_STORE_NO_PAGE_ID) {
            written = fprintf(file, "%s,%s,%s,,%u\n", record->student_id, record->name,
                              record->class_name, record->enabled ? 1U : 0U);
        } else {
            written = fprintf(file, "%s,%s,%s,%u,%u\n", record->student_id, record->name,
                              record->class_name, record->fingerprint_page_id, record->enabled ? 1U : 0U);
        }
        if (written < 0) {
            fclose(file);
            remove(tmp_path);
            set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
            return ESP_FAIL;
        }
    }

    if (fclose(file) != 0) {
        remove(tmp_path);
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_FAIL;
    }

    remove(path);
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        ESP_LOGE(TAG, "Failed to rename %s to %s", tmp_path, path);
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_FAIL;
    }

    set_status(status, STUDENT_STORE_STATUS_OK);
    return ESP_OK;
}

static esp_err_t ensure_initialized(student_store_status_t *status)
{
    if (!s_initialized) {
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t save_current(student_store_status_t *status)
{
    esp_err_t err = validate_records(s_records, s_record_count, status);
    if (err != ESP_OK) {
        return err;
    }
    return write_records_to_path(s_path, s_records, s_record_count, status);
}

esp_err_t student_store_init(const char *path)
{
    const char *store_path = (path == NULL || path[0] == '\0') ? STUDENT_STORE_DEFAULT_PATH : path;
    if (strlen(store_path) >= sizeof(s_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_path, store_path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    s_record_count = 0;
    s_initialized = true;

    student_store_status_t status = STUDENT_STORE_STATUS_OK;
    if (file_exists(s_path)) {
        esp_err_t err = student_store_reload(&status);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    if (file_exists(STUDENT_STORE_SDCARD_IMPORT_PATH)) {
        ESP_LOGI(TAG, "Initializing student store from %s", STUDENT_STORE_SDCARD_IMPORT_PATH);
        return student_store_import_csv_merge(STUDENT_STORE_SDCARD_IMPORT_PATH, &status);
    }

    if (file_exists(STUDENT_STORE_SPIFFS_IMPORT_PATH)) {
        ESP_LOGI(TAG, "Initializing student store from %s", STUDENT_STORE_SPIFFS_IMPORT_PATH);
        return student_store_import_csv_merge(STUDENT_STORE_SPIFFS_IMPORT_PATH, &status);
    }

    return student_store_save(&status);
}

esp_err_t student_store_deinit(void)
{
    s_record_count = 0;
    s_initialized = false;
    return ESP_OK;
}

esp_err_t student_store_reload(student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }

    student_store_record_t *loaded = calloc(STUDENT_STORE_MAX_RECORDS, sizeof(loaded[0]));
    if (loaded == NULL) {
        ESP_LOGE(TAG, "Failed to allocate reload buffer");
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_NO_MEM;
    }

    size_t loaded_count = 0;
    err = load_csv_file(s_path, loaded, STUDENT_STORE_MAX_RECORDS, &loaded_count, status);
    if (err != ESP_OK) {
        free(loaded);
        return err;
    }

    memcpy(s_records, loaded, sizeof(loaded[0]) * loaded_count);
    free(loaded);
    s_record_count = loaded_count;
    set_status(status, STUDENT_STORE_STATUS_OK);
    return ESP_OK;
}

esp_err_t student_store_save(student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    return save_current(status);
}

esp_err_t student_store_import_csv(const char *path, student_store_status_t *status)
{
    return student_store_import_csv_merge(path, status);
}

esp_err_t student_store_import_csv_merge(const char *path, student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (path == NULL || path[0] == '\0') {
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open import file %s", path);
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_FAIL;
    }

    student_store_record_t *imported = calloc(STUDENT_STORE_MAX_RECORDS, sizeof(imported[0]));
    if (imported == NULL) {
        fclose(file);
        ESP_LOGE(TAG, "Failed to allocate import buffer");
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_NO_MEM;
    }

    size_t imported_count = 0;
    char line[STUDENT_STORE_LINE_LEN];
    bool first_line = true;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = trim(line);
        if (text[0] == '\0') {
            continue;
        }
        if (first_line && is_header_line(text)) {
            first_line = false;
            continue;
        }
        first_line = false;

        if (imported_count >= STUDENT_STORE_MAX_RECORDS) {
            fclose(file);
            free(imported);
            set_status(status, STUDENT_STORE_STATUS_STORE_FULL);
            return ESP_ERR_INVALID_STATE;
        }

        parsed_record_t parsed;
        if (!parse_record_line(text, &parsed)) {
            fclose(file);
            free(imported);
            set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
            return ESP_ERR_INVALID_ARG;
        }

        int old_index = find_by_id_in(s_records, s_record_count, parsed.record.student_id);
        if (!parsed.page_id_provided && old_index >= 0) {
            parsed.record.fingerprint_page_id = s_records[old_index].fingerprint_page_id;
        }
        imported[imported_count++] = parsed.record;
    }

    fclose(file);

    err = validate_records(imported, imported_count, status);
    if (err != ESP_OK) {
        free(imported);
        return err;
    }

    memcpy(s_records, imported, sizeof(imported[0]) * imported_count);
    free(imported);
    s_record_count = imported_count;
    return save_current(status);
}

esp_err_t student_store_add(const student_store_record_t *record, student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (record == NULL) {
        set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_record_count >= STUDENT_STORE_MAX_RECORDS) {
        set_status(status, STUDENT_STORE_STATUS_STORE_FULL);
        return ESP_ERR_INVALID_STATE;
    }
    if (find_by_id_in(s_records, s_record_count, record->student_id) >= 0) {
        set_status(status, STUDENT_STORE_STATUS_DUPLICATE_STUDENT_ID);
        return ESP_ERR_INVALID_STATE;
    }
    if (record->fingerprint_page_id != STUDENT_STORE_NO_PAGE_ID &&
        find_by_page_id_in(s_records, s_record_count, record->fingerprint_page_id) >= 0) {
        set_status(status, STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED);
        return ESP_ERR_INVALID_STATE;
    }

    s_records[s_record_count++] = *record;
    return save_current(status);
}

esp_err_t student_store_get_by_id(const char *student_id, student_store_record_t *record,
                                  student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (student_id == NULL || record == NULL) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_by_id_in(s_records, s_record_count, student_id);
    if (index < 0) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }
    *record = s_records[index];
    set_status(status, STUDENT_STORE_STATUS_OK);
    return ESP_OK;
}

esp_err_t student_store_get_by_page_id(uint16_t page_id, student_store_record_t *record,
                                       student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (record == NULL || page_id == STUDENT_STORE_NO_PAGE_ID || page_id > STUDENT_STORE_PAGE_ID_MAX) {
        set_status(status, STUDENT_STORE_STATUS_PAGE_ID_UNBOUND);
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_by_page_id_in(s_records, s_record_count, page_id);
    if (index < 0) {
        set_status(status, STUDENT_STORE_STATUS_PAGE_ID_UNBOUND);
        return ESP_ERR_NOT_FOUND;
    }
    *record = s_records[index];
    set_status(status, STUDENT_STORE_STATUS_OK);
    return ESP_OK;
}

esp_err_t student_store_list(student_store_record_t *records, size_t capacity, size_t *count,
                             student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (count == NULL || (records == NULL && capacity > 0)) {
        set_status(status, STUDENT_STORE_STATUS_STORAGE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    size_t copy_count = s_record_count;
    if (copy_count > capacity) {
        copy_count = capacity;
    }
    if (copy_count > 0) {
        memcpy(records, s_records, sizeof(records[0]) * copy_count);
    }
    *count = s_record_count;
    set_status(status, STUDENT_STORE_STATUS_OK);
    return ESP_OK;
}

esp_err_t student_store_bind_page_id(const char *student_id, uint16_t page_id,
                                     student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (student_id == NULL || page_id > STUDENT_STORE_PAGE_ID_MAX) {
        set_status(status, STUDENT_STORE_STATUS_PARSE_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    int student_index = find_by_id_in(s_records, s_record_count, student_id);
    if (student_index < 0) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }

    int page_index = find_by_page_id_in(s_records, s_record_count, page_id);
    if (page_index >= 0 && page_index != student_index) {
        set_status(status, STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED);
        return ESP_ERR_INVALID_STATE;
    }

    s_records[student_index].fingerprint_page_id = page_id;
    return save_current(status);
}

esp_err_t student_store_unbind_student(const char *student_id, uint16_t *old_page_id,
                                       student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (student_id == NULL) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_by_id_in(s_records, s_record_count, student_id);
    if (index < 0) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }

    if (old_page_id != NULL) {
        *old_page_id = s_records[index].fingerprint_page_id;
    }
    s_records[index].fingerprint_page_id = STUDENT_STORE_NO_PAGE_ID;
    return save_current(status);
}

esp_err_t student_store_unbind_page_id(uint16_t page_id, student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    int index = find_by_page_id_in(s_records, s_record_count, page_id);
    if (index < 0) {
        set_status(status, STUDENT_STORE_STATUS_PAGE_ID_UNBOUND);
        return ESP_ERR_NOT_FOUND;
    }
    s_records[index].fingerprint_page_id = STUDENT_STORE_NO_PAGE_ID;
    return save_current(status);
}

esp_err_t student_store_delete_student(const char *student_id, uint16_t *old_page_id,
                                       student_store_status_t *status)
{
    esp_err_t err = ensure_initialized(status);
    if (err != ESP_OK) {
        return err;
    }
    if (student_id == NULL) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_by_id_in(s_records, s_record_count, student_id);
    if (index < 0) {
        set_status(status, STUDENT_STORE_STATUS_STUDENT_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }
    if (old_page_id != NULL) {
        *old_page_id = s_records[index].fingerprint_page_id;
    }

    for (size_t i = (size_t)index; i + 1 < s_record_count; ++i) {
        s_records[i] = s_records[i + 1];
    }
    --s_record_count;
    return save_current(status);
}

const char *student_store_status_to_text(student_store_status_t status)
{
    switch (status) {
    case STUDENT_STORE_STATUS_OK:
        return "OK";
    case STUDENT_STORE_STATUS_STUDENT_NOT_FOUND:
        return "Student not found";
    case STUDENT_STORE_STATUS_PAGE_ID_UNBOUND:
        return "Page ID unbound";
    case STUDENT_STORE_STATUS_DUPLICATE_STUDENT_ID:
        return "Duplicate student ID";
    case STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED:
        return "Page ID occupied";
    case STUDENT_STORE_STATUS_STORE_FULL:
        return "Store full";
    case STUDENT_STORE_STATUS_PARSE_ERROR:
        return "Parse error";
    case STUDENT_STORE_STATUS_STORAGE_ERROR:
        return "Storage error";
    default:
        return "Unknown";
    }
}
