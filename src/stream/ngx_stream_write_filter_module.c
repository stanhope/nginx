
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>


typedef struct {
    ngx_chain_t  *from_upstream;
    ngx_chain_t  *from_downstream;
} ngx_stream_write_filter_ctx_t;


static ngx_int_t ngx_stream_write_filter(ngx_stream_session_t *s,
    ngx_chain_t *in, ngx_uint_t from_upstream);
static ngx_int_t ngx_stream_write_filter_init(ngx_conf_t *cf);


static ngx_stream_module_t  ngx_stream_write_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_stream_write_filter_init,          /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL                                   /* merge server configuration */
};


ngx_module_t  ngx_stream_write_filter_module = {
    NGX_MODULE_V1,
    &ngx_stream_write_filter_module_ctx,   /* module context */
    NULL,                                  /* module directives */
    NGX_STREAM_MODULE,                     /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


#define HEXDUMP_COLS 32
void hexdump(void *mem, unsigned int len)
{
        unsigned int i, j;
        for(i = 0; i < len + ((len % HEXDUMP_COLS) ? (HEXDUMP_COLS - len % HEXDUMP_COLS) : 0); i++)
        {
                /* print offset */
                if(i % HEXDUMP_COLS == 0)
                {
                        printf("0x%06x: ", i);
                }
 
                /* print hex data */
                if(i < len)
                {
                        printf("%02x ", 0xFF & ((char*)mem)[i]);
                }
                else /* end of block, just aligning for ASCII dump */
                {
                        printf("   ");
                }
                
                /* print ASCII dump */
                if(i % HEXDUMP_COLS == (HEXDUMP_COLS - 1))
                {
                        for(j = i - (HEXDUMP_COLS - 1); j <= i; j++)
                        {
                                if(j >= len) /* end of block, not really printing */
                                {
                                        putchar(' ');
                                }
                                else if(isprint(((char*)mem)[j])) /* printable char */
                                {
                                        putchar(0xFF & ((char*)mem)[j]);        
                                }
                                else /* other char */
                                {
                                        putchar('.');
                                }
                        }
                        putchar('\n');
                }
        }
}

static ngx_int_t
ngx_stream_write_filter(ngx_stream_session_t *s, ngx_chain_t *in,
    ngx_uint_t from_upstream)
{
    off_t                           size;
    ngx_uint_t                      last, flush, sync;
    ngx_chain_t                    *cl, *ln, **ll, **out, *chain;
    ngx_connection_t               *c;
    ngx_stream_write_filter_ctx_t  *ctx;

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_write_filter_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(s->connection->pool,
                          sizeof(ngx_stream_write_filter_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_stream_set_ctx(s, ctx, ngx_stream_write_filter_module);
    }

    if (from_upstream) {
        c = s->connection;
        out = &ctx->from_upstream;
    } else {
        c = s->upstream->peer.connection;
        out = &ctx->from_downstream;
    }

    if (c->error) {
        return NGX_ERROR;
    }

    size = 0;
    flush = 0;
    sync = 0;
    last = 0;
    ll = out;

    /* find the size, the flush point and the last link of the saved chain */

    for (cl = *out; cl; cl = cl->next) {
        ll = &cl->next;

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "write old buf t:%d f:%d %p, pos %p, size: %z "
                       "file: %O, size: %O",
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);
	
#if 1
	if (ngx_buf_size(cl->buf) == 0 && !ngx_buf_special(cl->buf)) {
	  ngx_log_error(NGX_LOG_ALERT, c->log, 0,
			"zero size buf in writer "
			"t:%d r:%d f:%d %p %p-%p %p %O-%O",
			cl->buf->temporary,
                          cl->buf->recycled,
                          cl->buf->in_file,
                          cl->buf->start,
                          cl->buf->pos,
                          cl->buf->last,
                          cl->buf->file,
                          cl->buf->file_pos,
                          cl->buf->file_last);

            ngx_debug_point();
	    fprintf(stdout, "zero size buf in writer \n");
            return NGX_ERROR;
        }
#endif

	off_t size_buf = ngx_buf_size(cl->buf);
        size += size_buf;

        if (cl->buf->flush || cl->buf->recycled) {
            flush = 1;
        }

        if (cl->buf->sync) {
            sync = 1;
        }

        if (cl->buf->last_buf) {
            last = 1;
        }
    }

    /* add the new chain to the existent one */
    for (ln = in; ln; ln = ln->next) {
        cl = ngx_alloc_chain_link(c->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = ln->buf;
        *ll = cl;
        ll = &cl->next;

        ngx_log_debug7(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "write new buf t:%d f:%d %p, pos %p, size: %z "
                       "file: %O, size: %O",
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);

	
	ngx_stream_upstream_t *u = s->upstream;

	if (s->upstream->dyn_proxy_protocol && !from_upstream && c->type != SOCK_DGRAM) {

	    off_t msg_len = cl->buf->last - cl->buf->pos;
	    u_char proxy_line[NGX_PROXY_PROTOCOL_MAX_HEADER];
	    u_char *last = ngx_dyn_proxy_protocol_write(s->connection, proxy_line, proxy_line + NGX_PROXY_PROTOCOL_MAX_HEADER);
	    uint proxy_line_size = (last - proxy_line);
	    u_char *proxy_line_buff = ngx_pnalloc(c->pool, NGX_PROXY_PROTOCOL_MAX_HEADER);

	    // Add the proxy line (offset 2 to account for lead length bytes)
	    memcpy(proxy_line_buff+2, proxy_line, proxy_line_size);

	    // Add original message, adjust total length
	    memcpy(proxy_line_buff+2+proxy_line_size, cl->buf->pos+2, msg_len-2);
	    proxy_line_buff[0] = 0;
	    proxy_line_buff[1] = proxy_line_size + msg_len - 2;

	    // Free original buf
	    ngx_pfree(c->pool, cl->buf->pos);

	    // Adjust total length
	    cl->buf->pos = proxy_line_buff;
	    cl->buf->last = proxy_line_buff + proxy_line_size + msg_len + 2;

	    u->dyn_proxy_protocol = 0;
	}
	
	/*
        fprintf(stdout, "  write new buf t:%d f:%d start:%p, pos %p, size: %ld\n",
		cl->buf->temporary, cl->buf->in_file,
		cl->buf->start, cl->buf->pos,
		cl->buf->last - cl->buf->pos);

	hexdump(cl->buf->pos, (cl->buf->last - cl->buf->pos));
	*/

	off_t size_buf = ngx_buf_size(cl->buf);
        size += size_buf;

        if (cl->buf->flush || cl->buf->recycled) {
            flush = 1;
        }

        if (cl->buf->sync) {
            sync = 1;
        }

        if (cl->buf->last_buf) {
            last = 1;
        }
    }

    *ll = NULL;

    ngx_log_debug3(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream write filter: l:%ui f:%ui s:%O", last, flush, size);

    if (size == 0
        && !(c->buffered & NGX_LOWLEVEL_BUFFERED)
        && !(last && c->need_last_buf))
    {
        if (last || flush || sync) {
            for (cl = *out; cl; /* void */) {
                ln = cl;
                cl = cl->next;
                ngx_free_chain(c->pool, ln);
            }

            *out = NULL;
            c->buffered &= ~NGX_STREAM_WRITE_BUFFERED;

            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "the stream output chain is empty");

        ngx_debug_point();

        return NGX_ERROR;
    } 

    chain = c->send_chain(c, *out, 0);

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "stream write filter %p", chain);

    if (chain == NGX_CHAIN_ERROR) {
        c->error = 1;
        return NGX_ERROR;
    }

    for (cl = *out; cl && cl != chain; /* void */) {
        ln = cl;
        cl = cl->next;
        ngx_free_chain(c->pool, ln);
    }

    *out = chain;

    if (chain) {
        if (c->shared) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "shared connection is busy");
            return NGX_ERROR;
        }

        c->buffered |= NGX_STREAM_WRITE_BUFFERED;
        return NGX_AGAIN;
    }

    c->buffered &= ~NGX_STREAM_WRITE_BUFFERED;

    if (c->buffered & NGX_LOWLEVEL_BUFFERED) {
        return NGX_AGAIN;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_stream_write_filter_init(ngx_conf_t *cf)
{
    ngx_stream_top_filter = ngx_stream_write_filter;

    return NGX_OK;
}
