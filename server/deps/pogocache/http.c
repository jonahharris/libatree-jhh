// https://github.com/tidwall/pogocache
//
// Copyright 2025 Polypoint Labs, LLC. All rights reserved.
// This file is part of the Pogocache project.
// Use of this source code is governed by the MIT that can be found in
// the LICENSE file.
//
// For alternative licensing options or general questions, please contact
// us at licensing@polypointlabs.com.
//
// Unit http.c provides the parser for the HTTP wire protocol.
#define _GNU_SOURCE  
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "stats.h"
#include "util.h"
#include "parse.h"


ssize_t parse_http(const char *data, size_t len, struct args *args, 
    int *httpvers, bool *keepalive)
{
    *keepalive = false;
    *httpvers = 0;
    const char *method = 0;
    size_t methodlen = 0;
    const char *uri = 0;
    size_t urilen = 0;
    int proto = 0;
    const char *hdrname = 0; 
    size_t hdrnamelen = 0;
    const char *hdrval = 0;
    size_t hdrvallen = 0;
    size_t bodylen = 0;
    const char *accept = 0;
    size_t acceptlen = 0;
    const char *p = data;
    const char *e = p+len;
    const char *s = p;
    while (p < e) {
        if (*p == ' ') {
            method = s;
            methodlen = p-s;
            p++;
            break;
        }
        if (*p == '\n') {
            goto badreq;
        }
        p++;
    }
    s = p;
    while (p < e) {
        if (*p == ' ') {
            uri = s;
            urilen = p-s;
            p++;
            break;
        }
        if (*p == '\n') {
            goto badreq;
        }
        p++;
    }
    s = p;
    while (p < e) {
        if (*p == '\n') {
            if (*(p-1) != '\r') {
                goto badreq;
            }
            if (p-s-1 != 8 || !bytes_const_eq(s, 5, "HTTP/") || 
                s[5] < '0' || s[5] > '9' || s[6] != '.' || 
                s[7] < '0' || s[7] > '9')
            {
                goto badproto;
            }
            proto = (s[5]-'0')*10+(s[7]-'0');
            if (proto < 9 || proto >= 30) {
                goto badproto;
            }
            if (proto >= 11) {
                *keepalive = true;
            }
            *httpvers = proto;
            p++;
            goto readhdrs;
        }
        
        p++;
    }
    goto badreq;
readhdrs:
    // Parse the headers, pulling the pairs along the way.
    while (p < e) {
        hdrname = p;
        while (p < e) {
            if (*p == ':') {
                hdrnamelen = p-hdrname;
                p++;
                while (p < e && *p == ' ') {
                    p++;
                }
                hdrval = p;
                while (p < e) {
                    if (*p == '\n') {
                        if (*(p-1) != '\r') {
                            goto badreq;
                        }
                        hdrvallen = p-hdrval-1;
                        // printf("[%.*s]=[%.*s]\n", (int)hdrnamelen, hdrname,
                        //     (int)hdrvallen, hdrval);
                        // We have a new header pair (hdrname, hdrval);
                        if (argeq_bytes(hdrname, hdrnamelen, "content-length")){
                            uint64_t x;
                            if (!parse_u64(hdrval, hdrvallen, &x) || 
                                x > MAXARGSZ)
                            {
                                stat_store_too_large_incr(0);
                                goto badreq;
                            }
                            bodylen = x;
                        } else if (argeq_bytes(hdrname, hdrnamelen,
                            "connection"))
                        {
                            *keepalive = argeq_bytes(hdrval, hdrvallen, 
                                "keep-alive");
                        } else if (argeq_bytes(hdrname, hdrnamelen,
                            "accept"))
                        {
                            accept = hdrval;
                            acceptlen = hdrvallen;
                        }
                        p++;
                        if (p < e && *p == '\r') {
                            p++;
                            if (p < e && *p == '\n') {
                                p++;
                            } else {
                                goto badreq;
                            }
                            goto readbody;
                        }
                        break;
                    }
                    p++;
                }
                break;
            }
            p++;
        }
    }
    return 0;
readbody:
    // read the content body
    if ((size_t)(e-p) < bodylen) {
        return 0;
    }
    const char *body = p;
    p = e;

    // atreed: hand the request to the command layer as
    // ["HTTP", method, path, query, body, accept]; routing happens there.
    if (urilen == 0 || uri[0] != '/') {
        goto badreq;
    }
    size_t querylen = 0;
    const char *query = memchr(uri, '?', urilen);
    if (query) {
        querylen = urilen-(query-uri)-1;
        urilen = query-uri;
        query++;
    } else {
        query = "";
    }
    args_append(args, "HTTP", 4, true);
    args_append(args, method, methodlen, true);
    args_append(args, uri, urilen, true);
    args_append(args, query, querylen, true);
    args_append(args, body, bodylen, true);
    args_append(args, accept ? accept : "", acceptlen, true);
    return e-data;
badreq:
    parse_seterror("Bad Request");
    return -1;
badproto:
    parse_seterror("Bad Request");
    return -1;
}
