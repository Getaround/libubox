/*
 * blob - library for generating/parsing tagged binary data
 *
 * Copyright (C) 2010 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _BLOB_H__
#define _BLOB_H__

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN

#if defined(__linux__) || defined(__CYGWIN__)
#include <byteswap.h>
#include <endian.h>

#elif defined(__APPLE__)
#include <machine/endian.h>
#include <machine/byte_order.h>
#define bswap_16(x) OSSwapInt16(x)
#define bswap_32(x) OSSwapInt32(x)
#define bswap_64(x) OSSwapInt64(x)
#elif defined(__FreeBSD__)
#include <sys/endian.h>
#define bswap_16(x) bswap16(x)
#define bswap_32(x) bswap32(x)
#define bswap_64(x) bswap64(x)
#else
#include <machine/endian.h>
#define bswap_16(x) swap16(x)
#define bswap_32(x) swap32(x)
#define bswap_64(x) swap64(x)
#endif

#ifndef __BYTE_ORDER
#define __BYTE_ORDER BYTE_ORDER
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN BIG_ENDIAN
#endif
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#endif

#define cpu_to_be64(x) bswap_64(x)
#define cpu_to_be32(x) bswap_32(x)
#define cpu_to_be16(x) bswap_16(x)

#define be64_to_cpu(x) bswap_64(x)
#define be32_to_cpu(x) bswap_32(x)
#define be16_to_cpu(x) bswap_16(x)

#else

#define cpu_to_be64(x) (x)
#define cpu_to_be32(x) (x)
#define cpu_to_be16(x) (x)

#define be64_to_cpu(x) (x)
#define be32_to_cpu(x) (x)
#define be16_to_cpu(x) (x)

#endif

enum {
	BLOB_ATTR_UNSPEC,
	BLOB_ATTR_NESTED,
	BLOB_ATTR_BINARY,
	BLOB_ATTR_STRING,
	BLOB_ATTR_INT8,
	BLOB_ATTR_INT16,
	BLOB_ATTR_INT32,
	BLOB_ATTR_INT64,
	BLOB_ATTR_LAST
};

#define BLOB_ATTR_ID_MASK  0xff000000
#define BLOB_ATTR_ID_SHIFT 24
#define BLOB_ATTR_LEN_MASK 0x00ffffff
#define BLOB_ATTR_ALIGN    4

#ifndef __packed
#define __packed __attribute__((packed))
#endif

struct blob_attr {
	uint32_t id_len;
	char data[];
} __packed;

struct blob_attr_info {
	unsigned int type;
	unsigned int minlen;
	unsigned int maxlen;
	bool (*validate)(struct blob_attr_info *, struct blob_attr *);
};

struct blob_buf {
	struct blob_attr *head;
	bool (*grow)(struct blob_buf *buf, int minlen);
	int buflen;
	void *buf;
	void *priv;
};

/*
 * blob_data: returns the data pointer for an attribute
 */
static inline void *
blob_data(struct blob_attr *attr)
{
	return attr->data;
}

/*
 * blob_id: returns the id of an attribute
 */
static inline unsigned int
blob_id(struct blob_attr *attr)
{
	int id = (be32_to_cpu(attr->id_len) & BLOB_ATTR_ID_MASK) >> BLOB_ATTR_ID_SHIFT;
	return id;
}

/*
 * blob_len: returns the length of the attribute's payload
 */
static inline unsigned int
blob_len(struct blob_attr *attr)
{
	return (be32_to_cpu(attr->id_len) & BLOB_ATTR_LEN_MASK) - sizeof(struct blob_attr);
}

/*
 * blob_pad_len: returns the complete length of an attribute (including the header)
 */
static inline unsigned int
blob_raw_len(struct blob_attr *attr)
{
	return blob_len(attr) + sizeof(struct blob_attr);
}

/*
 * blob_pad_len: returns the padded length of an attribute (including the header)
 */
static inline unsigned int
blob_pad_len(struct blob_attr *attr)
{
	int len = blob_raw_len(attr);
	len = (len + BLOB_ATTR_ALIGN - 1) & ~(BLOB_ATTR_ALIGN - 1);
	return len;
}

static inline void
blob_set_raw_len(struct blob_attr *attr, unsigned int len)
{
	int id = blob_id(attr);
	len &= BLOB_ATTR_LEN_MASK;
	len |= (id << BLOB_ATTR_ID_SHIFT) & BLOB_ATTR_ID_MASK;
	attr->id_len = cpu_to_be32(len);
}

static inline uint8_t
blob_get_int8(struct blob_attr *attr)
{
	return *((uint8_t *) attr->data);
}

static inline uint16_t
blob_get_int16(struct blob_attr *attr)
{
	uint16_t *tmp = (uint16_t*)attr->data;
	return be16_to_cpu(*tmp);
}

static inline uint32_t
blob_get_int32(struct blob_attr *attr)
{
	uint32_t *tmp = (uint32_t*)attr->data;
	return be32_to_cpu(*tmp);
}

static inline uint64_t
blob_get_int64(struct blob_attr *attr)
{
	uint64_t *tmp = (uint64_t*)attr->data;
	return be64_to_cpu(*tmp);
}

static inline const char *
blob_get_string(struct blob_attr *attr)
{
	return attr->data;
}

static inline struct blob_attr *
blob_next(struct blob_attr *attr)
{
	return (struct blob_attr *) ((char *) attr + blob_pad_len(attr));
}

extern int blob_buf_init(struct blob_buf *buf, int id);
extern struct blob_attr *blob_new(struct blob_buf *buf, int id, int payload);
extern void *blob_nest_start(struct blob_buf *buf, int id);
extern void blob_nest_end(struct blob_buf *buf, void *cookie);
extern struct blob_attr *blob_put(struct blob_buf *buf, int id, const void *ptr, int len);
extern int blob_parse(struct blob_attr *attr, struct blob_attr **data, struct blob_attr_info *info, int max);

static inline struct blob_attr *
blob_put_string(struct blob_buf *buf, int id, const char *str)
{
	return blob_put(buf, id, str, strlen(str) + 1);
}

static inline struct blob_attr *
blob_put_int8(struct blob_buf *buf, int id, uint8_t val)
{
	return blob_put(buf, id, &val, sizeof(val));
}

static inline struct blob_attr *
blob_put_int16(struct blob_buf *buf, int id, uint16_t val)
{
	val = cpu_to_be16(val);
	return blob_put(buf, id, &val, sizeof(val));
}

static inline struct blob_attr *
blob_put_int32(struct blob_buf *buf, int id, uint32_t val)
{
	val = cpu_to_be32(val);
	return blob_put(buf, id, &val, sizeof(val));
}

static inline struct blob_attr *
blob_put_int64(struct blob_buf *buf, int id, uint64_t val)
{
	val = cpu_to_be64(val);
	return blob_put(buf, id, &val, sizeof(val));
}

#define __blob_for_each_attr(pos, attr, rem) \
	for (pos = (void *) attr; \
		 (blob_pad_len(pos) <= rem) && \
		 (blob_pad_len(pos) >= sizeof(struct blob_attr)); \
		 rem -= blob_pad_len(pos), pos = blob_next(pos))


#define blob_for_each_attr(pos, attr, rem) \
	for (rem = blob_len(attr), pos = blob_data(attr); \
		 (blob_pad_len(pos) <= rem) && \
		 (blob_pad_len(pos) >= sizeof(struct blob_attr)); \
		 rem -= blob_pad_len(pos), pos = blob_next(pos))


#endif