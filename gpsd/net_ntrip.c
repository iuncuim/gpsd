/* net_ntrip.c -- gather and dispatch DGNSS data from NTRIP broadcasters
 *
 * This file is Copyright 2010 by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 *
 * See:
 * https://igs.bkg.bund.de/root_ftp/NTRIP/documentation/NtripDocumentation.pdf
 *
 * NTRIP is not an open protocol.  So this file is based on guesswork.
 */

#include "../include/gpsd_config.h"  // must be before all includes

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/gpsd.h"
#include "../include/strfuncs.h"

// NTRIP 1.0 caster responses.  Based on Icecast audio servers
#define NTRIP_SOURCETABLE       "SOURCETABLE 200 OK\r\n"
#define NTRIP_ENDSOURCETABLE    "ENDSOURCETABLE"
#define NTRIP_ICY               "ICY 200 OK"

// NTRIP 2.0 caster responses.  Based on HTTP 1.1
#define NTRIP_SOURCETABLE2      "Content-Type: gnss/sourcetable\r\n"
#define NTRIP_BODY              "\r\n\r\n"
#define NTRIP_HTTP              "HTTP/1.1 200 OK"

// sourcetable stuff
#define NTRIP_CAS               "CAS;"
#define NTRIP_NET               "NET;"
#define NTRIP_STR               "STR;"
#define NTRIP_BR                "\r\n"
#define NTRIP_QSC               "\";\""

// HTTP 1.1
#define NTRIP_UNAUTH            "401 Unauthorized"

// table to convert format string to enum ntrip_fmt
static struct ntrip_fmt_s {
    const char *string;
    const enum ntrip_fmt format;
} const ntrip_fmts[] = {
    {"CMR+", FMT_CMRP},
    // RTCM1 required for the SAPOS derver in Gemany, confirmed as RTCM2.3
    {"RTCM1_", FMT_RTCM2_3},
    {"RTCM 2.0", FMT_RTCM2_0},
    {"RTCM 2.1", FMT_RTCM2_1},
    {"RTCM 2.2", FMT_RTCM2_2},
    {"RTCM22", FMT_RTCM2_2},
    {"RTCM 2.3", FMT_RTCM2_3},
    {"RTCM2.3", FMT_RTCM2_3},
    {"RTCM2", FMT_RTCM2},
    {"RTCM2", FMT_RTCM2},
    {"RTCM 3.0", FMT_RTCM3_0},
    {"RTCM3.0", FMT_RTCM3_0},
    {"RTCM 3.1", FMT_RTCM3_1},
    {"RTCM3.1", FMT_RTCM3_1},
    {"RTCM 3.2", FMT_RTCM3_2},
    {"RTCM32", FMT_RTCM3_2},
    {"RTCM 3.3", FMT_RTCM3_3},
    {"RTCM 3", FMT_RTCM3_0},
    {"RTCM3", FMT_RTCM3_0},
    {NULL, FMT_UNKNOWN},
};

/* Return pointer to one NUL terminated source table field
 * Return NULL on error
 * fields are separated by a semicolon (;)
 */
static char *ntrip_field_iterate(char *start,
                                 char *prev,
                                 const char *eol,
                                 const struct gpsd_errout_t *errout)
{
    char *s, *t, *u;

    if (start) {
        s = start;
    } else {
        if (!prev) {
            return NULL;
        }
        s = prev + strlen(prev) + 1;
        if (s >= eol) {
            return NULL;
        }
    }

    // ignore any quoted ; chars as they are part of the field content
    t = s;
    while ((u = strstr(t, NTRIP_QSC))) {
        t = u + strlen(NTRIP_QSC);
    }

    if ((t = strstr(t, ";"))) {
        *t = '\0';
    }

    GPSD_LOG(LOG_RAW, errout, "NTRIP: Next source table field %s\n", s);

    return s;
}


/* Decode a stream record from the sourcetable
 * See: http://software.rtcm-ntrip.org/wiki/STR
 */
static void ntrip_str_parse(char *str, size_t len,
                            struct ntrip_stream_t *hold,
                            const struct gpsd_errout_t *errout)
{
    char *s, *eol = str + len;

    memset(hold, 0, sizeof(*hold));

    // <mountpoint>
    if (NULL != (s = ntrip_field_iterate(str, NULL, eol, errout))) {
        (void)strlcpy(hold->mountpoint, s, sizeof(hold->mountpoint));
    }
    // <identifier>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <format>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        struct ntrip_fmt_s const *pfmt;

        hold->format = FMT_UNKNOWN;
        for (pfmt = ntrip_fmts; NULL != pfmt->string; pfmt++) {
            if (0 == strcasecmp(pfmt->string, s)) {
                hold->format = pfmt->format;
                break;
            }
        }
        if (FMT_UNKNOWN == hold->format) {
            GPSD_LOG(LOG_WARN, errout, "NTRIP: Got unknown format '%s'\n", s);
        }
    }
    // <format-details>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <carrier>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->carrier = atoi(s);
    }
    // <nav-system>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <network>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <country>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <latitude>
    hold->latitude = NAN;
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->latitude = safe_atof(s);
    }
    // <longitude>
    hold->longitude = NAN;
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->longitude = safe_atof(s);
    }
    // <nmea>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->nmea = atoi(s);
    }
    // <solution>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <generator>
    s = ntrip_field_iterate(NULL, s, eol, errout);
    // <compr-encryp>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        if ((0 == strcmp(" ", s)) ||
            (0 == strlen(s)) ||
            (0 == strcasecmp("none", s))) {
            hold->compr_encryp = CMP_ENC_NONE;
        } else {
            hold->compr_encryp = CMP_ENC_UNKNOWN;
            GPSD_LOG(LOG_WARN, errout,
                     "NTRIP: Got unknown {compress,encrypt}ion '%s'\n", s);
        }
    }
    // <authentication>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        if (0 == strcasecmp("N", s)) {
            hold->authentication = AUTH_NONE;
        } else if (0 == strcasecmp("B", s)) {
            hold->authentication = AUTH_BASIC;
        } else if (0 == strcasecmp("D", s)) {
            hold->authentication = AUTH_DIGEST;
        } else {
            hold->authentication = AUTH_UNKNOWN;
            GPSD_LOG(LOG_WARN, errout,
                     "NTRIP: Got unknown authenticatiion '%s'\n", s);
        }
    }
    // <fee>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->fee = atoi(s);
    }
    // <bitrate>
    if (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout))) {
        hold->bitrate = atoi(s);
    }
    // ...<misc>
    while (NULL != (s = ntrip_field_iterate(NULL, s, eol, errout)));
}

static int ntrip_sourcetable_parse(struct gps_device_t *device)
{
    struct ntrip_stream_t hold;
    ssize_t llen, len = 0;
    char *line;
    bool sourcetable = false;
    bool match = false;
    char buf[BUFSIZ];
    size_t blen = sizeof(buf);
    socket_t fd = device->gpsdata.gps_fd;

    for (;;) {
        char *eol;
        ssize_t rlen;

        memset(&buf[len], 0, (size_t) (blen - len));
        rlen = read(fd, &buf[len], (size_t)(blen - 1 - len));
        if (-1 == rlen) {
            if (EINTR == errno) {
                continue;
            }
            if (sourcetable &&
                !match &&
                EAGAIN == errno) {
                /* we have not yet found a match, but there currently
                 * is no more data */
                return 0;
            }
            if (match) {
                return 1;
            }
            GPSD_LOG(LOG_ERROR, &device->context->errout,
                     "NTRIP: stream read error %s(%d) on fd %d\n",
                     strerror(errno), errno, fd);
            return -1;
        }
        if (0 == rlen) {     // server closed the connection
            GPSD_LOG(LOG_ERROR, &device->context->errout,
                     "NTRIP: stream unexpected close %s(%d) on fd %d "
                     "during sourcetable read\n",
                     strerror(errno), errno, fd);
            return -1;
        }

        line = buf;
        rlen = len += rlen;
        line[rlen] = '\0';      // pacify coverity that this is NUL terminated

        GPSD_LOG(LOG_RAW, &device->context->errout,
                 "NTRIP: source table buffer %s\n", buf);

        sourcetable = device->ntrip.sourcetable_parse;
        if (!sourcetable) {
            if (str_starts_with(line, NTRIP_SOURCETABLE)) {
                // parse SOURCETABLE, NTRIP 1.0
                sourcetable = true;
                device->ntrip.sourcetable_parse = true;
                llen = (ssize_t)strlen(NTRIP_SOURCETABLE);
                line += llen;
                len -= llen;
            } else if (NULL != strstr(line, NTRIP_SOURCETABLE2) &&
                       NULL != (line = strstr(line, NTRIP_BODY))) {
                // parse sourcetable, NTRIP 2.0
                sourcetable = true;
                device->ntrip.sourcetable_parse = true;
                // FIXME: should parse length in header, this a hack
                llen = (ssize_t)strlen(NTRIP_BODY);
                line += 4;
                len -= llen;
            } else {
                GPSD_LOG(LOG_WARN, &device->context->errout,
                         "NTRIP: Unexpected reply: %s.\n",
                         buf);
                return -1;
            }
        }

        while (0 < len) {
            // parse ENDSOURCETABLE
            if (str_starts_with(line, NTRIP_ENDSOURCETABLE))
                goto done;

            eol = strstr(line, NTRIP_BR);
            if (NULL == eol){
                break;
            }

            GPSD_LOG(LOG_DATA, &device->context->errout,
                     "NTRIP: next source table line %s\n", line);

            *eol = '\0';
            llen = (ssize_t) (eol - line);

            // TODO: parse headers

            // parse STR
            if (str_starts_with(line, NTRIP_STR)) {
                ntrip_str_parse(line + strlen(NTRIP_STR),
                                (size_t) (llen - strlen(NTRIP_STR)),
                                &hold, &device->context->errout);
                if (0 == strcmp(device->ntrip.stream.mountpoint,
                                hold.mountpoint)) {
                    // TODO: support for RTCM 3.0, SBAS (WAAS, EGNOS), ...
                    if (FMT_UNKNOWN == hold.format) {
                        GPSD_LOG(LOG_ERROR, &device->context->errout,
                                 "NTRIP: stream %s format not supported\n",
                                 line);
                        return -1;
                    }
                    // TODO: support encryption and compression algorithms
                    if (CMP_ENC_NONE != hold.compr_encryp) {
                        GPSD_LOG(LOG_ERROR, &device->context->errout,
                                 "NTRIP. stream %s compression/encryption "
                                 "algorithm not supported\n",
                                 line);
                        return -1;
                    }
                    // TODO: support digest authentication
                    if (AUTH_NONE != hold.authentication &&
                        AUTH_BASIC != hold.authentication) {
                        GPSD_LOG(LOG_ERROR, &device->context->errout,
                                 "NTRIP. stream %s authentication method "
                                 "not supported\n",
                                line);
                        return -1;
                    }
                    // no memcpy, so we can keep the other infos
                    device->ntrip.stream.format = hold.format;
                    device->ntrip.stream.carrier = hold.carrier;
                    device->ntrip.stream.latitude = hold.latitude;
                    device->ntrip.stream.longitude = hold.longitude;
                    device->ntrip.stream.nmea = hold.nmea;
                    device->ntrip.stream.compr_encryp = hold.compr_encryp;
                    device->ntrip.stream.authentication = hold.authentication;
                    device->ntrip.stream.fee = hold.fee;
                    device->ntrip.stream.bitrate = hold.bitrate;
                    device->ntrip.stream.set = true;
                    match = true;
                }
                /* TODO: compare stream location to own location to
                 * find nearest stream if user hasn't provided one */
            }
            else if (str_starts_with(line, NTRIP_CAS)) {
                // TODO: parse CAS
                // See: http://software.rtcm-ntrip.org/wiki/CAS
                GPSD_LOG(LOG_WARN, &device->context->errout,
                         "NTRIP: Can't parse CAS '%s'\n", line);
            } else if (str_starts_with(line, NTRIP_NET)) {
                // TODO: parse NET
                // See: http://software.rtcm-ntrip.org/wiki/NET
                GPSD_LOG(LOG_WARN, &device->context->errout,
                         "NTRIP: Can't parse NET '%s'\n", line);
            }

            llen += strlen(NTRIP_BR);
            line += llen;
            len -= llen;
            GPSD_LOG(LOG_RAW, &device->context->errout,
                     "NTRIP: Remaining source table buffer %zd %s\n", len,
                     line);
        }
        // message too big to fit into buffer
        if ((blen - 1) == (size_t)len) {
            return -1;
        }

        if (0 < len) {
            memmove(buf, &buf[rlen - len], (size_t) len);
        }
    }

done:
    return match ? 1 : -1;
}

static int ntrip_stream_req_probe(const struct ntrip_stream_t *stream,
                                  struct gpsd_errout_t *errout)
{
    int dsock;
    ssize_t r;
    char buf[BUFSIZ];

    dsock = netlib_connectsock(AF_UNSPEC, stream->url, stream->port, "tcp");
    if (0 > dsock) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: stream connect error %d in req probe\n", dsock);
        return -1;
    }
    GPSD_LOG(LOG_SPIN, errout,
             "NTRIP: stream for req probe connected on fd %d\n", dsock);
    (void)snprintf(buf, sizeof(buf),
            "GET / HTTP/1.1\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "User-Agent: NTRIP gpsd/%s\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n", VERSION, stream->url);
    r = write(dsock, buf, strlen(buf));
    if ((ssize_t)strlen(buf) != r) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: stream write error %s on fd %d "
                 "during probe request %zd\n",
                 strerror(errno), dsock, r);
        (void)close(dsock);
        return -1;
    }
    // coverity[leaked_handle] This is an intentional allocation
    return dsock;
}

static int ntrip_auth_encode(const struct ntrip_stream_t *stream,
                             const char *auth,
                             char buf[],
                             size_t size)
{
    memset(buf, 0, size);
    if (AUTH_NONE == stream->authentication) {
        return 0;
    } else if (AUTH_BASIC == stream->authentication) {
        char authenc[64];
        if (!auth)
            return -1;
        memset(authenc, 0, sizeof(authenc));
        if (0 > b64_ntop((unsigned char *)auth, strlen(auth), authenc,
                         sizeof(authenc) - 1)) {
            return -1;
        }
        (void)snprintf(buf, size - 1, "Authorization: Basic %s\r\n", authenc);
    } else {
        // TODO: support digest authentication
    }
    return 0;
}

static socket_t ntrip_stream_get_req(const struct ntrip_stream_t *stream,
                                     const struct gpsd_errout_t *errout)
{
    int dsock;
    char buf[BUFSIZ];

    dsock = netlib_connectsock(AF_UNSPEC, stream->url, stream->port, "tcp");
    if (BAD_SOCKET(dsock)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: stream connect error %d\n", dsock);
        return -1;
    }

    GPSD_LOG(LOG_SPIN, errout,
             "NTRIP: netlib_connectsock() returns socket on fd %d\n",
             dsock);

    (void)snprintf(buf, sizeof(buf),
            "GET /%s HTTP/1.1\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "User-Agent: NTRIP gpsd/%s\r\n"
            "Host: %s\r\n"
            "Accept: rtk/rtcm, dgps/rtcm\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n", stream->mountpoint, VERSION, stream->url, stream->authStr);
    if ((ssize_t)strlen(buf) != write(dsock, buf, strlen(buf))) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: stream write error %s(%d) on fd %d during "
                 "get request\n",
                 strerror(errno), errno, dsock);
        (void)close(dsock);
        return -1;
    }
    return dsock;
}

/* parse the stream header
 * Return: 0 == OK
 *         -1 == fail
 */
static int ntrip_stream_get_parse(const struct ntrip_stream_t *stream,
                                  const int dsock,
                                  const struct gpsd_errout_t *errout)
{
    char buf[BUFSIZ];
    int opts;

    memset(buf, 0, sizeof(buf));
    while (-1 == read(dsock, buf, sizeof(buf) - 1)) {
        if (EINTR == errno) {
            continue;
        }
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: stream read error %s(%d) on fd %d during get rsp\n",
                 strerror(errno), errno, dsock);
        return -1;
    }
    buf[sizeof(buf) - 1] = '\0';   // pacify coverity about NUL-terminated.

    // parse 401 Unauthorized
    if (NULL != strstr(buf, NTRIP_UNAUTH)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: not authorized for stream %s/%s\n", stream->url,
                 stream->mountpoint);
        return -1;
    }
    // can't parse SOURCETABLE here
    if (NULL != strstr(buf, NTRIP_SOURCETABLE)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: caster doesn't recognize stream %s:%s/%s\n",
                 stream->url, stream->port, stream->mountpoint);
        return -1;
    }
    // parse "ICY 200 OK" or "HTTP/1.1 200 OK"
    if (NULL == strstr(buf, NTRIP_ICY) &&
        NULL == strstr(buf, NTRIP_HTTP)) {
        GPSD_LOG(LOG_ERROR, errout,
                 "NTRIP: Unknown reply %s from caster: %s:%s/%s\n", buf,
                 stream->url, stream->port, stream->mountpoint);
        return -1;
    }
    opts = fcntl(dsock, F_GETFL);

    if (0 <= opts) {
        (void)fcntl(dsock, F_SETFL, opts | O_NONBLOCK);
    }

    return 0;
}

// open a connection to a Ntrip broadcaster
int ntrip_open(struct gps_device_t *device, char *orig)
{
    char *amp, *colon, *slash;
    char *auth = NULL, dup[256], *caster = dup;
    char *port = NULL;
    char *stream = NULL;
    char *url = NULL;
    socket_t ret = -1;

    GPSD_LOG(LOG_PROG, &device->context->errout,
             "NTRIP: ntrip_open(%s) state = %d\n",
             orig, device->ntrip.conn_state);

    switch (device->ntrip.conn_state) {
    case NTRIP_CONN_INIT:
        /* this has to be done here,
         * because it is needed for multi-stage connection */
        // strlcpy() ensures dup is NUL terminated.
        strlcpy(dup, orig, 255);
        device->servicetype = SERVICE_NTRIP;
        device->ntrip.works = false;
        device->ntrip.sourcetable_parse = false;
        device->ntrip.stream.set = false;
        device->gpsdata.gps_fd = UNALLOCATED_FD;

        /* Test cases
         * ntrip://userid:passwd@ntrip.com:2101/MOUNT-POINT
         * ntrip://a@b.com:passwd@ntrip.com:2101/MOUNT-POINT
         * ntrip://userid:passwd@@@ntrip.com:2101/MOUNT-POINT
         * ntrip://a@b.com:passwd@@@ntrip.com:2101/MOUNT-POINT */
        if (NULL != (amp = strrchr(caster, '@')) &&
            NULL != (colon = strchr(caster, ':')) &&
            colon < amp) {
            auth = caster;
            *amp = '\0';
            caster = amp + 1;
            url = caster;
        }
        if (NULL != (slash = strchr(caster, '/'))) {
            *slash = '\0';
            stream = slash + 1;
        } else {
            // TODO: add autoconnect like in dgpsip.c
            GPSD_LOG(LOG_ERROR, &device->context->errout,
                     "NTRIP: can't extract stream from %s\n",
                     caster);
            device->ntrip.conn_state = NTRIP_CONN_ERR;
            return -1;
        }
        if (NULL != (colon = strchr(caster, ':'))) {
            port = colon + 1;
            *colon = '\0';
        }

        if (NULL == url) {
            // there was no @ in caster
            url = caster;
        }
        if (!port) {
            port = "rtcm-sc104";
            if (!getservbyname(port, "tcp")) {
                port = DEFAULT_RTCM_PORT;
            }
        }

        (void)strlcpy(device->ntrip.stream.mountpoint,
                      stream, sizeof(device->ntrip.stream.mountpoint));
        if (NULL != auth) {
            (void)strlcpy(device->ntrip.stream.credentials,
                          auth, sizeof(device->ntrip.stream.credentials));
        }
        /*
         * Semantically url and port ought to be non-NULL by now,
         * but just in case...this code appeases Coverity.
         */
        if (NULL != url) {
            (void)strlcpy(device->ntrip.stream.url,
                          url, sizeof(device->ntrip.stream.url));
        }
        if (NULL != port) {
            (void)strlcpy(device->ntrip.stream.port,
                          port, sizeof(device->ntrip.stream.port));
        }

        ret = ntrip_stream_req_probe(&device->ntrip.stream,
                                     &device->context->errout);
        if (-1 == ret) {
            device->ntrip.conn_state = NTRIP_CONN_ERR;
            return -1;
        }
        device->gpsdata.gps_fd = ret;
        device->ntrip.conn_state = NTRIP_CONN_SENT_PROBE;
        return ret;
    case NTRIP_CONN_SENT_PROBE:
        ret = ntrip_sourcetable_parse(device);
        if (-1 == ret) {
            device->ntrip.conn_state = NTRIP_CONN_ERR;
            return -1;
        }
        if (0 == ret &&
            false == device->ntrip.stream.set) {
            return ret;
        }
        (void)close(device->gpsdata.gps_fd);
        if (0 != ntrip_auth_encode(&device->ntrip.stream,
                                   device->ntrip.stream.credentials,
                                   device->ntrip.stream.authStr,
                                   sizeof(device->ntrip.stream.authStr))) {
            device->ntrip.conn_state = NTRIP_CONN_ERR;
            return -1;
        }
        ret = ntrip_stream_get_req(&device->ntrip.stream,
                                   &device->context->errout);
        if (-1 == ret) {
            device->gpsdata.gps_fd = UNALLOCATED_FD;
            device->ntrip.conn_state = NTRIP_CONN_ERR;
            return -1;
        }
        device->gpsdata.gps_fd = ret;
        device->ntrip.conn_state = NTRIP_CONN_SENT_GET;
        break;
    case NTRIP_CONN_SENT_GET:
        ret = ntrip_stream_get_parse(&device->ntrip.stream,
                                     device->gpsdata.gps_fd,
                                     &device->context->errout);
        if (-1 == ret) {
            (void)close(device->gpsdata.gps_fd);
            device->gpsdata.gps_fd = UNALLOCATED_FD;
            device->ntrip.conn_state = NTRIP_CONN_ERR;
            return -1;
        }
        device->ntrip.conn_state = NTRIP_CONN_ESTABLISHED;
        device->ntrip.works = true;   // we know, this worked.
        break;
    case NTRIP_CONN_ESTABLISHED:
        FALLTHROUGH
    case NTRIP_CONN_ERR:
        return -1;
    }
    return ret;
}

// may be time to ship a usage report to the NTRIP caster
void ntrip_report(struct gps_context_t *context,
                  struct gps_device_t *gps,
                  struct gps_device_t *caster)
{
    static int count;
    /*
     * 10 is an arbitrary number, the point is to have gotten several good
     * fixes before reporting usage to our NTRIP caster.
     *
     * count % 5 is as arbitrary a number as the fixcnt. But some delay
     * was needed here
     */
    count ++;
    if (0 != caster->ntrip.stream.nmea &&
        10 < context->fixcnt &&
        0 == (count % 5)) {
        if (-1 < caster->gpsdata.gps_fd) {
            char buf[BUFSIZ];
            ssize_t ret;

            gpsd_position_fix_dump(gps, buf, sizeof(buf));
            ret = write(caster->gpsdata.gps_fd, buf, strlen(buf));
            if ((ssize_t)strlen(buf) == ret) {
                GPSD_LOG(LOG_IO, &context->errout, "=> dgps %s\n", buf);
            } else if (0 > ret) {
                GPSD_LOG(LOG_ERROR, &context->errout,
                         "NTRIP: ntrip_report() write(%d) error %s(%d)\n",
                         caster->gpsdata.gps_fd, strerror(errno), errno);
            } else {
                GPSD_LOG(LOG_ERROR, &context->errout,
                         "NTRIP: ntrip_report() short write(%d) = %zd\n",
                         caster->gpsdata.gps_fd, ret);
            }
        }
    }
}

// vim: set expandtab shiftwidth=4
