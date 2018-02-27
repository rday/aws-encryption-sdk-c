#include <string.h> // memcpy
#include <aws/cryptosdk/header.h>
#include <aws/cryptosdk/private/compiler.h>

#define MESSAGE_ID_LEN 16

struct aws_cryptosdk_hdr {
    uint16_t alg_id;

    size_t aad_count;
    size_t edk_count;
    size_t ivlen;
    size_t frame_len; 

    struct aws_cryptosdk_buffer auth_tag, message_id;
    char message_id_arr[MESSAGE_ID_LEN];
    size_t taglen;

    struct aws_cryptosdk_hdr_aad *aad_tbl;
    struct aws_cryptosdk_hdr_edk *edk_tbl;
};

uint16_t aws_cryptosdk_hdr_get_algorithm(const struct aws_cryptosdk_hdr *hdr) {
    return hdr->alg_id;
}

size_t aws_cryptosdk_hdr_get_aad_count(const struct aws_cryptosdk_hdr *hdr) {
    return hdr->aad_count;
}

size_t aws_cryptosdk_hdr_get_edk_count(const struct aws_cryptosdk_hdr *hdr) {
    return hdr->edk_count;
}

size_t aws_cryptosdk_hdr_get_iv_len(const struct aws_cryptosdk_hdr *hdr) {
    return hdr->ivlen;
}

size_t aws_cryptosdk_hdr_get_frame_len(const struct aws_cryptosdk_hdr *hdr) {
    return hdr->frame_len;
}

int aws_cryptosdk_hdr_get_aad(const struct aws_cryptosdk_hdr *hdr, int index, struct aws_cryptosdk_hdr_aad *aad) {
    if (index < 0 || index >= hdr->aad_count) {
        return AWS_ERR_BAD_ARG;
    }

    *aad = hdr->aad_tbl[index];

    return AWS_ERR_OK;
}

int aws_cryptosdk_hdr_get_edk(const struct aws_cryptosdk_hdr *hdr, int index, struct aws_cryptosdk_hdr_edk *edk) {
    if (index < 0 || index >= hdr->edk_count) {
        return AWS_ERR_BAD_ARG;
    }

    *edk = hdr->edk_tbl[index];

    return AWS_ERR_OK;
}

int aws_cryptosdk_hdr_get_msgid(const struct aws_cryptosdk_hdr *hdr, struct aws_cryptosdk_buffer *buf) {
    *buf = hdr->message_id;

    return AWS_ERR_OK;
}

int aws_cryptosdk_hdr_get_authtag(const struct aws_cryptosdk_hdr *hdr, struct aws_cryptosdk_buffer *buf) {
    *buf = hdr->auth_tag;

    return AWS_ERR_OK;
}

int aws_cryptosdk_algorithm_is_known(uint16_t alg_id) {
    switch (alg_id) {
        case AES_256_GCM_IV12_AUTH16_KDSHA384_SIGEC384:
        case AES_192_GCM_IV12_AUTH16_KDSHA384_SIGEC384:
        case AES_128_GCM_IV12_AUTH16_KDSHA256_SIGEC256:
        case AES_256_GCM_IV12_AUTH16_KDSHA256_SIGNONE:
        case AES_192_GCM_IV12_AUTH16_KDSHA256_SIGNONE:
        case AES_128_GCM_IV12_AUTH16_KDSHA256_SIGNONE:
        case AES_256_GCM_IV12_AUTH16_KDNONE_SIGNONE:
        case AES_192_GCM_IV12_AUTH16_KDNONE_SIGNONE:
        case AES_128_GCM_IV12_AUTH16_KDNONE_SIGNONE:
            return 1;
        default:
            return 0;
    }
}

int aws_cryptosdk_algorithm_taglen(uint16_t alg_id) {
    if (aws_cryptosdk_algorithm_is_known(alg_id)) {
        // all known algorithms have a tag length of 16 bytes
        return 16;
    }

    return -1;
}

int aws_cryptosdk_algorithm_ivlen(uint16_t alg_id) {
    if (aws_cryptosdk_algorithm_is_known(alg_id)) {
        // all known algorithms have an IV length of 12 bytes
        return 12;
    }

    return -1;
}


static int is_known_type(uint8_t content_type) {
    switch (content_type) {
        case AWS_CRYPTOSDK_HEADER_CTYPE_FRAMED:
        case AWS_CRYPTOSDK_HEADER_CTYPE_NONFRAMED:
            return 1;
        default:
            return 0;
    }
}

static int allocate_ptr_tables(
    struct aws_cryptosdk_hdr * restrict hdr,
    struct aws_cryptosdk_buffer * restrict outbuf,
    size_t * restrict header_space_needed
) {
    size_t aad_tbl_size = sizeof(struct aws_cryptosdk_hdr_aad) * hdr->aad_count;
    size_t edk_tbl_size = sizeof(struct aws_cryptosdk_hdr_edk) * hdr->edk_count;

    *header_space_needed += aad_tbl_size + edk_tbl_size;

    hdr->aad_tbl = NULL;
    hdr->edk_tbl = NULL;

    if (!outbuf) {
        return AWS_ERR_OK;
    }

    // Align buffer to a multiple of sizeof(void *) if needed.
    // This isn't strictly guaranteed to work by a strict reading of the C spec, but should work
    // on any real platform.
    uintptr_t cur_offset = (uintptr_t)outbuf->ptr;
    uintptr_t misaligned = cur_offset % sizeof(void*);
    if (misaligned) {
        uint8_t *ignored;
        if (aws_cryptosdk_buffer_advance(outbuf, sizeof(void *) - misaligned, &ignored)) {
            return AWS_ERR_OOM;
        }
    }

    if (aws_cryptosdk_buffer_advance(outbuf, aad_tbl_size, &hdr->aad_tbl)) {
        return AWS_ERR_OOM;
    }
    if (aws_cryptosdk_buffer_advance(outbuf, edk_tbl_size, &hdr->edk_tbl)) {
        return AWS_ERR_OOM;
    }

    return AWS_ERR_OK;
}

static int read_field_u16(
    struct aws_cryptosdk_buffer * restrict field,
    struct aws_cryptosdk_buffer * restrict inbuf,
    struct aws_cryptosdk_buffer * restrict arena,
    size_t * restrict header_space_needed
) {
    uint16_t length;
    if (aws_cryptosdk_buffer_read_u16(inbuf, &length)) return AWS_ERR_TRUNCATED;

    const uint8_t *field_data;
    if (aws_cryptosdk_buffer_advance(inbuf, length, &field_data)) {
        return AWS_ERR_TRUNCATED;
    }

    *header_space_needed += length;
    field->len = length;
    field->ptr = NULL;
    if (arena) {
        void *destbuf;
        if (aws_cryptosdk_buffer_advance(arena, length, &destbuf)) return AWS_ERR_OOM;
        memcpy(destbuf, field_data, length);
        field->ptr = destbuf;
    }

    return AWS_ERR_OK;
}

#define PROPAGATE_ERR(expr) do { \
    int prop_err_rv = (expr); \
    if (aws_cryptosdk_unlikely(prop_err_rv)) return prop_err_rv; \
} while (0)

static inline int hdr_parse_core(
    struct aws_cryptosdk_hdr * restrict hdr,
    struct aws_cryptosdk_buffer * restrict outbuf,
    struct aws_cryptosdk_buffer * restrict inbuf,
    size_t * restrict header_space_needed
) {
    assert(hdr);
    assert(inbuf);
    assert(header_space_needed);

    uint8_t bytefield;
    if (aws_cryptosdk_buffer_read_u8(inbuf, &bytefield)) return AWS_ERR_TRUNCATED;
    if (aws_cryptosdk_unlikely(bytefield != AWS_CRYPTOSDK_HEADER_VERSION_1_0)) return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;

    if (aws_cryptosdk_buffer_read_u8(inbuf, &bytefield)) return AWS_ERR_TRUNCATED;
    if (aws_cryptosdk_unlikely(bytefield != AWS_CRYPTOSDK_HEADER_TYPE_CUSTOMER_AED)) return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;

    uint16_t alg_id;
    if (aws_cryptosdk_buffer_read_u16(inbuf, &alg_id)) return AWS_ERR_TRUNCATED;
    if (aws_cryptosdk_unlikely(!aws_cryptosdk_algorithm_is_known(alg_id))) {
        return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    }
    hdr->alg_id = alg_id;

    if (aws_cryptosdk_buffer_read(inbuf, hdr->message_id_arr, MESSAGE_ID_LEN)) return AWS_ERR_TRUNCATED;
    hdr->message_id.len = MESSAGE_ID_LEN;
    hdr->message_id.ptr = hdr->message_id_arr;

    uint16_t aad_len, aad_count;
    if (aws_cryptosdk_buffer_read_u16(inbuf, &aad_len)) return AWS_ERR_TRUNCATED;

    const uint8_t *expect_end_aad = (const uint8_t *)inbuf->ptr + aad_len;

    if (aws_cryptosdk_buffer_read_u16(inbuf, &aad_count)) return AWS_ERR_TRUNCATED;

    struct aws_cryptosdk_buffer aad_start = *inbuf;

    hdr->aad_count = aad_count;
    // Skip forward and get the EDK count, so we can preallocate our tables
    if (aws_cryptosdk_unlikely(aws_cryptosdk_buffer_skip(inbuf, aad_len - 2))) {
        return AWS_ERR_TRUNCATED;
    }

    uint16_t edk_count;
    if (aws_cryptosdk_buffer_read_u16(inbuf, &edk_count)) return AWS_ERR_TRUNCATED;
    if (aws_cryptosdk_unlikely(!edk_count)) return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    hdr->edk_count = edk_count;

    if (allocate_ptr_tables(hdr, outbuf, header_space_needed)) {
        return AWS_ERR_OOM;
    }

    // Now go back and parse the AAD entries
    *inbuf = aad_start;

    for (size_t i = 0; i < aad_count; i++) {
        struct aws_cryptosdk_hdr_aad aad;

        PROPAGATE_ERR(read_field_u16(&aad.key, inbuf, outbuf, header_space_needed));
        PROPAGATE_ERR(read_field_u16(&aad.value, inbuf, outbuf, header_space_needed));

        if (hdr->aad_tbl) {
            hdr->aad_tbl[i] = aad;
        }
    }

    if (inbuf->ptr != expect_end_aad) {
        return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    }

    // Skip the EDK count as we've already read it
    if (aws_cryptosdk_buffer_skip(inbuf, 2)) return AWS_ERR_UNKNOWN;

    // The EDK structure doesn't have a handy length field, so we need to walk it here.
    for (int i = 0; i < edk_count; i++) {
        struct aws_cryptosdk_hdr_edk edk;
        
        PROPAGATE_ERR(read_field_u16(&edk.provider_id, inbuf, outbuf, header_space_needed));
        PROPAGATE_ERR(read_field_u16(&edk.provider_info, inbuf, outbuf, header_space_needed));
        PROPAGATE_ERR(read_field_u16(&edk.enc_data_key, inbuf, outbuf, header_space_needed));

        if (hdr->edk_tbl) {
            hdr->edk_tbl[i] = edk;
        }
    }

    uint8_t content_type;
    if (aws_cryptosdk_buffer_read_u8(inbuf, &content_type)) return AWS_ERR_TRUNCATED;

    if (aws_cryptosdk_unlikely(!is_known_type(content_type))) {
        return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    }

    // skip reserved
    if (aws_cryptosdk_buffer_skip(inbuf, 4)) return AWS_ERR_TRUNCATED;

    uint8_t ivlen;
    if (aws_cryptosdk_buffer_read_u8(inbuf, &ivlen)) return AWS_ERR_TRUNCATED;

    if (ivlen != aws_cryptosdk_algorithm_ivlen(alg_id)) {
        return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    }

    hdr->ivlen = ivlen;

    uint32_t frame_len;
    if (aws_cryptosdk_buffer_read_u32(inbuf, &frame_len)) return AWS_ERR_TRUNCATED;

    if (content_type == AWS_CRYPTOSDK_HEADER_CTYPE_NONFRAMED && frame_len != 0) {
        return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    }

    if (content_type == AWS_CRYPTOSDK_HEADER_CTYPE_FRAMED && frame_len == 0) {
        return AWS_CRYPTOSDK_ERR_BAD_CIPHERTEXT;
    }

    hdr->frame_len = frame_len;

    // verify we can still fit header auth
    size_t taglen = aws_cryptosdk_algorithm_taglen(alg_id);
    const uint8_t *p_hdr_auth_src;
    if (aws_cryptosdk_buffer_advance(inbuf, taglen, &p_hdr_auth_src)) return AWS_ERR_TRUNCATED;

    uint8_t *p_hdr_auth_dst = NULL;
    if (outbuf) {
        if (aws_cryptosdk_buffer_advance(outbuf, taglen, &p_hdr_auth_dst)) return AWS_ERR_OOM;
        memcpy(p_hdr_auth_dst, p_hdr_auth_src, taglen);
    }

    hdr->auth_tag.ptr = p_hdr_auth_dst;
    hdr->auth_tag.len = taglen;

    *header_space_needed += taglen;

    return AWS_ERR_OK;
}

int aws_cryptosdk_hdr_preparse(const uint8_t *hdrbuf, size_t buflen, size_t *header_space_needed, size_t *header_length) {
    struct aws_cryptosdk_buffer inbuf;
    inbuf.ptr = (void *)hdrbuf;
    inbuf.len = buflen;

    struct aws_cryptosdk_hdr hdr;

    *header_space_needed = sizeof(hdr);
    // Reserve space for alignment padding
    *header_space_needed += sizeof(void *) - 1;

    int rv = hdr_parse_core(&hdr, NULL, &inbuf, header_space_needed);

    if (aws_cryptosdk_likely(rv == AWS_ERR_OK)) {
        *header_length = (const uint8_t *)inbuf.ptr - hdrbuf;
        *header_space_needed += *header_length;
    }

    return rv;
}

int aws_cryptosdk_hdr_parse(
    struct aws_cryptosdk_hdr **hdr,
    uint8_t *outbufp, size_t outlen,
    const uint8_t *inbufp, size_t inlen
) {
    struct aws_cryptosdk_buffer inbuf = { .ptr = (void *)inbufp, .len = inlen };
    struct aws_cryptosdk_buffer outbuf = { .ptr = (void *)outbufp, .len = outlen };

    // Align header to the size of a void *
    uintptr_t align = (uintptr_t)outbufp % sizeof(void *);
    if (align == sizeof(void *)) {
        align = 0;
    }

    // Don't need to check result: If we don't have enough space for a void *, then we definitely
    // won't have enough for the full header structure (and thus the next advance will fail instead).
    aws_cryptosdk_buffer_skip(&outbuf, align);

    struct aws_cryptosdk_hdr *pHeader;
    if (aws_cryptosdk_buffer_advance(&outbuf, sizeof(*pHeader), &pHeader)) return AWS_ERR_OOM;

    if (aws_cryptosdk_unlikely(!pHeader)) {
        return AWS_ERR_OOM;
    }

    size_t header_space_needed = 0;
    int rv = hdr_parse_core(pHeader, &outbuf, &inbuf, &header_space_needed);

    if (aws_cryptosdk_unlikely(rv != AWS_ERR_OK)) {
        *hdr = NULL;
        return rv;
    }
   
    *hdr = pHeader;
    return rv;
}
