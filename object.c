#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ─────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ─────────────────────────────────────────

// WRITE
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str =
        (type == OBJ_BLOB) ? "blob" :
        (type == OBJ_TREE) ? "tree" : "commit";

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    size_t total_len = header_len + len;
    unsigned char *buf = malloc(total_len);
    if (!buf) return -1;

    memcpy(buf, header, header_len);
    memcpy(buf + header_len, data, len);

    compute_hash(buf, total_len, id_out);

    if (object_exists(id_out)) {
        free(buf);
        return 0;
    }

    mkdir(".pes", 0755);
    mkdir(OBJECTS_DIR, 0755);

    char hex[65];
    hash_to_hex(id_out, hex);

    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, hex + 2);

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    if (write(fd, buf, total_len) != (ssize_t)total_len) {
        close(fd);
        free(buf);
        return -1;
    }

    fsync(fd);
    close(fd);

    rename(tmp, path);

    free(buf);
    return 0;
}

// READ
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    unsigned char *buf = malloc(size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    fread(buf, 1, size, f);
    fclose(f);

    ObjectID check;
    compute_hash(buf, size, &check);

    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    char *null_pos = memchr(buf, '\0', size);
    if (!null_pos) {
        free(buf);
        return -1;
    }

    char type_str[10];
    size_t data_len;
    sscanf((char*)buf, "%s %zu", type_str, &data_len);

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else *type_out = OBJ_COMMIT;

    *data_out = malloc(data_len);
    if (!*data_out) {
        free(buf);
        return -1;
    }

    memcpy(*data_out, null_pos + 1, data_len);
    *len_out = data_len;

    free(buf);
    return 0;
}
