#ifndef _MOD_XMLTRANSFORM_H
#define _MOD_XMLTRANSFORM_H
 
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_core.h"
#include "http_log.h"
#include "apr_tables.h"
#include <libxml/tree.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltutils.h>
#include <libxslt/transform.h>

#ifdef __cplusplus
extern "C" {
#endif
 
void mod_xmltransform_set_XSLT(request_rec* r, const char* name) ;
void mod_xmltransform_XSLTDoc(request_rec* r, xmlDocPtr doc) ;

#ifdef __cplusplus
}
#endif
 
#endif
