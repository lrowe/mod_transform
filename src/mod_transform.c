/**
 *    Copyright (c) 2002 WebThing Ltd
 *    Copyright (c) 2004 Edward Rudd
 *    Copyright (c) 2004 Paul Querna
 *    Authors:    Nick Kew <nick webthing.com>
 *                Edward Rudd <eddie at omegaware dot com>
 *                Paul Querna <chip at force-elite.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


#define XSLT_FILTER_NAME "XSLT"

#include "mod_transform.h"

#include "http_config.h"
#include "http_protocol.h"
#include "http_core.h"
#include "http_log.h"
#include "apr_buckets.h"
#include "apr_strings.h"
#include "apr_uri.h"
#include "apr_tables.h"

#include <libxml/xinclude.h>
#include <libxml/xmlIO.h>
#include <libxslt/xsltutils.h>
#include <libxslt/transform.h>

#include <libgen.h> /* for dirname() */

module AP_MODULE_DECLARE_DATA transform_module;

/* TransformOptions */
#define NO_OPTIONS          (1 <<  0)
#define USE_APACHE_FS       (1 <<  1)
#define XINCLUDES           (1 <<  2)

/* Static Style Sheet Caching */
typedef struct transform_xslt_cache
{
    const char *id;
    xsltStylesheetPtr transform;
    struct transform_xslt_cache *next;
} transform_xslt_cache;

typedef struct svr_cfg
{
    transform_xslt_cache *data;
} svr_cfg;

static void *transform_get_cached(svr_cfg * sconf, const char *descriptor)
{
    transform_xslt_cache *p;
    if (!descriptor)
        return 0;

    for (p = sconf->data; p; p = p->next) {
        if (!strcmp(descriptor, p->id)) {
            return p->transform;
        }
    }

    return 0;
}

static const char *transform_add_xslt_cache(cmd_parms * cmd, void *cfg,
                                        const char *url, const char *path)
{
    svr_cfg *conf = ap_get_module_config(cmd->server->module_config,
                                         &transform_module);
    xsltStylesheetPtr xslt = xsltParseStylesheetFile(path);
    if (url && path && xslt) {
        transform_xslt_cache *me = apr_palloc(cmd->pool, sizeof(transform_xslt_cache));
        me->id = apr_pstrdup(cmd->pool, url);
        me->transform = xslt;
        me->next = conf->data;
        conf->data = me;
        ap_log_perror(APLOG_MARK, APLOG_NOTICE, 0, cmd->pool,
                      "Cached precompiled XSLT %s", url);
        return NULL;
    }
    else {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, cmd->pool,
                      "Error fetching or compiling XSLT from %s", path);
        return "Error trying to precompile XSLT";
    }
}

static apr_status_t freeCache(void *conf)
{
    transform_xslt_cache *p;
    svr_cfg *cfg = conf;
    for (p = cfg->data; p; p = p->next) {
        xsltFreeStylesheet(p->transform);
    }
    return APR_SUCCESS;
}

static void *create_server_cfg(apr_pool_t * p, server_rec * x)
{
    svr_cfg *cfg = apr_pcalloc(p, sizeof(svr_cfg));
    apr_pool_cleanup_register(p, cfg, freeCache, apr_pool_cleanup_null);
    return cfg;
}


typedef struct dir_cfg
{
    const char *xslt;
    apr_int32_t opts;
    apr_int32_t incremented_opts;
    apr_int32_t decremented_opts;
} dir_cfg;

typedef struct
{
    const char *xslt;
    xmlDocPtr document;
} modxml_notes;


static void transform_error_cb(void *ctx, const char *msg, ...)
{
    va_list args;
    char *fmsg;
    ap_filter_t *f = (ap_filter_t *) ctx;
    va_start(args, msg);
    fmsg = apr_pvsprintf(f->r->pool, msg, args);
    va_end(args);
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, f->r, fmsg);
}

static apr_status_t pass_failure(ap_filter_t * filter, const char *msg,
                                 modxml_notes * notes)
{
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, filter->r, msg);
    xmlSetGenericErrorFunc(NULL, NULL);
    return HTTP_INTERNAL_SERVER_ERROR;;
}


typedef struct
{
    ap_filter_t *next;
    apr_bucket_brigade *bb;
} transform_output_ctx;

typedef struct
{
    ap_filter_t *f;
} transform_input_ctx;

static int transform_output_write(void *context, const char *buffer, int len)
{
    if (len > 0) {
        transform_output_ctx *octx = (transform_output_ctx *) context;
        ap_fwrite(octx->next, octx->bb, buffer, len);
    }
    return len;
}

static int transform_output_close(void *context)
{
    transform_output_ctx *octx = (transform_output_ctx *) context;
    apr_bucket *b = apr_bucket_eos_create(octx->bb->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(octx->bb, b);
    return 0;
}

/**
 * This Function is part of a patch to APR-Util: http://issues.apache.org/bugzilla/show_bug.cgi?id=28453
 * Even if it is commited to APR-Util, it will be one full release cycle before 
 * it shows up in the HTTPd Release. 
 * In addition it is doubtful that it will be in the 0.9 Branch, and therefore, 
 * we would have to wait untill 2.1 becomes 2.2. BAH. Sometimes I hate Apache/APR.
 * Special Thanks to Nick Kew :)
 */
/* Resolve relative to a base.  This means host/etc, and (crucially) path */
static apr_status_t ex_apr_uri_resolve_relative(apr_pool_t * pool,
                                                apr_uri_t * base,
                                                apr_uri_t * uptr)
{
    if (uptr == NULL
        || base == NULL || !base->is_initialized || !uptr->is_initialized) {
        return APR_EGENERAL;
    }
    /* The interesting bit is the path.  */
    if (uptr->path == NULL) {
        if (uptr->hostname == NULL) {
            /* is this compatible with is_initialised?  Harmless in any case */
            uptr->path = base->path ? base->path : apr_pstrdup(pool, "/");
        }
        else {
            /* deal with the idiosyncracy of APR allowing path==NULL
             ** without risk of breaking back-compatibility
             */
            uptr->path = apr_pstrdup(pool, "/");
        }
    }
    else if (uptr->path[0] != '/') {
        size_t baselen;
        const char *basepath = base->path ? base->path : "/";
        const char *path = uptr->path;
        const char *base_end = ap_strrchr_c(basepath, '/');

        /* if base is nonsensical, bail out */
        if (basepath[0] != '/') {
            return APR_EGENERAL;
        }
        /* munch "up" components at the start, and chop them from base path */
        while (!strncmp(path, "../", 3)) {
            while (base_end > basepath) {
                if (*--base_end == '/') {
                    break;
                }
            }
            path += 3;
        }
        /* munch "here" components at the start */
        while (!strncmp(path, "./", 2)) {
            path += 2;
        }
        baselen = base_end - basepath + 1;
        uptr->path = apr_palloc(pool, baselen + strlen(path) + 1);
        memcpy(uptr->path, basepath, baselen);
        strcpy(uptr->path + baselen, path);
    }

    /* The trivial bits are everything-but-path */
    if (uptr->scheme == NULL) {
        uptr->scheme = base->scheme;
    }
    if (uptr->hostinfo == NULL) {
        uptr->hostinfo = base->hostinfo;
    }
    if (uptr->user == NULL) {
        uptr->user = base->user;
    }
    if (uptr->password == NULL) {
        uptr->password = base->password;
    }
    if (uptr->hostname == NULL) {
        uptr->hostname = base->hostname;
    }
    if (uptr->port_str == NULL) {
        uptr->port_str = base->port_str;
    }
    if (uptr->hostent == NULL) {
        uptr->hostent = base->hostent;
    }
    if (!uptr->port) {
        uptr->port = base->port;
    }
    return APR_SUCCESS;
}

static const char* find_relative_uri(ap_filter_t * f, const char* orig_href)
{
    apr_uri_t url;
    apr_uri_t base_url;
    const char *basedir;
    char* href;
    if (orig_href) {
        if (apr_uri_parse(f->r->pool, orig_href, &url) == APR_SUCCESS) {
            // TODO: dirname() is not Win32 Portable.
            // TODO: Replace with custom dirname() like function. strrchr() is our friend.
            basedir = dirname(apr_pstrdup(f->r->pool, f->r->filename));
            apr_uri_parse(f->r->pool,
                          apr_psprintf(f->r->pool, "file://%s/", basedir),
                          &base_url);
            ex_apr_uri_resolve_relative(f->r->pool, &base_url, &url);
            href = apr_uri_unparse(f->r->pool, &url, 0);
            return href;
        }
    }
    return orig_href;
}

static xmlParserInputBufferPtr transform_get_input(const char *URI, xmlCharEncoding enc)
{
    xmlParserInputBufferPtr ret;
    transform_input_ctx* input_ctx;
    ap_filter_t *f = (ap_filter_t *)xmlGenericErrorContext;
    dir_cfg *dconf = ap_get_module_config(f->r->per_dir_config,
                                          &transform_module);

    if (URI == NULL) 
        return NULL;

    if (dconf->opts & USE_APACHE_FS && 0) {
        /* We want to use an Apache based Fliesystem for Libxml. Let the fun begin. */
        ret = xmlAllocParserInputBuffer(enc);
        if (ret != NULL) {
            input_ctx = apr_palloc(f->r->pool, sizeof(input_ctx));
            input_ctx->f = f;
            ret->context = input_ctx;
//            ret->readcallback = xmlInputCallbackTable[i].readcallback;
//            ret->closecallback = xmlInputCallbackTable[i].closecallback;
        }
    }
    else {
        /* TODO: Fixup Relative Paths here */
        ret = __xmlParserInputBufferCreateFilename(find_relative_uri(f, URI), enc);
    }
    return ret;
}

static apr_status_t transform_run(ap_filter_t * f, xmlDocPtr doc)
{
    size_t length;
    transform_output_ctx output_ctx;
    int stylesheet_is_cached = 0;
    xsltStylesheetPtr transform = NULL;
    xmlDocPtr result = NULL;
    xmlOutputBufferPtr output;
    xmlParserInputBufferCreateFilenameFunc orig;
    modxml_notes *notes =
        ap_get_module_config(f->r->request_config, &transform_module);
    dir_cfg *dconf = ap_get_module_config(f->r->per_dir_config,
                                          &transform_module);
    svr_cfg *sconf = ap_get_module_config(f->r->server->module_config,
                                          &transform_module);

    if (!doc)
        return pass_failure(f, "XSLT: Couldn't parse document", notes);

    orig = xmlParserInputBufferCreateFilenameDefault(transform_get_input);

    if (dconf->opts & XINCLUDES) {
#if LIBXML_VERSION >= 20603
        /* TODO: Make an easy way to enable/disable Loading Files from the Network. */
        /* TODO: xsltSetXIncludeDefault(1); ? */
        xmlXIncludeProcessFlags(doc,
                                XML_PARSE_RECOVER | XML_PARSE_XINCLUDE |
                                XML_PARSE_NONET);
#else
        xmlXIncludeProcess(doc);
#endif
    }
    if (notes->xslt) {
        if (transform = transform_get_cached(sconf, notes->xslt), transform) {
            stylesheet_is_cached = 1;
        }
        else {
            transform = xsltParseStylesheetFile(notes->xslt);
        }
    }
    else {
        transform = xsltLoadStylesheetPI(doc);
    }

    if (!transform) {
        /* TODO: Need better error reporting here. Why couldn't we Load it? */
        xmlParserInputBufferCreateFilenameDefault(orig);
        return pass_failure(f, "XSLT: Couldn't load transform", notes);
    }
    result = xsltApplyStylesheet(transform, doc, 0);
    if (!result) {
        if (!stylesheet_is_cached)
            xsltFreeStylesheet(transform);
        /* TODO: Need better error reporting here. What Went Wrong? */
        xmlParserInputBufferCreateFilenameDefault(orig);
        return pass_failure(f, "XSLT: Couldn't run transform", notes);
    }

    if (transform->mediaType) {
        /* Note: If the XSLT We are using doesn't have an encoding, 
               We will use the server default. */
        if (transform->encoding) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, f->r,
                          "Setting content-type to: '%s; charset=%s'",
                          transform->mediaType, transform->encoding);
            ap_set_content_type(f->r,
                                apr_psprintf(f->r->pool, "%s; charset=%s",
                                             transform->mediaType,
                                             transform->encoding));
        }
        else if (doc->encoding) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, f->r,
                          "Setting content-type to: '%s; charset=%s'",
                          transform->mediaType, doc->encoding);
            ap_set_content_type(f->r,
                                apr_psprintf(f->r->pool, "%s; charset=%s",
                                             transform->mediaType,
                                             doc->encoding));
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, f->r,
                          "Setting content-type to: '%s'",
                          transform->mediaType);
            ap_set_content_type(f->r,
                                apr_pstrdup(f->r->pool,
                                            transform->mediaType));
        }
    }
    else if (transform->method) {
        if (!strcmp(transform->method, "html")) {
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, f->r,
                          "Setting content-type as default to: text/html");
            ap_set_content_type(f->r, apr_pstrdup(f->r->pool, "text/html"));
        }
    }
    else {
            ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, f->r,
                          "mod_transform: Warning, no content type was set!");
    }

    output_ctx.next = f->next;
    output_ctx.bb = apr_brigade_create(f->r->pool,
                                       apr_bucket_alloc_create(f->r->pool));
    output = xmlOutputBufferCreateIO(&transform_output_write, &transform_output_close,
                                     &output_ctx, 0);
    length = xsltSaveResultTo(output, result, transform);
    if (!f->r->chunked)
        ap_set_content_length(f->r, length);

    xmlOutputBufferClose(output);
    xmlFreeDoc(result);
    if (!stylesheet_is_cached)
        xsltFreeStylesheet(transform);

    xmlParserInputBufferCreateFilenameDefault(orig);

    ap_pass_brigade(output_ctx.next, output_ctx.bb);
    return APR_SUCCESS;
}

static apr_status_t transform_filter(ap_filter_t * f, apr_bucket_brigade * bb)
{
    apr_bucket *b;
    const char *buf = 0;
    apr_size_t bytes = 0;
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) f->ctx;
    apr_status_t ret = APR_SUCCESS;
    xmlSetGenericErrorFunc((void *) f, transform_error_cb);

    /* First Run of this Filter */
    if (!ctxt) {
        /* unset content-length */
        apr_table_unset(f->r->headers_out, "Content-Length");

        /* TODO: Find a better way to determine if any resources needed to 
              create this document have changed.
         apr_table_unset(f->r->headers_out, "Last-Modified"); 
        */
    }

    if ((f->r->proto_num >= 1001) && !f->r->main && !f->r->prev)
        f->r->chunked = 1;

    for (b = APR_BRIGADE_FIRST(bb);
         b != APR_BRIGADE_SENTINEL(bb); b = APR_BUCKET_NEXT(b)) {
        if (APR_BUCKET_IS_EOS(b)) {
            if (ctxt) {    /* done reading the file. run the transform now */
                xmlParseChunk(ctxt, buf, 0, 1);
                ret = transform_run(f, ctxt->myDoc);
                xmlFreeParserCtxt(ctxt);
            }
        }
        else if (apr_bucket_read(b, &buf, &bytes, APR_BLOCK_READ)
                 == APR_SUCCESS) {
            if (ctxt) {
                xmlParseChunk(ctxt, buf, bytes, 0);
            }
            else {
                f->ctx = ctxt = xmlCreatePushParserCtxt(0, 0, buf, bytes, 0);
#if LIBXML_VERSION >= 20600
                xmlCtxtUseOptions(ctxt, XML_PARSE_NOENT | XML_PARSE_NOCDATA);
#endif
                ctxt->directory = xmlParserGetDirectory(f->r->filename);
            }
        }
    }
    apr_brigade_destroy(bb);
    xmlSetGenericErrorFunc(NULL, NULL);
    return ret;
}

/* -------------------------------------------------------------
    Config command stuff
   -------------------------------------------------------------
*/

static void *xml_merge_dir_config(apr_pool_t * p, void *basev, void *addv)
{
    dir_cfg *from = basev;
    dir_cfg *merge = addv;
    dir_cfg *to = apr_palloc(p, sizeof(dir_cfg));

    to->xslt = (merge->xslt != 0) ? merge->xslt : from->xslt;

    /* This code comes from mod_autoindex's IndexOptions */
    if (merge->opts & NO_OPTIONS) {
        /*
         * If the current directory says 'no options' then we also
         * clear any incremental mods from being inheritable further down.
         */
        to->opts = NO_OPTIONS;
        to->incremented_opts = 0;
        to->decremented_opts = 0;
    }
    else {
        /*
         * If there were any nonincremental options selected for
         * this directory, they dominate and we don't inherit *anything.*
         * Contrariwise, we *do* inherit if the only settings here are
         * incremental ones.
         */
        if (merge->opts == 0) {
            to->incremented_opts = (from->incremented_opts
                                    | merge->incremented_opts)
                & ~merge->decremented_opts;
            to->decremented_opts = (from->decremented_opts
                                    | merge->decremented_opts);
            /*
             * We may have incremental settings, so make sure we don't
             * inadvertently inherit an IndexOptions None from above.
             */
            to->opts = (from->opts & ~NO_OPTIONS);
        }
        else {
            /*
             * There are local nonincremental settings, which clear
             * all inheritance from above.  They *are* the new base settings.
             */
            to->opts = merge->opts;;
        }
        /*
         * We're guaranteed that there'll be no overlap between
         * the add-options and the remove-options.
         */
        to->opts |= to->incremented_opts;
        to->opts &= ~to->decremented_opts;
    }

    return to;
}

static void *xml_create_dir_config(apr_pool_t * p, char *x)
{
    dir_cfg *conf = apr_pcalloc(p, sizeof(dir_cfg));
    /* Enable XIncludes By Default (backwards compat..?) */
    conf->opts = 0 & XINCLUDES;
    conf->incremented_opts = 0;
    conf->decremented_opts = 0;
    return conf;
}

static const char *use_xslt(cmd_parms * cmd, void *cfg, const char *xslt)
{
    dir_cfg *conf = (dir_cfg *) cfg;
    conf->xslt = apr_pstrdup(cmd->pool, xslt);
    return NULL;
}

static int init_notes(request_rec * r)
{
    dir_cfg *conf = ap_get_module_config(r->per_dir_config,
                                         &transform_module);
    modxml_notes *notes = apr_pcalloc(r->pool, sizeof(modxml_notes));
    notes->xslt = conf->xslt;

    ap_set_module_config(r->request_config, &transform_module, notes);
    return OK;
}

static const char *add_opts(cmd_parms * cmd, void *d, const char *optstr)
{
    char *w;
    apr_int32_t opts;
    apr_int32_t opts_add;
    apr_int32_t opts_remove;
    char action;
    dir_cfg *d_cfg = (dir_cfg *) d;

    opts = d_cfg->opts;
    opts_add = d_cfg->incremented_opts;
    opts_remove = d_cfg->decremented_opts;
    while (optstr[0]) {
        int option = 0;

        w = ap_getword_conf(cmd->pool, &optstr);

        if ((*w == '+') || (*w == '-')) {
            action = *(w++);
        }
        else {
            action = '\0';
        }


        if (!strcasecmp(w, "ApacheFS")) {
            option = USE_APACHE_FS;
        }
        else if (!strcasecmp(w, "XIncludes")) {
            option = XINCLUDES;
        }
        else if (!strcasecmp(w, "None")) {
            if (action != '\0') {
                return "Cannot combine '+' or '-' with 'None' keyword";
            }
            opts = NO_OPTIONS;
            opts_add = 0;
            opts_remove = 0;
        }
        else {
            return "Invalid TransformOption";
        }

        if (action == '\0') {
            opts |= option;
            opts_add = 0;
            opts_remove = 0;
        }
        else if (action == '+') {
            opts_add |= option;
            opts_remove &= ~option;
        }
        else {
            opts_remove |= option;
            opts_add &= ~option;
        }
    }
    if ((opts & NO_OPTIONS) && (opts & ~NO_OPTIONS)) {
        return "Cannot combine other TransformOptions keywords with 'None'";
    }
    d_cfg->incremented_opts = opts_add;
    d_cfg->decremented_opts = opts_remove;
    d_cfg->opts = opts;
    return NULL;
}

static const command_rec transform_cmds[] = {

    AP_INIT_TAKE1("TransformSet", use_xslt, NULL, OR_ALL,
                  "Stylesheet to use"),

    AP_INIT_TAKE2("TransformCache", transform_add_xslt_cache, NULL, RSRC_CONF,
                  "URL and Path for stylesheet to preload"),

    AP_INIT_RAW_ARGS("TransformOptions", add_opts, NULL, OR_INDEXES,
                     "one or more index options [+|-][]"),
    {NULL}
};

static void transform_hooks(apr_pool_t * p)
{
    ap_hook_post_read_request(init_notes, NULL, NULL, APR_HOOK_MIDDLE);

    ap_register_output_filter(XSLT_FILTER_NAME, transform_filter, NULL,
                              AP_FTYPE_RESOURCE);

};

module AP_MODULE_DECLARE_DATA transform_module = {
    STANDARD20_MODULE_STUFF,
    xml_create_dir_config,
    xml_merge_dir_config,
    create_server_cfg,
    NULL,
    transform_cmds,
    transform_hooks
};

/* Exported Functions */
void mod_transform_set_XSLT(request_rec * r, const char *name)
{
    modxml_notes *notes = ap_get_module_config(r->request_config,
                                               &transform_module);
    notes->xslt = apr_pstrdup(r->pool, name);
}

void mod_transform_XSLTDoc(request_rec * r, xmlDocPtr doc)
{
    modxml_notes *notes = ap_get_module_config(r->request_config,
                                               &transform_module);
    notes->document = doc;
}
