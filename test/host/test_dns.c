#include <arpa/inet.h>

#include "dns_server.h"
#include "test.h"

static size_t make_query(uint8_t *buf, const char *name, uint16_t qtype)
{
    uint8_t *p = buf;
    const uint8_t hdr[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(p, hdr, sizeof(hdr));
    p += sizeof(hdr);
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t len = dot ? (size_t)(dot - s) : strlen(s);
        *p++ = (uint8_t)len;
        memcpy(p, s, len);
        p += len;
        s += len + (dot ? 1 : 0);
    }
    *p++ = 0;
    *p++ = qtype >> 8;
    *p++ = qtype & 0xFF;
    *p++ = 0;
    *p++ = 1;
    return (size_t)(p - buf);
}

void test_dns(void)
{
    uint8_t q[256], r[512];
    uint32_t ip = inet_addr("192.168.4.1");

    size_t ql = make_query(q, "connectivitycheck.gstatic.com", 1);
    size_t rl = dns_build_response(q, ql, ip, r, sizeof(r));
    CHECK(rl == ql + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34);    /* même identifiant */
    CHECK((r[2] & 0x80) && (r[2] & 0x04)); /* réponse, autoritaire */
    CHECK(r[7] == 1);                      /* une réponse */
    CHECK(memcmp(r + rl - 4, &ip, 4) == 0);

    /* AAAA : réponse vide (le client se rabat sur IPv4) */
    ql = make_query(q, "captive.apple.com", 28);
    rl = dns_build_response(q, ql, ip, r, sizeof(r));
    CHECK(rl == ql && r[7] == 0);

    /* Requêtes invalides ignorées */
    CHECK(dns_build_response(q, 8, ip, r, sizeof(r)) == 0);
    ql = make_query(q, "a.b", 1);
    q[2] |= 0x80; /* déjà une réponse */
    CHECK(dns_build_response(q, ql, ip, r, sizeof(r)) == 0);
    ql = make_query(q, "a.b", 1);
    CHECK(dns_build_response(q, ql - 3, ip, r, sizeof(r)) == 0); /* tronquée */
    q[12] = 0xC0;                                                  /* pointeur de compression */
    CHECK(dns_build_response(q, ql, ip, r, sizeof(r)) == 0);
    ql = make_query(q, "a.b", 1);
    CHECK(dns_build_response(q, ql, ip, r, 10) == 0); /* tampon trop petit */
}
