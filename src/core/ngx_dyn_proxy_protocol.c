
/*
 * Copyright (C) Roman Arutyunyan
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static double current_time(void) {
        struct timeval tv;
        if (gettimeofday(&tv, 0) < 0 )
		return 0;
        double now = tv.tv_sec + tv.tv_usec / 1e6;
        return now;
}

u_char *
ngx_dyn_proxy_protocol_write(ngx_connection_t *c, u_char *buf, u_char *last)
{
    ngx_uint_t  port;

    fprintf(stdout, "dyn_proxy_protocol_write max=%ld c=%p\n", last - buf, c);
    
    if (last - buf < NGX_PROXY_PROTOCOL_MAX_HEADER) {
        return NULL;
    }

    if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
        return NULL;
    }

    port = ngx_inet_get_port(c->sockaddr);
    unsigned char SRC_ADDR[64];
    size_t cnt = ngx_sock_ntop(c->sockaddr, c->socklen, SRC_ADDR, 63, 0);
    SRC_ADDR[cnt] = 0;
    unsigned char DST_ADDR[64];
    cnt = ngx_sock_ntop(c->local_sockaddr, c->local_socklen, DST_ADDR, 63, 0);
    DST_ADDR[cnt] = 0;
    
    double now = current_time();
    char PROXY_INFO[128];
    sprintf(PROXY_INFO, "%f %s %lu %s", now, SRC_ADDR, port, DST_ADDR);
    char BUFF[128];
    int PROXY_INFO_LEN = strlen(PROXY_INFO);
    sprintf(BUFF+2,"%s", PROXY_INFO);
    BUFF[0] = 0xFF;
    BUFF[1] = PROXY_INFO_LEN;
    BUFF[PROXY_INFO_LEN+3] = 0;
    
    fprintf(stdout, " DYN_PROXY_INFO => \"%s\" [len:%d]\n", PROXY_INFO, PROXY_INFO_LEN);
	      
    memcpy(buf, BUFF, PROXY_INFO_LEN+2);
    fprintf(stdout, " %s\n", BUFF);
    return buf + (PROXY_INFO_LEN+2);

}
